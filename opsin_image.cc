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

#include "opsin_image.h"

#include <stddef.h>
#include <array>

#undef PROFILER_ENABLED
#define PROFILER_ENABLED 1
#include "approx_cube_root.h"
#include "compiler_specific.h"
#include "gamma_correct.h"
#include "profiler.h"

namespace pik {

namespace {

PIK_INLINE float SimpleGamma(float v) {
  return ApproxCubeRoot(v);
}

void LinearXybTransform(float r, float g, float b, float* PIK_RESTRICT valx,
                        float* PIK_RESTRICT valy, float* PIK_RESTRICT valz) {
  *valx = (kScaleR * r - kScaleG * g) * 0.5f;
  *valy = (kScaleR * r + kScaleG * g) * 0.5f;
  *valz = b;
}

void LinearToXyb(const float rgb[3], float* PIK_RESTRICT valx,
                 float* PIK_RESTRICT valy, float* PIK_RESTRICT valz) {
  float mixed[3];
  OpsinAbsorbance(rgb, mixed);
  mixed[0] = SimpleGamma(mixed[0]);
  mixed[1] = SimpleGamma(mixed[1]);
  mixed[2] = SimpleGamma(mixed[2]);
  LinearXybTransform(mixed[0], mixed[1], mixed[2], valx, valy, valz);
}

}  // namespace

void RgbToXyb(uint8_t r, uint8_t g, uint8_t b, float* PIK_RESTRICT valx,
              float* PIK_RESTRICT valy, float* PIK_RESTRICT valz) {
  // TODO(janwas): replace with polynomial to enable vectorization.
  const float* lut = Srgb8ToLinearTable();
  const float rgb[3] = {lut[r], lut[g], lut[b]};
  LinearToXyb(rgb, valx, valy, valz);
}

Image3F OpsinDynamicsImage(const Image3B& srgb) {
  PROFILER_FUNC;
  // This is different from butteraugli::OpsinDynamicsImage() in the sense that
  // it does not contain a sensitivity multiplier based on the blurred image.
  const size_t xsize = srgb.xsize();
  const size_t ysize = srgb.ysize();
  Image3F opsin(xsize, ysize);
  for (size_t iy = 0; iy < ysize; iy++) {
    const uint8_t* PIK_RESTRICT row_srgb0 = srgb.ConstPlaneRow(0, iy);
    const uint8_t* PIK_RESTRICT row_srgb1 = srgb.ConstPlaneRow(1, iy);
    const uint8_t* PIK_RESTRICT row_srgb2 = srgb.ConstPlaneRow(2, iy);
    float* PIK_RESTRICT row_xyb0 = opsin.PlaneRow(0, iy);
    float* PIK_RESTRICT row_xyb1 = opsin.PlaneRow(1, iy);
    float* PIK_RESTRICT row_xyb2 = opsin.PlaneRow(2, iy);
    for (size_t ix = 0; ix < xsize; ix++) {
      RgbToXyb(row_srgb0[ix], row_srgb1[ix], row_srgb2[ix], &row_xyb0[ix],
               &row_xyb1[ix], &row_xyb2[ix]);
    }
  }
  return opsin;
}

Image3F OpsinDynamicsImage(const Image3F& linear) {
  PROFILER_FUNC;
  // This is different from butteraugli::OpsinDynamicsImage() in the sense that
  // it does not contain a sensitivity multiplier based on the blurred image.
  const size_t xsize = linear.xsize();
  const size_t ysize = linear.ysize();
  Image3F opsin(xsize, ysize);
  for (size_t iy = 0; iy < ysize; iy++) {
    const float* PIK_RESTRICT row_in0 = linear.ConstPlaneRow(0, iy);
    const float* PIK_RESTRICT row_in1 = linear.ConstPlaneRow(1, iy);
    const float* PIK_RESTRICT row_in2 = linear.ConstPlaneRow(2, iy);
    float* PIK_RESTRICT row_xyb0 = opsin.PlaneRow(0, iy);
    float* PIK_RESTRICT row_xyb1 = opsin.PlaneRow(1, iy);
    float* PIK_RESTRICT row_xyb2 = opsin.PlaneRow(2, iy);
    for (size_t ix = 0; ix < xsize; ix++) {
      const float rgb[3] = {row_in0[ix], row_in1[ix], row_in2[ix]};
      LinearToXyb(rgb, &row_xyb0[ix], &row_xyb1[ix], &row_xyb2[ix]);
    }
  }
  return opsin;
}

}  // namespace pik
