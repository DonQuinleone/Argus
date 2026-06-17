#include <algorithm>
#include <cmath>
#include <vector>

#include "../Util.h"
#include "Analyses.h"

namespace argus {
namespace {
// A click is a sample that departs sharply from where its neighbours predict it
// should be (interpolation residual), far above the *local residual* level (i.e. the
// usual sample-to-sample roughness of the surrounding audio), and very localised.
// Tuned conservatively so clean masters stay clean.
constexpr double kRatio = 10.0;      // residual must exceed this * local residual RMS
constexpr float kAbsFloor = 0.01f;   // and this absolute residual (~ -40 dBFS)
constexpr double kMergeMs = 5.0;     // merge detections closer than this into one event
constexpr int kMaxReport = 12;       // cap listed locations
}  // namespace

void analyzeClicks(const AudioBuffer& buf, std::vector<Issue>& out) {
    const std::size_t n = buf.frames();
    const double sr = buf.sampleRate;
    if (n < 4 || sr <= 0) return;

    const int win = std::max(8, static_cast<int>(sr * 0.003));  // ~3 ms local scale
    const int half = win / 2;

    std::vector<double> allTimes;             // raw detections across all channels

    std::vector<float> resid(n, 0.0f);        // interpolation residual per sample
    std::vector<double> prefix(n + 1, 0.0);   // prefix sum of resid^2 for O(1) local scale

    for (int c = 0; c < buf.channels; ++c) {
        const auto& x = buf.data[c];
        for (std::size_t i = 1; i + 1 < n; ++i)
            resid[i] = static_cast<float>(std::fabs(x[i] - 0.5 * (x[i - 1] + x[i + 1])));
        resid[0] = resid[n - 1] = 0.0f;
        prefix[0] = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            prefix[i + 1] = prefix[i] + static_cast<double>(resid[i]) * resid[i];

        for (std::size_t i = 1; i + 1 < n; ++i) {
            std::size_t lo = (i > static_cast<std::size_t>(half)) ? i - half : 0;
            std::size_t hi = std::min(n, i + half);
            // Local residual scale, excluding the candidate's own energy so a big
            // click doesn't inflate its own threshold.
            double energy = prefix[hi] - prefix[lo] - static_cast<double>(resid[i]) * resid[i];
            double cnt = std::max<std::size_t>(1, hi - lo - 1);
            double local = std::sqrt(energy / cnt);

            double r = resid[i];
            if (r < kAbsFloor || r <= kRatio * local) continue;

            // Require an isolated spike: bigger than the neighbouring residuals.
            double rPrev = resid[i - 1];
            double rNext = resid[i + 1];
            if (r < rPrev || r < rNext) continue;

            allTimes.push_back(static_cast<double>(i) / sr);
        }
    }

    // Merge detections (across channels) that fall within kMergeMs into single events.
    std::sort(allTimes.begin(), allTimes.end());
    std::vector<double> clickTimes;
    long long total = 0;
    double lastT = -1e9;
    for (double t : allTimes) {
        if (t - lastT > kMergeMs * 0.001) {
            ++total;
            if (static_cast<int>(clickTimes.size()) < kMaxReport) clickTimes.push_back(t);
            lastT = t;
        }
    }

    Issue is;
    is.check = "Clicks";
    if (total == 0) {
        is.severity = Severity::Pass;
        is.summary = "No clicks or impulsive glitches detected";
    } else {
        is.severity = Severity::Warn;
        is.summary = fmtInt(total) + " click(s) / impulsive glitch(es) detected";
        is.detail = "Isolated samples jump far from their neighbours - audible as clicks or "
                    "ticks. Often edit points or digital corruption; de-click or repair.";
        if (!clickTimes.empty()) is.tStart = clickTimes.front();
        std::string locs;
        for (std::size_t i = 0; i < clickTimes.size(); ++i) {
            if (i) locs += ", ";
            locs += timecode(clickTimes[i]);
        }
        is.field("Count", fmtInt(total))
            .field("First " + fmtInt(static_cast<long long>(clickTimes.size())) + " at", locs);
    }
    out.push_back(std::move(is));
}

}  // namespace argus
