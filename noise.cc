#include <cmath>
#include <cstdio>
#include <numeric>

#include "af_stats.h"
#include "convolve.h"
#include "noise.h"
#include "opsin_params.h"
#include "optimize.h"
#include "rational_polynomial.h"
#include "simd_helpers.h"
#include "write_bits.h"
#include "xorshift128plus.h"

namespace pik {
namespace {
using namespace SIMD_NAMESPACE;

ImageF RandomImage(ImageF* PIK_RESTRICT temp, Xorshift128Plus* rng) {
  const size_t xsize = temp->xsize();
  const size_t ysize = temp->ysize();
  for (size_t y = 0; y < ysize; ++y) {
    float* PIK_RESTRICT row = temp->Row(y);
    const Full<float> df;
    const Full<uint32_t> du;
    for (size_t x = 0; x < xsize; x += df.N) {
      const auto bits = cast_to(du, (*rng)());
      // 1.0 + 23 random mantissa bits = [1, 2)
      const auto rand12 =
          cast_to(df, shift_right<9>(bits) | set1(du, 0x3F800000));
      const auto rand01 = rand12 - set1(df, 1.0f);
      store(rand01, df, row + x);
    }
  }

  ImageF out(xsize, ysize);
  ConvolveT<strategy::Laplacian3>::Run(*temp, kernel::Laplacian3(), &out);
  return out;
}

float GetScoreSumsOfAbsoluteDifferences(const Image3F& opsin, const int x,
                                        const int y, const int block_size) {
  const int small_bl_size_x = 3;
  const int small_bl_size_y = 4;
  const int kNumSAD =
      (block_size - small_bl_size_x) * (block_size - small_bl_size_y);
  // block_size x block_size reference pixels
  int counter = 0;
  const int offset = 2;

  std::vector<float> sad(kNumSAD, 0);
  for (int y_bl = 0; y_bl + small_bl_size_y < block_size; ++y_bl) {
    for (int x_bl = 0; x_bl + small_bl_size_x < block_size; ++x_bl) {
      float sad_sum = 0;
      // size of the center patch, we compare all the patches inside window with
      // the center one
      for (int cy = 0; cy < small_bl_size_y; ++cy) {
        for (int cx = 0; cx < small_bl_size_x; ++cx) {
          float wnd = 0.5f * (opsin.PlaneRow(1, y + y_bl + cy)[x + x_bl + cx] +
                             opsin.PlaneRow(0, y + y_bl + cy)[x + x_bl + cx]);
          float center =
              0.5f * (opsin.PlaneRow(1, y + offset + cy)[x + offset + cx] +
                     opsin.PlaneRow(0, y + offset + cy)[x + offset + cx]);
          sad_sum += std::abs(center - wnd);
        }
      }
      sad[counter++] = sad_sum;
    }
  }
  const int kSamples = (kNumSAD) / 2;
  // As with ROAD (rank order absolute distance), we keep the smallest half of
  // the values in SAD (we use here the more robust patch SAD instead of
  // absolute single-pixel differences).
  std::sort(sad.begin(), sad.end());
  const float total_sad_sum =
      std::accumulate(sad.begin(), sad.begin() + kSamples, 0.0f);
  return total_sad_sum / kSamples;
}

std::vector<float> GetSADScoresForPatches(const Image3F& opsin,
                                          const int block_s, const int num_bin,
                                          Histogram* sad_histogram) {
  std::vector<float> sad_scores(
      (opsin.ysize() / block_s) * (opsin.xsize() / block_s), 0.0f);

  int block_index = 0;

  for (int y = 0; y + block_s <= opsin.ysize(); y += block_s) {
    for (int x = 0; x + block_s <= opsin.xsize(); x += block_s) {
      // We assume that we work with Y opsin channel [-0.5, 0.5]
      float sad_sc = GetScoreSumsOfAbsoluteDifferences(opsin, x, y, block_s);
      sad_scores[block_index++] = sad_sc;
      sad_histogram->Increment(sad_sc * num_bin);
    }
  }
  return sad_scores;
}

float GetSADThreshold(const Histogram& histogram, const int num_bin) {
  // Here we assume that the most patches with similar SAD value is a "flat"
  // patches. However, some images might contain regular texture part and
  // generate second strong peak at the histogram
  // TODO(user) handle bimodal and heavy-tailed case
  const int mode = histogram.Mode();
  return static_cast<float>(mode) / Histogram::kBins;
}

// x is in [0+delta, 1+delta], delta ~= 0.06
template <class StrengthEval>
typename StrengthEval::V NoiseStrength(const StrengthEval& eval,
                                       const typename StrengthEval::V x) {
  const typename StrengthEval::D d;
  return Clamp0ToMax(d, eval(x), set1(d, 1.0f));
}

// General case: slow but precise.
class StrengthEvalPow {
 public:
  using D = Scalar<float>;
  using V = D::V;

