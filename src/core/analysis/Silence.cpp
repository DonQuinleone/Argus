#include <cmath>
#include <vector>

#include "../Util.h"
#include "Analyses.h"

namespace argus {
namespace {
constexpr float kSilenceLin = 1e-4f;     // ~ -80 dBFS counts as silence
constexpr double kEmbeddedMinMs = 10.0;  // min length of an embedded digital-zero gap
constexpr double kDcWarnDb = -60.0;      // |DC| above this is flagged
constexpr double kDeadChanDb = -80.0;    // channel quieter than this = effectively dead
}  // namespace

void analyzeSilence(const AudioBuffer& buf, std::vector<Issue>& out) {
    const std::size_t n = buf.frames();
    const int ch = buf.channels;
    const double sr = buf.sampleRate;
    if (n == 0 || ch <= 0) return;

    // --- Per-channel DC offset and RMS ---
    std::vector<double> rms(ch, 0.0), dc(ch, 0.0);
    for (int c = 0; c < ch; ++c) {
        double sum = 0, sumSq = 0;
        for (float v : buf.data[c]) {
            sum += v;
            sumSq += static_cast<double>(v) * v;
        }
        dc[c] = sum / n;
        rms[c] = std::sqrt(sumSq / n);
    }

    // --- DC offset ---
    {
        double worst = 0;
        int worstC = 0;
        for (int c = 0; c < ch; ++c)
            if (std::fabs(dc[c]) > std::fabs(worst)) { worst = dc[c]; worstC = c; }
        double worstDb = toDb(std::fabs(worst));
        Issue is;
        is.check = "DC offset";
        if (worstDb > kDcWarnDb) {
            is.severity = Severity::Warn;
            is.summary = "DC offset present (" + fmt(worstDb, 1) + " dBFS)";
            is.detail = "A constant bias is offsetting the waveform from zero. It wastes "
                        "headroom and can cause clicks at edits; high-pass or DC-block to fix.";
        } else {
            is.severity = Severity::Pass;
            is.summary = "Negligible DC offset";
        }
        for (int c = 0; c < ch; ++c)
            is.field("DC ch " + fmtInt(c), fmt(dc[c], 6) + " (" + fmtDb(toDb(std::fabs(dc[c]))) + ")");
        (void)worstC;
        out.push_back(std::move(is));
    }

    // --- Channel balance / dead channel (stereo+) ---
    if (ch >= 2) {
        double maxDb = -300, minDb = 300;
        int deadCount = 0;
        for (int c = 0; c < ch; ++c) {
            double d = toDb(rms[c]);
            maxDb = std::max(maxDb, d);
            minDb = std::min(minDb, d);
            if (d < kDeadChanDb) ++deadCount;
        }
        double spread = maxDb - minDb;
        Issue is;
        is.check = "Channel balance";
        if (deadCount > 0 && maxDb > kDeadChanDb) {
            is.severity = Severity::Fail;
            is.summary = "A channel is silent while another carries audio";
            is.detail = "One channel is effectively dead - the file may be mono mistakenly "
                        "delivered as stereo, or a channel was lost.";
        } else if (spread > 6.0) {
            is.severity = Severity::Warn;
            is.summary = "Strong channel level imbalance (" + fmt(spread, 1) + " dB)";
            is.detail = "The channels differ substantially in level; confirm this is intended.";
        } else {
            is.severity = Severity::Pass;
            is.summary = "Channels reasonably balanced (" + fmt(spread, 1) + " dB spread)";
        }
        for (int c = 0; c < ch; ++c)
            is.field("RMS ch " + fmtInt(c), fmtDb(toDb(rms[c])));
        out.push_back(std::move(is));
    }

    // --- Leading / trailing silence ---
    auto isSilentFrame = [&](std::size_t i) {
        for (int c = 0; c < ch; ++c)
            if (std::fabs(buf.data[c][i]) > kSilenceLin) return false;
        return true;
    };
    std::size_t firstSound = 0;
    while (firstSound < n && isSilentFrame(firstSound)) ++firstSound;
    if (firstSound >= n) {
        Issue is;
        is.check = "Silence";
        is.severity = Severity::Fail;
        is.summary = "File is entirely silent";
        out.push_back(std::move(is));
        return;
    }
    std::size_t lastSound = n - 1;
    while (lastSound > firstSound && isSilentFrame(lastSound)) --lastSound;

    double lead = firstSound / sr;
    double trail = (n - 1 - lastSound) / sr;
    {
        Issue is;
        is.check = "Head/tail silence";
        is.severity = Severity::Info;
        is.summary = "Lead-in " + fmt(lead, 3) + " s, lead-out " + fmt(trail, 3) + " s";
        is.field("Leading silence", fmt(lead, 3) + " s")
            .field("Trailing silence", fmt(trail, 3) + " s")
            .field("First audio at", timecode(lead))
            .field("Last audio at", timecode(lastSound / sr));
        out.push_back(std::move(is));
    }

    // --- Embedded exact-zero gaps (hard digital dropouts) within the content ---
    {
        const std::size_t minRun =
            static_cast<std::size_t>(kEmbeddedMinMs * 0.001 * sr);
        int gaps = 0;
        double firstGap = -1, longestMs = 0, longestAt = -1;
        std::size_t i = firstSound;
        while (i <= lastSound) {
            bool zero = true;
            for (int c = 0; c < ch; ++c)
                if (buf.data[c][i] != 0.0f) { zero = false; break; }
            if (!zero) { ++i; continue; }
            std::size_t j = i;
            while (j <= lastSound) {
                bool z = true;
                for (int c = 0; c < ch; ++c)
                    if (buf.data[c][j] != 0.0f) { z = false; break; }
                if (!z) break;
                ++j;
            }
            if (j - i >= minRun) {
                ++gaps;
                double t = i / sr, durMs = 1000.0 * (j - i) / sr;
                if (firstGap < 0) firstGap = t;
                if (durMs > longestMs) { longestMs = durMs; longestAt = t; }
            }
            i = j + 1;
        }
        if (gaps > 0) {
            Issue is;
            is.check = "Digital silence gap";
            is.severity = Severity::Fail;
            is.tStart = longestAt;
            is.summary = fmtInt(gaps) + " embedded digital-silence gap(s) - hard dropout";
            is.detail = "Stretches of exact-zero samples appear inside the programme. This is "
                        "a hard dropout (missing audio), not a musical rest.";
            is.field("Gaps", fmtInt(gaps))
                .field("First at", timecode(firstGap))
                .field("Longest", fmt(longestMs, 1) + " ms @ " + timecode(longestAt));
            out.push_back(std::move(is));
        }
    }
}

}  // namespace argus
