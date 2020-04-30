#pragma once

#ifdef USE_PYTORCH_QNNPACK
#include <ATen/ATen.h>
#include <pytorch_qnnpack.h>
#include <qnnpack_func.h>

#include <utility>

struct QnnpackOperatorDeleter {
  void operator()(pytorch_qnnp_operator_t op) {
    pytorch_qnnp_delete_operator(op);
  }
};

// PackedWeight struct for QNNPACK stores the original Weight and Bias as
// QNNPACK currently does not support an unpack function. Possible optimization -
// For PyTorch Mobile, once the model is scripted and serialized we don't need
// to call unpack, so we can save some memory by checking for this case.
// Input scale is set to null in pre-pack step. QNNPACK needs bias quantized with
// input scale which is available at runtime in pytorch. During runtime if input
// scale value changes then we requantize bias with the updated scale.
// For inference we expect the graph to be static so the input scale should
// not change across consecutive inference calls.
struct PackedLinearWeightsQnnp {
  std::unique_ptr<qnnpack::PackBMatrix> w;
  at::Tensor orig_weight;
  at::Tensor bias;
  c10::optional<double> input_scale;
  at::Tensor w_scales;
  at::Tensor w_zero_points;
  std::vector<float> requantization_scale;
};

struct PackedConvWeightsQnnp {
  std::unique_ptr<qnnpack::PrePackConvWeights> w;
  at::Tensor orig_weight;
  at::Tensor bias;
  c10::optional<double> input_scale;
  std::vector<int64_t> kernel;
  at::Tensor w_scales;
  at::Tensor w_zero_points;
  std::vector<float> requantization_scale;
};

enum class Activation : uint8_t { NONE = 0, RELU = 1 };

#if defined(__ANDROID__) && !defined(__NDK_MAJOR__)
template <class T>
inline float Round(const float x) {
  return ::nearbyintf(x);
}
inline double Round(const double x) {
  return ::nearbyint(x);
}
#else
template <class T>
inline T Round(const T x) {
  return std::nearbyint(x);
}
#endif

inline uint8_t QuantizeUint8(float scale, int32_t zero_point, float value) {
  const int32_t qmin = std::numeric_limits<uint8_t>::min();
  const int32_t qmax = std::numeric_limits<uint8_t>::max();
  auto r = zero_point + static_cast<int32_t>(Round(value / scale));
  r = std::max(r, qmin);
  r = std::min(r, qmax);
  return static_cast<uint8_t>(r);
}

inline std::pair<uint8_t, uint8_t> activationLimits(
    float scale,
    int32_t zero_point,
    Activation Ac) {
  switch (Ac) {
    case Activation::NONE:
      return {std::numeric_limits<uint8_t>::min(),
              std::numeric_limits<uint8_t>::max()};
    case Activation::RELU:
      return {QuantizeUint8(scale, zero_point, 0.0),
              std::numeric_limits<uint8_t>::max()};
    default:
#ifdef _MSC_VER
      __assume(0);
#else
      __builtin_unreachable();
#endif
  }
}
#endif

namespace {
std::vector<float> generate_requantization_scales(
    const at::Tensor& weight_scales,
    const float input_scale,
    const float output_scale) {
  auto num_output_channels = weight_scales.numel();
  float* weight_scales_data = weight_scales.data_ptr<float>();
  std::vector<float> requant_scales(num_output_channels, 1.f);
  for (int i = 0; i < num_output_channels; ++i) {
    requant_scales[i] = weight_scales_data[i] * input_scale / output_scale;
  }
  return requant_scales;
}

std::pair<at::Tensor, at::Tensor> make_zero_points_and_scales_tensor(
    const at::Tensor& weight_contig
    ) {
  auto num_output_channels = weight_contig.size(0);
  const auto qtype = weight_contig.qscheme();
  at::Tensor weight_zp =
    at::native::empty_cpu(
        {num_output_channels},
        at::device(at::kCPU).dtype(at::kQUInt8));
  // Adjust weight zero point, similar to weight data.
  uint8_t* weight_zp_data = (uint8_t*)weight_zp.data_ptr<c10::quint8>();
  if (qtype == at::kPerTensorAffine) {
    for (int i = 0; i < num_output_channels; ++i) {
      weight_zp_data[i] = (uint8_t)(weight_contig.q_zero_point() + 128);
    }
  } else if (qtype == at::kPerChannelAffine) {
    for (int i = 0; i < num_output_channels; ++i) {
      weight_zp_data[i] =
          (uint8_t)(
              weight_contig.q_per_channel_zero_points()[i].item<int32_t>() +
              128);
    }
  }
  at:: Tensor weight_scales =
    at::native::empty_cpu(
        {num_output_channels},
        at::device(at::kCPU).dtype(at::kFloat));
  float* weight_scales_data = weight_scales.data_ptr<float>();
  if (qtype == at::kPerTensorAffine) {
    for (int i = 0; i < num_output_channels; ++i) {
      weight_scales_data[i] = weight_contig.q_scale();
    }
  } else if (qtype == at::kPerChannelAffine) {
    for (int i = 0; i < num_output_channels; ++i) {
      weight_scales_data[i] =
        weight_contig.q_per_channel_scales()[i].item<float>();
    }
  }
  return {weight_zp, weight_scales};
}

} // namespace
