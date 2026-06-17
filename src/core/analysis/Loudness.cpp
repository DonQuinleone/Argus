#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "../Util.h"
#include "Analyses.h"

namespace argus {
namespace {

struct Target {
    const char* name;
    double lufs;
};
// Reference integrated-loudness targets DSPs normalise to. Informational: most
// platforms turn *down* louder masters, so being above target is not an error, but
// being far off is worth knowing. True-peak ceiling is the hard delivery rule.
constexpr Target kTargets[] = {
    {"Apple Music", -16.0}, {"Spotify", -14.0}, {"YouTube", -14.0},
    {"Amazon Music", -14.0}, {"Tidal", -14.0},
};

inline double lin2db(double v) { return v <= 1e-12 ? -200.0 : 20.0 * std::log10(v); }

}  // namespace

void analyzeLoudness(const AudioBuffer& buf, const Profile& profile, std::vector<Issue>& out) {
    const int ch = buf.channels;
    const std::size_t n = buf.frames();
    if (ch <= 0 || n == 0) return;

    ebur128_state* st = ebur128_init(
        static_cast<unsigned>(ch), static_cast<unsigned long>(buf.sampleRate),
        EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_SAMPLE_PEAK);
    if (!st) return;

    // Feed interleaved float blocks.
    const std::size_t kBlock = 1 << 15;
    std::vector<float> inter(kBlock * ch);
    for (std::size_t start = 0; start < n; start += kBlock) {
        std::size_t cnt = std::min(kBlock, n - start);
        for (std::size_t i = 0; i < cnt; ++i)
            for (int c = 0; c < ch; ++c)
                inter[i * ch + c] = buf.data[c][start + i];
        ebur128_add_frames_float(st, inter.data(), cnt);
    }

    double lufs = -200.0, lra = 0.0;
    ebur128_loudness_global(st, &lufs);
    ebur128_loudness_range(st, &lra);

    double truePeak = 0.0, samplePeak = 0.0;
    for (int c = 0; c < ch; ++c) {
        double tp = 0.0, sp = 0.0;
        if (ebur128_true_peak(st, c, &tp) == EBUR128_SUCCESS) truePeak = std::max(truePeak, tp);
        if (ebur128_sample_peak(st, c, &sp) == EBUR128_SUCCESS) samplePeak = std::max(samplePeak, sp);
    }
    ebur128_destroy(&st);

    const double tpDb = lin2db(truePeak);
    const double spDb = lin2db(samplePeak);
    const bool lufsValid = lufs > -70.0;  // -inf / very low => essentially silent

    // --- Loudness (informational) ---
    {
        Issue is;
        is.check = "Loudness";
        is.severity = Severity::Info;
        is.summary = lufsValid ? (fmt(lufs, 1) + " LUFS integrated, LRA " + fmt(lra, 1) + " LU")
                               : "Programme too quiet to measure integrated loudness";
        is.field("Integrated loudness", lufsValid ? fmt(lufs, 1) + " LUFS" : "n/a")
            .field("Loudness range (LRA)", fmt(lra, 1) + " LU")
            .field("Sample peak", fmt(spDb, 1) + " dBFS");
        if (lufsValid)
            for (const auto& t : kTargets) {
                double delta = lufs - t.lufs;
                is.field(t.name + std::string(" (") + fmt(t.lufs, 0) + " LUFS)",
                         (delta >= 0 ? "+" : "") + fmt(delta, 1) + " LU vs target");
            }
        // Profile loudness target (optional, enforced as a soft warning).
        if (profile.checkLoudnessTarget && lufsValid) {
            double delta = lufs - profile.lufsTarget;
            is.field("Profile target", fmt(profile.lufsTarget, 1) + " LUFS  (" +
                                           (delta >= 0 ? "+" : "") + fmt(delta, 1) + " LU)");
            if (std::fabs(delta) > profile.lufsTolerance) {
                is.severity = Severity::Warn;
                is.summary = fmt(lufs, 1) + " LUFS integrated (" + (delta >= 0 ? "+" : "") +
                             fmt(delta, 1) + " LU vs " + fmt(profile.lufsTarget, 1) +
                             " LUFS target)";
            }
        }
        out.push_back(std::move(is));
    }

    // --- True peak (hard delivery rule) ---
    {
        const double ceil = profile.truePeakCeiling;
        Issue is;
        is.check = "True peak";
        is.field("True peak", fmt(tpDb, 2) + " dBTP");
        if (profile.enforceTruePeak)
            is.field("Ceiling", fmt(ceil, 1) + " dBTP").field("Headroom", fmt(ceil - tpDb, 2) + " dB");
        if (tpDb > 0.0) {
            // Actual full-scale overs are a real defect regardless of profile.
            is.severity = Severity::Fail;
            is.summary = "True-peak overs (" + fmt(tpDb, 2) + " dBTP, above 0 dBFS)";
            is.detail = "Inter-sample peaks exceed full scale and will clip on reconstruction "
                        "and after lossy encoding. Apply a true-peak limiter.";
        } else if (profile.enforceTruePeak && tpDb > ceil) {
            is.severity = Severity::Warn;
            is.summary = "True peak above " + fmt(ceil, 1) + " dBTP ceiling (" + fmt(tpDb, 2) +
                         " dBTP)";
            is.detail = "Within full scale but above the delivery ceiling; lossy encoders may "
                        "introduce clipping. (Replaces the AAC round-trip check as the modern "
                        "predictor of encoder clipping.)";
        } else {
            is.severity = Severity::Pass;
            is.summary = profile.enforceTruePeak
                             ? "True peak within ceiling (" + fmt(tpDb, 2) + " dBTP)"
                             : "True peak " + fmt(tpDb, 2) + " dBTP";
        }
        out.push_back(std::move(is));
    }
}

}  // namespace argus
