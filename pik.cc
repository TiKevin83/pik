// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "pik.h"

#include <limits.h>  // PATH_MAX
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#define PROFILER_ENABLED 1
#include "adaptive_quantization.h"
#include "af_edge_preserving_filter.h"
#include "arch_specific.h"
#include "bit_buffer.h"
#include "bits.h"
#include "brunsli_v2_decode.h"
#include "brunsli_v2_encode.h"
#include "butteraugli_comparator.h"
#include "byte_order.h"
#include "common.h"
#include "compiler_specific.h"
#include "compressed_image.h"
#include "convolve.h"
#include "dct_util.h"
#include "fast_log.h"
#include "guetzli/jpeg_data.h"
#include "guetzli/jpeg_data_decoder.h"
#include "guetzli/jpeg_data_encoder.h"
#include "guetzli/jpeg_data_reader.h"
#include "guetzli/jpeg_data_writer.h"
#include "guetzli/jpeg_error.h"
#include "guetzli/processor.h"
#include "header.h"
#include "image_io.h"
#include "noise.h"
#include "opsin_codec.h"
#include "opsin_image.h"
#include "opsin_inverse.h"
#include "pik_alpha.h"
#include "profiler.h"
#include "quantizer.h"
#include "simd/dispatch.h"

bool FLAGS_log_search_state = false;
// If true, prints the quantization maps at each iteration.
bool FLAGS_dump_quant_state = false;

const float kEpfMulScale = 10000.0f;
const int32_t FLAGS_epf_mul = 256;

