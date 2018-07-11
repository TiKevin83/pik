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

#ifndef ENTROPY_CODER_H_
#define ENTROPY_CODER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ans_decode.h"
#include "ans_encode.h"
#include "bit_reader.h"
#include "cluster.h"
#include "common.h"
#include "compiler_specific.h"
#include "context.h"
#include "context_map_encode.h"
#include "fast_log.h"
#include "image.h"
#include "lehmer_code.h"
#include "pik_info.h"
#include "status.h"

namespace pik {

const int32_t kNaturalCoeffOrder[kBlockSize + 16] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40,
    48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
    // extra entries for safety in decoder
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

// Reorder the symbols by decreasing population-count (keeping the first
// end-of-block symbol in place).
static const uint8_t kIndexLut[256] = {
    0,   1,   2,   3,   5,   10,  17,  32,  68,  83,  84,  85,  86,  87,  88,
    89,  90,  4,   7,   12,  22,  31,  43,  60,  91,  92,  93,  94,  95,  96,
    97,  98,  99,  6,   14,  26,  36,  48,  66,  100, 101, 102, 103, 104, 105,
    106, 107, 108, 109, 8,   19,  34,  44,  57,  78,  110, 111, 112, 113, 114,
    115, 116, 117, 118, 119, 9,   27,  39,  52,  61,  79,  120, 121, 122, 123,
    124, 125, 126, 127, 128, 129, 11,  28,  41,  53,  64,  80,  130, 131, 132,
    133, 134, 135, 136, 137, 138, 139, 13,  33,  46,  63,  72,  140, 141, 142,
    143, 144, 145, 146, 147, 148, 149, 150, 15,  35,  47,  65,  69,  151, 152,
    153, 154, 155, 156, 157, 158, 159, 160, 161, 16,  37,  51,  62,  74,  162,
    163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 18,  38,  50,  59,  75,
    173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 20,  40,  54,  76,
    82,  184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 23,  42,  55,
    77,  195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 24,  45,
    56,  70,  207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 25,
    49,  58,  71,  219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230,
    29,  67,  81,  231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242,
    21,  30,  73,  243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
    255,
};

static const uint8_t kSymbolLut[256] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x21, 0x12, 0x31, 0x41, 0x05, 0x51,
    0x13, 0x61, 0x22, 0x71, 0x81, 0x06, 0x91, 0x32, 0xa1, 0xf0, 0x14, 0xb1,
    0xc1, 0xd1, 0x23, 0x42, 0x52, 0xe1, 0xf1, 0x15, 0x07, 0x62, 0x33, 0x72,
    0x24, 0x82, 0x92, 0x43, 0xa2, 0x53, 0xb2, 0x16, 0x34, 0xc2, 0x63, 0x73,
    0x25, 0xd2, 0x93, 0x83, 0x44, 0x54, 0xa3, 0xb3, 0xc3, 0x35, 0xd3, 0x94,
    0x17, 0x45, 0x84, 0x64, 0x55, 0x74, 0x26, 0xe2, 0x08, 0x75, 0xc4, 0xd4,
    0x65, 0xf2, 0x85, 0x95, 0xa4, 0xb4, 0x36, 0x46, 0x56, 0xe3, 0xa5, 0x09,
    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
    0x1d, 0x1e, 0x1f, 0x20, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e,
    0x2f, 0x30, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
    0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x57, 0x58,
    0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b,
    0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c,
    0x9d, 0x9e, 0x9f, 0xa0, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad,
    0xae, 0xaf, 0xb0, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd,
    0xbe, 0xbf, 0xc0, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd,
    0xce, 0xcf, 0xd0, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd,
    0xde, 0xdf, 0xe0, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb,
    0xfc, 0xfd, 0xfe, 0xff,
};

// Block context used for scanning order, # nonzero, AC coefficients.
// 0..2: flat, = channel; 3..5: directional (ignore channel)
static const size_t kOrderContexts = 6;
static const size_t kNumContexts = 128 + kOrderContexts * (32 + 120);

