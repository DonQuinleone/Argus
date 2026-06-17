#include "Spectrogram.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../dsp/Envelope.h"
#include "../dsp/Fft.h"
#include "Colormap.h"

namespace argus {
namespace {
constexpr double kPi = 3.14159265358979323846;

// Close-up framing: minimum context padding on each side of an event, and the
// minimum total window width so a very short defect still has room to breathe.
constexpr double kCloseupPadSec = 0.45;
constexpr double kCloseupMinSec = 0.90;

double hzToMel(double f) { return 2595.0 * std::log10(1.0 + f / 700.0); }
double melToHz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }

// Render an STFT magnitude spectrogram of mono[rangeStart .. rangeStart+rangeLen)
// into an RGBA8 raster (row-major, top = high freq). Shared by the full-file view
// and per-event close-ups. Frequency rows are bilinearly interpolated between the
// two bracketing FFT bins so the image stays smooth instead of banded.
void renderRangeRaster(const std::vector<float>& mono, int sampleRate,
                       std::size_t rangeStart, std::size_t rangeLen, int width, int height,
                       const SpectrogramSettings& s, std::vector<unsigned char>& rgba,
                       double& outMinF, double& outMaxF) {
    rgba.clear();
    const std::size_t n = mono.size();
    const int win = s.fftSize;
    if (sampleRate <= 0 || n < static_cast<std::size_t>(win) || width <= 0 || height <= 0) return;

    if (rangeStart >= n) rangeStart = 0;
    if (rangeLen == 0 || rangeStart + rangeLen > n) rangeLen = n - rangeStart;
    if (rangeLen < static_cast<std::size_t>(win)) {
        rangeLen = std::min(n, static_cast<std::size_t>(win));
        if (rangeStart + rangeLen > n) rangeStart = n - rangeLen;
    }

    const std::size_t span = (rangeLen > static_cast<std::size_t>(win)) ? rangeLen - win : 1;
    const double hop = static_cast<double>(span) / std::max(1, width - 1);

    // Hann window, normalised so a full-scale sine reads ~0 dBFS in its bin.
    std::vector<float> hann(win);
    for (int i = 0; i < win; ++i)
        hann[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / (win - 1)));
    const float norm = 4.0f / win;

    Fft fft(win);
    const std::size_t bins = fft.bins();
    const double nyquist = sampleRate / 2.0;

    // Pass 1: column magnitudes in dBFS.
    std::vector<float> colDb(static_cast<std::size_t>(width) * bins);
    std::vector<float> frame(win), mag;
    for (int c = 0; c < width; ++c) {
        std::size_t start = rangeStart + static_cast<std::size_t>(std::llround(c * hop));
        if (start + win > n) start = n - win;
        for (int i = 0; i < win; ++i) frame[i] = mono[start + i] * hann[i];
        fft.magnitude(frame.data(), mag);
        for (std::size_t b = 0; b < bins; ++b)
            colDb[static_cast<std::size_t>(c) * bins + b] =
                20.0f * std::log10(mag[b] * norm + 1e-9f);
    }

    const double fMin = std::max(20.0, nyquist / 2000.0);
    const double fMax = nyquist;
    outMinF = fMin;
    outMaxF = fMax;
    const double span_db = std::max(1.0, s.dbHigh - s.dbLow);

    // Fractional source bin for each output row under the chosen frequency scale.
    std::vector<double> rowBinF(height);
    for (int row = 0; row < height; ++row) {
        double frac = 1.0 - static_cast<double>(row) / (height - 1);  // 0 bottom .. 1 top
        double freq;
        switch (s.freqScale) {
            case FreqScale::Mel:
                freq = melToHz(hzToMel(fMin) + frac * (hzToMel(fMax) - hzToMel(fMin)));
                break;
            case FreqScale::Log:
                freq = std::pow(10.0, std::log10(fMin) + frac * (std::log10(fMax) - std::log10(fMin)));
                break;
            case FreqScale::Linear:
            default:
                freq = fMin + frac * (fMax - fMin);
                break;
        }
        double bf = freq / nyquist * (bins - 1);
        rowBinF[row] = std::min(static_cast<double>(bins - 1), std::max(0.0, bf));
    }

    rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (int row = 0; row < height; ++row) {
        double bf = rowBinF[row];
        int b0 = static_cast<int>(bf);
        int b1 = std::min(static_cast<int>(bins) - 1, b0 + 1);
        float w1 = static_cast<float>(bf - b0), w0 = 1.0f - w1;
        for (int c = 0; c < width; ++c) {
            const float* col = &colDb[static_cast<std::size_t>(c) * bins];
            float d = col[b0] * w0 + col[b1] * w1;
            float t = static_cast<float>((d - s.dbLow) / span_db);
            unsigned char r, g, bb;
            colormap(s.colormap, t, r, g, bb);
            std::size_t px = (static_cast<std::size_t>(row) * width + c) * 4;
            rgba[px + 0] = r;
            rgba[px + 1] = g;
            rgba[px + 2] = bb;
            rgba[px + 3] = 255;
        }
    }
}
}  // namespace

void renderSpectrogram(const AudioBuffer& buf, Report& rep, int targetWidth, int height,
                       const SpectrogramSettings& s) {
    rep.specRGBA.clear();
    rep.specWidth = rep.specHeight = 0;
    const std::size_t n = buf.frames();
    if (buf.sampleRate <= 0 || n < static_cast<std::size_t>(s.fftSize)) return;

    std::vector<float> mono;
    downmixMono(buf, mono);

    const int width = std::max(1, targetWidth);
    double minF = 0.0, maxF = 0.0;
    renderRangeRaster(mono, buf.sampleRate, 0, n, width, height, s, rep.specRGBA, minF, maxF);
    if (rep.specRGBA.empty()) return;

    rep.specMinFreq = minF;
    rep.specMaxFreq = maxF;
    rep.specLogFreq = (s.freqScale != FreqScale::Linear);
    rep.specScale = static_cast<int>(s.freqScale);
    rep.specDuration = buf.durationSec();
    rep.specWidth = width;
    rep.specHeight = height;
}

CloseupView renderCloseup(const AudioBuffer& buf, const Issue& issue,
                          const SpectrogramSettings& s) {
    CloseupView cv;
    const std::size_t n = buf.frames();
    if (buf.sampleRate <= 0 || n < static_cast<std::size_t>(s.fftSize) || !issue.localised())
        return cv;

    const double total = buf.durationSec();
    const double evStart = std::max(0.0, issue.tStart);
    const double evEnd = (issue.tEnd > issue.tStart) ? issue.tEnd : evStart;
    const double pad = std::max(kCloseupPadSec, (evEnd - evStart) * 1.5);

    double winStart = std::max(0.0, evStart - pad);
    double winEnd = std::min(total, evEnd + pad);
    if (winEnd - winStart < kCloseupMinSec) {
        double centre = 0.5 * (winStart + winEnd);
        winStart = std::max(0.0, centre - kCloseupMinSec * 0.5);
        winEnd = std::min(total, winStart + kCloseupMinSec);
        winStart = std::max(0.0, winEnd - kCloseupMinSec);
    }

    std::vector<float> mono;
    downmixMono(buf, mono);

    const std::size_t rs = static_cast<std::size_t>(winStart * buf.sampleRate);
    std::size_t rl = static_cast<std::size_t>((winEnd - winStart) * buf.sampleRate);
    if (rs + rl > n) rl = n - rs;

    const int width = 900, height = 380;
    double minF = 0.0, maxF = 0.0;
    renderRangeRaster(mono, buf.sampleRate, rs, rl, width, height, s, cv.rgba, minF, maxF);
    if (cv.rgba.empty()) return cv;

    cv.width = width;
    cv.height = height;
    cv.minFreq = minF;
    cv.maxFreq = maxF;
    cv.scale = static_cast<int>(s.freqScale);
    cv.winStart = winStart;
    cv.winEnd = winEnd;
    cv.evStart = evStart;
    cv.evEnd = evEnd;
    return cv;
}

}  // namespace argus