namespace pik {
namespace {

template <class Image>
size_t TargetSize(const CompressParams& params, const Image& img) {
  if (params.target_size > 0) {
    return params.target_size;
  }
  if (params.target_bitrate > 0.0) {
    return 0.5 + params.target_bitrate * img.xsize() * img.ysize() / 8;
  }
  return 0;
}

inline int Clamp(int minval, int maxval, int val) {
  return std::min(maxval, std::max(minval, val));
}

ImageF TileDistMap(const butteraugli::ImageF& distmap, int tile_size,
                   int margin) {
  PROFILER_FUNC;
  const int tile_xsize = (distmap.xsize() + tile_size - 1) / tile_size;
  const int tile_ysize = (distmap.ysize() + tile_size - 1) / tile_size;
  ImageF tile_distmap(tile_xsize, tile_ysize);
  for (int tile_y = 0; tile_y < tile_ysize; ++tile_y) {
    for (int tile_x = 0; tile_x < tile_xsize; ++tile_x) {
      int y_min = std::max<int>(0, tile_size * tile_y - margin);
      int x_min = std::max<int>(0, tile_size * tile_x - margin);
      int x_max = std::min<int>(distmap.xsize(), tile_size * (tile_x + 1) + margin);
      int y_max = std::min<int>(distmap.ysize(), tile_size * (tile_y + 1) + margin);
      float max_dist = 0.0;
      for (int y = y_min; y < y_max; ++y) {
        const float* const PIK_RESTRICT row = distmap.Row(y);
        for (int x = x_min; x < x_max; ++x) {
          float v = row[x];
          max_dist = std::max(max_dist, v);
        }
      }
      tile_distmap.Row(tile_y)[tile_x] = max_dist;
    }
  }
  return tile_distmap;
}

ImageF DistToPeakMap(const ImageF& field, float peak_min,
                     int local_radius, float peak_weight) {
  ImageF result(field.xsize(), field.ysize(), -1.0f);
  for (int y0 = 0; y0 < field.ysize(); ++y0) {
    for (int x0 = 0; x0 < field.xsize(); ++x0) {
      int x_min = std::max(0, x0 - local_radius);
      int y_min = std::max(0, y0 - local_radius);
      int x_max = std::min<int>(field.xsize(), x0 + 1 + local_radius);
      int y_max = std::min<int>(field.ysize(), y0 + 1 + local_radius);
      float local_max = peak_min;
      for (int y = y_min; y < y_max; ++y) {
        for (int x = x_min; x < x_max; ++x) {
          local_max = std::max(local_max, field.Row(y)[x]);
        }
      }
      if (field.Row(y0)[x0] >
          (1.0f - peak_weight) * peak_min + peak_weight * local_max) {
        for (int y = y_min; y < y_max; ++y) {
          for (int x = x_min; x < x_max; ++x) {
            float dist = std::max(std::abs(y - y0), std::abs(x - x0));
            float cur_dist = result.Row(y)[x];
            if (cur_dist < 0.0 || cur_dist > dist) {
              result.Row(y)[x] = dist;
            }
          }
        }
      }
    }
  }
  return result;
}

bool AdjustQuantVal(float* const PIK_RESTRICT q,
                    const float d, const float factor,
                    const float quant_max) {
  if (*q >= 0.999f * quant_max) return false;
  const float inv_q = 1.0f / *q;
  const float adj_inv_q = inv_q - factor / (d + 1.0f);
  *q = 1.0f / std::max(1.0f / quant_max, adj_inv_q);
  return true;
}

void DumpHeatmap(const PikInfo* info, const std::string& label,
                 const std::vector<float>& vals, size_t xsize, size_t ysize,
                 float good_threshold, float bad_threshold) {
  std::vector<uint8_t> heatmap(3 * xsize * ysize);
  butteraugli::CreateHeatMapImage(vals, good_threshold, bad_threshold,
                                  xsize, ysize, &heatmap);
  char filename[200];
  snprintf(filename, sizeof(filename), "%s%05d", label.c_str(),
           info->num_butteraugli_iters);
  info->DumpImage(filename,
                  Image3FromInterleaved(&heatmap[0], xsize, ysize, 3 * xsize));
}

void DumpHeatmaps(const PikInfo* info,
                  size_t xsize, size_t ysize, int qres,
                  float ba_target,
                  const ImageF& quant_field,
                  const ImageF& tile_heatmap) {
  if (!WantDebugOutput(info)) return;
  std::vector<float> qmap(xsize * ysize);
  std::vector<float> dmap(xsize * ysize);
  for (int y = 0; y < quant_field.ysize(); ++y) {
    auto row_q = quant_field.Row(y);
    auto row_d = tile_heatmap.Row(y);
    for (int x = 0; x < quant_field.xsize(); ++x) {
      for (int dy = 0; dy < qres; ++dy) {
        for (int dx = 0; dx < qres; ++dx) {
          int px = qres * x + dx;
          int py = qres * y + dy;
          if (px < xsize && py < ysize) {
            qmap[py * xsize + px] = 1.0f / row_q[x];  // never zero
            dmap[py * xsize + px] = row_d[x];
          }
        }
      }
    }
  }
  DumpHeatmap(info, "quant_heatmap", qmap, xsize, ysize,
              4.0f * ba_target, 6.0f * ba_target);
  DumpHeatmap(info, "tile_heatmap", dmap, xsize, ysize,
              ba_target, 1.5f * ba_target);
}

void DoDenoise(const Quantizer& quantizer, Image3F* PIK_RESTRICT opsin) {
  const float scale = quantizer.Scale() * kEpfMulScale;
  epf::AdaptiveFilterParams epf_params;
  epf_params.dc_quant = quantizer.RawDC();  // unused
  // TODO(janwas): also store min/max opsin value, pass to epf
  epf_params.ac_quant = &quantizer.RawQuantField();
  epf_params.sigma_add = 0;
  epf_params.sigma_mul = scale / (FLAGS_epf_mul << epf::kSigmaShift);
  dispatch::Run(dispatch::SupportedTargets(), epf::EdgePreservingFilter(),
                opsin, epf_params);
}

void FindBestQuantization(const Image3F& opsin_orig,
                          const Image3F& opsin_arg,
                          const CompressParams& cparams,
                          const Header& header,
                          float butteraugli_target,
                          const ColorTransform& ctan,
                          Quantizer* quantizer,
                          PikInfo* aux_out) {
  ButteraugliComparator comparator(opsin_orig, cparams.hf_asymmetry);
  const float butteraugli_target_dc =
      std::min<float>(butteraugli_target,
                      pow(butteraugli_target, 0.74500252220422669));
  const float kInitialQuantDC = (0.93831260858660503 ) / butteraugli_target_dc;
  const float kQuantAC = (1.176060090135594 ) / butteraugli_target;
  ImageF quant_field =
      ScaleImage(kQuantAC,
                 AdaptiveQuantizationMap(opsin_orig.plane(1), 8));
  ImageF best_quant_field = CopyImage(quant_field);
  float best_butteraugli = 1000.0f;
  ImageF tile_distmap;
  CompressParams search_params = cparams;
  for (int i = 0; i < cparams.max_butteraugli_iters; ++i) {
    if (FLAGS_dump_quant_state) {
      printf("\nQuantization field:\n");
      for (int y = 0; y < quant_field.ysize(); ++y) {
        for (int x = 0; x < quant_field.xsize(); ++x) {
          printf(" %.5f", quant_field.Row(y)[x]);
        }
        printf("\n");
      }
    }
    float qmin, qmax;
    ImageMinMax(quant_field, &qmin, &qmax);
    if (quantizer->SetQuantField(kInitialQuantDC, quant_field, cparams)) {
      ThreadPool null_pool(0);
      // TODO(janwas): split into DC (hoisted outside) and AC
      QuantizedCoeffs qcoeffs = ComputeCoefficients(
          search_params, header, opsin_arg, *quantizer, ctan, &null_pool);
      // TODO(janwas): Avoid by caching?
      Image3F recon =
          ReconOpsinImage(header, qcoeffs, *quantizer, ctan, &null_pool);

      // (no need for any additional override: in the encoder, kDenoise is only
      // set if the override allowed it)
      if (header.flags & Header::kDenoise) {
        DoDenoise(*quantizer, &recon);
      }

      Image3B srgb;
      const bool dither = (header.flags & Header::kDither) != 0;
      CenteredOpsinToSrgb(recon, dither, &null_pool, &srgb);
      comparator.Compare(srgb);
      bool best_quant_updated = false;
      if (comparator.distance() <= best_butteraugli) {
        best_quant_field = CopyImage(quant_field);
        best_butteraugli = std::max(comparator.distance(), butteraugli_target);
        best_quant_updated = true;
      }
      static const int kMargins[100] = { 0, 0, 1, 2, 1, 0, 0 };
      tile_distmap = TileDistMap(comparator.distmap(), 8, kMargins[i]);
      if (WantDebugOutput(aux_out)) {
        DumpHeatmaps(aux_out, opsin_orig.xsize(), opsin_orig.ysize(),
                     8, butteraugli_target, quant_field, tile_distmap);
          char pathname[200];
          snprintf(pathname, 200, "%s%s%05d.png", aux_out->debug_prefix.c_str(),
                   "rgb_out", aux_out->num_butteraugli_iters);
          WriteImage(ImageFormatPNG(), srgb, pathname);
          ++aux_out->num_butteraugli_iters;
      }
      if (FLAGS_log_search_state) {
        float minval, maxval;
        ImageMinMax(quant_field, &minval, &maxval);
        printf("\nButteraugli iter: %d/%d%s\n", i,
               cparams.max_butteraugli_iters,
               best_quant_updated ? " (*)" : "");
        printf("Butteraugli distance: %f\n", comparator.distance());
        printf("quant range: %f ... %f  DC quant: %f\n", minval, maxval,
               kInitialQuantDC);
        if (FLAGS_dump_quant_state) {
          quantizer->DumpQuantizationMap();
        }
      }
    }
    static const double kPow[7] = {
        0.99905005931122937,
        1.0027778288237166,
        0.74286297793691547,
        0.85172198919496955,
        0.0,
        0.0,
        0.0,
    };
    const double cur_pow = kPow[i];
    // pow(x, 0) == x, so skip pow.
    if (cur_pow == 0.0) {
      for (int y = 0; y < quant_field.ysize(); ++y) {
        const float* const PIK_RESTRICT row_dist = tile_distmap.Row(y);
        float* const PIK_RESTRICT row_q = quant_field.Row(y);
        for (int x = 0; x < quant_field.xsize(); ++x) {
          const float diff = row_dist[x] / butteraugli_target;
          if (diff >= 1.0f) {
            row_q[x] *= diff;
          }
        }
      }
    } else {
      for (int y = 0; y < quant_field.ysize(); ++y) {
        const float* const PIK_RESTRICT row_dist = tile_distmap.Row(y);
        float* const PIK_RESTRICT row_q = quant_field.Row(y);
        for (int x = 0; x < quant_field.xsize(); ++x) {
          const float diff = row_dist[x] / butteraugli_target;
          if (diff < 1.0f) {
            row_q[x] *= pow(diff, cur_pow);
          } else {
            row_q[x] *= diff;
          }
        }
      }
    }
  }
  quantizer->SetQuantField(kInitialQuantDC, best_quant_field, cparams);
}

void FindBestQuantizationHQ(const Image3F& opsin_orig,
                            const Image3F& opsin,
                            const CompressParams& cparams,
                            const Header& header,
                            float butteraugli_target,
                            const ColorTransform& ctan,
                            Quantizer* quantizer,
                            PikInfo* aux_out) {
  const bool slow = cparams.really_slow_mode;
  ButteraugliComparator comparator(opsin_orig, cparams.hf_asymmetry);
  ImageF quant_field =
      ScaleImage(slow ? 1.2f : 1.5f,
                 AdaptiveQuantizationMap(opsin_orig.plane(1), 8));
  ImageF best_quant_field = CopyImage(quant_field);
  float best_butteraugli = 1000.0f;
  ImageF tile_distmap;
  static const int kMaxOuterIters = 2;
  int outer_iter = 0;
  int butteraugli_iter = 0;
  int search_radius = 0;
  float quant_ceil = 5.0f;
  float quant_dc = slow ? 1.2f : 1.6f;
  int num_stalling_iters = 0;
  int max_iters = slow ? cparams.max_butteraugli_iters_really_slow_mode :
                  cparams.max_butteraugli_iters;
  CompressParams search_params = cparams;
  for (;;) {
    if (FLAGS_dump_quant_state) {
      printf("\nQuantization field:\n");
      for (int y = 0; y < quant_field.ysize(); ++y) {
        for (int x = 0; x < quant_field.xsize(); ++x) {
          printf(" %.5f", quant_field.Row(y)[x]);
        }
        printf("\n");
      }
    }
    float qmin, qmax;
    ImageMinMax(quant_field, &qmin, &qmax);
    if (quantizer->SetQuantField(quant_dc, quant_field, cparams)) {
      ThreadPool null_pool(0);
      // TODO(janwas): split into DC (hoisted outside) and AC
      QuantizedCoeffs qcoeffs = ComputeCoefficients(
          search_params, header, opsin, *quantizer, ctan, &null_pool);
      // TODO(janwas): Avoid by caching?
      Image3F recon =
          ReconOpsinImage(header, qcoeffs, *quantizer, ctan, &null_pool);

      Image3B srgb;
      const bool dither = (header.flags & Header::kDither) != 0;
      CenteredOpsinToSrgb(recon, dither, &null_pool, &srgb);
      comparator.Compare(srgb);
      ++butteraugli_iter;
      bool best_quant_updated = false;
      if (comparator.distance() <= best_butteraugli) {
        best_quant_field = CopyImage(quant_field);
        best_butteraugli = std::max(comparator.distance(), butteraugli_target);
        best_quant_updated = true;
        num_stalling_iters = 0;
      } else if (outer_iter == 0) {
        ++num_stalling_iters;
      }
      tile_distmap = TileDistMap(comparator.distmap(), 8, 0);
      if (WantDebugOutput(aux_out)) {
        DumpHeatmaps(aux_out, opsin_orig.xsize(), opsin_orig.ysize(),
                     8, butteraugli_target, quant_field, tile_distmap);
          char pathname[200];
          snprintf(pathname, 200, "%s%s%05d.png", aux_out->debug_prefix.c_str(),
                   "rgb_out", aux_out->num_butteraugli_iters);
          WriteImage(ImageFormatPNG(), srgb, pathname);
      }
      if (aux_out) {
        ++aux_out->num_butteraugli_iters;
      }
      if (FLAGS_log_search_state) {
        float minval, maxval;
        ImageMinMax(quant_field, &minval, &maxval);
        printf("\nButteraugli iter: %d/%d%s\n", butteraugli_iter, max_iters,
               best_quant_updated ? " (*)" : "");
        printf("Butteraugli distance: %f\n", comparator.distance());
        printf("quant range: %f ... %f  DC quant: %f\n", minval, maxval,
               quant_dc);
        printf("search radius: %d\n", search_radius);
        if (FLAGS_dump_quant_state) {
          quantizer->DumpQuantizationMap();
        }
      }
      if (butteraugli_iter >= max_iters) {
        break;
      }
    }
    bool changed = false;
    while (!changed && comparator.distance() > butteraugli_target) {
      for (int radius = 0; radius <= search_radius && !changed; ++radius) {
        ImageF dist_to_peak_map = DistToPeakMap(
            tile_distmap, butteraugli_target, radius, 0.0);
        for (int y = 0; y < quant_field.ysize(); ++y) {
          float* const PIK_RESTRICT row_q = quant_field.Row(y);
          const float* const PIK_RESTRICT row_dist = dist_to_peak_map.Row(y);
          for (int x = 0; x < quant_field.xsize(); ++x) {
            if (row_dist[x] >= 0.0f) {
              static const float kAdjSpeed[kMaxOuterIters] = { 0.1, 0.04 };
              const float factor =
                  (slow ? kAdjSpeed[outer_iter] : 0.2f) *
                  tile_distmap.Row(y)[x];
              if (AdjustQuantVal(&row_q[x], row_dist[x], factor, quant_ceil)) {
                changed = true;
              }
            }
          }
        }
      }
      if (!changed || num_stalling_iters >= (slow ? 3 : 1)) {
        // Try to extend the search parameters.
        if ((search_radius < 4) &&
            (qmax < 0.99f * quant_ceil ||
             quant_ceil >= 3.0f + search_radius)) {
          ++search_radius;
          continue;
        }
        if (quant_dc < 0.4f * quant_ceil - 0.8f) {
          quant_dc += 0.2f;
          changed = true;
          continue;
        }
        if (quant_ceil < 8.0f) {
          quant_ceil += 0.5f;
          continue;
        }
        break;
      }
    }
    if (!changed) {
      if (!slow || ++outer_iter == kMaxOuterIters) break;
      static const float kQuantScale = 0.75f;
      for (int y = 0; y < quant_field.ysize(); ++y) {
        for (int x = 0; x < quant_field.xsize(); ++x) {
          quant_field.Row(y)[x] *= kQuantScale;
        }
      }
      num_stalling_iters = 0;
    }
  }
  quantizer->SetQuantField(quant_dc, best_quant_field, cparams);
}

template <typename T>
inline size_t IndexOfMaximum(const T* array, const size_t len) {
  PIK_ASSERT(len > 0);
  T maxval = array[0];
  size_t maxidx = 0;
  for (size_t i = 1; i < len; ++i) {
    if (array[i] > maxval) {
      maxval = array[i];
      maxidx = i;
    }
  }
  return maxidx;
}

void FindBestYToBCorrelation(const Image3F& opsin,
                             ImageI* PIK_RESTRICT ytob_map,
                             int* PIK_RESTRICT ytob_dc) {
  const float kYToBScale = 128.0f;
  const float kZeroThresh = kYToBScale * 0.7f;
  const float* const PIK_RESTRICT kDequantMatrix = &DequantMatrix(0)[128];
  float qm[64];
  for (int k = 0; k < 64; ++k) {
    qm[k] = 1.0f / kDequantMatrix[k];
  }
  Image3F dct = TransposedScaledDCT(opsin);
  uint32_t num_zeros[256] = {0};
  for (size_t y = 0; y < dct.ysize(); ++y) {
    const float* const PIK_RESTRICT row_y = dct.ConstPlaneRow(1, y);
    const float* const PIK_RESTRICT row_b = dct.ConstPlaneRow(2, y);
    for (size_t x = 0; x < dct.xsize(); ++x) {
      if (x % 64 == 0) continue;
      const float scaled_b = kYToBScale * row_b[x] * qm[x % 64];
      const float scaled_y = row_y[x] * qm[x % 64];
      for (int ytob = 0; ytob < 256; ++ytob) {
        if (std::abs(scaled_b - ytob * scaled_y) < kZeroThresh) {
          ++num_zeros[ytob];
        }
      }
    }
  }
  *ytob_dc = IndexOfMaximum(num_zeros, 256);
  for (int tile_y = 0; tile_y < ytob_map->ysize(); ++tile_y) {
    int* PIK_RESTRICT row_ytob = ytob_map->Row(tile_y);
    for (int tile_x = 0; tile_x < ytob_map->xsize(); ++tile_x) {
      const int y0 = tile_y * kTileInBlocks;
      const int x0 = tile_x * kTileInBlocks * 64;
      const int y1 = std::min<int>(y0 + kTileInBlocks, dct.ysize());
      const int x1 = std::min<int>(x0 + kTileInBlocks * 64, dct.xsize());
      uint32_t num_zeros[256] = {0};
      for (size_t y = y0; y < y1; ++y) {
        const float* const PIK_RESTRICT row_y = dct.ConstPlaneRow(1, y);
        const float* const PIK_RESTRICT row_b = dct.ConstPlaneRow(2, y);
        for (size_t x = x0; x < x1; ++x) {
          if (x % 64 == 0) continue;
          const float scaled_b = kYToBScale * row_b[x] * qm[x % 64];
          const float scaled_y = row_y[x] * qm[x % 64];
          for (int ytob = 0; ytob < 256; ++ytob) {
            if (std::abs(scaled_b - ytob * scaled_y) < kZeroThresh) {
              ++num_zeros[ytob];
            }
          }
        }
      }
      int best_ytob = IndexOfMaximum(num_zeros, 256);
      // Revert to the global factor used for dc if the number of zeros is
      // not much different.
      if (num_zeros[best_ytob] - num_zeros[*ytob_dc] <= 10) {
        best_ytob = *ytob_dc;
      }
      row_ytob[tile_x] = best_ytob;
    }
  }
}

void FindBestYToXCorrelation(const Image3F& opsin,
                             ImageI* PIK_RESTRICT ytox_map,
                             int* PIK_RESTRICT ytox_dc) {
  const float kYToXScale = 256.0f;
  const float kZeroThresh = kYToXScale * 0.65f;
  const float* const PIK_RESTRICT kDequantMatrix = DequantMatrix(0);
  float qm[64];
  for (int k = 0; k < 64; ++k) {
    qm[k] = 1.0f / kDequantMatrix[k];
  }
  Image3F dct = TransposedScaledDCT(opsin);
  uint32_t num_zeros[256] = {0};
  for (size_t y = 0; y < dct.ysize(); ++y) {
    const float* const PIK_RESTRICT row_y = dct.ConstPlaneRow(1, y);
    const float* const PIK_RESTRICT row_x = dct.ConstPlaneRow(0, y);
    for (size_t x = 0; x < dct.xsize(); ++x) {
      if (x % 64 == 0) continue;
      const float scaled_x = kYToXScale * row_x[x] * qm[x % 64];
      const float scaled_y = row_y[x] * qm[x % 64];
      for (int ytox = 0; ytox < 256; ++ytox) {
        if (std::abs(scaled_x - (ytox - 128) * scaled_y) < kZeroThresh) {
          ++num_zeros[ytox];
        }
      }
    }
  }
  *ytox_dc = IndexOfMaximum(num_zeros, 256);
  for (int tile_y = 0; tile_y < ytox_map->ysize(); ++tile_y) {
    int* PIK_RESTRICT row_ytox = ytox_map->Row(tile_y);
    for (int tile_x = 0; tile_x < ytox_map->xsize(); ++tile_x) {
      const int y0 = tile_y * kTileInBlocks;
      const int x0 = tile_x * kTileInBlocks * 64;
      const int y1 = std::min<int>(y0 + kTileInBlocks, dct.ysize());
      const int x1 = std::min<int>(x0 + kTileInBlocks * 64, dct.xsize());
      size_t num_zeros[256] = { 0 };
      for (size_t y = y0; y < y1; ++y) {
        const float* const PIK_RESTRICT row_y = dct.ConstPlaneRow(1, y);
        const float* const PIK_RESTRICT row_x = dct.ConstPlaneRow(0, y);
        for (size_t x = x0; x < x1; ++x) {
          if (x % 64 == 0) continue;
          const float scaled_x = kYToXScale * row_x[x] * qm[x % 64];
          const float scaled_y = row_y[x] * qm[x % 64];
          for (int ytox = 0; ytox < 256; ++ytox) {
            if (std::abs(scaled_x - (ytox - 128) * scaled_y) < kZeroThresh) {
              ++num_zeros[ytox];
            }
          }
        }
      }
      int best_ytox = IndexOfMaximum(num_zeros, 256);
      // Revert to the global factor used for dc if the number of zeros is
      // the same.
      if (num_zeros[best_ytox] == num_zeros[*ytox_dc]) {
        best_ytox = *ytox_dc;
      }
      row_ytox[tile_x] = best_ytox;
    }
  }
}

bool ScaleQuantizationMap(const float quant_dc,
                          const ImageF& quant_field_ac,
                          const CompressParams& cparams,
                          float scale,
                          Quantizer* quantizer) {
  float scale_dc = 0.8 * scale + 0.2;
  bool changed = quantizer->SetQuantField(
      scale_dc * quant_dc, ScaleImage(scale, quant_field_ac), cparams);
  if (FLAGS_dump_quant_state) {
    printf("\nScaling quantization map with scale %f\n", scale);
    quantizer->DumpQuantizationMap();
  }
  return changed;
}

void ScaleToTargetSize(const Image3F& opsin,
                       const CompressParams& cparams,
                       const NoiseParams& noise_params,
                       const Header& header,
                       size_t target_size,
                       const ColorTransform& ctan,
                       Quantizer* quantizer,
                       PikInfo* aux_out) {
  ThreadPool null_pool(0);
  float quant_dc;
  ImageF quant_ac;
  quantizer->GetQuantField(&quant_dc, &quant_ac);
  float scale_bad = 1.0;
  float scale_good = 1.0;
  bool found_candidate = false;
  std::string candidate;
  for (int i = 0; i < 10; ++i) {
    ScaleQuantizationMap(quant_dc, quant_ac, cparams, scale_good, quantizer);
    QuantizedCoeffs qcoeffs =
        ComputeCoefficients(cparams, header, opsin, *quantizer, ctan,
                            &null_pool);
    candidate = EncodeToBitstream(qcoeffs, *quantizer, noise_params, ctan,
                                  false, nullptr);
    if (candidate.size() <= target_size) {
      found_candidate = true;
      break;
    }
    scale_bad = scale_good;
    scale_good *= 0.5;
  }
  if (!found_candidate) {
    // We could not make the compressed size small enough
    return;
  }
  if (scale_good == 1.0) {
    // We dont want to go below butteraugli distance 1.0
    return;
  }
  for (int i = 0; i < 16; ++i) {
    float scale = 0.5 * (scale_bad + scale_good);
    if (!ScaleQuantizationMap(quant_dc, quant_ac, cparams, scale, quantizer)) {
      break;
    }
    QuantizedCoeffs qcoeffs =
        ComputeCoefficients(cparams, header, opsin, *quantizer, ctan,
                            &null_pool);
    candidate = EncodeToBitstream(qcoeffs, *quantizer, noise_params, ctan,
                                  false, nullptr);
    if (candidate.size() <= target_size) {
      scale_good = scale;
    } else {
      scale_bad = scale;
    }
  }
  ScaleQuantizationMap(quant_dc, quant_ac, cparams, scale_good, quantizer);
}

void CompressToTargetSize(const Image3F& opsin_orig,
                          const Image3F& opsin,
                          const CompressParams& cparams,
                          const NoiseParams& noise_params,
                          const Header& header,
                          size_t target_size,
                          const ColorTransform& ctan,
                          Quantizer* quantizer,
                          PikInfo* aux_out) {
  ThreadPool null_pool(0);
  float quant_dc_good = 1.0;
  ImageF quant_ac_good;
  const float kIntervalLenThresh = 0.05f;
  float dist_bad = -1.0f;
  float dist_good = -1.0f;
  for (;;) {
    float dist = 1.0f;
    if (dist_good >= 0.0f && dist_bad >= 0.0f) {
      if (dist_good - dist_bad < kIntervalLenThresh) {
        break;
      }
      dist = 0.5f * (dist_good + dist_bad);
    } else if (dist_good >= 0.0f) {
      dist = dist_good * 0.8f;
      if (dist < 0.3) {
        break;
      }
    } else if (dist_bad >= 0.0f) {
      dist = dist_bad * 1.25f;
      if (dist > 32.0f) {
        break;
      }
    }
    FindBestQuantization(opsin_orig, opsin, cparams, header, dist,
                         ctan, quantizer, aux_out);
    QuantizedCoeffs qcoeffs =
        ComputeCoefficients(cparams, header, opsin, *quantizer, ctan,
                            &null_pool);
    std::string candidate = EncodeToBitstream(
        qcoeffs, *quantizer, noise_params, ctan, false, nullptr);
    if (candidate.size() <= target_size) {
      dist_good = dist;
      quantizer->GetQuantField(&quant_dc_good, &quant_ac_good);
    } else {
      dist_bad = dist;
    }
  }
  quantizer->SetQuantField(quant_dc_good, quant_ac_good, cparams);
}

bool JpegToPikLossless(const guetzli::JPEGData& jpg, PaddedBytes* compressed,
                       PikInfo* aux_out) {
  Header header;
  header.bitstream = Header::kBitstreamBrunsli;
  compressed->resize(MaxCompressedHeaderSize() +
                     BrunsliV2MaximumEncodedSize(jpg));
  uint8_t* end = StoreHeader(header, compressed->data());
  if (end == nullptr) return false;
  const size_t header_size = end - compressed->data();
  if (!BrunsliV2EncodeJpegData(jpg, header_size, compressed)) {
    return PIK_FAILURE("Invalid jpeg input.");
  }
  return true;
}

bool BrunsliToPixels(const PaddedBytes& compressed, size_t pos, Image3B* srgb) {
  guetzli::JPEGData jpg;
  if (!BrunsliV2DecodeJpegData(compressed.data() + pos,
      compressed.size() - pos, &jpg)) {
    return PIK_FAILURE("Brunsli v2 decoding error");
  }
  std::vector<uint8_t> rgb = DecodeJpegToRGB(jpg);
  if (rgb.empty()) {
    return PIK_FAILURE("JPEG decoding error.");
  }
  *srgb = Image3FromInterleaved(&rgb[0], jpg.width, jpg.height, 3 * jpg.width);
  return true;
}

bool BrunsliToPixels(const PaddedBytes& compressed, size_t pos, Image3U* srgb) {
  return PIK_FAILURE("Brunsli not supported for Image3U");
}

bool BrunsliToPixels(const PaddedBytes& compressed, size_t pos, Image3F* srgb) {
  return PIK_FAILURE("Brunsli not supported for Image3F");
}

}  // namespace

bool PixelsToBrunsli(const CompressParams& params, const Image3B& srgb,
                     PaddedBytes* compressed, PikInfo* aux_out) {
  const std::vector<uint8_t>& rgb = InterleavedFromImage3(srgb);
  guetzli::JPEGData jpeg;
  if (params.butteraugli_distance >= 0.0) {
    guetzli::Params guetzli_params;
    guetzli_params.butteraugli_target = params.butteraugli_distance;
    if (!guetzli::Process(guetzli_params, rgb, srgb.xsize(), srgb.ysize(),
                          &jpeg)) {
      return PIK_FAILURE("Guetzli processing failed.");
    }
  } else {
    // TODO(janwas): use params.jpeg_quality to derive a quant matrix (arg 3).
    if (!EncodeRGBToJpeg(rgb, srgb.xsize(), srgb.ysize(), &jpeg)) {
      return PIK_FAILURE("Guetzli processing failed.");
    }
  }
  return JpegToPikLossless(jpeg, compressed, aux_out);
}

bool PixelsToBrunsli(const CompressParams& params, const Image3F& srgb,
                     PaddedBytes* compressed, PikInfo* aux_out) {
  return PIK_FAILURE("Brunsli not supported for Image3F");
}

template<typename T>
bool PixelsToBrunsli(const CompressParams& params, const MetaImage<T>& image,
                     PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToBrunsli(params, image.GetColor(), compressed, aux_out);
}

template<typename T>
MetaImageF OpsinDynamicsMetaImage(const Image3<T>& image) {
  Image3F opsin = OpsinDynamicsImage(image);
  MetaImageF out;
  out.SetColor(std::move(opsin));
  return out;
}

template<typename T>
MetaImageF OpsinDynamicsMetaImage(const MetaImage<T>& image) {
  MetaImageF out = OpsinDynamicsMetaImage(image.GetColor());
  out.CopyAlpha(image);
  return out;
}

template <typename Image>
bool PixelsToPikT(const CompressParams& params_in, const Image& image,
                  ThreadPool* pool, PaddedBytes* compressed, PikInfo* aux_out) {
  if (image.xsize() == 0 || image.ysize() == 0) {
    return PIK_FAILURE("Empty image");
  }
  if (params_in.use_brunsli_v2) {
    return PixelsToBrunsli(params_in, image, compressed, aux_out);
  }
  MetaImageF opsin = OpsinDynamicsMetaImage(image);

  Header header;
  header.xsize = image.xsize();
  header.ysize = image.ysize();
  if (opsin.HasAlpha()) {
    header.flags |= Header::kAlpha;
  }
  // default decision (later: depending on quality)
  bool enable_denoise = false;
  if (params_in.denoise != Override::kDefault) {
    enable_denoise = params_in.denoise == Override::kOn;
  }
  if (enable_denoise) {
    header.flags |= Header::kDenoise;
  }

  if (params_in.butteraugli_distance < kMaxButteraugliForHQ) {
    header.quant_template = kQuantHQ;
  } else {
    header.quant_template = kQuantDefault;
    header.flags |= Header::kSmoothDCPred;
    header.flags |= Header::kGaborishTransform;
  }
  // Dithering is important at higher distances but leads to visible
  // checkboarding at very high qualities.
  if (params_in.butteraugli_distance > kMinButteraugliForDither) {
    header.flags |= Header::kDither;
  }
  compressed->resize(MaxCompressedHeaderSize());
  uint8_t* header_end = StoreHeader(header, compressed->data());
  if (header_end == nullptr) return false;
  const size_t header_size = header_end - compressed->data();
  compressed->resize(header_size);  // no copy!
  if (aux_out) {
    aux_out->layers[kLayerHeader].total_size += header_size;
  }

  if (opsin.HasAlpha()) {
    PROFILER_ZONE("enc alpha");
    size_t bytepos = compressed->size();
    if (!AlphaToPik(params_in, opsin.GetAlpha(), opsin.AlphaBitDepth(),
                    &bytepos, compressed)) {
      return false;
    }
    if (aux_out) {
      aux_out->layers[kLayerAlpha].total_size +=
          compressed->size() - header_size;
    }
  }

  CompressParams params = params_in;
  size_t target_size = TargetSize(params, image);
  size_t opsin_target_size =
      (compressed->size() < target_size ? target_size - compressed->size() : 1);
  if (params.target_size > 0 || params.target_bitrate > 0.0) {
    params.target_size = opsin_target_size;
  }
  if (!OpsinToPik(params, header, opsin, pool, compressed, aux_out)) {
    return false;
  }
  return true;
}

bool PixelsToPik(const CompressParams& params, const Image3B& image,
                 ThreadPool* pool, PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, pool, compressed, aux_out);
}

