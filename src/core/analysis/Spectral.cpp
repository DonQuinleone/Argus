#include <algorithm>
#include <cmath>
#include <vector>

#include "../Util.h"
#include "../dsp/Envelope.h"
#include "../dsp/Fft.h"
#include "Analyses.h"

namespace argus {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kFftSize = 4096;
constexpr int kMaxWindows = 3000;       // cap for speed on long files
constexpr double kContentDb = -85.0;    // "content extends to" reporting threshold
constexpr double kCliffDropDb = 35.0;   // drop within the probe band that counts as a brickwall
constexpr double kProbeKHz = 1.5;       // width of the cliff probe band
}  // namespace

void analyzeSpectral(const AudioBuffer& buf, std::vector<Issue>& out) {
    const std::size_t n = buf.frames();
    if (buf.sampleRate <= 0 || n < static_cast<std::size_t>(kFftSize)) return;

    std::vector<float> mono;
    downmixMono(buf, mono);

    std::vector<float> hann(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        hann[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / (kFftSize - 1)));

    Fft fft(kFftSize);
    const std::size_t bins = fft.bins();
    std::vector<double> power(bins, 0.0);
    std::vector<float> frame(kFftSize), mag;

    const std::size_t nWin = (n - kFftSize) / kFftSize + 1;
    const std::size_t stride = std::max<std::size_t>(1, nWin / kMaxWindows);
    std::size_t used = 0;
    for (std::size_t w = 0; w * kFftSize < n - kFftSize + 1; w += stride) {
        std::size_t start = w * kFftSize;
        for (int i = 0; i < kFftSize; ++i) frame[i] = mono[start + i] * hann[i];
        fft.magnitude(frame.data(), mag);
        for (std::size_t b = 0; b < bins; ++b) power[b] += static_cast<double>(mag[b]) * mag[b];
        ++used;
    }
    if (used == 0) return;

    // Smoothed spectrum, relative to peak.
    std::vector<double> db(bins);
    double peak = -300.0;
    for (std::size_t b = 0; b < bins; ++b) {
        double acc = 0;
        int cnt = 0;
        for (int k = -2; k <= 2; ++k) {
            long bb = static_cast<long>(b) + k;
            if (bb >= 0 && bb < static_cast<long>(bins)) { acc += power[bb]; ++cnt; }
        }
        db[b] = 10.0 * std::log10(acc / cnt / used + 1e-12);
        peak = std::max(peak, db[b]);
    }
    for (auto& d : db) d -= peak;

    const double nyquist = buf.sampleRate / 2.0;
    const double hzPerBin = nyquist / (bins - 1);
    auto dbAtKHz = [&](double khz) {
        long b = std::lround(khz * 1000.0 / hzPerBin);
        b = std::min<long>(static_cast<long>(bins) - 1, std::max<long>(0, b));
        return db[b];
    };

    // Content extent (deepest reporting threshold).
    double contentKHz = 0.0;
    for (std::size_t b = bins; b-- > 0;)
        if (db[b] > kContentDb) { contentKHz = b * hzPerBin / 1000.0; break; }

    // Brickwall cliff: largest drop across a narrow probe band in the upper spectrum.
    double maxDrop = 0.0, cliffKHz = 0.0;
    for (double f = 5.0; f + kProbeKHz < nyquist / 1000.0; f += 0.25) {
        double drop = dbAtKHz(f) - dbAtKHz(f + kProbeKHz);
        if (drop > maxDrop) { maxDrop = drop; cliffKHz = f; }
    }
    bool brickwall = maxDrop >= kCliffDropDb && cliffKHz < nyquist / 1000.0 * 0.97;

    Issue is;
    is.check = "Spectral bandwidth";
    is.field("Content extends to", fmt(contentKHz, 1) + " kHz")
        .field("Nyquist", fmt(nyquist / 1000.0, 1) + " kHz")
        .field("Steepest rolloff", fmt(maxDrop, 0) + " dB / " + fmt(kProbeKHz, 1) +
                                        " kHz @ " + fmt(cliffKHz, 1) + " kHz");

    if (brickwall && buf.sampleRate >= 88200 && cliffKHz < 24.5) {
        is.severity = Severity::Warn;
        is.summary = "Hard cutoff at ~" + fmt(cliffKHz, 1) +
                     " kHz - likely upsampled, not true hi-res";
        is.detail = "The spectrum drops off a cliff well below Nyquist. Genuine high-rate "
                    "captures carry content/noise smoothly above 24 kHz; this looks upsampled "
                    "from 44.1/48 kHz (or from a lossy source).";
    } else if (brickwall && cliffKHz < 20.5) {
        is.severity = Severity::Warn;
        is.summary = "Hard cutoff at ~" + fmt(cliffKHz, 1) +
                     " kHz - possible lossy (MP3/AAC) source";
        is.detail = "Energy stops abruptly below Nyquist (a brickwall), the hallmark of a file "
                    "that passed through a lossy codec.";
    } else {
        is.severity = Severity::Pass;
        is.summary = "Full-bandwidth spectrum (gradual rolloff, content to ~" +
                     fmt(contentKHz, 1) + " kHz)";
    }
    out.push_back(std::move(is));
}

}  // namespace argus
