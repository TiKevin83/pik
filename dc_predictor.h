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

#ifndef DC_PREDICTOR_H_
#define DC_PREDICTOR_H_

// DC coefficients serve as an image preview, so they are coded separately.
// Subtracting predicted values leads to a "residual" distribution with lower
// entropy and magnitudes than the original values. These can be coded more
// efficiently, even when context modeling is used.
//
// Our predictors use immediately adjacent causal pixels because more distant
// pixels are only weakly correlated in subsampled DC images. We also utilize
// cross-channel correlation by choosing a predictor based upon its performance
// on a previously decoded channel.
//
// This module decreases final size of DC images by 2-4% vs. the standard
// MED/MAP predictor from JPEG-LS and processes 330 M coefficients per second.
// The average residual is about 1.3% of the maximum DC value.

#include <stdint.h>

#include "compiler_specific.h"
#include "image.h"

namespace pik {

// The predictors operate on DCT coefficients or perhaps original pixels.
// Must be 16-bit because we have 128 bit vectors and 8 predictors.
using DC = int16_t;

// Predicts "in_y" coefficients within "rect_in" based on their neighbors and
// stores the residuals into "residuals" within "rect_res". The predictors
// are tuned for luminance.
void ShrinkY(const Rect& rect_in, const ImageS& in_y, const Rect& rect_res,
             ImageS* PIK_RESTRICT residuals);

// All tmp_* images are thread-specific group-sized subsets:

// Expands "residuals" within "rect" (same as a prior call to ShrinkY) into
// preallocated "tmp_expanded", using predictions from prior pixels.
void ExpandY(const Rect& rect, const ImageS& residuals,
             ImageS* PIK_RESTRICT tmp_expanded);

// Stores residuals of predicting XB pairs in "tmp_xb" from their neighbors
// and the window "rect" [blocks] within already expanded "y" (luminance).
void ShrinkXB(const Rect& rect, const ImageS& in_y, const ImageS& tmp_xb,
              ImageS* PIK_RESTRICT tmp_xb_residuals);

// Expands "tmp_xb_residuals" (a subset of the result of ShrinkXB) into
// "tmp_xb_expanded", using predictions from prior pixels and "tmp_y". All
// images are at least xsize * ysize (2*xsize for xz).
void ExpandXB(const size_t xsize, const size_t ysize, const ImageS& tmp_y,
              const ImageS& tmp_xb_residuals,
              ImageS* PIK_RESTRICT tmp_xb_expanded);

}  // namespace pik

#endif  // DC_PREDICTOR_H_
