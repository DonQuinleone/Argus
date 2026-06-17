#include "Envelope.h"

#include <algorithm>
#include <cmath>

namespace argus {

void downmixMono(const AudioBuffer& buf, std::vector<float>& out) {
    const std::size_t n = buf.frames();
    out.assign(n, 0.0f);
    if (buf.channels <= 0) return;
    const float scale = 1.0f / buf.channels;
    for (int c = 0; c < buf.channels; ++c) {
        const auto& ch = buf.data[c];
        for (std::size_t i = 0; i < n; ++i) out[i] += ch[i] * scale;
    }
}

Envelope computeEnvelope(const AudioBuffer& buf, double winMs, double hopMs) {
    std::vector<float> mono;
    downmixMono(buf, mono);
    return computeEnvelopeMono(mono, buf.sampleRate, winMs, hopMs);
}

void highpass(std::vector<float>& x, int sampleRate, double cutoffHz) {
    if (sampleRate <= 0 || x.empty()) return;
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * cutoffHz / sampleRate;
    const double cosw = std::cos(w0), sinw = std::sin(w0);
    const double Q = 0.70710678;
    const double alpha = sinw / (2.0 * Q);
    double b0 = (1.0 + cosw) / 2.0;
    double b1 = -(1.0 + cosw);
    double b2 = (1.0 + cosw) / 2.0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosw;
    double a2 = 1.0 - alpha;
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;

    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (auto& s : x) {
        double in = s;
        double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in;
        y2 = y1; y1 = out;
        s = static_cast<float>(out);
    }
}

Envelope computeEnvelopeMono(const std::vector<float>& mono, int sampleRate, double winMs,
                             double hopMs) {
    Envelope env;
    env.sampleRate = sampleRate;
    if (sampleRate <= 0 || mono.empty()) return env;

    env.win = std::max(1, static_cast<int>(std::lround(winMs * 0.001 * sampleRate)));
    env.hop = std::max(1, static_cast<int>(std::lround(hopMs * 0.001 * sampleRate)));

    const std::size_t n = mono.size();

    const std::size_t nFrames = (n >= static_cast<std::size_t>(env.win))
                                    ? (n - env.win) / env.hop + 1
                                    : 1;
    env.rms.reserve(nFrames);
    env.peak.reserve(nFrames);

    for (std::size_t start = 0; start + 0 < n; start += env.hop) {
        std::size_t end = std::min(n, start + static_cast<std::size_t>(env.win));
        double sumSq = 0.0;
        float pk = 0.0f;
        for (std::size_t i = start; i < end; ++i) {
            float a = std::fabs(mono[i]);
            sumSq += static_cast<double>(mono[i]) * mono[i];
            if (a > pk) pk = a;
        }
        std::size_t len = end - start;
        env.rms.push_back(len ? static_cast<float>(std::sqrt(sumSq / len)) : 0.0f);
        env.peak.push_back(pk);
        if (end >= n) break;
    }
    return env;
}

}  // namespace argus
