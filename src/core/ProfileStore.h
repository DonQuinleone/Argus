// Persistence for user-defined delivery profiles. Custom profiles are stored in the
// per-user config directory so both the GUI and the headless CLI can use them. The
// built-in profiles (Profile.cpp) are never written to disk.
#pragma once

#include <string>
#include <vector>

#include "Profile.h"

namespace argus {

// Absolute path to the custom-profiles file (creates the config directory).
std::string customProfilesPath();

// Load user-defined profiles from disk (empty if none / unreadable).
std::vector<Profile> loadCustomProfiles();

// Persist the given custom profiles, replacing the file. Returns success.
bool saveCustomProfiles(const std::vector<Profile>& profiles);

}  // namespace argus
