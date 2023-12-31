#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include "../generators.h"
#include "../search.h"
#include "../search_cuda.h"
#include "../models/gpt_cpu.h"
#include "../models/gpt_cuda.h"
#include "../models/llama_cpu.h"
#include "../models/llama_cuda.h"
#include <iostream>

using namespace pybind11::literals;

#define STRINGIFY(x) #x
#define MACRO_STRINGIFY(x) STRINGIFY(x)

#ifdef _WIN32
#include <Windows.h>

struct ORTCHAR_String {
  ORTCHAR_String(const char* utf8) {
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    string_.resize(wide_length);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &string_[0], wide_length);
  }

  operator const ORTCHAR_T*() const { return string_.c_str(); }

 private:
  std::wstring string_;
};
#else
#define ORTCHAR_String(string) string
#endif

struct float16 {
  uint16_t v_;
  float AsFloat32() const {
    // Extract sign, exponent, and fraction from numpy.float16
    int sign = (v_ & 0x8000) >> 15;
    int exponent = (v_ & 0x7C00) >> 10;
    int fraction = v_ & 0x03FF;

    // Handle special cases
    if (exponent == 0) {
      if (fraction == 0) {
        // Zero
        return sign ? -0.0f : 0.0f;
      } else {
        // Subnormal number
        return std::ldexp((sign ? -1.0f : 1.0f) * fraction / 1024.0f, -14);
      }
    } else if (exponent == 31) {
      if (fraction == 0) {
        // Infinity
        return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
      } else {
        // NaN
        return std::numeric_limits<float>::quiet_NaN();
      }
    }

    // Normalized number
    return std::ldexp((sign ? -1.0f : 1.0f) * (1.0f + fraction / 1024.0f), exponent - 15);
  }
};

namespace pybind11 {
namespace detail {
template <>
struct npy_format_descriptor<float16> {
  static constexpr auto name = _("float16");
  static pybind11::dtype dtype() {
    handle ptr = npy_api::get().PyArray_DescrFromType_(23 /*NPY_FLOAT16*/); /* import numpy as np; print(np.dtype(np.float16).num */
    return reinterpret_borrow<pybind11::dtype>(ptr);
  }
  static std::string format() {
    // following: https://docs.python.org/3/library/struct.html#format-characters
    return "e";
  }
};
}  // namespace detail
}  // namespace pybind11

template <typename T>
std::span<T> ToSpan(pybind11::array_t<T> v) {
  if constexpr (std::is_const_v<T>)
    return {v.data(), static_cast<size_t>(v.size())};
  else
    return {v.mutable_data(), static_cast<size_t>(v.size())};
}

template <typename T>
pybind11::array_t<T> ToPython(std::span<T> v) {
  return pybind11::array_t<T>(v.size(), v.data());
}

template <typename T>
pybind11::array_t<T> ToUnownedPython(std::span<T> v) {
  return pybind11::array_t<T>({v.size()}, {sizeof(T)}, v.data(), pybind11::capsule(v.data(), [](void*) {}));
}

namespace Generators {

void TestFP32(pybind11::array_t<float> inputs) {
  pybind11::buffer_info buf_info = inputs.request();
  const float* p = static_cast<const float*>(buf_info.ptr);

  std::cout << "float32 values: ";

  for (unsigned i = 0; i < buf_info.size; i++)
    std::cout << p[i] << " ";
  std::cout << std::endl;
}

void TestFP16(pybind11::array_t<float16> inputs) {
  pybind11::buffer_info buf_info = inputs.request();
  const float16* p = static_cast<const float16*>(buf_info.ptr);

  std::cout << "float16 values: ";

  for (unsigned i = 0; i < buf_info.size; i++)
    std::cout << p[i].AsFloat32() << " ";
  std::cout << std::endl;
}

std::string ToString(const SearchParams& v) {
  std::ostringstream oss;
  oss << "SearchParams("
         "num_beams="
      << v.num_beams << ", batch_size=" << v.batch_size << ", sequence_length=" << v.sequence_length << ", max_length=" << v.max_length << ", pad_token_id=" << v.pad_token_id << ", eos_token_id=" << v.eos_token_id << ", vocab_size=" << v.vocab_size << ", length_penalty=" << v.length_penalty << ", early_stopping=" << v.early_stopping << ")";

  return oss.str();
}

std::string ToString(const Gpt_Model& v) {
  std::ostringstream oss;
  oss << "Gpt_Model("
         "vocab_size="
      << v.vocab_size_ << ", head_count=" << v.head_count_ << ", hidden_size=" << v.hidden_size_ << ", layer_count=" << v.layer_count_ << ")";

  return oss.str();
}

std::unique_ptr<OrtEnv> g_ort_env;

OrtEnv& GetOrtEnv() {
  if (!g_ort_env)
    g_ort_env = OrtEnv::Create();
  return *g_ort_env;
}

// A roaming array is one that can be in CPU or GPU memory, and will copy the memory as needed to be used from anywhere
template <typename T>
struct RoamingArray {
  void SetCPU(std::span<T> cpu) {
    cpu_memory_ = cpu;
    device_memory_ = {};
  }