  StrengthEvalPow(const NoiseParams& noise_params)
      : noise_params_(noise_params) {}

  V operator()(const V vx) const {
    float x;
    store(vx, D(), &x);
    return set1(D(), noise_params_.alpha * std::pow(x, noise_params_.gamma) +
                         noise_params_.beta);
  }

 private:
  const NoiseParams noise_params_;
};

// For noise_params.alpha == 0: cheaper to evaluate than a polynomial and
// avoids BLAS errors in RationalPolynomial.
template <class D_Arg>
class StrengthEvalLinear {
 public:
  using D = D_Arg;
  using V = typename D::V;

  StrengthEvalLinear(const NoiseParams& noise_params)
      : strength_(set1(D(), noise_params.beta)) {}

  V operator()(const V x) const { return strength_; }

 private:
  V strength_;
};

// Uses rational polynomial - faster than Pow.
template <class D_Arg>
class StrengthEvalPoly {
  // Max err < 1E-6.
  static constexpr size_t kDegreeP = 3;
  static constexpr size_t kDegreeQ = 2;
  using Polynomial =
      SIMD_NAMESPACE::RationalPolynomial<D_Arg, kDegreeP, kDegreeQ>;

 public:
  using D = D_Arg;
  using V = typename D::V;

  static Polynomial InitPoly() {
    const float p[kDegreeP + 1] = {
        2.8334176974065262E-05, -4.0383997904166469E-03, 1.3657279781005727E-01,
        1.0765042185381457E+00};
    const float q[kDegreeQ + 1] = {7.6921408240996481E-01,
                                   5.2686210349332230E-01,
                                   -8.7053691084335916E-02};
    return Polynomial(p, q);
  }

  StrengthEvalPoly(const NoiseParams& noise_params)
      : poly_(InitPoly()),
        mul_(set1(D(), noise_params.alpha)),
        add_(set1(D(), noise_params.beta)) {}

  PIK_INLINE V operator()(const V x) const {
    return mul_add(mul_, poly_(x), add_);
  }