// Predicts |rect_dc| (typically a "group" of DC values, or less on the borders)
// within |dc| and stores residuals in |tmp_residuals| starting at 0,0.
void ShrinkDC(const Rect& rect_dc, const Image3S& dc,
              Image3S* PIK_RESTRICT tmp_residuals);

// Reconstructs |rect_dc| within |dc|: replaces these prediction residuals
// (generated by ShrinkDC) with reconstructed DC values. All images are at least
// rect.xsize * ysize (2*xsize for xz); tmp_* start at 0,0. Must be called with
// (one of) the same rect arguments passed to ShrinkDC.
void ExpandDC(const Rect& rect_dc, Image3S* PIK_RESTRICT dc,
              ImageS* PIK_RESTRICT tmp_y, ImageS* PIK_RESTRICT tmp_xz_residuals,
              ImageS* PIK_RESTRICT tmp_xz_expanded);

void ComputeCoeffOrder(const Image3S& ac, const Image3B& block_ctx,
                       int32_t* PIK_RESTRICT order);

std::string EncodeCoeffOrders(const int32_t* PIK_RESTRICT order,
                              PikInfo* PIK_RESTRICT pik_info);

// Encodes the "rect" subset of "img".
std::string EncodeImage(const Rect& rect, const Image3S& img,
                        PikImageSizeInfo* info);

struct Token {
  Token(uint32_t c, uint32_t s, uint32_t nb, uint32_t b)
      : context(c), bits(b), nbits(nb), symbol(s) {}
  uint32_t context;
  uint16_t bits;
  uint8_t nbits;
  uint8_t symbol;
};

// Only the subset "rect" [in units of blocks] within all images.
// Warning: uses the DC coefficients in "coeffs"!
std::vector<Token> TokenizeCoefficients(const int32_t* orders, const Rect& rect,
                                        const ImageI& quant_field,
                                        const Image3S& coeffs,
                                        const Image3B& block_ctx);

std::string BuildAndEncodeHistograms(
    size_t num_contexts, const std::vector<std::vector<Token> >& tokens,
    std::vector<ANSEncodingData>* codes, std::vector<uint8_t>* context_map,
    PikImageSizeInfo* info);

std::string BuildAndEncodeHistogramsFast(
    const std::vector<std::vector<Token> >& tokens,
    std::vector<ANSEncodingData>* codes, std::vector<uint8_t>* context_map,
    PikImageSizeInfo* info);

std::string WriteTokens(const std::vector<Token>& tokens,
                        const std::vector<ANSEncodingData>& codes,
                        const std::vector<uint8_t>& context_map,
                        PikImageSizeInfo* pik_info);

bool DecodeCoeffOrder(int32_t* order, BitReader* br);

bool DecodeHistograms(BitReader* br, const size_t num_contexts,
                      const size_t max_alphabet_size, const uint8_t* symbol_lut,
                      size_t symbol_lut_size, ANSCode* code,
                      std::vector<uint8_t>* context_map);

// Decodes into "rect" within "img".
bool DecodeImage(BitReader* PIK_RESTRICT br, const Rect& rect,
                 Image3S* PIK_RESTRICT img);

// "rect_ac/qf" are in blocks.
// DC component in ac's DCT blocks is invalid.
bool DecodeAC(const Image3B& tmp_block_ctx, const ANSCode& code,
              const std::vector<uint8_t>& context_map,
              const int32_t* PIK_RESTRICT coeff_order,
              BitReader* PIK_RESTRICT br, const Rect& rect_ac,
              Image3S* PIK_RESTRICT ac, const Rect& rect_qf,
              ImageI* PIK_RESTRICT quant_field,
              Image3I* PIK_RESTRICT tmp_num_nzeroes);

}  // namespace pik

#endif  // ENTROPY_CODER_H_