  void SetGPU(std::span<T> device) {
    device_memory_ = device;
    cpu_memory_ = {};
  }

  std::span<T> GetCPUArray() {
    if (cpu_memory_.empty() && !device_memory_.empty()) {
      cpu_memory_owner_ = CudaMallocHostArray<T>(device_memory_.size(), &cpu_memory_);
      cudaMemcpy(cpu_memory_.data(), device_memory_.data(), cpu_memory_.size_bytes(), cudaMemcpyDeviceToHost);
    }

    return cpu_memory_;
  }

  pybind11::array_t<T> GetNumpyArray() {
    GetCPUArray();
    py_cpu_array_ = pybind11::array_t<T>({cpu_memory_.size()}, {sizeof(T)}, cpu_memory_.data(), pybind11::capsule(cpu_memory_.data(), [](void*) {}));
    return py_cpu_array_;
  }

  std::span<T> GetGPUArray() {
    if (device_memory_.empty() && !cpu_memory_.empty()) {
      device_memory_owner_ = CudaMallocArray<T>(cpu_memory_.size(), &device_memory_);
      cudaMemcpy(device_memory_.data(), cpu_memory_.data(), cpu_memory_.size_bytes(), cudaMemcpyHostToDevice);
    }
    return device_memory_;
  }

  std::span<T> device_memory_;
  cuda_unique_ptr<T> device_memory_owner_;

  std::span<T> cpu_memory_;
  cuda_host_unique_ptr<T> cpu_memory_owner_;
  pybind11::array_t<T> py_cpu_array_;
};

template <typename T>
void Declare_DeviceArray(pybind11::module& m, const char* name) {
  using Type = RoamingArray<T>;
  pybind11::class_<Type>(m, name)
      .def("GetArray", [](Type& t) -> pybind11::array_t<T> { return t.GetNumpyArray(); }, pybind11::return_value_policy::reference_internal);
}

struct PySearchParams : SearchParams {
  pybind11::array_t<int32_t> py_input_ids_;
};

struct PyGreedySearch {
  PyGreedySearch(const PySearchParams& params, DeviceType device_type) {
    if (device_type == DeviceType::CUDA) {
      SearchParams_Cuda params_cuda; // Includes cuda_stream, which defaults to nullptr
      static_cast<SearchParams&>(params_cuda) = params;
      cuda_ = std::make_unique<GreedySearch_Cuda>(params_cuda);
    }
    else
      cpu_ = std::make_unique<GreedySearch>(params);
  }

  void SetLogits(RoamingArray<float>& inputs) {
    if (cuda_)
      cuda_->SetLogits(inputs.GetGPUArray());
    else
      cpu_->SetLogits(inputs.GetCPUArray());
  }

  int GetSequenceLength() const {
     if (cuda_)
      return cuda_->GetSequenceLength();
     return cpu_->GetSequenceLength();
  }

  RoamingArray<int32_t>& GetNextTokens() {
    if(cuda_)
      py_tokens_.SetGPU(cuda_->GetNextTokens());
    else
      py_tokens_.SetCPU(cpu_->GetNextTokens());
    return py_tokens_;
  }

