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

#include "butteraugli_comparator.h"

#include <stddef.h>
#include <array>
#include <memory>
#include <vector>

#include "compiler_specific.h"
#include "gamma_correct.h"
#include "opsin_inverse.h"
#include "simd/simd.h"
#include "status.h"

namespace pik {
namespace SIMD_NAMESPACE {
namespace {

// REQUIRES: xsize <= srgb.xsize(), ysize <= srgb.ysize()
std::vector<butteraugli::ImageF> SrgbToLinearRgb(
    const int xsize, const int ysize,
    const Image3B& srgb) {
  PIK_ASSERT(xsize <= srgb.xsize());
  PIK_ASSERT(ysize <= srgb.ysize());
  const float* lut = Srgb8ToLinearTable();
  std::vector<butteraugli::ImageF> planes =
      butteraugli::CreatePlanes<float>(xsize, ysize, 3);
  for (int c = 0; c < 3; ++c) {
    for (size_t y = 0; y < ysize; ++y) {
      const uint8_t* PIK_RESTRICT row_in = srgb.PlaneRow(c, y);
      float* PIK_RESTRICT row_out = planes[c].Row(y);
      for (size_t x = 0; x < xsize; ++x) {
        row_out[x] = lut[row_in[x]];
      }
    }
  }
  return planes;
}

// REQUIRES: xsize <= srgb.xsize(), ysize <= srgb.ysize()
std::vector<butteraugli::ImageF> OpsinToLinearRgb(
    const int xsize, const int ysize,
    const Image3F& opsin) {
  const Full<float> d;
  using V = Full<float>::V;
  V inverse_matrix[9];
  for (size_t i = 0; i < 9; ++i) {
    inverse_matrix[i] = set1(d, GetOpsinAbsorbanceInverseMatrix()[i]);
  }

  PIK_ASSERT(xsize <= opsin.xsize());
  PIK_ASSERT(ysize <= opsin.ysize());
  std::vector<butteraugli::ImageF> planes =
      butteraugli::CreatePlanes<float>(xsize, ysize, 3);
  for (size_t y = 0; y < ysize; ++y) {
    const float* PIK_RESTRICT row_xyb0 = opsin.PlaneRow(0, y);
    const float* PIK_RESTRICT row_xyb1 = opsin.PlaneRow(1, y);
    const float* PIK_RESTRICT row_xyb2 = opsin.PlaneRow(2, y);
    float* PIK_RESTRICT row_rgb0 = planes[0].Row(y);
    float* PIK_RESTRICT row_rgb1 = planes[1].Row(y);
    float* PIK_RESTRICT row_rgb2 = planes[2].Row(y);
    for (size_t x = 0; x < xsize; x += d.N) {
      const auto vx = load(d, row_xyb0 + x);
      const auto vy = load(d, row_xyb1 + x);
      const auto vb = load(d, row_xyb2 + x);
      V r, g, b;
      XybToRgb(d, vx, vy, vb, inverse_matrix, &r, &g, &b);
      store(r, d, row_rgb0 + x);
      store(g, d, row_rgb1 + x);
      store(b, d, row_rgb2 + x);
    }
  }
  return planes;
}

Image3F Image3FromButteraugliPlanes(
    const std::vector<butteraugli::ImageF>& planes) {
  Image3F img(planes[0].xsize(), planes[0].ysize());
  for (int c = 0; c < 3; ++c) {
    for (size_t y = 0; y < img.ysize(); ++y) {
      const float* PIK_RESTRICT row_in = planes[c].Row(y);
      float* PIK_RESTRICT row_out = img.PlaneRow(c, y);
      for (size_t x = 0; x < img.xsize(); ++x) {
        row_out[x] = row_in[x];
      }
    }
  }
  return img;
}

}  // namespace
}  // namespace

ButteraugliComparator::ButteraugliComparator(const Image3B& srgb,
                                             float hf_asymmetry)
    : xsize_(srgb.xsize()),
      ysize_(srgb.ysize()),
      comparator_(SIMD_NAMESPACE::SrgbToLinearRgb(xsize_, ysize_, srgb),
                  hf_asymmetry),
      distance_(0.0),
      distmap_(xsize_, ysize_, 0) {}

ButteraugliComparator::ButteraugliComparator(const Image3F& opsin,
                                             float hf_asymmetry)
    : xsize_(opsin.xsize()),
      ysize_(opsin.ysize()),
      comparator_(SIMD_NAMESPACE::OpsinToLinearRgb(xsize_, ysize_, opsin),
                  hf_asymmetry),
      distance_(0.0),
      distmap_(xsize_, ysize_, 0) {}

void ButteraugliComparator::Compare(const Image3B& srgb) {
  comparator_.Diffmap(SIMD_NAMESPACE::SrgbToLinearRgb(xsize_, ysize_, srgb),
                      distmap_);
  distance_ = butteraugli::ButteraugliScoreFromDiffmap(distmap_);
}

void ButteraugliComparator::Mask(Image3F* mask, Image3F* mask_dc) {
  std::vector<butteraugli::ImageF> ba_mask, ba_mask_dc;
  comparator_.Mask(&ba_mask, &ba_mask_dc);
  *mask = SIMD_NAMESPACE::Image3FromButteraugliPlanes(ba_mask);
  *mask_dc = SIMD_NAMESPACE::Image3FromButteraugliPlanes(ba_mask_dc);
}

}  // namespace pik