 private:
  Polynomial poly_;
  const V mul_;
  const V add_;
};

template <class D>
void AddNoiseToRGB(const typename D::V rnd_noise_r,
                   const typename D::V rnd_noise_g,
                   const typename D::V rnd_noise_cor,
                   const typename D::V noise_strength_g,
                   const typename D::V noise_strength_r,
                   float* PIK_RESTRICT out_x, float* PIK_RESTRICT out_y,
                   float* PIK_RESTRICT out_b) {
  const D d;
  const auto kRGCorr = set1(d, 0.9f);
  const auto kRGNCorr = set1(d, 0.1f);

  const auto red_noise = kRGNCorr * rnd_noise_r * noise_strength_r +
                         kRGCorr * rnd_noise_cor * noise_strength_r;
  const auto green_noise = kRGNCorr * rnd_noise_g * noise_strength_g +
                           kRGCorr * rnd_noise_cor * noise_strength_g;

  auto vx = load(d, out_x);
  auto vy = load(d, out_y);
  auto vb = load(d, out_b);

  vx += red_noise - green_noise;
  vy += red_noise + green_noise;
  vb += set1(d, 0.9375f) * (red_noise + green_noise);

  vx = clamp(vx, set1(d, -kXybRange[0]), set1(d, kXybRange[0]));
  vy = clamp(vy, set1(d, -kXybRange[1]), set1(d, kXybRange[1]));
  vb = clamp(vb, set1(d, -kXybRange[2]), set1(d, kXybRange[2]));

  store(vx, d, out_x);
  store(vy, d, out_y);
  store(vb, d, out_b);
}

template <class StrengthEval>
void AddNoiseT(const StrengthEval& noise_model, Image3F* opsin) {
  using D = typename StrengthEval::D;
  const D d;
  const auto half = set1(d, 0.5f);

  const size_t xsize = opsin->xsize();
  const size_t ysize = opsin->ysize();

  Xorshift128Plus rng(65537, 123456789);
  ImageF temp(xsize, ysize);
  const ImageF& rnd_noise_red = RandomImage(&temp, &rng);
  const ImageF& rnd_noise_green = RandomImage(&temp, &rng);
  const ImageF& rnd_noise_correlated = RandomImage(&temp, &rng);

  // With the prior subtract-random Laplacian approximation, rnd_* ranges were
  // about [-1.5, 1.6]; Laplacian3 about doubles this to [-3.6, 3.6], so the
  // normalizer is half of what it was before (0.5).
  const auto norm_const = set1(d, 0.22f);

  for (size_t y = 0; y < ysize; ++y) {
    float* PIK_RESTRICT row_x = opsin->PlaneRow(0, y);
    float* PIK_RESTRICT row_y = opsin->PlaneRow(1, y);
    float* PIK_RESTRICT row_b = opsin->PlaneRow(2, y);
    const float* PIK_RESTRICT row_rnd_r = rnd_noise_red.Row(y);
    const float* PIK_RESTRICT row_rnd_g = rnd_noise_green.Row(y);
    const float* PIK_RESTRICT row_rnd_c = rnd_noise_correlated.Row(y);
    for (size_t x = 0; x < xsize; x += d.N) {
      const auto vx = load(d, row_x + x);
      const auto vy = load(d, row_y + x);
      const auto in_g = half * (vy - vx);
      const auto in_r = half * (vy + vx);
      const auto clamped_g =
          clamp(in_g, set1(d, -kXybRange[1]), set1(d, kXybRange[1]));
      const auto clamped_r =
          clamp(in_r, set1(d, -kXybRange[1]), set1(d, kXybRange[1]));
      const auto noise_strength_g =
          NoiseStrength(noise_model, clamped_g + set1(d, kXybCenter[1]));
      const auto noise_strength_r =
          NoiseStrength(noise_model, clamped_r + set1(d, kXybCenter[1]));
      const auto addit_rnd_noise_red = load(d, row_rnd_r + x) * norm_const;
      const auto addit_rnd_noise_green = load(d, row_rnd_g + x) * norm_const;
      const auto addit_rnd_noise_correlated =
          load(d, row_rnd_c + x) * norm_const;
      AddNoiseToRGB<D>(addit_rnd_noise_red, addit_rnd_noise_green,
                       addit_rnd_noise_correlated, noise_strength_g,
                       noise_strength_r, row_x + x, row_y + x, row_b + x);
    }
  }
}

// Returns max absolute error at uniformly spaced x.
template <class EvalApprox>
float MaxAbsError(const NoiseParams& noise_params,
                  const EvalApprox& eval_approx) {
  const StrengthEvalPow eval_pow(noise_params);

  float max_abs_err = 0.0f;
  const float x0 = -kXybRange[1] + kXybCenter[1];
  const float x1 = kXybRange[1] + kXybCenter[1];
  for (float x = x0; x < x1; x += 1E-1f) {
    const Scalar<float> d1;
    const Full<float> d;
    const auto expected_v = NoiseStrength(eval_pow, set1(d1, x));
    const auto actual_v = NoiseStrength(eval_approx, set1(d, x));
    float expected;
    SIMD_ALIGN float actual[d.N];
    store(expected_v, d1, &expected);
    store(actual_v, d, actual);
    const float abs_err = std::abs(expected - actual[0]);
    if (abs_err > max_abs_err) {
      // printf("  x=%f %E %E = %E\n", x, expected, actual[0], abs_err);
      max_abs_err = abs_err;
    }
  }
  // printf("max abs %.2E\n", max_abs_err);
  return max_abs_err;
}

}  // namespace

void AddNoise(const NoiseParams& noise_params, Image3F* opsin) {
  // SIMD descriptor.
  using D = Full<float>;

  if (noise_params.alpha == 0.0f) {
    // No noise at all
    if (noise_params.beta == 0.0f && noise_params.gamma == 0.0f) return;

    // Constant noise strength independent of pixel intensity
    AddNoiseT(StrengthEvalLinear<D>(noise_params), opsin);
    return;
  }

  const StrengthEvalPoly<D> poly(noise_params);
  if (MaxAbsError(noise_params, poly) < 1E-3f) {
    AddNoiseT(poly, opsin);
  } else {
    printf("Reverting to pow: %.3f %.3f ^%.3f\n", noise_params.alpha,
           noise_params.beta, noise_params.gamma);
    AddNoiseT(StrengthEvalPow(noise_params), opsin);
  }
}

// F(alpha, beta, gamma| x,y) = (1-n) * sum_i(y_i - (alpha x_i ^ gamma +
// beta))^2 + n * alpha * gamma.
struct LossFunction {
  explicit LossFunction(const std::vector<NoiseLevel>& nl0) : nl(nl0) {}

