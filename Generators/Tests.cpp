#include "Generators.h"
#include "onnxruntime_cxx_api_2.h"
#include <assert.h>
#include <iostream>

#include "Sequences.h"
#include "generation_device_helper.h"
#include "logits_processor.h"
#include "greedy_search_impl_base.h"
#include "feeds_fetches_manager.h"
#include "greedy_search_impl_gpt.h"

#define ASSERT_EQ(a, b) assert((a) == (b))
#define ASSERT_TRUE(a) assert(a)

void Test_BeamSearchTest_GptBeamSearchFp32() {
  auto ort_env = OrtEnv::Create();

  std::vector<int64_t> input_ids_shape{3, 12};
  std::vector<int32_t> input_ids{
      0, 0, 0, 0, 0, 52, 195, 731, 321, 301, 734, 620,
      41, 554, 74, 622, 206, 222, 75, 223, 221, 198, 224, 572,
      0, 0, 0, 52, 328, 219, 328, 206, 288, 227, 896, 328};

  std::vector<int64_t> parameter_shape{1};
  std::vector<int32_t> max_length{20};
  std::vector<int32_t> min_length{1};
  std::vector<int32_t> num_beams{4};
  std::vector<int32_t> num_return_sequences{1};
  std::vector<float> length_penalty{1.0f};
  std::vector<float> repetition_penalty{1.0f};

  std::vector<int64_t> expected_output_shape{input_ids_shape[0], num_return_sequences[0], max_length[0]};
  std::vector<int32_t> expected_output{
      0, 0, 0, 0, 0, 52, 195, 731, 321, 301, 734, 620, 131, 131, 131, 181, 638, 638, 638, 638,
      41, 554, 74, 622, 206, 222, 75, 223, 221, 198, 224, 572, 292, 292, 292, 292, 292, 292, 292, 292,
      0, 0, 0, 52, 328, 219, 328, 206, 288, 227, 896, 328, 328, 669, 669, 669, 669, 669, 669, 669};

  auto info = OrtMemoryInfo::Create("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);
  auto input_ids_tensor = OrtValue::CreateTensor(
      *info, input_ids.data(), input_ids.size(), input_ids_shape.data(), input_ids_shape.size());

  auto max_length_tensor = OrtValue::CreateTensor(
      *info, max_length.data(), max_length.size(), parameter_shape.data(), parameter_shape.size());

  auto min_length_tensor = OrtValue::CreateTensor(
      *info, min_length.data(), min_length.size(), parameter_shape.data(), parameter_shape.size());

  auto num_beams_tensor = OrtValue::CreateTensor(
      *info, num_beams.data(), num_beams.size(), parameter_shape.data(), parameter_shape.size());

  auto num_return_sequences_tensor = OrtValue::CreateTensor(
      *info, num_return_sequences.data(), num_return_sequences.size(), parameter_shape.data(), parameter_shape.size());

  auto length_penalty_tensor = OrtValue::CreateTensor(
      *info, length_penalty.data(), length_penalty.size(), parameter_shape.data(), parameter_shape.size());

  auto repetition_penalty_tensor = OrtValue::CreateTensor(
      *info, repetition_penalty.data(), repetition_penalty.size(), parameter_shape.data(), parameter_shape.size());

  std::vector<OrtValue*> ort_inputs;
  ort_inputs.push_back(input_ids_tensor.get());
  ort_inputs.push_back(max_length_tensor.get());
  ort_inputs.push_back(min_length_tensor.get());
  ort_inputs.push_back(num_beams_tensor.get());
  ort_inputs.push_back(num_return_sequences_tensor.get());
  ort_inputs.push_back(length_penalty_tensor.get());
  ort_inputs.push_back(repetition_penalty_tensor.get());
  const char* input_names[] = {"input_ids", "max_length", "min_length", "num_beams", "num_return_sequences",
                               "length_penalty", "repetition_penalty"};
  const char* const output_names[] = {"sequences"};

  auto session_options = OrtSessionOptions::Create();
#ifdef USE_CUDA
  Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CUDA(session_options, 0));
#endif

  // The ONNX model is generated like the following:
  // python convert_generation.py --model_type gpt2 -m hf-internal-testing/tiny-random-gpt2
  //        --output tiny_gpt2_beamsearch_fp16.onnx --use_gpu --max_length 20
  // (with separate_gpt2_decoder_for_init_run set to False as it is now set to True by default)
  auto session = OrtSession::Create(*ort_env, ORT_TSTR("C:/code/github/generators/Generators/models/tiny_gpt2_beamsearch.onnx"), session_options.get());
  auto ort_outputs = session->Run(nullptr, input_names, ort_inputs.data(), ort_inputs.size(),
                                  output_names, 1);

  ASSERT_EQ(ort_outputs.size(), 1U);
  const auto& sequences = ort_outputs[0];
  ASSERT_TRUE(sequences->IsTensor());

  auto result_ts = sequences->GetTensorTypeAndShapeInfo();
  ASSERT_EQ(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, result_ts->GetElementType());

  ASSERT_EQ(expected_output_shape, result_ts->GetShape());
  const auto* result_vals = sequences->GetTensorData<int32_t>();
  auto result_span = gsl::make_span(result_vals, expected_output.size());
  ASSERT_TRUE(std::equal(expected_output.cbegin(), expected_output.cend(), result_span.begin(), result_span.end()));
  std::cout << "Test_BeamSearchTest_GptBeamSearchFp32 complete\r\n";
}

