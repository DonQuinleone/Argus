#include <cmath>
#include <vector>

#include "../Util.h"
#include "Analyses.h"

namespace argus {
namespace {
// Sample is "at full scale" within this margin of +/-1.0.
constexpr float kClipLevel = 0.99970f;
// A run of at least this many consecutive full-scale samples is called clipping
// (isolated single FS peaks are normal).
constexpr int kMinRun = 3;
}  // namespace

void analyzeClipping(const AudioBuffer& buf, std::vector<Issue>& out) {
    long long totalClipped = 0;
    int totalRegions = 0;
    double firstAt = -1.0;
    double worstLongestMs = 0.0;
    double worstAt = -1.0;

    for (int c = 0; c < buf.channels; ++c) {
        const auto& x = buf.data[c];
        const std::size_t n = x.size();
        std::size_t i = 0;
        while (i < n) {
            if (std::fabs(x[i]) < kClipLevel) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < n && std::fabs(x[j]) >= kClipLevel) ++j;
            int run = static_cast<int>(j - i);
            if (run >= kMinRun) {
                totalClipped += run;
                ++totalRegions;
                double t = static_cast<double>(i) / buf.sampleRate;
                double durMs = 1000.0 * run / buf.sampleRate;
                if (firstAt < 0) firstAt = t;
                if (durMs > worstLongestMs) {
                    worstLongestMs = durMs;
                    worstAt = t;
                }
            }
            i = j;
        }
    }

    Issue is;
    is.check = "Clipping";
    if (totalRegions == 0) {
        is.severity = Severity::Pass;
        is.summary = "No on-sample clipping detected";
    } else {
        is.severity = Severity::Warn;
        is.tStart = worstAt;
        is.summary = fmtInt(totalClipped) + " clipped samples in " +
                     fmtInt(totalRegions) + " region(s)";
        is.detail =
            "Consecutive samples are pinned at full scale - the waveform was driven into "
            "the digital ceiling, which can cause audible distortion and will likely clip "
            "again after lossy encoding. Reduce gain / apply a true-peak limiter.";
        is.field("Clipped samples", fmtInt(totalClipped))
            .field("Clipped regions", fmtInt(totalRegions))
            .field("First at", timecode(firstAt))
            .field("Longest run", fmt(worstLongestMs, 1) + " ms @ " + timecode(worstAt));
    }
    out.push_back(std::move(is));
}

}  // namespace argus
