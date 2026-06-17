#pragma once

#include <cstddef>
#include <vector>

namespace argus {

// Minimal in-place iterative radix-2 Cooley-Tukey FFT (power-of-two sizes only).
// Dependency-free; window sizes are chosen by the caller, so power-of-two is fine.
class Fft {
public:
    explicit Fft(std::size_t n);  // n must be a power of two

    // Magnitude spectrum (size n/2+1) of a real, already-windowed frame of length n.
    void magnitude(const float* frame, std::vector<float>& magOut) const;

    std::size_t size() const { return n_; }
    std::size_t bins() const { return n_ / 2 + 1; }

private:
    void transform(std::vector<float>& re, std::vector<float>& im) const;

    std::size_t n_;
    std::vector<std::size_t> rev_;  // bit-reversal table
    std::vector<float> cosT_, sinT_;
};

}  // namespace argus
