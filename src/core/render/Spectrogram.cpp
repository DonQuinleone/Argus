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

    // Clipping close-ups also carry a waveform overlay so the flat-topped peaks and the
    // clip ceiling are directly visible (a spectrogram alone hides them).
    if (issue.check == "Clipping" && rl > 0) {
        cv.hasWave = true;
        cv.waveMin.assign(width, 0.0f);
        cv.waveMax.assign(width, 0.0f);
        for (int x = 0; x < width; ++x) {
            std::size_t a = rs + static_cast<std::size_t>(rl) * x / width;
            std::size_t b = rs + static_cast<std::size_t>(rl) * (x + 1) / width;
            float lo = 0.0f, hi = 0.0f;
            for (std::size_t i = a; i < b && i < n; ++i) {
                lo = std::min(lo, mono[i]);
                hi = std::max(hi, mono[i]);
            }
            cv.waveMin[x] = lo;
            cv.waveMax[x] = hi;
        }
        const std::size_t es = static_cast<std::size_t>(evStart * buf.sampleRate);
        const std::size_t ee = std::max(es + 1, static_cast<std::size_t>(evEnd * buf.sampleRate));
        float peak = 0.0f;
        for (std::size_t i = es; i < ee && i < n; ++i) peak = std::max(peak, std::fabs(mono[i]));
        cv.clipLevel = peak;
    }
    return cv;
}

void renderChannelSpectrogram(const std::vector<float>& chan, int sampleRate, int width,
                              int height, const SpectrogramSettings& s,
                              std::vector<unsigned char>& rgba) {
    double minF = 0, maxF = 0;
    renderRangeRaster(chan, sampleRate, 0, chan.size(), width, height, s, rgba, minF, maxF);
}

void renderGoniometer(const AudioBuffer& buf, int size, std::vector<unsigned char>& rgba) {
    rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    if (buf.channels < 2) return;
    const auto& L = buf.data[0];
    const auto& R = buf.data[1];
    const std::size_t n = std::min(L.size(), R.size());
    if (n == 0) return;

    std::vector<float> dens(static_cast<std::size_t>(size) * size, 0.0f);
    // Decimate to keep the plot fast on long files.
    const std::size_t step = std::max<std::size_t>(1, n / 400000);
    const float c = 0.70710678f;  // 1/sqrt(2)
    // Auto-gain: scale so the loudest point fills ~92% of the scope (otherwise quiet
    // material is a tiny dot in the centre and tells you nothing).
    float maxR = 1e-6f;
    for (std::size_t i = 0; i < n; i += step) {
        float x = std::fabs(L[i] - R[i]) * c, y = std::fabs(L[i] + R[i]) * c;
        maxR = std::max(maxR, std::max(x, y));
    }
    const float g = 0.92f / maxR;
    for (std::size_t i = 0; i < n; i += step) {
        float x = (L[i] - R[i]) * c * g;  // side -> horizontal
        float y = (L[i] + R[i]) * c * g;  // mid  -> vertical (up = in phase)
        if (x < -1) x = -1; else if (x > 1) x = 1;
        if (y < -1) y = -1; else if (y > 1) y = 1;
        int px = static_cast<int>((x * 0.5f + 0.5f) * (size - 1));
        int py = static_cast<int>((0.5f - y * 0.5f) * (size - 1));
        dens[static_cast<std::size_t>(py) * size + px] += 1.0f;
    }
    float peak = 0.0f;
    for (float d : dens) peak = std::max(peak, d);
    if (peak <= 0) return;
    const float inv = 1.0f / std::log1p(peak);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            std::size_t idx = static_cast<std::size_t>(py) * size + px;
            float t = std::log1p(dens[idx]) * inv;  // 0..1
            unsigned char r, g, b;
            colormap(Colormap::CyanOrange, t, r, g, b);
            unsigned char* p = &rgba[idx * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
    }
    // Faint reference axes (vertical = mono, diagonals = hard L/R).
    auto blend = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= size || py >= size) return;
        unsigned char* p = &rgba[(static_cast<std::size_t>(py) * size + px) * 4];
        for (int k = 0; k < 3; ++k) p[k] = static_cast<unsigned char>(std::min(255, p[k] + 40));
        p[3] = 255;
    };
    for (int i = 0; i < size; ++i) { blend(size / 2, i); blend(i, i); blend(size - 1 - i, i); }
}