bool PixelsToPik(const CompressParams& params, const Image3F& image,
                 ThreadPool* pool, PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, pool, compressed, aux_out);
}

bool PixelsToPik(const CompressParams& params, const MetaImageB& image,
                 ThreadPool* pool, PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, pool, compressed, aux_out);
}

bool PixelsToPik(const CompressParams& params, const MetaImageF& image,
                 ThreadPool* pool, PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, pool, compressed, aux_out);
}

bool OpsinToPik(const CompressParams& params, const Header& header,
                const MetaImageF& opsin_orig,
                ThreadPool* pool, PaddedBytes* compressed, PikInfo* aux_out) {
  PROFILER_ZONE("enc OpsinToPik uninstrumented");
  if (opsin_orig.xsize() == 0 || opsin_orig.ysize() == 0) {
    return PIK_FAILURE("Empty image");
  }
  const size_t xsize = opsin_orig.xsize();
  const size_t ysize = opsin_orig.ysize();
  const size_t block_xsize = DivCeil(xsize, 8);
  const size_t block_ysize = DivCeil(ysize, 8);
  Image3F opsin = AlignImage(opsin_orig.GetColor(), 8);
  CenterOpsinValues(&opsin);
  NoiseParams noise_params;
  // We don't add noise at low butteraugli distances, since the
  // original noise is stored within the compressed image. Adding the
  // noise there only makes things worse. We start adding noise at
  // the kNoiseModelingRampUpDistanceMin distance, but we don't start
  // at zero amplitude since adding noise is expensive -- it significantly
  // slows down decoding, and this is unlikely to completely go away even
  // with advanced optimizations. After the kNoiseModelingRampUpDistanceRange
  // we have reached the full level, i.e., noise is no longer represented by
  // the compressed image, so we can add full noise by the noise modeling
  // itself.
  static const double kNoiseModelingRampUpDistanceMin = 1.4;
  static const double kNoiseModelingRampUpDistanceRange = 0.6;
  static const double kNoiseLevelAtStartOfRampUp = 0.25;
  bool enable_noise =
      params.butteraugli_distance > kNoiseModelingRampUpDistanceMin;
  if (params.apply_noise != Override::kDefault) {
    enable_noise = params.apply_noise == Override::kOn;
  }
  if (enable_noise) {
    PROFILER_ZONE("enc GetNoiseParam");
    // TODO(user) test and properly select quality_coef with smooth filter
    float quality_coef = 1.0f;
    const double rampup =
        (params.butteraugli_distance - kNoiseModelingRampUpDistanceMin) /
        kNoiseModelingRampUpDistanceRange;
    if (rampup < 1.0) {
      quality_coef = kNoiseLevelAtStartOfRampUp +
                     (1.0 - kNoiseLevelAtStartOfRampUp) * rampup;
    }
    GetNoiseParameter(opsin, &noise_params, quality_coef);
  }
  if (header.flags & Header::kGaborishTransform) {
    GaborishInverse(opsin);
  }
  ColorTransform ctan(xsize, ysize);
  if (!params.fast_mode &&
      (params.butteraugli_distance >= 0.0 || params.target_bitrate > 0.0 ||
       params.target_size > 0)) {
    PROFILER_ZONE("enc YTo* correlation");
    FindBestYToBCorrelation(opsin, &ctan.ytob_map, &ctan.ytob_dc);
    FindBestYToXCorrelation(opsin, &ctan.ytox_map, &ctan.ytox_dc);
  }
  Quantizer quantizer(header.quant_template, block_xsize, block_ysize);
  quantizer.SetQuant(1.0f);
  if (params.fast_mode) {
    PROFILER_ZONE("enc fast quant");
    const float butteraugli_target = params.butteraugli_distance;
    const float butteraugli_target_dc =
        std::min<float>(butteraugli_target,
                        pow(butteraugli_target, 0.69822238825785388));
    const float kQuantDC = 0.57 / butteraugli_target_dc;
    const float kQuantAC = ( 1.8570494508273865 ) / butteraugli_target;
    ImageF qf = AdaptiveQuantizationMap(opsin_orig.GetColor().plane(1), 8);
    quantizer.SetQuantField(kQuantDC, ScaleImage(kQuantAC, qf), params);
  } else if (params.target_size > 0 || params.target_bitrate > 0.0) {
    size_t target_size = TargetSize(params, opsin);
    if (params.target_size_search_fast_mode) {
      PROFILER_ZONE("enc find best + scaleToTarget");
      FindBestQuantization(opsin_orig.GetColor(), opsin, params, header, 1.0,
                           ctan, &quantizer, aux_out);
      ScaleToTargetSize(opsin, params, noise_params, header, target_size, ctan,
                        &quantizer, aux_out);
    } else {
      PROFILER_ZONE("enc compressToTarget");
      CompressToTargetSize(opsin_orig.GetColor(), opsin, params, noise_params,
                           header, target_size, ctan, &quantizer, aux_out);
    }
  } else if (params.uniform_quant > 0.0) {
    PROFILER_ZONE("enc SetQuant");
    quantizer.SetQuant(params.uniform_quant, params);
  } else {
    // Normal PIK encoding to a butteraugli score.
    if(params.butteraugli_distance < 0) {
      return false;
    }
    PROFILER_ZONE("enc find best2");
    if (params.butteraugli_distance <= kNoiseModelingRampUpDistanceMin) {
      FindBestQuantizationHQ(opsin_orig.GetColor(), opsin, params, header,
                             params.butteraugli_distance, ctan,
                             &quantizer, aux_out);
    } else {
      FindBestQuantization(opsin_orig.GetColor(), opsin, params, header,
                           params.butteraugli_distance, ctan,
                           &quantizer, aux_out);
    }
  }
  QuantizedCoeffs qcoeffs =
      ComputeCoefficients(params, header, opsin, quantizer, ctan, pool,
                          aux_out);
  std::string compressed_data = EncodeToBitstream(
      qcoeffs, quantizer, noise_params, ctan, params.fast_mode, aux_out);

  size_t old_size = compressed->size();
  compressed->resize(compressed->size() + compressed_data.size());
  memcpy(compressed->data() + old_size, compressed_data.data(),
         compressed_data.size());
  return true;
}

