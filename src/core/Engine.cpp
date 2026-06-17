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
                       const SpectrogramSettings& settings, const Profile& profile) {
    Report rep;
    DecodeResult dec = decodeFile(path);
    rep.meta = dec.meta;
    if (!dec.ok) {
        rep.decodeError = dec.error;
        return rep;
    }
    runAnalyses(dec.buffer, rep, profile);
    renderSpectrogram(dec.buffer, rep, specWidth, specHeight, settings);

    // Per-finding close-ups, rendered while the decoded buffer is still alive so the
    // UI dropdown and the PDF can both show a zoomed view of each defect.
    for (std::size_t i = 0; i < rep.issues.size(); ++i) {
        const Issue& is = rep.issues[i];
        if (!is.localised() || is.severity < Severity::Warn) continue;
        CloseupView cv = renderCloseup(dec.buffer, is, settings);
        if (cv.valid()) {
            cv.issueIndex = static_cast<int>(i);
            rep.closeups.push_back(std::move(cv));
        }
    }

    // Downsampled peak overview for the waveform plot.
    const std::size_t pts = 2000;
    const std::size_t n = dec.buffer.frames();
    rep.overview.assign(pts, 0.0f);
    if (n > 0) {
        for (std::size_t p = 0; p < pts; ++p) {
            std::size_t a = n * p / pts, b = n * (p + 1) / pts;
            float pk = 0.0f;
            for (std::size_t i = a; i < b; ++i)
                for (int c = 0; c < dec.buffer.channels; ++c)
                    pk = std::max(pk, std::fabs(dec.buffer.data[c][i]));
            rep.overview[p] = pk;
        }
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
