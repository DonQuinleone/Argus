#pragma once

#include <vector>

#include "../AudioBuffer.h"
#include "../Issue.h"
#include "../Profile.h"

// Each analysis appends its findings to `out`. Engine runs them in order.
// Analyses are pure functions of the decoded audio + metadata so the engine
// can run headless (CLI / CI) with no UI dependency. The spec-driven checks
// (metadata, loudness) also take the active delivery profile.
namespace argus {

void analyzeMetadata(const FileMetadata& meta, const Profile& profile, std::vector<Issue>& out);
void analyzeClipping(const AudioBuffer& buf, std::vector<Issue>& out);
void analyzeDropouts(const AudioBuffer& buf, std::vector<Issue>& out);
void analyzeClicks(const AudioBuffer& buf, std::vector<Issue>& out);
void analyzeSilence(const AudioBuffer& buf, std::vector<Issue>& out);
void analyzePhase(const AudioBuffer& buf, std::vector<Issue>& out);
void analyzeLoudness(const AudioBuffer& buf, const Profile& profile, std::vector<Issue>& out);
void analyzeSpectral(const AudioBuffer& buf, std::vector<Issue>& out);

}  // namespace argus
