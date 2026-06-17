#include "Profile.h"

#include <algorithm>
#include <cctype>

#include "ProfileStore.h"

namespace argus {

const std::vector<Profile>& builtinProfiles() {
    static const std::vector<Profile> profiles = [] {
        std::vector<Profile> p;

        Profile adm;
        adm.name = "Apple Digital Masters";
        adm.targetLayout = TargetLayout::Stereo;
        adm.requiredBitDepth = 24;
        adm.requiredChannels = 2;
        adm.requireLossless = true;
        adm.approvedRates = {44100, 48000, 88200, 96000, 176400, 192000};
        adm.truePeakCeiling = -1.0;
        adm.enforceTruePeak = true;
        adm.checkLoudnessTarget = false;
        p.push_back(adm);

        Profile stream;
        stream.name = "Streaming (-14 LUFS / -1 dBTP)";
        stream.targetLayout = TargetLayout::Stereo;
        stream.requiredBitDepth = 0;   // 16- or 24-bit both accepted
        stream.requiredChannels = 2;
        stream.requireLossless = false;
        stream.approvedRates = {44100, 48000, 88200, 96000};
        stream.truePeakCeiling = -1.0;
        stream.enforceTruePeak = true;
        stream.checkLoudnessTarget = true;
        stream.lufsTarget = -14.0;
        stream.lufsTolerance = 1.0;
        p.push_back(stream);

        Profile cd;
        cd.name = "CD master (16-bit / 44.1)";
        cd.targetLayout = TargetLayout::Stereo;
        cd.requiredBitDepth = 16;
        cd.requiredChannels = 2;
        cd.requireLossless = true;
        cd.approvedRates = {44100};
        cd.truePeakCeiling = -0.1;
        cd.enforceTruePeak = true;
        cd.checkLoudnessTarget = false;
        p.push_back(cd);

        Profile surround;
        surround.name = "5.1 / Surround (24-bit)";
        surround.targetLayout = TargetLayout::Surround;
        surround.requiredBitDepth = 24;
        surround.requireLossless = true;
        surround.approvedRates = {48000, 96000};
        surround.truePeakCeiling = -1.0;
        surround.enforceTruePeak = true;
        surround.checkLoudnessTarget = false;
        p.push_back(surround);

        Profile atmos;
        atmos.name = "Dolby Atmos (ADM BWF)";
        atmos.targetLayout = TargetLayout::Atmos;
        atmos.requiredBitDepth = 24;
        atmos.requireLossless = true;
        atmos.approvedRates = {48000, 96000};
        atmos.truePeakCeiling = -1.0;
        atmos.enforceTruePeak = true;
        atmos.checkLoudnessTarget = false;
        p.push_back(atmos);

        Profile permissive;
        permissive.name = "Permissive (no spec)";
        permissive.targetLayout = TargetLayout::Any;
        permissive.truePeakCeiling = 0.0;   // report headroom only
        permissive.enforceTruePeak = false;
        permissive.checkLoudnessTarget = false;
        p.push_back(permissive);

        return p;
    }();
    return profiles;
}

bool layoutMatches(TargetLayout t, LayoutFamily f) {
    switch (t) {
        case TargetLayout::Any:      return true;
        case TargetLayout::Stereo:   return f == LayoutFamily::Stereo || f == LayoutFamily::Mono;
        case TargetLayout::Surround: return f == LayoutFamily::Surround;
        case TargetLayout::Atmos:    return f == LayoutFamily::Atmos;
    }
    return false;
}

const Profile& profileForLayout(LayoutFamily family) {
    const auto& ps = builtinProfiles();
    const char* want = nullptr;
    if (family == LayoutFamily::Atmos) want = "Dolby Atmos";
    else if (family == LayoutFamily::Surround) want = "Surround";
    if (want)
        for (const auto& p : ps)
            if (p.name.find(want) != std::string::npos) return p;
    return defaultProfile();
}

std::string suggestedProfileName(const FileMetadata& m, const Profile& active) {
    if (layoutMatches(active.targetLayout, m.layoutFamily)) return "";
    const char* want = nullptr;
    switch (m.layoutFamily) {
        case LayoutFamily::Atmos:    want = "Dolby Atmos"; break;
        case LayoutFamily::Surround: want = "Surround"; break;
        case LayoutFamily::Stereo:
        case LayoutFamily::Mono:     want = "Apple Digital Masters"; break;
        default:                     return "";
    }
    for (const auto& p : builtinProfiles())
        if (p.name.find(want) != std::string::npos) return p.name;
    return "";
}

std::vector<Profile> allProfiles() {
    std::vector<Profile> all = builtinProfiles();
    std::vector<Profile> custom = loadCustomProfiles();
    all.insert(all.end(), custom.begin(), custom.end());
    return all;
}

const Profile& defaultProfile() { return builtinProfiles()[0]; }

Profile profileByName(const std::string& nameOrIndex) {
    const std::vector<Profile> profiles = allProfiles();
    // Numeric index?
    if (!nameOrIndex.empty() &&
        std::all_of(nameOrIndex.begin(), nameOrIndex.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
        int idx = std::stoi(nameOrIndex);
        if (idx >= 0 && idx < static_cast<int>(profiles.size())) return profiles[idx];
        return defaultProfile();
    }
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string want = lower(nameOrIndex);
    for (const auto& p : profiles)
        if (lower(p.name).find(want) != std::string::npos) return p;
    return defaultProfile();
}

}  // namespace argus
