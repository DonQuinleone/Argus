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
constexpr int kMaxList = 60;         // cap timecodes listed in the report text
constexpr std::size_t kMaxMarks = 2000;  // cap spectrogram markers
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
    std::vector<double> clickTimes;  // every distinct click
    double lastT = -1e9;
    for (double t : allTimes) {
        if (t - lastT > kMergeMs * 0.001) {
            clickTimes.push_back(t);
            lastT = t;
        }
    }
    const long long total = static_cast<long long>(clickTimes.size());

    Issue is;
    is.check = "Clicks";
    if (total == 0) {
        is.severity = Severity::Pass;
        is.summary = "No clicks or impulsive glitches detected";
        out.push_back(std::move(is));
        return;
    }

    // Periodicity: a recurring digital fault clicks at a near-constant interval. Measure
    // the coefficient of variation of the gaps; low CV with enough clicks => periodic.
    bool periodic = false;
    double meanGap = 0.0;
    if (total >= 4) {
        std::vector<double> gaps;
        for (std::size_t i = 1; i < clickTimes.size(); ++i)
            gaps.push_back(clickTimes[i] - clickTimes[i - 1]);
        double sum = 0;
        for (double g : gaps) sum += g;
        meanGap = sum / gaps.size();
        double var = 0;
        for (double g : gaps) var += (g - meanGap) * (g - meanGap);
        double sd = std::sqrt(var / gaps.size());
        if (meanGap > 0 && sd / meanGap < 0.15) periodic = true;
    }

    is.severity = Severity::Warn;
    is.tStart = clickTimes.front();
    is.marks = clickTimes;
    if (is.marks.size() > kMaxMarks) is.marks.resize(kMaxMarks);

    if (periodic) {
        double hz = meanGap > 0 ? 1.0 / meanGap : 0.0;
        is.summary = fmtInt(total) + " periodic clicks (every " + fmt(meanGap * 1000.0, 1) +
                     " ms, ~" + fmt(hz, 1) + " Hz)";
        is.detail = "Regularly-spaced clicks - the signature of a recurring digital fault (clock/"
                    "buffer/sync), not a one-off edit. Investigate the source chain.";
        is.field("Spacing", fmt(meanGap * 1000.0, 1) + " ms (~" + fmt(hz, 1) + " Hz)");
    } else {
        is.summary = fmtInt(total) +
                     (total == 1 ? " isolated click / impulsive glitch detected"
                                 : " click(s) / impulsive glitch(es) detected");
        is.detail = "Isolated samples jump far from their neighbours - audible as clicks or "
                    "ticks. Often edit points or digital corruption; de-click or repair.";
    }
    is.field("Count", fmtInt(total));
    std::string locs;
    int listed = std::min<int>(kMaxList, static_cast<int>(clickTimes.size()));
    for (int i = 0; i < listed; ++i) {
        if (i) locs += ", ";
        locs += timecode(clickTimes[i]);
    }
    if (listed < total) locs += ", … (+" + fmtInt(total - listed) + " more)";
    is.field("Locations", locs);
    out.push_back(std::move(is));
}

}  // namespace argus
