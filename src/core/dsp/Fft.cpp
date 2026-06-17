#include "Fft.h"

#include <cmath>

namespace argus {
namespace {
constexpr double kPi = 3.14159265358979323846;
}

Fft::Fft(std::size_t n) : n_(n) {
    // Bit-reversal permutation table.
    rev_.resize(n_);
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < n_) ++bits;
    for (std::size_t i = 0; i < n_; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b)
            if (i & (std::size_t{1} << b)) r |= (std::size_t{1} << (bits - 1 - b));
        rev_[i] = r;
    }
    // Twiddle tables.
    cosT_.resize(n_ / 2);
    sinT_.resize(n_ / 2);
    for (std::size_t i = 0; i < n_ / 2; ++i) {
        double a = -2.0 * kPi * i / n_;
        cosT_[i] = static_cast<float>(std::cos(a));
        sinT_[i] = static_cast<float>(std::sin(a));
    }
}

void Fft::transform(std::vector<float>& re, std::vector<float>& im) const {
    for (std::size_t i = 0; i < n_; ++i) {
        std::size_t j = rev_[i];
        if (j > i) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= n_; len <<= 1) {
        std::size_t half = len >> 1;
        std::size_t step = n_ / len;
        for (std::size_t i = 0; i < n_; i += len) {
            std::size_t k = 0;
            for (std::size_t j = 0; j < half; ++j, k += step) {
                float wr = cosT_[k], wi = sinT_[k];
                float ur = re[i + j], ui = im[i + j];
                float vr = re[i + j + half] * wr - im[i + j + half] * wi;
                float vi = re[i + j + half] * wi + im[i + j + half] * wr;
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
            }
        }
    }
}

void Fft::magnitude(const float* frame, std::vector<float>& magOut) const {
    std::vector<float> re(n_), im(n_, 0.0f);
    for (std::size_t i = 0; i < n_; ++i) re[i] = frame[i];
    transform(re, im);
    magOut.resize(bins());
    for (std::size_t i = 0; i < bins(); ++i)
        magOut[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]);
}

}  // namespace argus
