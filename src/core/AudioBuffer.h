#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace argus {

// Deinterleaved float32 audio. data[channel][frame], samples normalised to [-1, 1].
struct AudioBuffer {
    int sampleRate = 0;
    int channels = 0;
    std::vector<std::vector<float>> data;

    std::size_t frames() const { return data.empty() ? 0 : data[0].size(); }
    double durationSec() const {
        return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0;
    }
    bool empty() const { return frames() == 0; }
};

// Broadcast Wave (BWF) bext-chunk metadata plus any embedded iXML. All optional; a
// field is simply empty when the file carries no such data. `present` is true when the
// file had a Broadcast Audio Extension (bext) chunk at all.
struct BroadcastInfo {
    bool present = false;
    std::string description;          // free-text description
    std::string originator;           // producing studio / device
    std::string originatorReference;  // unique source reference
    std::string originationDate;      // "yyyy-mm-dd"
    std::string originationTime;      // "hh:mm:ss"
    std::uint64_t timeReference = 0;  // first-sample timecode, in samples since midnight
    int version = 0;                  // bext version
    std::string umid;                 // SMPTE UMID (hex), if present
    std::string codingHistory;        // free-text coding history
    std::string ixml;                 // raw iXML document, if present (best-effort)
};

// Descriptive tags read from RIFF LIST/INFO and/or an embedded ID3v2 chunk, plus any
// embedded cover artwork. All optional; empty when the file carries no such data.
struct TagInfo {
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string trackNo;
    std::string date;
    std::vector<unsigned char> artwork;  // raw image bytes (JPEG/PNG) as embedded
    std::string artworkMime;             // e.g. "image/jpeg"

    // Every tag found, in file order (INFO + ID3), e.g. {"Composer","..."},
    // {"ISRC","..."}, {"Engineer","..."}. The named fields above are convenience
    // duplicates of the common ones.
    std::vector<std::pair<std::string, std::string>> allTags;

    bool hasText() const {
        return !title.empty() || !artist.empty() || !album.empty() || !genre.empty() ||
               !trackNo.empty() || !date.empty() || !allTags.empty();
    }
    bool any() const { return hasText() || !artwork.empty(); }
};

// Loudspeaker layout family inferred from channel count, the WAVE channel mask, and/or
// embedded ADM (Dolby Atmos) metadata. Drives which checks are layout-appropriate.
enum class LayoutFamily { Unknown, Mono, Stereo, Surround, Atmos };

// Dolby Atmos / ADM (Audio Definition Model) information parsed from the axml/chna/dbmd
// chunks of an ADM BWF. Empty/false when the file is not ADM.
struct AdmInfo {
    bool present = false;             // axml + chna present
    bool hasDbmd = false;             // Dolby bitstream metadata chunk present
    std::string programme;            // audioProgramme name (e.g. "Atmos_Master")
    std::vector<std::string> objects; // audioObject names (bed + objects)
    std::string bedLayout;            // e.g. "7.1.6", when derivable
};

// Technical metadata describing the file as it sits on disk.
struct FileMetadata {
    std::string path;
    std::string filename;
    std::string container;   // "WAV", "AIFF", "FLAC", ...
    std::string codec;       // "PCM signed 24-bit", "FLAC", "MPEG-1 Layer III", ...
    int bitDepth = 0;        // bits per sample; 0 if not meaningful (compressed)
    int sampleRate = 0;      // Hz
    int channels = 0;
    std::uint64_t frames = 0;
    double durationSec = 0.0;
    bool lossless = true;
    std::uint64_t fileBytes = 0;
    BroadcastInfo broadcast;  // BWF / iXML, when present
    TagInfo tags;             // RIFF INFO / ID3 tags + cover art, when present

    // Channel layout.
    std::uint32_t channelMask = 0;          // WAVE_FORMAT_EXTENSIBLE mask (0 if none)
    LayoutFamily layoutFamily = LayoutFamily::Unknown;
    std::string layoutName;                 // human label, e.g. "7.1.6 (Dolby Atmos)"
    std::vector<std::string> channelRoles;  // per-channel role label ("L","R","LFE",...)
    AdmInfo adm;                            // Dolby Atmos / ADM, when present
};

}  // namespace argus