void Test_Lib_BeamSearchTest_GptBeamSearchFp32() {
  auto ort_env = OrtEnv::Create();

  std::vector<int64_t> input_ids_shape{3, 12};
  std::vector<int32_t> input_ids{
      0, 0, 0, 0, 0, 52, 195, 731, 321, 301, 734, 620,
      41, 554, 74, 622, 206, 222, 75, 223, 221, 198, 224, 572,
      0, 0, 0, 52, 328, 219, 328, 206, 288, 227, 896, 328};

  std::vector<int64_t> parameter_shape{1};
  std::vector<int32_t> max_length{20};
  std::vector<int32_t> min_length{1};
  std::vector<int32_t> num_beams{4};
  std::vector<int32_t> num_return_sequences{1};
  std::vector<float> length_penalty{1.0f};
  std::vector<float> repetition_penalty{1.0f};

  std::vector<int64_t> expected_output_shape{input_ids_shape[0], num_return_sequences[0], max_length[0]};
  std::vector<int32_t> expected_output{
      0, 0, 0, 0, 0, 52, 195, 731, 321, 301, 734, 620, 131, 131, 131, 181, 638, 638, 638, 638,
      41, 554, 74, 622, 206, 222, 75, 223, 221, 198, 224, 572, 292, 292, 292, 292, 292, 292, 292, 292,
      0, 0, 0, 52, 328, 219, 328, 206, 288, 227, 896, 328, 328, 669, 669, 669, 669, 669, 669, 669};

  auto info = OrtMemoryInfo::Create("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);
  auto input_ids_tensor = OrtValue::CreateTensor(
      *info, input_ids.data(), input_ids.size(), input_ids_shape.data(), input_ids_shape.size());

  auto max_length_tensor = OrtValue::CreateTensor(
      *info, max_length.data(), max_length.size(), parameter_shape.data(), parameter_shape.size());

  auto min_length_tensor = OrtValue::CreateTensor(
      *info, min_length.data(), min_length.size(), parameter_shape.data(), parameter_shape.size());

  auto num_beams_tensor = OrtValue::CreateTensor(
      *info, num_beams.data(), num_beams.size(), parameter_shape.data(), parameter_shape.size());

  auto num_return_sequences_tensor = OrtValue::CreateTensor(
      *info, num_return_sequences.data(), num_return_sequences.size(), parameter_shape.data(), parameter_shape.size());

  auto length_penalty_tensor = OrtValue::CreateTensor(
      *info, length_penalty.data(), length_penalty.size(), parameter_shape.data(), parameter_shape.size());

  auto repetition_penalty_tensor = OrtValue::CreateTensor(
      *info, repetition_penalty.data(), repetition_penalty.size(), parameter_shape.data(), parameter_shape.size());

  std::vector<OrtValue*> ort_inputs;
  ort_inputs.push_back(input_ids_tensor.get());
  ort_inputs.push_back(max_length_tensor.get());
  ort_inputs.push_back(min_length_tensor.get());
  ort_inputs.push_back(num_beams_tensor.get());
  ort_inputs.push_back(num_return_sequences_tensor.get());
  ort_inputs.push_back(length_penalty_tensor.get());
  ort_inputs.push_back(repetition_penalty_tensor.get());
  const char* input_names[] = {"input_ids", "max_length", "min_length", "num_beams", "num_return_sequences",
                               "length_penalty", "repetition_penalty"};
  const char* const output_names[] = {"sequences"};

  auto session_options = OrtSessionOptions::Create();
#ifdef USE_CUDA
  Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CUDA(session_options, 0));
#endif

  // The ONNX model is generated like the following:
  // python convert_generation.py --model_type gpt2 -m hf-internal-testing/tiny-random-gpt2
  //        --output tiny_gpt2_beamsearch_fp16.onnx --use_gpu --max_length 20
  // (with separate_gpt2_decoder_for_init_run set to False as it is now set to True by default)
  auto session = OrtSession::Create(*ort_env, ORT_TSTR("C:/code/github/generators/Generators/models/tiny_gpt2_beamsearch.onnx"), session_options.get());
  auto ort_outputs = session->Run(nullptr, input_names, ort_inputs.data(), ort_inputs.size(),
                                  output_names, 1);

  ASSERT_EQ(ort_outputs.size(), 1U);
  const auto& sequences = ort_outputs[0];
  ASSERT_TRUE(sequences->IsTensor());

  auto result_ts = sequences->GetTensorTypeAndShapeInfo();
  ASSERT_EQ(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, result_ts->GetElementType());

  ASSERT_EQ(expected_output_shape, result_ts->GetShape());
  const auto* result_vals = sequences->GetTensorData<int32_t>();
  auto result_span = gsl::make_span(result_vals, expected_output.size());
  ASSERT_TRUE(std::equal(expected_output.cbegin(), expected_output.cend(), result_span.begin(), result_span.end()));

  std::cout << "Test_Lib_BeamSearchTest_GptBeamSearchFp32 complete\r\n";
}

