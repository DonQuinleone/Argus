#include "Profile.h"

#include <algorithm>
#include <cctype>

namespace argus {

const std::vector<Profile>& builtinProfiles() {
    static const std::vector<Profile> profiles = [] {
        std::vector<Profile> p;

        Profile adm;
        adm.name = "Apple Digital Masters";
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
        cd.requiredBitDepth = 16;
        cd.requiredChannels = 2;
        cd.requireLossless = true;
        cd.approvedRates = {44100};
        cd.truePeakCeiling = -0.1;
        cd.enforceTruePeak = true;
        cd.checkLoudnessTarget = false;
        p.push_back(cd);

        Profile permissive;
        permissive.name = "Permissive (no spec)";
        permissive.truePeakCeiling = 0.0;   // report headroom only
        permissive.enforceTruePeak = false;
        permissive.checkLoudnessTarget = false;
        p.push_back(permissive);

        return p;
    }();
    return profiles;
}

const Profile& defaultProfile() { return builtinProfiles()[0]; }

const Profile& profileByName(const std::string& nameOrIndex) {
    const auto& profiles = builtinProfiles();
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
