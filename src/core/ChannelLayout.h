// Loudspeaker-layout classification: turns a channel count, an optional
// WAVE_FORMAT_EXTENSIBLE channel mask, and any parsed ADM (Dolby Atmos) metadata into a
// layout family, a human label, and per-channel role labels. Used to keep the
// stereo-oriented checks (channel balance, mono/phase) from mis-firing on surround/Atmos.
#pragma once

#include "AudioBuffer.h"

namespace argus {

// Populate m.layoutFamily, m.layoutName and m.channelRoles from m.channels, m.channelMask
// and m.adm. Safe to call on any file.
void classifyLayout(FileMetadata& m);

}  // namespace argus
