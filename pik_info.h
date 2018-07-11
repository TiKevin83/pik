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

#ifndef PIK_INFO_H_
#define PIK_INFO_H_

#include <cstddef>
#include <string>
#include <vector>
#include "image.h"
#include "image_io.h"

namespace pik {

struct PikImageSizeInfo {
  PikImageSizeInfo() {}

  void Assimilate(const PikImageSizeInfo& victim) {
    num_clustered_histograms += victim.num_clustered_histograms;
    histogram_size += victim.histogram_size;
    entropy_coded_bits += victim.entropy_coded_bits;
    extra_bits += victim.extra_bits;
    total_size += victim.total_size;
    clustered_entropy += victim.clustered_entropy;
  }
  void Print(size_t num_inputs) const {
    printf("%10zd", total_size);
    if (histogram_size > 0) {
      printf("   [%6.2f %8zd %8zd %8zd %12.3f",
             num_clustered_histograms * 1.0 / num_inputs, histogram_size,
             entropy_coded_bits >> 3, extra_bits >> 3,
             histogram_size + (clustered_entropy + extra_bits) / 8.0f);
      printf("]");
    }
    printf("\n");
  }
  size_t num_clustered_histograms = 0;
  size_t histogram_size = 0;
  size_t entropy_coded_bits = 0;
  size_t extra_bits = 0;
  size_t total_size = 0;
  double clustered_entropy = 0.0f;
};

static const int kNumImageLayers = 7;
static const int kLayerHeader = 0;
static const int kLayerSections = 1;
static const int kLayerQuant = 2;
static const int kLayerOrder = 3;
static const int kLayerCtan = 4;
static const int kLayerDC = 5;
static const int kLayerAC = 6;
static const char* kImageLayers[kNumImageLayers] = {
    "header", "sections", "quant", "order", "ctan", "DC", "AC"};

// Metadata and statistics gathered during compression or decompression.
struct PikInfo {
  PikInfo() : layers(kNumImageLayers), num_dict_matches(3) {}
  void Assimilate(const PikInfo& victim) {
    for (int i = 0; i < layers.size(); ++i) {
      layers[i].Assimilate(victim.layers[i]);
    }
    for (int c = 0; c < 3; ++c) {
      num_dict_matches[c] += victim.num_dict_matches[c];
    }
    num_blocks += victim.num_blocks;
    num_butteraugli_iters += victim.num_butteraugli_iters;
  }
  PikImageSizeInfo TotalImageSize() const {
    PikImageSizeInfo total;
    for (int i = 0; i < layers.size(); ++i) {
      total.Assimilate(layers[i]);
    }
    return total;
  }

  void Print(size_t num_inputs) const {
    if (num_inputs == 0) return;
    printf("Average butteraugli iters: %10.2f\n",
           num_butteraugli_iters * 1.0 / num_inputs);
    if (num_dict_matches[0] + num_dict_matches[1] + num_dict_matches[2] > 0) {
      printf("Average dictionary matches: %9.2f%% %9.2f%% %9.2f%%\n",
             num_dict_matches[0] * 100.0f / num_blocks,
             num_dict_matches[1] * 100.0f / num_blocks,
             num_dict_matches[2] * 100.0f / num_blocks);
    }
    for (int i = 0; i < layers.size(); ++i) {
      if (layers[i].total_size > 0) {
        printf("Total layer size %-10s", kImageLayers[i]);
        layers[i].Print(num_inputs);
      }
    }
    printf("Total image size           ");
    TotalImageSize().Print(num_inputs);
  }

  template <typename Img>
  void DumpImage(const char* label, const Img& image) const {
    if (debug_prefix.empty()) return;
    char pathname[200];
    snprintf(pathname, sizeof(pathname), "%s%s.png", debug_prefix.c_str(),
             label);
    WriteImage(ImageFormatPNG(), image, pathname);
  }

  // This dumps coefficients as a 16-bit PNG with coefficients of a block placed
  // in the area that would contain that block in a normal image. To view the
  // resulting image manually, rescale intensities by using:
  // $ convert -auto-level IMAGE.PNG - | display -
  void DumpCoeffImage(const char* label, const Image3S& coeff_image) const;

  std::vector<PikImageSizeInfo> layers;
  std::vector<int> num_dict_matches;
  std::size_t num_blocks = 0;
  int num_butteraugli_iters = 0;
  size_t decoded_size = 0;
  // If not empty, additional debugging information (e.g. debug images) is
  // saved in files with this prefix.
  std::string debug_prefix;
};

// Used to skip image creation if they won't be written to debug directory.
static inline bool WantDebugOutput(const PikInfo* info) {
  // Need valid pointer and filename.
  return info != nullptr && !info->debug_prefix.empty();
}

}  // namespace pik

#endif  // PIK_INFO_H_
