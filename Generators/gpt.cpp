#include "Generators.h"

Gpt::Gpt(OrtEnv& ort_env, const ORTCHAR_T* init_path, const ORTCHAR_T* decode_path,
         std::unique_ptr<OrtValue>&& input_ids, SearchParams params)
    : initial_input_ids_{std::move(input_ids)},
      params_{params} {
  auto session_options = OrtSessionOptions::Create();

  session_init_ = OrtSession::Create(ort_env, init_path, session_options.get());
  session_decode_ = OrtSession::Create(ort_env, decode_path, session_options.get());
}

void Gpt::CreateInputs(gsl::span<int32_t> sequence_lengths) {
  CreateInputsInternal(sequence_lengths);
  auto& allocator = Ort::Allocator::GetWithDefaultOptions();

  for (auto* input : {expanded_input_ids_.get(), expanded_position_ids_.get(), expanded_attention_mask_.get()})
    inputs_.push_back(input);
  for (auto* name : {"input_ids", "position_ids", "attention_mask"})
    input_name_strings_.push_back(name);

  output_name_strings_.push_back("logits");

  auto past_type = /*IsOutputFloat16()*/ Ort::TypeToTensorType<float>::type;
  if (!past_present_share_buffer_) {
    // Initialize empty past state
    int64_t past_shape[] = {2, params_.batch_size * params_.num_beams, params_.num_heads, 0, params_.head_size};

    // The remaining inputs are past state.
    for (int i = 0; i < c_counts; ++i) {
      pasts_[i] = OrtValue::CreateTensor(allocator, past_shape, std::size(past_shape), past_type);
      inputs_.push_back(pasts_[i].get());

      char string[32];
      snprintf(string, std::size(string), "past_%d", i);
      input_name_strings_.push_back(string);
    }
  } else {
    assert(false);
  }

  {
    int64_t logits_shape[] = {params_.batch_size, 1, params_.vocab_size};
    logits_ = OrtValue::CreateTensor(allocator, logits_shape, std::size(logits_shape), past_type);
    outputs_.push_back(logits_.get());
  }
  {
    int64_t present_shape[] = {2, params_.batch_size * params_.num_beams, params_.num_heads, 4, params_.head_size};

    for (int i = 0; i < c_counts; ++i) {
      presents_[i] = OrtValue::CreateTensor(allocator, present_shape, std::size(present_shape), past_type);
      outputs_.push_back(presents_[i].get());

      char string[32];
      snprintf(string, std::size(string), "present_%d", i);
      output_name_strings_.push_back(string);
    }
  }

  for (auto& input_name : input_name_strings_)
    input_names_.push_back(input_name.c_str());
  for (auto& output_name : output_name_strings_)
    output_names_.push_back(output_name.c_str());
}

void Gpt::CreateInputsInternal(gsl::span<int32_t> sequence_lengths) {
#if 0
}
    const Tensor* original_input_ids,
    const OrtValue* attn_mask_value,
    int num_beams,
    int pad_token_id,
    gsl::span<int32_t>& sequence_lengths,
    OrtAllocator* allocator,
    OrtValue& expanded_input_ids,
    OrtValue& expanded_position_ids,
    OrtValue& expanded_attention_mask) {
#endif

  const TensorShape input_ids_shape = initial_input_ids_->GetTensorTypeAndShapeInfo()->GetShape();
  ORT_ENFORCE(input_ids_shape.NumDimensions() == 2);
  const int64_t batch_size = input_ids_shape[0];
  const int64_t sequence_length = input_ids_shape[1];

  // Allocate position_ids and attention_mask based on shape of input_ids
  auto element_type = Ort::TypeToTensorType<int32_t>::type;
  auto& allocator = Ort::Allocator::GetWithDefaultOptions();

  const OrtMemoryInfo& location = allocator.GetInfo();

  // Use original input_ids. This requires the input_ids for subgraph is also int32.
  // Current shape is (batch_size, sequence_length)
  // Note that we will expand it to (batch_size * num_beams, sequence_length) later.
  // To avoid cloning input_ids, we use const_cast here since this function does not change its content.
  input_ids_ = OrtValue::CreateTensor<int32_t>(allocator, input_ids_shape.GetDims().data(), input_ids_shape.GetDims().size());
  std::copy(initial_input_ids_->GetTensorMutableData<int32_t>(),
            initial_input_ids_->GetTensorMutableData<int32_t>() + input_ids_shape.Size(),
            input_ids_->GetTensorMutableData<int32_t>());
  //  Tensor::InitOrtValue(element_type, input_ids_shape,
  //                       const_cast<Tensor*>(original_input_ids)->MutableData<int32_t>(), location, input_ids);

  position_ids_ = OrtValue::CreateTensor<int32_t>(allocator, input_ids_shape.GetDims().data(), input_ids_shape.GetDims().size());
  //  Tensor::InitOrtValue(element_type, input_ids_shape, allocator, position_ids);

  void* attn_mask_value = nullptr;  // TODO: Temporary hack until needed
#if 0
  attention_mask_;
  if (attn_mask_value != nullptr) {
    const Tensor& attn_mask = attn_mask_value->Get<Tensor>();
    Tensor::InitOrtValue(element_type, input_ids_shape, const_cast<Tensor*>(&attn_mask)->MutableData<int32_t>(),
                         allocator->Info(), attention_mask);
  } else {
#endif
  attention_mask_ = OrtValue::CreateTensor<int32_t>(allocator, input_ids_shape.GetDims().data(), input_ids_shape.GetDims().size());

  // Set attention mask to be 0 for pad tokens, and 1 for all other tokens.
  // Set position id to be 0 for pad tokens, and accumulated sum of mask in a batch for other tokens
  int32_t* mask_data = attention_mask_->GetTensorMutableData<int32_t>();
  int32_t* position_data = position_ids_->GetTensorMutableData<int32_t>();
  const int32_t* word_id = initial_input_ids_->GetTensorMutableData<int32_t>();
  int32_t* mask = mask_data;
  int32_t* position = position_data;
  for (int i = 0; i < batch_size; i++) {
    int32_t abs_position = 0;
    for (int j = 0; j < sequence_length; j++, word_id++, mask++, position++) {
      if (*word_id == params_.pad_token_id) {
        if (attn_mask_value == nullptr) {
          *mask = 0;
        }
        *position = 0;
      } else {
        if (attn_mask_value == nullptr) {
          *mask = 1;
        }
        *position = abs_position;
        abs_position++;
      }
    }

    for (int k = 0; k < params_.num_beams; k++) {
      sequence_lengths[SafeInt<gsl::index>(i) * params_.num_beams + k] = abs_position;
    }
  }

  // Expand (batch_size, sequence_length) to (batch_size * num_beams, sequence_length)
  // TODO(tianleiwu): Try expand outputs after first subgraph call instead. That may get better performance.
  if (params_.num_beams == 1) {
    expanded_input_ids_ = std::move(input_ids_);
    expanded_position_ids_ = std::move(position_ids_);
    expanded_attention_mask_ = std::move(attention_mask_);
  } else {
    assert(false);
#if 0
    ExpandInputs<int32_t>(input_ids, num_beams, allocator, expanded_input_ids);
    ExpandInputs<int32_t>(position_ids, num_beams, allocator, expanded_position_ids);
    ExpandInputs<int32_t>(attention_mask, num_beams, allocator, expanded_attention_mask);
#endif
  }
}

void Gpt::Run() {
  session_init_->Run(nullptr, input_names_.data(), inputs_.data(), input_names_.size(), output_names_.data(), outputs_.data(), output_names_.size());
}