void Test_GreedySearchTest_GptGreedySearchFp32() {
  auto ort_env = OrtEnv::Create();

  std::vector<int64_t> input_ids_shape{2, 4};
  std::vector<int32_t> input_ids{
      0, 0, 0, 52, 0, 0, 195, 731};

  std::vector<int64_t> parameter_shape{1};
  std::vector<int32_t> max_length{10};
  std::vector<int32_t> min_length{1};
  std::vector<float> repetition_penalty{1.0f};

  std::vector<int64_t> expected_output_shape{input_ids_shape[0], max_length[0]};

  std::vector<int32_t> expected_output{
      0, 0, 0, 52, 204, 204, 204, 204, 204, 204,
      0, 0, 195, 731, 731, 114, 114, 114, 114, 114};

  auto info = OrtMemoryInfo::Create("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);
  auto input_ids_tensor = OrtValue::CreateTensor(
      *info, input_ids.data(), input_ids.size(), input_ids_shape.data(), input_ids_shape.size());

  auto max_length_tensor = OrtValue::CreateTensor(
      *info, max_length.data(), max_length.size(), parameter_shape.data(), parameter_shape.size());

  auto min_length_tensor = OrtValue::CreateTensor(
      *info, min_length.data(), min_length.size(), parameter_shape.data(), parameter_shape.size());

  auto repetition_penalty_tensor = OrtValue::CreateTensor(
      *info, repetition_penalty.data(), repetition_penalty.size(), parameter_shape.data(), parameter_shape.size());

  std::vector<OrtValue*> ort_inputs;
  ort_inputs.push_back(input_ids_tensor.get());
  ort_inputs.push_back(max_length_tensor.get());
  ort_inputs.push_back(min_length_tensor.get());
  ort_inputs.push_back(repetition_penalty_tensor.get());
  const char* input_names[] = {"input_ids", "max_length", "min_length", "repetition_penalty"};
  const char* const output_names[] = {"sequences"};

  constexpr int min_cuda_architecture = 530;
  auto session_options = OrtSessionOptions::Create();
#ifdef USE_CUDA
  Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CUDA(session_options, 0));
#endif

  auto session = OrtSession::Create(*ort_env, ORT_TSTR("C:/code/github/generators/Generators/models/tiny_gpt2_greedysearch_with_init_decoder.onnx"), session_options.get());

  auto ort_outputs = session->Run(nullptr, input_names, ort_inputs.data(), ort_inputs.size(), output_names, 1);

  ASSERT_EQ(ort_outputs.size(), 1U);
  const auto& sequences = ort_outputs[0];
  ASSERT_TRUE(sequences->IsTensor());

  auto result_ts = sequences->GetTensorTypeAndShapeInfo();
  ASSERT_EQ(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, result_ts->GetElementType());

  ASSERT_EQ(expected_output_shape, result_ts->GetShape());
  const auto* result_vals = sequences->GetTensorData<int32_t>();
  auto result_span = gsl::make_span(result_vals, expected_output.size());
  ASSERT_TRUE(std::equal(expected_output.cbegin(), expected_output.cend(), result_span.begin(), result_span.end()));

  std::cout << "Test_GreedySearchTest_GptGreedySearchFp32 complete\r\n";
}

