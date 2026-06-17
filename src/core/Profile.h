// A delivery profile parameterises the spec-driven checks (container/format rules
// and the true-peak / loudness targets) so Argus is not hard-wired to one delivery
// standard. The defect detectors (dropouts, clicks, silence, phase, spectral) are
// signal-quality checks and are not affected by the profile.
#pragma once

#include <string>
#include <vector>

namespace argus {

struct Profile {
    std::string name;

    // Container / format rules. A zero / empty value means "do not enforce".
    int requiredBitDepth = 0;          // e.g. 24
    int requiredChannels = 0;          // e.g. 2 (stereo)
    bool requireLossless = false;
    std::vector<int> approvedRates;    // empty = any sample rate allowed

    // True-peak ceiling (dBTP). Always reported; a value of 0 disables the
    // ceiling comparison and reports headroom informationally.
    double truePeakCeiling = -1.0;
    bool enforceTruePeak = true;

    // Optional integrated-loudness target check.
    bool checkLoudnessTarget = false;
    double lufsTarget = -14.0;
    double lufsTolerance = 1.0;

    // True when this profile enforces at least one container/format rule.
    bool hasFormatRules() const {
        return requiredBitDepth != 0 || requiredChannels != 0 || requireLossless ||
               !approvedRates.empty();
    }
};

// The built-in profiles, in display order. Index 0 is the default.
const std::vector<Profile>& builtinProfiles();

// Default profile (Apple Digital Masters), used when none is specified.
const Profile& defaultProfile();

// Look up a profile by case-insensitive name or by numeric index (as a string).
// Returns the default profile when no match is found.
const Profile& profileByName(const std::string& nameOrIndex);

}  // namespace argus
