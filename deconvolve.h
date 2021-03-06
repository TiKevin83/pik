#ifndef DECONVOLVE_H_
#define DECONVOLVE_H_

namespace pik {

// Compute a filter such that convolving with it is an approximation of the
// inverse of convolving with the provided filter.
// The resulting filter is written into inverse_filter and is of the provided
// inverse_filter_length length.
// filter_length and inverse_filter_length have to be odd.
// Returns the L2 distance between the identity filter and the composition of
// the two filters.
float InvertConvolution(const float* filter, int filter_length,
                        float* inverse_filter, int inverse_filter_length);

}  // namespace pik

#endif  // DECONVOLVE_H_
