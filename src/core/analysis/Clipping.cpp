#include <cmath>
#include <vector>

#include "../Util.h"
#include "Analyses.h"

namespace argus {
namespace {
// Sample is "at full scale" within this margin of +/-1.0.
constexpr float kClipLevel = 0.99970f;
// A run of at least this many consecutive samples pinned at a common level is called
// clipping (isolated single peaks are normal).
constexpr int kMinRun = 3;

// ---- Baked-in (sub-full-scale) clipping ----
// When a signal is hard-clipped and then gained down (normalised / bounced / limited
// makeup), the flat-topped waveform survives but no longer touches full scale, so the
// on-sample test above is blind to it. The tell-tale that survives a gain change is the
// pile-up of the loudest peaks at one common amplitude: clipping caps every peak to the
// same ceiling, so many flat-topped runs share the file's maximum sample value.
//
// We re-run the same run-scan but against the file's own ceiling instead of full scale,
// and require several such runs before reporting (a single sustained peak is not
// clipping). This is a confidence-based heuristic, not the zero-false-positive certainty
// of the on-sample test: heavily distorted / saturated material is legitimately
// flat-topped too, so we surface it as WARN/INFO with an explanatory note rather than a
// hard failure.

// Plateau samples must sit within this relative margin of the ceiling to count as pinned.
constexpr float kPinnedMargin = 3.0e-4f;
// Don't bother below this ceiling - too quiet to be a delivered master with baked clipping.
constexpr float kMinCeiling = 0.0316f;  // ~ -30 dBFS
// A flat run's internal sample-to-sample step must not exceed this (a true clip is flat).
constexpr float kFlatStep = 1.0e-5f;
// The flat run must sit within this fraction of the file's ceiling.
constexpr float kNearCeil = 0.03f;
// ...and be bordered by a step at least this large - the steep transition a smooth peak
// lacks. This corner is what separates real clipping from a rounded tonal maximum.
constexpr float kCornerStep = 1.0e-3f;
// Region-count thresholds separating "likely" (WARN) from "possible" (INFO) clipping.
constexpr int kWarnRegions = 6;
constexpr int kInfoRegions = 2;

struct PinnedScan {
    long long pinnedSamples = 0;
    int regions = 0;
    double firstAt = -1.0;
    double worstLongestMs = 0.0;
    double worstAt = -1.0;
};

void recordRegion(PinnedScan& s, std::size_t start, int run, int sampleRate) {
    s.pinnedSamples += run;
    ++s.regions;
    double t = static_cast<double>(start) / sampleRate;
    double durMs = 1000.0 * run / sampleRate;
    if (s.firstAt < 0) s.firstAt = t;
    if (durMs > s.worstLongestMs) {
        s.worstLongestMs = durMs;
        s.worstAt = t;
    }
}

// Count runs of >= kMinRun consecutive samples whose magnitude is pinned within
// kPinnedMargin of `ceiling`, across all channels (the on-sample full-scale test).
PinnedScan scanPinned(const AudioBuffer& buf, float ceiling) {
    PinnedScan s;
    const float thresh = ceiling * (1.0f - kPinnedMargin);
    for (int c = 0; c < buf.channels; ++c) {
        const auto& x = buf.data[c];
        const std::size_t n = x.size();
        std::size_t i = 0;
        while (i < n) {
            if (std::fabs(x[i]) < thresh) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < n && std::fabs(x[j]) >= thresh) ++j;
            int run = static_cast<int>(j - i);
            if (run >= kMinRun) recordRegion(s, i, run, buf.sampleRate);
            i = j;
        }
    }
    return s;
}

// Count flat-topped plateaus near `ceiling`: runs of >= kMinRun consecutive samples that
// are internally flat (steps <= kFlatStep), sit within kNearCeil of the ceiling, and are
// bordered by a step >= kCornerStep. The corner requirement rejects smoothly rounded tonal
// peaks, which are also "near the ceiling" but ramp gently rather than snapping flat.
PinnedScan scanFlatTops(const AudioBuffer& buf, float ceiling) {
    PinnedScan s;
    const float nearThresh = ceiling * (1.0f - kNearCeil);
    for (int c = 0; c < buf.channels; ++c) {
        const auto& x = buf.data[c];
        const std::size_t n = x.size();
        std::size_t i = 0;
        while (i + 1 < n) {
            // Extend a run of internally-flat samples starting at i.
            std::size_t j = i;
            while (j + 1 < n && std::fabs(x[j + 1] - x[j]) <= kFlatStep) ++j;
            int run = static_cast<int>(j - i + 1);
            if (run >= kMinRun) {
                float level = std::fabs(x[i]);
                bool nearCeiling = level >= nearThresh;
                float preStep = i > 0 ? std::fabs(x[i] - x[i - 1]) : 0.0f;
                float postStep = j + 1 < n ? std::fabs(x[j + 1] - x[j]) : 0.0f;
                bool corner = std::max(preStep, postStep) >= kCornerStep;
                if (nearCeiling && corner) recordRegion(s, i, run, buf.sampleRate);
            }
            i = j + 1;
        }
    }
    return s;
}

}  // namespace