void renderDcMeter(const std::vector<float>& dc, int w, int h, std::vector<unsigned char>& rgba) {
    rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    const int n = static_cast<int>(dc.size());
    if (n == 0 || w <= 0 || h <= 0) return;
    auto px = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        unsigned char* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    auto fill = [&](int x0, int x1, int y0, int y1, unsigned char r, unsigned char g, unsigned char b) {
        if (x1 < x0) std::swap(x0, x1);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) px(x, y, r, g, b);
    };
    fill(0, w, 0, h, 18, 18, 22);
    const int cx = w / 2;
    const float scaleMax = 0.005f;   // ±0.005 (~ -46 dBFS) maps to the meter edges
    const float thresh = 0.001f;     // ~ -60 dBFS warn line
    const int rowH = std::max(8, h / n);
    // Per-channel track backgrounds (alternating) so the meter reads as a meter even when
    // every channel is centred (no DC) - common on clean/Atmos masters.
    for (int c = 0; c < n; ++c) {
        int y0 = c * rowH, y1 = (c + 1) * rowH;
        if (c & 1) fill(0, w, y0, y1, 24, 24, 30);
        for (int x = 0; x < w; ++x) px(x, (y0 + y1) / 2, 40, 40, 48);  // baseline
    }
    int tx = static_cast<int>(thresh / scaleMax * (w / 2 - 4));
    for (int y = 0; y < h; ++y) {  // 0 line + amber warn ticks
        px(cx, y, 110, 110, 122);
        px(cx + tx, y, 120, 90, 50);
        px(cx - tx, y, 120, 90, 50);
    }
    for (int c = 0; c < n; ++c) {
        int yc = c * rowH + rowH / 2;
        int y0 = yc - std::max(2, rowH / 2 - 2), y1 = yc + std::max(2, rowH / 2 - 2);
        float v = dc[c] / scaleMax;
        if (v > 1) v = 1; else if (v < -1) v = -1;
        int len = static_cast<int>(v * (w / 2 - 4));
        bool over = std::fabs(dc[c]) > thresh;
        unsigned char r = over ? 230 : 90, g = over ? 90 : 200, b = over ? 80 : 120;
        // Always draw at least a small nub at centre so each channel is visible.
        if (len >= 0) fill(cx, cx + std::max(len, 1), y0, y1, r, g, b);
        else fill(cx + len, cx, y0, y1, r, g, b);
    }
}

void computeCorrelation(const AudioBuffer& buf, std::vector<float>& out, double winSec) {
    out.clear();
    if (buf.channels < 2 || buf.sampleRate <= 0) return;
    const auto& L = buf.data[0];
    const auto& R = buf.data[1];
    const std::size_t n = std::min(L.size(), R.size());
    const std::size_t win = std::max<std::size_t>(1, static_cast<std::size_t>(winSec * buf.sampleRate));
    for (std::size_t s = 0; s + win <= n; s += win) {
        long double sLR = 0, sLL = 0, sRR = 0;
        for (std::size_t i = s; i < s + win; ++i) {
            sLR += static_cast<long double>(L[i]) * R[i];
            sLL += static_cast<long double>(L[i]) * L[i];
            sRR += static_cast<long double>(R[i]) * R[i];
        }
        double denom = std::sqrt(static_cast<double>(sLL) * static_cast<double>(sRR));
        out.push_back(denom > 0 ? static_cast<float>(static_cast<double>(sLR) / denom) : 0.0f);
    }
}

}  // namespace argus
