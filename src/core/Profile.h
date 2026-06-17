// A delivery profile parameterises the spec-driven checks (container/format rules
// and the true-peak / loudness targets) so Argus is not hard-wired to one delivery
// standard. The defect detectors (dropouts, clicks, silence, phase, spectral) are
// signal-quality checks and are not affected by the profile.
#pragma once

#include <string>
#include <vector>

#include "AudioBuffer.h"  // LayoutFamily

namespace argus {

// The loudspeaker layout a profile targets. `Any` skips the layout check entirely.
enum class TargetLayout { Any, Stereo, Surround, Atmos };

struct Profile {
    std::string name;

    // The delivery's expected channel layout. Stereo profiles target Stereo; the
    // surround/Atmos profiles target Surround/Atmos. Replaces the old hard channel count.
    TargetLayout targetLayout = TargetLayout::Stereo;

    // Container / format rules. A zero / empty value means "do not enforce".
    int requiredBitDepth = 0;          // e.g. 24
    int requiredChannels = 0;          // e.g. 2 (stereo) - legacy; layout check preferred
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

// Built-in profiles followed by the user's saved custom profiles (loaded from disk).
// This is the order presented in the UI and used for numeric --profile indices.
std::vector<Profile> allProfiles();

// Default profile (Apple Digital Masters), used when none is specified.
const Profile& defaultProfile();

// Look up a profile by case-insensitive name or by numeric index (as a string), across
// both built-in and custom profiles. Returns the default profile when no match is found.
Profile profileByName(const std::string& nameOrIndex);

// True if a file of layout `f` satisfies a profile targeting `t`.
bool layoutMatches(TargetLayout t, LayoutFamily f);

// The built-in profile whose layout best fits this file, when the active profile's layout
// does not match the file's. Empty string when the active profile already fits.
std::string suggestedProfileName(const FileMetadata& m, const Profile& active);

// The built-in profile that best matches a layout family (for per-file auto-pick):
// Atmos -> Atmos profile, Surround -> 5.1, Stereo/Mono/Unknown -> the default.
const Profile& profileForLayout(LayoutFamily family);

}  // namespace argus