  RoamingArray<int32_t>& GetSequenceLengths() {
    if(cuda_)
      py_sequencelengths_.SetGPU(cuda_->sequence_lengths_);
    else
      py_sequencelengths_.SetCPU(cpu_->sequence_lengths_);

    return py_sequencelengths_;
  }

  RoamingArray<int32_t>& GetSequence(int index) {
    if (cuda_)
      py_sequence_.SetGPU(cuda_->sequences_.GetSequence(index));
    else
      py_sequence_.SetCPU(cpu_->sequences_.GetSequence(index));
    return py_sequence_;
  }

  bool IsDone() const {
    if (cuda_)
      return cuda_->IsDone();
    return cpu_->IsDone();
  }

  void SelectTop() {
    if(cuda_)
      cuda_->SelectTop();
    else
      cpu_->SelectTop();
  }

  void SampleTopK(int k, float t) {
    if (cuda_)
      cuda_->SampleTopK(k, t);
    else
      cpu_->SampleTopK(k, t);
  }

  void SampleTopP(float p, float t) {
    if(cuda_)
      cuda_->SampleTopP(p, t);
    else
      cpu_->SampleTopP(p, t);
  }

 private:
  std::unique_ptr<GreedySearch> cpu_;
  std::unique_ptr<GreedySearch_Cuda> cuda_;

  RoamingArray<int32_t> py_tokens_;
  RoamingArray<int32_t> py_sequence_;
  RoamingArray<int32_t> py_sequencelengths_;
};

struct PyBeamSearch {
  PyBeamSearch(const PySearchParams& params, DeviceType device_type) {
    if (device_type == DeviceType::CUDA) {
      SearchParams_Cuda params_cuda;  // Includes cuda_stream, which defaults to nullptr
      static_cast<SearchParams&>(params_cuda) = params;
      cuda_ = std::make_unique<BeamSearch_Cuda>(params_cuda);
    } else
      cpu_ = std::make_unique<BeamSearch>(params);
  }

  void SetLogits(RoamingArray<float>& inputs) {
    if (cuda_)
      cuda_->SetLogits(inputs.GetGPUArray());
    else
      cpu_->SetLogits(inputs.GetCPUArray());
  }

  RoamingArray<int32_t>& GetNextTokens() {
    if (cuda_)
      py_tokens_.SetGPU(cuda_->GetNextTokens());
    else
      py_tokens_.SetCPU(cpu_->GetNextTokens());
    return py_tokens_;
  }

  RoamingArray<int32_t>& GetNextIndices() {
    if(cuda_)
      py_indices_.SetGPU(cuda_->GetNextIndices());
    else
      py_indices_.SetCPU(cpu_->GetNextIndices());
    return py_indices_;
  }

  RoamingArray<int32_t>& GetSequenceLengths() {
    if(cuda_)
      py_sequencelengths_.SetGPU(cuda_->sequence_lengths_);
    else
      py_sequencelengths_.SetCPU(cpu_->sequence_lengths_);
    return py_sequencelengths_;
  }

  RoamingArray<int32_t>& GetSequence(int index) {
    if (cuda_)
      py_sequence_.SetGPU(cuda_->sequences_.GetSequence(index));
    else
      py_sequence_.SetCPU(cpu_->sequences_.GetSequence(index));
    return py_sequence_;
  }

  int GetSequenceLength() const {
    if (cuda_)
      return cuda_->GetSequenceLength();
    return cpu_->GetSequenceLength();
  }

  bool IsDone() const {
    if (cuda_)
      return cuda_->IsDone();
    return cpu_->IsDone();
  }

  void SelectTop() {
    if (cuda_)
      cuda_->SelectTop();
    else
      cpu_->SelectTop();
  }

 private:
  std::unique_ptr<BeamSearch_Cuda> cuda_;
  std::unique_ptr<BeamSearch> cpu_;