bool JpegToPik(const CompressParams& params, const guetzli::JPEGData& jpeg,
               ThreadPool* pool, PaddedBytes* compressed, PikInfo* aux_out) {
  if (params.butteraugli_distance <= 0.0) {
    return JpegToPikLossless(jpeg, compressed, aux_out);
  }

  guetzli::Params guetzli_params;
  guetzli_params.butteraugli_target = params.butteraugli_distance;
  guetzli_params.clear_metadata = params.clear_metadata;
  guetzli::JPEGData jpeg_out;
  if (!guetzli::Process(guetzli_params, jpeg, &jpeg_out)) {
    return PIK_FAILURE("Guetzli processing failed.");
  }
  return JpegToPikLossless(jpeg_out, compressed, aux_out);
}

template <typename T>
bool PikToPixelsT(const DecompressParams& params, const PaddedBytes& compressed,
                  ThreadPool* pool, MetaImage<T>* image, PikInfo* aux_out) {
  PROFILER_ZONE("PikToPixels uninstrumented");
  if (compressed.size() == 0) {
    return PIK_FAILURE("Empty input.");
  }
  Image3<T> srgb;
  const uint8_t* const compressed_end = compressed.data() + compressed.size();

  Header header;
  const uint8_t* header_end = LoadHeader(compressed.data(), &header);
  if (header_end == nullptr) return false;
  if (header_end > compressed_end) {
    return PIK_FAILURE("Truncated header.");
  }
  size_t byte_pos = header_end - compressed.data();
  PIK_ASSERT(byte_pos <= compressed.size());

  if (header.bitstream == Header::kBitstreamBrunsli) {
    if (!BrunsliToPixels(compressed, byte_pos, &srgb)) {
      return false;
    }
    image->SetColor(std::move(srgb));
    return true;
  }
  if (header.bitstream != Header::kBitstreamDefault) {
    return PIK_FAILURE("Unsupported bitstream");
  }

  // Default bitstream:
  if (header.xsize == 0 || header.ysize == 0) {
    return PIK_FAILURE("Empty image.");
  }
  static const uint32_t kMaxWidth = (1 << 25) - 1;
  if (header.xsize > kMaxWidth) {
    return PIK_FAILURE("Image too wide.");
  }
  uint64_t num_pixels = static_cast<uint64_t>(header.xsize) * header.ysize;
  if (num_pixels > params.max_num_pixels) {
    return PIK_FAILURE("Image too big.");
  }
  if (header.quant_template >= kNumQuantTables) {
    return PIK_FAILURE("Invalid quant table.");
  }

  ImageU alpha(header.xsize, header.ysize);
  int alpha_bit_depth = 0;
  if (header.flags & Header::kAlpha) {
    size_t bytes_read;
    if (!PikToAlpha(params, byte_pos, compressed, &bytes_read, &alpha_bit_depth,
                    &alpha)) {
      return false;
    }
    byte_pos += bytes_read;
    PIK_ASSERT(byte_pos <= compressed.size());
  }

  int block_xsize = (header.xsize + 7) / 8;
  int block_ysize = (header.ysize + 7) / 8;
  Quantizer quantizer(header.quant_template, block_xsize, block_ysize);
  QuantizedCoeffs qcoeffs;
  NoiseParams noise_params;
  ColorTransform ctan(header.xsize, header.ysize);
  size_t bytes_read;
  {
    PROFILER_ZONE("dec_bitstr");
    if (!DecodeFromBitstream(compressed.data() + byte_pos,
                             compressed.size() - byte_pos, header.xsize,
                             header.ysize, &ctan, &noise_params, &quantizer,
                             &qcoeffs, &bytes_read)) {
      return PIK_FAILURE("Pik decoding failed.");
    }
  }
  byte_pos += bytes_read;
  PIK_ASSERT(byte_pos <= compressed.size());
  Image3F opsin =
      ReconOpsinImage(header, qcoeffs, quantizer, ctan, pool, aux_out);
  bool enable_denoise = (header.flags & Header::kDenoise) != 0;
  if (params.denoise != Override::kDefault) {
    enable_denoise = params.denoise == Override::kOn;
  }
  if (enable_denoise) {
    PROFILER_ZONE("denoise");
    DoDenoise(quantizer, &opsin);
  }
  {
    PROFILER_ZONE("add_noise");
    AddNoise(noise_params, &opsin);
  }
  // TODO(janwas): merge with TF graphs for Gaborish, denoise, AddNoise.
  const bool dither = (header.flags & Header::kDither) != 0;
  CenteredOpsinToSrgb(opsin, dither, pool, &srgb);
  srgb.ShrinkTo(header.xsize, header.ysize);

  image->SetColor(std::move(srgb));
  if (alpha_bit_depth > 0) {
    image->SetAlpha(std::move(alpha), alpha_bit_depth);
  }
  if (params.check_decompressed_size && byte_pos != compressed.size()) {
    return PIK_FAILURE("Pik compressed data size mismatch.");
  }
  if (aux_out != nullptr) {
    aux_out->decoded_size = byte_pos;
  }
  return true;
}

bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 ThreadPool* pool, MetaImageB* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, pool, image, aux_out);
}

bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 ThreadPool* pool, MetaImageU* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, pool, image, aux_out);
}

bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 ThreadPool* pool, MetaImageF* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, pool, image, aux_out);
}

template <typename T>
bool PikToPixelsT(const DecompressParams& params, const PaddedBytes& compressed,
                  ThreadPool* pool, Image3<T>* image, PikInfo* aux_out) {
  PROFILER_ZONE("PikToPixels alpha uninstrumented");
  MetaImage<T> temp;
  if (!PikToPixelsT(params, compressed, pool, &temp, aux_out)) {
    return false;
  }
  if (temp.HasAlpha()) {
    return PIK_FAILURE("Unable to output alpha channel");
  }
  *image = std::move(temp.GetColor());
  return true;
}

bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 ThreadPool* pool, Image3B* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, pool, image, aux_out);
}
bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 ThreadPool* pool, Image3U* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, pool, image, aux_out);
}
bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 ThreadPool* pool, Image3F* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, pool, image, aux_out);
}

}  // namespace pik
