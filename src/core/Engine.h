#pragma once

#include <string>
#include <vector>

#include "Profile.h"
#include "Report.h"
#include "render/Spectrogram.h"

namespace argus {

// Decode a single file and run the full analysis suite (no spectrogram). Never
// throws; decode failures are reported via Report::decodeError.
Report analyzeFile(const std::string& path, const Profile& profile = defaultProfile());

// Decode + analyse + render the spectrogram with the given settings/size. When
// `autoProfile` is true the profile is chosen from the file's detected channel layout
// (after decode) instead of using `profile` — used for per-file auto-selection in the GUI.
Report analyzeFileFull(const std::string& path, int specWidth, int specHeight,
                       const SpectrogramSettings& settings,
                       const Profile& profile = defaultProfile(), bool autoProfile = false);

// Expand a path into the set of analysable audio files (single file, or all
// supported audio files directly inside a directory). Sorted by name.
std::vector<std::string> collectInputs(const std::string& path);

}  // namespace argus