  RoamingArray<int32_t> py_tokens_;
  RoamingArray<int32_t> py_indices_;
  RoamingArray<int32_t> py_sequence_;
  RoamingArray<int32_t> py_sequencelengths_;
};

struct PyGpt_State {
  PyGpt_State(Gpt_Model& model, RoamingArray<int32_t>& sequence_lengths, const SearchParams& search_params) {
    if (model.GetDeviceType()==DeviceType::CUDA)
      cuda_ = std::make_unique<Gpt_Cuda>(model, sequence_lengths.GetGPUArray(), search_params);
    else
      cpu_ = std::make_unique<Gpt_State>(model, sequence_lengths.GetCPUArray(), search_params);
  }

  RoamingArray<float>& Run(int current_length, RoamingArray<int32_t>& next_tokens, RoamingArray<int32_t>& next_indices)
  {
    if (cuda_)
      py_logits_.SetGPU(cuda_->Run(current_length, next_tokens.GetGPUArray(), next_indices.GetGPUArray()));
    else
      py_logits_.SetCPU(cpu_->Run(current_length, next_tokens.GetCPUArray(), next_indices.GetCPUArray()));

    return py_logits_;
  }

 private:
  std::unique_ptr<Gpt_State> cpu_;
  std::unique_ptr<Gpt_Cuda> cuda_;
  RoamingArray<float> py_logits_;
};

struct PyLlama_State {
  PyLlama_State(Llama_Model& model, RoamingArray<int32_t>& sequence_lengths, const SearchParams& search_params) {
    if (model.GetDeviceType() == DeviceType::CUDA)
      cuda_ = std::make_unique<Llama_Cuda>(model, sequence_lengths.GetGPUArray(), search_params);
    else
      cpu_ = std::make_unique<Llama_State>(model, sequence_lengths.GetCPUArray(), search_params);
  }

  RoamingArray<float>& Run(int current_length, RoamingArray<int32_t>& next_tokens) {
    if (cuda_)
      py_logits_.SetGPU(cuda_->Run(current_length, next_tokens.GetGPUArray()));
    else
      py_logits_.SetCPU(cpu_->Run(current_length, next_tokens.GetCPUArray()));

    return py_logits_;
  }

 private:
  std::unique_ptr<Llama_Cuda> cuda_;
  std::unique_ptr<Llama_State> cpu_;

  RoamingArray<float> py_logits_;
};

PYBIND11_MODULE(ort_generators, m) {
  m.doc() = R"pbdoc(
        Ort Generators library
        ----------------------

        .. currentmodule:: cmake_example

        .. autosummary::
           :toctree: _generate

    )pbdoc";

  Declare_DeviceArray<ScoreType>(m, "DeviceArray_ScoreType");
  Declare_DeviceArray<int32_t>(m, "DeviceArray_int32");

  pybind11::enum_<DeviceType>(m, "DeviceType")
      .value("Auto", DeviceType::Auto)
      .value("CPU", DeviceType::CPU)
      .value("CUDA", DeviceType::CUDA)
      .export_values();

  pybind11::class_<PySearchParams>(m, "SearchParams")
      .def(pybind11::init<>())
      .def_readwrite("num_beams", &PySearchParams::num_beams)
      .def_readwrite("batch_size", &PySearchParams::batch_size)
      .def_readwrite("sequence_length", &PySearchParams::sequence_length)
      .def_readwrite("max_length", &PySearchParams::max_length)
      .def_readwrite("pad_token_id", &PySearchParams::pad_token_id)
      .def_readwrite("eos_token_id", &PySearchParams::eos_token_id)
      .def_readwrite("vocab_size", &PySearchParams::vocab_size)
      .def_readwrite("length_penalty", &PySearchParams::length_penalty)
      .def_readwrite("early_stopping", &PySearchParams::early_stopping)
      .def_property(
          "input_ids",
          [](PySearchParams& s) -> pybind11::array_t<int32_t> { return s.py_input_ids_; },
          [](PySearchParams& s, pybind11::array_t<int32_t> v) { s.py_input_ids_=v; s.input_ids = ToSpan(s.py_input_ids_); })
      .def("__repr__", [](PySearchParams& s) { return ToString(s); });