void analyzeClipping(const AudioBuffer& buf, std::vector<Issue>& out) {
    // ---- On-sample clipping: runs pinned at full scale ----
    PinnedScan fs = scanPinned(buf, 1.0f);

    Issue is;
    is.check = "Clipping";
    if (fs.regions == 0) {
        is.severity = Severity::Pass;
        is.summary = "No on-sample clipping detected";
    } else {
        is.severity = Severity::Warn;
        is.tStart = fs.worstAt;
        is.summary = fmtInt(fs.pinnedSamples) + " clipped samples in " +
                     fmtInt(fs.regions) + " region(s)";
        is.detail =
            "Consecutive samples are pinned at full scale - the waveform was driven into "
            "the digital ceiling, which can cause audible distortion and will likely clip "
            "again after lossy encoding. Reduce gain / apply a true-peak limiter.";
        is.field("Clipped samples", fmtInt(fs.pinnedSamples))
            .field("Clipped regions", fmtInt(fs.regions))
            .field("First at", timecode(fs.firstAt))
            .field("Longest run", fmt(fs.worstLongestMs, 1) + " ms @ " + timecode(fs.worstAt));
    }
    out.push_back(std::move(is));

    // ---- Baked-in clipping: runs pinned at a sub-full-scale ceiling ----
    // The file's own maximum sample magnitude is the candidate clip ceiling.
    float ceiling = 0.0f;
    for (int c = 0; c < buf.channels; ++c)
        for (float v : buf.data[c]) ceiling = std::max(ceiling, std::fabs(v));

    // Skip when on-sample clipping already covers it (ceiling at full scale), or when the
    // peak level is too low for baked clipping to be a meaningful master defect.
    if (ceiling >= kClipLevel || ceiling < kMinCeiling) return;

    PinnedScan bk = scanFlatTops(buf, ceiling);
    if (bk.regions < kInfoRegions) return;

    Issue ib;
    ib.check = "Clipping";
    ib.severity = bk.regions >= kWarnRegions ? Severity::Warn : Severity::Info;
    ib.tStart = bk.worstAt;
    const double levelDb = toDb(ceiling);
    ib.summary = (ib.severity == Severity::Warn ? "Likely baked-in clipping" : "Possible baked-in clipping") +
                 std::string(" at ") + fmt(levelDb, 1) + " dBFS (" + fmtInt(bk.regions) +
                 " flat-topped region(s))";
    ib.detail =
        "Many of the loudest peaks are flat-topped at a common level below full scale - the "
        "signature of audio that was hard-clipped and then gained down (normalised, limited or "
        "re-bounced). The distortion is baked into the signal even though it no longer touches "
        "the digital ceiling. Heavily distorted or saturated material can read the same way, so "
        "audition before acting; if it is unintended, re-render from a source that was never "
        "clipped.";
    ib.field("Estimated clip level", fmt(levelDb, 1) + " dBFS")
        .field("Flat-topped samples", fmtInt(bk.pinnedSamples))
        .field("Flat-topped regions", fmtInt(bk.regions))
        .field("First at", timecode(bk.firstAt))
        .field("Longest run", fmt(bk.worstLongestMs, 1) + " ms @ " + timecode(bk.worstAt));
    out.push_back(std::move(ib));
}

}  // namespace argus