  double Compute(const std::vector<double>& w, std::vector<double>* df) const {
    double loss_function = 0;
    const double kEpsilon = 1e-2;
    const double kRegul = 0.00005;
    (*df)[0] = 0;
    (*df)[1] = 0;
    (*df)[2] = 0;
    for (int ind = 0; ind < nl.size(); ++ind) {
      double shifted_intensity = nl[ind].intensity + kXybCenter[1];
      if (shifted_intensity > kEpsilon) {
        double l_f =
            nl[ind].noise_level - (w[0] * pow(shifted_intensity, w[1]) + w[2]);
        (*df)[0] += (1 - kRegul) * 2.0 * l_f * pow(shifted_intensity, w[1]) +
                    kRegul * w[1];
        (*df)[1] += (1 - kRegul) * 2.0 * l_f * w[0] *
                        pow(shifted_intensity, w[1]) * log(shifted_intensity) +
                    kRegul * w[0];
        (*df)[2] += (1 - kRegul) * 2.0 * l_f;
        loss_function += (1 - kRegul) * l_f * l_f + kRegul * w[0] * w[1];
      }
    }
    return loss_function;
  }

  std::vector<NoiseLevel> nl;
};

void AddPointsForExtrapolation(std::vector<NoiseLevel>* noise_level) {
  NoiseLevel nl_min;
  NoiseLevel nl_max;
  nl_min.noise_level = 2;
  nl_max.noise_level = -2;
  for (auto nl : *noise_level) {
    if (nl.noise_level < nl_min.noise_level) {
      nl_min.intensity = nl.intensity;
      nl_min.noise_level = nl.noise_level;
    }
    if (nl.noise_level > nl_max.noise_level) {
      nl_max.intensity = nl.intensity;
      nl_max.noise_level = nl.noise_level;
    }
  }
  nl_max.intensity = -0.5;
  nl_min.intensity = 0.5;
  noise_level->push_back(nl_min);
  noise_level->push_back(nl_max);
}

void GetNoiseParameter(const Image3F& opsin, NoiseParams* noise_params,
                       float quality_coef) {
  // The size of a patch in decoder might be different from encoder's patch
  // size.
  // For encoder: the patch size should be big enough to estimate
  //              noise level, but, at the same time, it should be not too big
  //              to be able to estimate intensity value of the patch
  const int block_s = 8;
  const int kNumBin = 256;
  Histogram sad_histogram;
  std::vector<float> sad_scores =
      GetSADScoresForPatches(opsin, block_s, kNumBin, &sad_histogram);
  float sad_threshold = GetSADThreshold(sad_histogram, kNumBin);
  // If threshold is too large, the image has a strong pattern. This pattern
  // fools our model and it will add too much noise. Therefore, we do not add
  // noise for such images
  if (sad_threshold > 0.15f || sad_threshold <= 0.0f) {
    noise_params->alpha = 0;
    noise_params->beta = 0;
    noise_params->gamma = 0;
    return;
  }
  std::vector<NoiseLevel> nl =
      GetNoiseLevel(opsin, sad_scores, sad_threshold, block_s);

  AddPointsForExtrapolation(&nl);
  OptimizeNoiseParameters(nl, noise_params);
  noise_params->alpha *= quality_coef;
  noise_params->beta *= quality_coef;
}

const float kNoisePrecision = 1000.0f;

void EncodeFloatParam(float val, float precision, size_t* storage_ix,
                      uint8_t* storage) {
  WriteBits(1, val >= 0 ? 1 : 0, storage_ix, storage);
  const int absval_quant = static_cast<int>(std::abs(val) * precision + 0.5f);
  PIK_ASSERT(absval_quant < (1 << 16));
  WriteBits(16, absval_quant, storage_ix, storage);
}

void DecodeFloatParam(float precision, float* val, BitReader* br) {
  const int sign = 2 * br->ReadBits(1) - 1;
  const int absval_quant = br->ReadBits(16);
  *val = sign * absval_quant / precision;
}

std::string EncodeNoise(const NoiseParams& noise_params) {
  const size_t kMaxNoiseSize = 16;
  std::string output(kMaxNoiseSize, 0);
  size_t storage_ix = 0;
  uint8_t* storage = reinterpret_cast<uint8_t*>(&output[0]);
  storage[0] = 0;
  const bool have_noise =
      (noise_params.alpha != 0.0f || noise_params.gamma != 0.0f ||
       noise_params.beta != 0.0f);
  WriteBits(1, have_noise, &storage_ix, storage);
  if (have_noise) {
    EncodeFloatParam(noise_params.alpha, kNoisePrecision, &storage_ix, storage);
    EncodeFloatParam(noise_params.gamma, kNoisePrecision, &storage_ix, storage);
    EncodeFloatParam(noise_params.beta, kNoisePrecision, &storage_ix, storage);
  }
  size_t jump_bits = ((storage_ix + 7) & ~7) - storage_ix;
  WriteBits(jump_bits, 0, &storage_ix, storage);
  PIK_ASSERT(storage_ix % 8 == 0);
  size_t output_size = storage_ix >> 3;
  output.resize(output_size);
  return output;
}

bool DecodeNoise(BitReader* br, NoiseParams* noise_params) {
  const bool have_noise = br->ReadBits(1);
  if (have_noise) {
    DecodeFloatParam(kNoisePrecision, &noise_params->alpha, br);
    DecodeFloatParam(kNoisePrecision, &noise_params->gamma, br);
    DecodeFloatParam(kNoisePrecision, &noise_params->beta, br);
  } else {
    noise_params->alpha = noise_params->gamma = noise_params->beta = 0.0f;
  }
  br->JumpToByteBoundary();
  return true;
}

void OptimizeNoiseParameters(const std::vector<NoiseLevel>& noise_level,
                             NoiseParams* noise_params) {
  static const double kPrecision = 1e-8;
  static const int kMaxIter = 1000;

  LossFunction loss_function(noise_level);
  std::vector<double> parameter_vector(3);
  parameter_vector[0] = -0.05;
  parameter_vector[1] = 2.6;
  parameter_vector[2] = 0.025;

  parameter_vector = optimize::OptimizeWithScaledConjugateGradientMethod(
      loss_function, parameter_vector, kPrecision, kMaxIter);

  noise_params->alpha = parameter_vector[0];
  noise_params->gamma = parameter_vector[1];
  noise_params->beta = parameter_vector[2];
}

std::vector<float> GetTextureStrength(const Image3F& opsin, const int block_s) {
  std::vector<float> texture_strength_index((opsin.ysize() / block_s) *
                                            (opsin.xsize() / block_s));
  int block_index = 0;

  for (int y = 0; y + block_s <= opsin.ysize(); y += block_s) {
    for (int x = 0; x + block_s <= opsin.xsize(); x += block_s) {
      float texture_strength = 0;
      for (int y_bl = 0; y_bl < block_s; ++y_bl) {
        for (int x_bl = 0; x_bl + 1 < block_s; ++x_bl) {
          float diff = opsin.PlaneRow(1, y)[x + x_bl + 1] -
                       opsin.PlaneRow(1, y)[x + x_bl];
          texture_strength += diff * diff;
        }
      }
      for (int y_bl = 0; y_bl + 1 < block_s; ++y_bl) {
        for (int x_bl = 0; x_bl < block_s; ++x_bl) {
          float diff = opsin.PlaneRow(1, y + 1)[x + x_bl] -
                       opsin.PlaneRow(1, y)[x + x_bl];
          texture_strength += diff * diff;
        }
      }
      texture_strength_index[block_index] = texture_strength;
      ++block_index;
    }
  }
  return texture_strength_index;
}

float GetThresholdFlatIndices(const std::vector<float>& texture_strength,
                              const int n_patches) {
  std::vector<float> kth_statistic = texture_strength;
  std::stable_sort(kth_statistic.begin(), kth_statistic.end());
  return kth_statistic[n_patches];
}

std::vector<NoiseLevel> GetNoiseLevel(
    const Image3F& opsin, const std::vector<float>& texture_strength,
    const float threshold, const int block_s) {
  std::vector<NoiseLevel> noise_level_per_intensity;

  const int filt_size = 1;
  static const float kLaplFilter[filt_size * 2 + 1][filt_size * 2 + 1] = {
      {-0.25f, -1.0f, -0.25f},
      {-1.0f, 5.0f, -1.0f},
      {-0.25f, -1.0f, -0.25f},
  };

  // The noise model is build based on channel 0.5 * (X+Y) as we notices that it
  // is similar to the model 0.5 * (Y-X)
  int patch_index = 0;

  for (int y = 0; y + block_s <= opsin.ysize(); y += block_s) {
    for (int x = 0; x + block_s <= opsin.xsize(); x += block_s) {
      if (texture_strength[patch_index] <= threshold) {
        // Calculate mean value
        float mean_int = 0;
        for (int y_bl = 0; y_bl < block_s; ++y_bl) {
          for (int x_bl = 0; x_bl < block_s; ++x_bl) {
            mean_int += 0.5f * (opsin.PlaneRow(1, y + y_bl)[x + x_bl] +
                               opsin.PlaneRow(0, y + y_bl)[x + x_bl]);
          }
        }
        mean_int /= block_s * block_s;

        // Calculate Noise level
        float noise_level = 0;
        int count = 0;
        for (int y_bl = 0; y_bl < block_s; ++y_bl) {
          for (int x_bl = 0; x_bl < block_s; ++x_bl) {
            float filtered_value = 0;
            for (int y_f = -1 * filt_size; y_f <= filt_size; ++y_f) {
              if (((y_bl + y_f) < block_s) && ((y_bl + y_f) >= 0)) {
                for (int x_f = -1 * filt_size; x_f <= filt_size; ++x_f) {
                  if ((x_bl + x_f) >= 0 && (x_bl + x_f) < block_s) {
                    filtered_value +=
                        0.5f *
                        (opsin.PlaneRow(1, y + y_bl + y_f)[x + x_bl + x_f] +
                         opsin.PlaneRow(0, y + y_bl + y_f)[x + x_bl + x_f]) *
                        kLaplFilter[y_f + filt_size][x_f + filt_size];
                  } else {
                    filtered_value +=
                        0.5f *
                        (opsin.PlaneRow(1, y + y_bl + y_f)[x + x_bl - x_f] +
                         opsin.PlaneRow(0, y + y_bl + y_f)[x + x_bl - x_f]) *
                        kLaplFilter[y_f + filt_size][x_f + filt_size];
                  }
                }
              } else {
                for (int x_f = -1 * filt_size; x_f <= filt_size; ++x_f) {
                  if ((x_bl + x_f) >= 0 && (x_bl + x_f) < block_s) {
                    filtered_value +=
                        0.5f *
                        (opsin.PlaneRow(1, y + y_bl - y_f)[x + x_bl + x_f] +
                         opsin.PlaneRow(0, y + y_bl - y_f)[x + x_bl + x_f]) *
                        kLaplFilter[y_f + filt_size][x_f + filt_size];
                  } else {
                    filtered_value +=
                        0.5f *
                        (opsin.PlaneRow(1, y + y_bl - y_f)[x + x_bl - x_f] +
                         opsin.PlaneRow(0, y + y_bl - y_f)[x + x_bl - x_f]) *
                        kLaplFilter[y_f + filt_size][x_f + filt_size];
                  }
                }
              }
            }
            noise_level += std::abs(filtered_value);
            ++count;
          }
        }
        noise_level /= count;
        NoiseLevel nl;
        nl.intensity = mean_int;
        nl.noise_level = noise_level;
        noise_level_per_intensity.push_back(nl);
      }
      ++patch_index;
    }
  }
  return noise_level_per_intensity;
}

}  // namespace pik
