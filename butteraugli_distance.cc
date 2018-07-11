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

#include "butteraugli_distance.h"

#include <stddef.h>
#include <string.h>
#include <array>
#include <utility>
#include <vector>

#include "alpha_blend.h"
#include "butteraugli/butteraugli.h"
#include "compiler_specific.h"
#include "gamma_correct.h"
#include "image.h"

namespace pik {

float ButteraugliDistance(const Image3F& rgb0, const Image3F& rgb1,
                          float hf_asymmetry,
                          ImageF* distmap_out) {
  const size_t xsize = rgb0.xsize();
  const size_t ysize = rgb0.ysize();
  const size_t row_size = xsize * sizeof(*rgb0.PlaneRow(0, 0));
  std::vector<butteraugli::ImageF> rgb0b;
  std::vector<butteraugli::ImageF> rgb1b;
  for (int c = 0; c < 3; ++c) {
    butteraugli::ImageF plane0(xsize, ysize);
    butteraugli::ImageF plane1(xsize, ysize);
    for (int y = 0; y < ysize; ++y) {
      memcpy(plane0.Row(y), rgb0.PlaneRow(c, y), row_size);
    }
    for (int y = 0; y < ysize; ++y) {
      memcpy(plane1.Row(y), rgb1.PlaneRow(c, y), row_size);
    }
    rgb0b.emplace_back(std::move(plane0));
    rgb1b.emplace_back(std::move(plane1));
  }
  butteraugli::ImageF distmap;
  butteraugli::ButteraugliDiffmap(rgb0b, rgb1b, hf_asymmetry, distmap);
  if (distmap_out != nullptr) {
    *distmap_out = ImageF(rgb0.xsize(), rgb0.ysize());
    for (int y = 0; y < rgb0.ysize(); ++y) {
      const float* const PIK_RESTRICT row = distmap.Row(y);
      float* const PIK_RESTRICT row_out = distmap_out->Row(y);
      memcpy(row_out, row, rgb0.xsize() * sizeof(row[0]));
    }
  }
  return butteraugli::ButteraugliScoreFromDiffmap(distmap);
}

float ButteraugliDistance(const Image3B& rgb0, const Image3B& rgb1,
                          float hf_asymmetry,
                          ImageF* distmap_out) {
  return ButteraugliDistance(LinearFromSrgb(rgb0),
                             LinearFromSrgb(rgb1),
                             hf_asymmetry,
                             distmap_out);
}

float ButteraugliDistance(const MetaImageF& rgb0, const MetaImageF& rgb1,
                          float hf_asymmetry,
                          ImageF* distmap_out) {
  if (!rgb0.HasAlpha() && !rgb1.HasAlpha()) {
    return ButteraugliDistance(rgb0.GetColor(), rgb1.GetColor(),
                               hf_asymmetry, distmap_out);
  }
  ImageF distmap_black, distmap_white;
  float dist_black = ButteraugliDistance(AlphaBlend(rgb0, 0),
                                         AlphaBlend(rgb1, 0),
                                         hf_asymmetry,
                                         &distmap_black);
  float dist_white = ButteraugliDistance(AlphaBlend(rgb0, 255),
                                         AlphaBlend(rgb1, 255),
                                         hf_asymmetry,
                                         &distmap_white);
  if (distmap_out != nullptr) {
    const size_t xsize = rgb0.xsize();
    const size_t ysize = rgb0.ysize();
    *distmap_out = ImageF(xsize, ysize);
    for (int y = 0; y < ysize; ++y) {
      for (int x = 0; x < xsize; ++x) {
        distmap_out->Row(y)[x] = std::max(distmap_black.Row(y)[x],
                                          distmap_white.Row(y)[x]);
      }
    }
  }
  return std::max(dist_black, dist_white);
}

}  // namespace pik