void Test_Lib_GreedySearchTest_GptGreedySearchFp32() {
  auto ort_env = OrtEnv::Create();

  std::vector<int64_t> input_ids_shape{2, 4};
  std::vector<int32_t> input_ids{
      0, 0, 0, 52, 0, 0, 195, 731};

  std::vector<int64_t> parameter_shape{1};
  std::vector<int32_t> max_length{10};
  std::vector<int32_t> min_length{1};
  std::vector<float> repetition_penalty{1.0f};

  std::vector<int64_t> expected_output_shape{input_ids_shape[0], max_length[0]};

  std::vector<int32_t> expected_output{
      0, 0, 0, 52, 204, 204, 204, 204, 204, 204,
      0, 0, 195, 731, 731, 114, 114, 114, 114, 114};

  auto info = OrtMemoryInfo::Create("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);
  auto input_ids_tensor = OrtValue::CreateTensor(
      *info, input_ids.data(), input_ids.size(), input_ids_shape.data(), input_ids_shape.size());

  auto max_length_tensor = OrtValue::CreateTensor(
      *info, max_length.data(), max_length.size(), parameter_shape.data(), parameter_shape.size());

  auto min_length_tensor = OrtValue::CreateTensor(
      *info, min_length.data(), min_length.size(), parameter_shape.data(), parameter_shape.size());

  auto repetition_penalty_tensor = OrtValue::CreateTensor(
      *info, repetition_penalty.data(), repetition_penalty.size(), parameter_shape.data(), parameter_shape.size());

  std::vector<OrtValue*> ort_inputs;
  ort_inputs.push_back(input_ids_tensor.get());
  ort_inputs.push_back(max_length_tensor.get());
  ort_inputs.push_back(min_length_tensor.get());
  ort_inputs.push_back(repetition_penalty_tensor.get());
  const char* input_names[] = {"input_ids", "max_length", "min_length", "repetition_penalty"};
  const char* const output_names[] = {"sequences"};

  constexpr int min_cuda_architecture = 530;
  auto session_options = OrtSessionOptions::Create();
#ifdef USE_CUDA
  Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CUDA(session_options, 0));
#endif

  GreedySearchGpt<float, GreedySearchParameters> impl{
      *ctx_internal,
      has_init_decoder_ ? init_run_decoder_session_state : nullptr,
      has_init_decoder_ ? init_run_gpt_subgraph_.get() : nullptr,
      *decoder_session_state,
      *gpt_subgraph_,
      thread_pool,
      ctx->GetComputeStream(),
      dumper_,
      parameters,
      GenerationCpuDeviceHelper::CreateGptInputs,
      add_to_feeds_func_ ? add_to_feeds_func_ : GenerationCpuDeviceHelper::AddToFeeds,
      topk_func_ ? topk_func_ : GenerationCpuDeviceHelper::TopK,
      process_logits_func_ ? process_logits_func_ : GenerationCpuDeviceHelper::GreedySearchProcessLogits<float>,
      init_greedy_state_func_ ? init_greedy_state_func_ : GenerationCpuDeviceHelper::InitGreedyState<float>,
      device_copy_func_ ? device_copy_func_ : GenerationCpuDeviceHelper::DeviceCopy<float>,
      update_gpt_feeds_func_ ? update_gpt_feeds_func_ : GenerationCpuDeviceHelper::UpdateGptFeeds<float>};

  auto session = OrtSession::Create(*ort_env, ORT_TSTR("C:/code/github/generators/Generators/models/tiny-random-gpt2_past_fp32.onnx"), session_options.get());

  auto ort_outputs = session->Run(nullptr, input_names, ort_inputs.data(), ort_inputs.size(), output_names, 1);

  ASSERT_EQ(ort_outputs.size(), 1U);
  const auto& sequences = ort_outputs[0];
  ASSERT_TRUE(sequences->IsTensor());

  auto result_ts = sequences->GetTensorTypeAndShapeInfo();
  ASSERT_EQ(ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, result_ts->GetElementType());

  ASSERT_EQ(expected_output_shape, result_ts->GetShape());
  const auto* result_vals = sequences->GetTensorData<int32_t>();
  auto result_span = gsl::make_span(result_vals, expected_output.size());
  ASSERT_TRUE(std::equal(expected_output.cbegin(), expected_output.cend(), result_span.begin(), result_span.end()));

  std::cout << "Test_Lib_GreedySearchTest_GptGreedySearchFp32 complete\r\n";
}