  pybind11::class_<PyGreedySearch>(m, "GreedySearch")
      .def(pybind11::init<const PySearchParams&, DeviceType>())
      .def("SetLogits", &PyGreedySearch::SetLogits)
      .def("GetSequenceLength", &PyGreedySearch::GetSequenceLength)
      .def("GetSequenceLengths", &PyGreedySearch::GetSequenceLengths, pybind11::return_value_policy::reference_internal)
      .def("GetNextTokens", &PyGreedySearch::GetNextTokens, pybind11::return_value_policy::reference_internal)
      .def("IsDone", &PyGreedySearch::IsDone)
      .def("SelectTop", &PyGreedySearch::SelectTop)
      .def("SampleTopK", &PyGreedySearch::SampleTopK)
      .def("SampleTopP", &PyGreedySearch::SampleTopP)
      .def("GetSequence", &PyGreedySearch::GetSequence, pybind11::return_value_policy::reference_internal);

  pybind11::class_<PyBeamSearch>(m, "BeamSearch")
      .def(pybind11::init<const PySearchParams&, DeviceType>())
      .def("SetLogits", &PyBeamSearch::SetLogits)
      .def("GetSequenceLength", &PyBeamSearch::GetSequenceLength)
      .def("GetSequenceLengths", &PyBeamSearch::GetSequenceLengths, pybind11::return_value_policy::reference_internal)
      .def("GetNextTokens", &PyBeamSearch::GetNextTokens, pybind11::return_value_policy::reference_internal)
      .def("GetNextIndices", &PyBeamSearch::GetNextIndices, pybind11::return_value_policy::reference_internal)
      .def("IsDone", &PyBeamSearch::IsDone)
      .def("SelectTop", &PyBeamSearch::SelectTop)
      .def("GetSequence", &PyBeamSearch::GetSequence, pybind11::return_value_policy::reference_internal);

  // If we support models, we need to init the OrtApi
  Ort::InitApi();

  m.def("print", &TestFP32, "Test float32");
  m.def("print", &TestFP16, "Test float16");

  pybind11::class_<Gpt_Model>(m, "Gpt_Model")
      .def(pybind11::init([](const std::string& str, DeviceType device_type) {
             if (device_type == DeviceType::CUDA)
               return new Gpt_Model(GetOrtEnv(), ORTCHAR_String(str.c_str()), nullptr);
             return new Gpt_Model(GetOrtEnv(), ORTCHAR_String(str.c_str()));
           }),
           "str"_a, "device_type"_a = DeviceType::Auto)
      .def("GetVocabSize", &Gpt_Model::GetVocabSize)
      .def_property_readonly("DeviceType", [](const Gpt_Model& s) { return s.GetDeviceType(); });

  pybind11::class_<PyGpt_State>(m, "Gpt_State")
      .def(pybind11::init<Gpt_Model& , RoamingArray<int32_t>& , const PySearchParams&>())
      .def("Run", &PyGpt_State::Run, "current_length"_a, "next_tokens"_a, "next_indices"_a = RoamingArray<int32_t>{},
           pybind11::return_value_policy::reference_internal);

  pybind11::class_<Llama_Model>(m, "Llama_Model")
      .def(pybind11::init([](const std::string& str, DeviceType device_type) {
             if (device_type == DeviceType::CUDA)
               return new Llama_Model(GetOrtEnv(), ORTCHAR_String(str.c_str()), nullptr);
             return new Llama_Model(GetOrtEnv(), ORTCHAR_String(str.c_str()));
           }),
           "str"_a, "device_type"_a = DeviceType::Auto)
      .def("GetVocabSize", &Llama_Model::GetVocabSize)
      .def_property_readonly("DeviceType", [](const Llama_Model& s) { return s.GetDeviceType(); });

  pybind11::class_<PyLlama_State>(m, "Llama_State")
      .def(pybind11::init<Llama_Model&, RoamingArray<int32_t>&, const PySearchParams&>())
      .def("Run", &PyLlama_State::Run,
          pybind11::return_value_policy::reference_internal);

#ifdef VERSION_INFO
  m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
  m.attr("__version__") = "dev";
#endif
}

}  // namespace Generators