#include "Engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>

#include "Decoder.h"
#include "analysis/Analyses.h"
#include "render/Spectrogram.h"

namespace argus {
namespace {

bool isSupportedAudio(const std::filesystem::path& p) {
    static const std::set<std::string> exts = {
        ".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".caf", ".w64", ".rf64",
        ".ogg", ".oga", ".opus", ".mp3", ".m4a", ".mp4", ".aac", ".m4b"};
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return exts.count(e) > 0;
}

void runAnalyses(const AudioBuffer& buf, Report& rep, const Profile& profile) {
    // Order defines report layout: identity, then levels, then anomalies, then spectrum.
    analyzeMetadata(rep.meta, profile, rep.issues);
    analyzeLoudness(buf, profile, rep.issues);
    analyzeClipping(buf, rep.issues);
    analyzeDropouts(buf, rep.issues);
    analyzeClicks(buf, rep.issues);
    analyzeSilence(buf, rep.issues);
    analyzePhase(buf, rep.issues);
    analyzeSpectral(buf, rep.issues);

    // Suggest a better-fitting profile when the file's layout doesn't match the active one.
    rep.suggestedProfile = suggestedProfileName(rep.meta, profile);
    rep.usedProfileName = profile.name;
}

}  // namespace

Report analyzeFile(const std::string& path, const Profile& profile) {
    Report rep;
    DecodeResult dec = decodeFile(path);
    rep.meta = dec.meta;
    if (!dec.ok) {
        rep.decodeError = dec.error;
        return rep;
    }
    runAnalyses(dec.buffer, rep, profile);
    return rep;
}

Report analyzeFileFull(const std::string& path, int specWidth, int specHeight,
                       const SpectrogramSettings& settings, const Profile& profile,
                       bool autoProfile) {
    Report rep;
    DecodeResult dec = decodeFile(path);
    rep.meta = dec.meta;
    if (!dec.ok) {
        rep.decodeError = dec.error;
        return rep;
    }
    // Per-file auto-selection: choose the profile from the detected layout once decoded.
    const Profile& used = autoProfile ? profileForLayout(dec.meta.layoutFamily) : profile;
    runAnalyses(dec.buffer, rep, used);
    renderSpectrogram(dec.buffer, rep, specWidth, specHeight, settings);

    // Per-finding close-ups, rendered while the decoded buffer is still alive so the UI
    // dropdown and the PDF can both show a zoomed view. Rendered for every localised
    // finding (not just Warn/Fail) so the report's "include all diagrams" option can use them.
    for (std::size_t i = 0; i < rep.issues.size(); ++i) {
        const Issue& is = rep.issues[i];
        if (!is.localised()) continue;
        CloseupView cv = renderCloseup(dec.buffer, is, settings);
        if (cv.valid()) {
            cv.issueIndex = static_cast<int>(i);
            rep.closeups.push_back(std::move(cv));
        }
    }

    // Downsampled peak overview for the waveform plot (max across channels), plus a
    // per-channel overview for the multitrack Channels view.
    const std::size_t pts = 2000;
    const std::size_t n = dec.buffer.frames();
    const int ch = dec.buffer.channels;
    rep.overview.assign(pts, 0.0f);
    rep.channelOverviews.assign(ch, std::vector<float>(pts, 0.0f));
    if (n > 0) {
        for (std::size_t p = 0; p < pts; ++p) {
            std::size_t a = n * p / pts, b = n * (p + 1) / pts;
            float pkAll = 0.0f;
            for (int c = 0; c < ch; ++c) {
                float pk = 0.0f;
                for (std::size_t i = a; i < b; ++i) pk = std::max(pk, std::fabs(dec.buffer.data[c][i]));
                rep.channelOverviews[c][p] = pk;
                pkAll = std::max(pkAll, pk);
            }
            rep.overview[p] = pkAll;
        }
    }

    // Per-channel spectrogram strips for the Channels view (skip absurd channel counts).
    if (ch > 0 && ch <= 32) {
        rep.chanSpecW = 760;
        rep.chanSpecH = 64;
        rep.channelSpecs.resize(ch);
        for (int c = 0; c < ch; ++c)
            renderChannelSpectrogram(dec.buffer.data[c], dec.buffer.sampleRate, rep.chanSpecW,
                                     rep.chanSpecH, settings, rep.channelSpecs[c]);
    }

    // Stereo phase visuals (goniometer + correlation timeline). Uses the front pair.
    if (ch >= 2) {
        renderGoniometer(dec.buffer, 320, rep.goniRGBA);
        rep.goniW = rep.goniH = 320;
        computeCorrelation(dec.buffer, rep.correlation);
    }

    // Per-channel DC-offset bar meter.
    if (ch > 0 && n > 0) {
        std::vector<float> dc(ch, 0.0f);
        for (int c = 0; c < ch; ++c) {
            double sum = 0;
            for (float v : dec.buffer.data[c]) sum += v;
            dc[c] = static_cast<float>(sum / n);
        }
        rep.dcMeterW = 360;
        rep.dcMeterH = std::max(40, ch * 16);
        renderDcMeter(dc, rep.dcMeterW, rep.dcMeterH, rep.dcMeterRGBA);
    }
    return rep;
}

std::vector<std::string> collectInputs(const std::string& path) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;

    if (fs::is_directory(path, ec)) {
        // Recurse into subfolders so nested delivery trees are scanned in full.
        fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied,
                                            ec),
            end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && isSupportedAudio(it->path()))
                out.push_back(it->path().string());
        }
        std::sort(out.begin(), out.end());
    } else if (fs::is_regular_file(path, ec)) {
        out.push_back(path);
    }
    return out;
}

}  // namespace argus
