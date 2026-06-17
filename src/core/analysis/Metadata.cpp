#include <algorithm>

#include "../Util.h"
#include "Analyses.h"

namespace argus {

// Container / format compliance, driven by the active delivery profile. (The
// defaults reimplement adm_preflight.sh: LPCM, 24-bit, stereo, approved rates.)
namespace {
std::string humanBytes(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    return fmt(v, v < 10 ? 2 : 1) + " " + units[u];
}
}  // namespace

void analyzeMetadata(const FileMetadata& m, const Profile& profile, std::vector<Issue>& out) {
    // Always emit the technical metadata as an INFO row.
    Issue info;
    info.check = "File";
    info.severity = Severity::Info;
    info.summary = m.container + " / " + m.codec + ", " +
                   fmt(m.sampleRate / 1000.0, 1) + " kHz, " +
                   (m.layoutName.empty() ? fmtInt(m.channels) + " ch" : m.layoutName);
    info.field("Filename", m.filename)
        .field("Container", m.container)
        .field("Codec", m.codec)
        .field("Bit depth", m.bitDepth > 0 ? fmtInt(m.bitDepth) + "-bit" : "n/a (compressed)")
        .field("Sample rate", fmtInt(m.sampleRate) + " Hz")
        .field("Channels", fmtInt(m.channels) + (m.layoutName.empty() ? "" : " (" + m.layoutName + ")"))
        .field("Frames", fmtInt(static_cast<long long>(m.frames)))
        .field("Duration", timecode(m.durationSec))
        .field("File size", humanBytes(m.fileBytes))
        .field("Lossless", m.lossless ? "yes" : "no");
    if (m.adm.present || m.adm.hasDbmd) {
        info.field("ADM / Atmos", m.adm.hasDbmd ? "yes (Dolby metadata present)" : "yes");
        if (!m.adm.programme.empty()) info.field("ADM programme", m.adm.programme);
        if (!m.adm.objects.empty())
            info.field("ADM objects", fmtInt(static_cast<long long>(m.adm.objects.size())));
    }
    out.push_back(std::move(info));

    // Container / format compliance against the active profile.
    Issue adm;
    adm.check = "Format compliance";
    adm.field("Profile", profile.name);

    if (!profile.hasFormatRules()) {
        // Permissive profile: report identity but enforce nothing.
        adm.severity = Severity::Info;
        adm.summary = "No container/format spec enforced (" + profile.name + ")";
        adm.detail = "This profile does not impose bit-depth, channel, sample-rate or "
                     "lossless requirements.";
        out.push_back(std::move(adm));
        return;
    }

    const bool layoutOk = layoutMatches(profile.targetLayout, m.layoutFamily);

    std::vector<std::string> violations;
    if (profile.requireLossless && !m.lossless)
        violations.push_back("not lossless PCM (" + m.codec + ")");
    if (profile.requiredBitDepth && m.bitDepth != profile.requiredBitDepth)
        violations.push_back("bit depth is " +
                             (m.bitDepth ? fmtInt(m.bitDepth) + "-bit" : std::string("unknown")) +
                             " (requires " + fmtInt(profile.requiredBitDepth) + "-bit)");
    // Exact channel count is only meaningful within the same layout family (e.g. a mono
    // vs stereo deliverable). A layout mismatch (Atmos vs a stereo profile) is handled
    // separately below and must never hard-FAIL on channel count.
    if (profile.requiredChannels && layoutOk && m.channels != profile.requiredChannels)
        violations.push_back("has " + fmtInt(m.channels) + " channel(s) (requires " +
                             fmtInt(profile.requiredChannels) + ")");
    if (!profile.approvedRates.empty()) {
        bool rateOk = std::find(profile.approvedRates.begin(), profile.approvedRates.end(),
                                m.sampleRate) != profile.approvedRates.end();
        if (!rateOk)
            violations.push_back("sample rate " + fmtInt(m.sampleRate) +
                                 " Hz is not an approved rate");
    }

    std::string suggested = suggestedProfileName(m, profile);
    std::string layoutNote;
    if (!layoutOk)
        layoutNote = "File layout is " + (m.layoutName.empty() ? "non-stereo" : m.layoutName) +
                     ", but " + profile.name + " targets a " +
                     (profile.targetLayout == TargetLayout::Stereo ? "stereo"
                      : profile.targetLayout == TargetLayout::Surround ? "surround"
                      : profile.targetLayout == TargetLayout::Atmos ? "Dolby Atmos"
                                                                    : "any") +
                     " deliverable" +
                     (suggested.empty() ? "" : " - switch to the \"" + suggested + "\" profile");

    if (!violations.empty()) {
        // Real format problems (bit depth / rate / lossless) regardless of layout.
        adm.severity = Severity::Fail;
        adm.summary = "Does not meet " + profile.name + " requirements";
        std::string d = "Problems: ";
        for (std::size_t i = 0; i < violations.size(); ++i) {
            d += violations[i];
            if (i + 1 < violations.size()) d += "; ";
        }
        adm.detail = d + (layoutNote.empty() ? "." : ". " + layoutNote + ".");
        for (std::size_t i = 0; i < violations.size(); ++i)
            adm.field("Violation " + fmtInt(static_cast<long long>(i + 1)), violations[i]);
        if (!suggested.empty()) adm.field("Suggested profile", suggested);
    } else if (!layoutOk) {
        // Wrong profile for this layout - inform and suggest, do not FAIL.
        adm.severity = Severity::Info;
        adm.summary = "Layout (" + (m.layoutName.empty() ? "multichannel" : m.layoutName) +
                      ") does not match the " + profile.name + " profile";
        adm.detail = layoutNote + ". Bit depth, sample rate and lossless are within spec.";
        if (!suggested.empty()) adm.field("Suggested profile", suggested);
    } else {
        adm.severity = Severity::Pass;
        adm.summary = "Meets " + profile.name + " core file requirements";
        adm.detail = "Container, bit depth, channels and sample rate all within spec.";
    }
    out.push_back(std::move(adm));
}

}  // namespace argus
