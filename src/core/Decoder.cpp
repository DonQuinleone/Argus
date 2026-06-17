#include "Decoder.h"

#include <sndfile.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "ChannelLayout.h"
#include "decode/PlatformDecode.h"

namespace argus {
namespace {

// Trim a fixed-size, possibly non-null-terminated char field to a std::string,
// dropping trailing NULs and whitespace (bext fields are space/zero padded).
std::string fixedStr(const char* p, std::size_t n) {
    std::size_t len = 0;
    while (len < n && p[len] != '\0') ++len;
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\r' || p[len - 1] == '\n')) --len;
    return std::string(p, len);
}

// Read the Broadcast Audio Extension (bext) chunk via libsndfile, if present.
void extractBroadcast(SNDFILE* snd, BroadcastInfo& bi) {
    SF_BROADCAST_INFO info;
    std::memset(&info, 0, sizeof(info));
    if (sf_command(snd, SFC_GET_BROADCAST_INFO, &info, sizeof(info)) != SF_TRUE) return;

    bi.present = true;
    bi.description = fixedStr(info.description, sizeof(info.description));
    bi.originator = fixedStr(info.originator, sizeof(info.originator));
    bi.originatorReference = fixedStr(info.originator_reference, sizeof(info.originator_reference));
    bi.originationDate = fixedStr(info.origination_date, sizeof(info.origination_date));
    bi.originationTime = fixedStr(info.origination_time, sizeof(info.origination_time));
    bi.timeReference = (static_cast<std::uint64_t>(info.time_reference_high) << 32) |
                       static_cast<std::uint32_t>(info.time_reference_low);
    bi.version = info.version;

    // UMID is 64 raw bytes; surface it as hex only when it carries data.
    bool umidSet = false;
    for (char c : info.umid)
        if (c != '\0') { umidSet = true; break; }
    if (umidSet) {
        static const char* hex = "0123456789ABCDEF";
        std::string s;
        s.reserve(sizeof(info.umid) * 2);
        for (unsigned char c : info.umid) {
            s += hex[c >> 4];
            s += hex[c & 0x0F];
        }
        bi.umid = s;
    }

    std::size_t chLen = std::min<std::size_t>(info.coding_history_size, sizeof(info.coding_history));
    bi.codingHistory = fixedStr(info.coding_history, chLen);
}

// Best-effort iXML extraction: scan a RIFF (WAV/RF64) container for an "iXML" chunk and
// return its raw payload. libsndfile has no first-class iXML API, so we parse the chunk
// table directly. Returns "" for non-RIFF files or when no iXML chunk is present.
std::string readIxmlChunk(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    char riff[4];
    if (!f.read(riff, 4)) return "";
    bool isRiff = std::memcmp(riff, "RIFF", 4) == 0 || std::memcmp(riff, "RF64", 4) == 0;
    if (!isRiff) return "";
    f.seekg(8, std::ios::beg);  // skip RIFF size + "WAVE"
    char form[4];
    if (!f.read(form, 4) || std::memcmp(form, "WAVE", 4) != 0) return "";

    auto readU32 = [&](std::uint32_t& v) -> bool {
        unsigned char b[4];
        if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
        v = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        return true;
    };

    char id[4];
    std::uint32_t size = 0;
    while (f.read(id, 4) && readU32(size)) {
        if (std::memcmp(id, "iXML", 4) == 0) {
            std::string data(size, '\0');
            if (!f.read(data.data(), size)) return "";
            // Trim trailing padding/NULs from the XML payload.
            while (!data.empty() && (data.back() == '\0' || data.back() == ' ')) data.pop_back();
            return data;
        }
        f.seekg(size + (size & 1), std::ios::cur);  // chunks are word-aligned
    }
    return "";
}

std::string majorFormatName(int format) {
    switch (format & SF_FORMAT_TYPEMASK) {
        case SF_FORMAT_WAV:   return "WAV";
        case SF_FORMAT_WAVEX: return "WAVE (extensible)";
        case SF_FORMAT_AIFF:  return "AIFF";
        case SF_FORMAT_FLAC:  return "FLAC";
        case SF_FORMAT_CAF:   return "CAF";
        case SF_FORMAT_OGG:   return "Ogg";
        case SF_FORMAT_W64:   return "Wave64";
        case SF_FORMAT_RF64:  return "RF64";
        case SF_FORMAT_MPEG:  return "MPEG";
        default:              return "Unknown";
    }
}

// Returns codec description + nominal bit depth + lossless flag.
struct SubtypeInfo {
    std::string codec;
    int bitDepth = 0;
    bool lossless = true;
};

SubtypeInfo subtypeInfo(int format) {
    switch (format & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_S8: return {"PCM signed 8-bit", 8, true};
        case SF_FORMAT_PCM_U8: return {"PCM unsigned 8-bit", 8, true};
        case SF_FORMAT_PCM_16: return {"PCM signed 16-bit", 16, true};
        case SF_FORMAT_PCM_24: return {"PCM signed 24-bit", 24, true};
        case SF_FORMAT_PCM_32: return {"PCM signed 32-bit", 32, true};
        case SF_FORMAT_FLOAT:  return {"IEEE float 32-bit", 32, true};
        case SF_FORMAT_DOUBLE: return {"IEEE float 64-bit", 64, true};
        case SF_FORMAT_VORBIS: return {"Ogg Vorbis", 0, false};
        case SF_FORMAT_OPUS:   return {"Opus", 0, false};
        case SF_FORMAT_MPEG_LAYER_I:   return {"MPEG Layer I", 0, false};
        case SF_FORMAT_MPEG_LAYER_II:  return {"MPEG Layer II", 0, false};
        case SF_FORMAT_MPEG_LAYER_III: return {"MPEG Layer III (MP3)", 0, false};
        default:               return {"Unknown subtype", 0, true};
    }
}

// ---- Descriptive tags (RIFF LIST/INFO) + embedded ID3 (text + cover art) ----

std::string latin1ToUtf8(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += static_cast<char>(c);
        else { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return o;
}

void appendUtf8(std::string& o, unsigned int cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string utf16ToUtf8(const unsigned char* p, std::size_t n, bool be) {
    std::string o;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        unsigned int u = be ? (p[i] << 8) | p[i + 1] : (p[i + 1] << 8) | p[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {  // surrogate pair
            unsigned int lo = be ? (p[i + 2] << 8) | p[i + 3] : (p[i + 3] << 8) | p[i + 2];
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        appendUtf8(o, u);
    }
    return o;
}

// Decode an ID3v2 text-frame body (leading byte = encoding) to UTF-8, trimming NULs.
std::string decodeId3Text(const unsigned char* body, std::size_t len) {
    if (len == 0) return "";
    unsigned char enc = body[0];
    const unsigned char* d = body + 1;
    std::size_t n = len - 1;
    std::string out;
    if (enc == 1) {  // UTF-16 with BOM
        bool be = n >= 2 && d[0] == 0xFE && d[1] == 0xFF;
        bool le = n >= 2 && d[0] == 0xFF && d[1] == 0xFE;
        if (be || le) { d += 2; n -= 2; }
        out = utf16ToUtf8(d, n, be);
    } else if (enc == 2) {  // UTF-16BE, no BOM
        out = utf16ToUtf8(d, n, true);
    } else if (enc == 3) {  // UTF-8
        out.assign(reinterpret_cast<const char*>(d), n);
    } else {  // 0 = ISO-8859-1
        out = latin1ToUtf8(std::string(reinterpret_cast<const char*>(d), n));
    }
    while (!out.empty() && (out.back() == '\0' || out.back() == '\r' || out.back() == '\n'))
        out.pop_back();
    return out;
}

// Append a tag, skipping a duplicate of an already-present name (case-insensitive). A file
// with both RIFF INFO and an ID3 chunk otherwise lists Title/Artist/etc. twice.
void addUniqueTag(TagInfo& t, const std::string& name, const std::string& val) {
    if (name.empty() || val.empty()) return;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string ln = lower(name);
    for (const auto& kv : t.allTags)
        if (lower(kv.first) == ln) return;
    t.allTags.emplace_back(name, val);
}

// Friendly label for an ID3 frame id (v2.2/2.3/2.4), or the raw id if unknown.
std::string id3Name(const std::string& id) {
    if (id == "TIT2" || id == "TT2") return "Title";
    if (id == "TPE1" || id == "TP1") return "Artist";
    if (id == "TPE2" || id == "TP2") return "Album artist";
    if (id == "TALB" || id == "TAL") return "Album";
    if (id == "TCON" || id == "TCO") return "Genre";
    if (id == "TRCK" || id == "TRK") return "Track";
    if (id == "TPOS" || id == "TPA") return "Disc";
    if (id == "TYER" || id == "TYE" || id == "TDRC") return "Year";
    if (id == "TCOM" || id == "TCM") return "Composer";
    if (id == "TSRC" || id == "TRC") return "ISRC";
    if (id == "TPUB" || id == "TPB") return "Publisher";
    if (id == "TCOP" || id == "TCR") return "Copyright";
    if (id == "TENC" || id == "TEN") return "Encoded by";
    if (id == "TBPM" || id == "TBP") return "BPM";
    return id;
}

// Parse an ID3v2 tag: fill any still-empty text tags and extract the first cover image.
void parseId3(const std::vector<unsigned char>& b, TagInfo& t) {
    if (b.size() < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return;
    int ver = b[3];
    auto synch = [](const unsigned char* p) {
        return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3];
    };
    std::size_t pos = 10;
    std::size_t end = std::min(b.size(), 10 + static_cast<std::size_t>(synch(&b[6])));
    const int idLen = (ver == 2) ? 3 : 4;
    const int hdrLen = (ver == 2) ? 6 : 10;
    auto setIfEmpty = [](std::string& dst, const std::string& v) {
        if (dst.empty() && !v.empty()) dst = v;
    };
    while (pos + hdrLen <= end) {
        std::string id(reinterpret_cast<const char*>(&b[pos]), idLen);
        if (id[0] == '\0') break;
        std::size_t fsz;
        if (ver == 2)
            fsz = (b[pos + 3] << 16) | (b[pos + 4] << 8) | b[pos + 5];
        else if (ver == 4)
            fsz = synch(&b[pos + 4]);
        else
            fsz = (b[pos + 4] << 24) | (b[pos + 5] << 16) | (b[pos + 6] << 8) | b[pos + 7];
        std::size_t bodyPos = pos + hdrLen;
        if (bodyPos + fsz > b.size()) break;
        const unsigned char* body = &b[bodyPos];

        if (id == "APIC" || id == "PIC") {
            if (t.artwork.empty() && fsz > 4) {
                // enc(1) | mime(\0-terminated; "PIC" uses a 3-char type) | pictype(1) |
                // desc(\0, in enc) | image bytes.
                std::size_t k = 1;
                std::string mime;
                if (id == "PIC") {  // v2.2: 3-byte image format ("JPG"/"PNG")
                    std::string fmt(reinterpret_cast<const char*>(body + 1), 3);
                    mime = fmt == "PNG" ? "image/png" : "image/jpeg";
                    k = 4;
                } else {
                    while (k < fsz && body[k] != '\0') ++k;
                    mime.assign(reinterpret_cast<const char*>(body + 1), k - 1);
                    ++k;  // skip mime NUL
                }
                if (k < fsz) ++k;  // picture type byte
                // Skip description (terminator depends on text encoding of the frame).
                unsigned char enc = body[0];
                if (enc == 1 || enc == 2) {
                    while (k + 1 < fsz && !(body[k] == 0 && body[k + 1] == 0)) k += 2;
                    k += 2;
                } else {
                    while (k < fsz && body[k] != '\0') ++k;
                    ++k;
                }
                if (k < fsz) {
                    t.artwork.assign(body + k, body + fsz);
                    t.artworkMime = mime.empty() ? "image/jpeg" : mime;
                }
            }
        } else if (!id.empty() && id[0] == 'T') {
            std::string name, v;
            if (id == "TXXX" || id == "TXX") {
                // enc | description (the key) \0 | value, both in the frame's encoding.
                std::string both = decodeId3Text(body, fsz);
                std::size_t nul = both.find('\0');
                if (nul != std::string::npos) {
                    name = both.substr(0, nul);
                    v = both.substr(nul + 1);
                } else {
                    name = "TXXX";
                    v = both;
                }
                // A UTF-16 value carries its own BOM after the description separator.
                auto stripBom = [](std::string& s) {
                    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
                        static_cast<unsigned char>(s[1]) == 0xBB &&
                        static_cast<unsigned char>(s[2]) == 0xBF)
                        s.erase(0, 3);
                };
                stripBom(name);
                stripBom(v);
            } else {
                v = decodeId3Text(body, fsz);
                name = id3Name(id);
            }
            if (!v.empty()) {
                if (id == "TIT2" || id == "TT2") setIfEmpty(t.title, v);
                else if (id == "TPE1" || id == "TP1") setIfEmpty(t.artist, v);
                else if (id == "TALB" || id == "TAL") setIfEmpty(t.album, v);
                else if (id == "TCON" || id == "TCO") setIfEmpty(t.genre, v);
                else if (id == "TRCK" || id == "TRK") setIfEmpty(t.trackNo, v);
                else if (id == "TYER" || id == "TYE" || id == "TDRC") setIfEmpty(t.date, v);
                addUniqueTag(t, name, v);
            }
        }
        pos = bodyPos + fsz;
    }
}

// Walk a RIFF (WAV/RF64) container for LIST/INFO tags and an embedded id3 chunk.
void readRiffTags(const std::string& path, TagInfo& t) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    char riff[4];
    if (!f.read(riff, 4)) return;
    if (std::memcmp(riff, "RIFF", 4) != 0 && std::memcmp(riff, "RF64", 4) != 0) return;
    f.seekg(8, std::ios::beg);
    char form[4];
    if (!f.read(form, 4) || std::memcmp(form, "WAVE", 4) != 0) return;

    auto rd32 = [&](std::uint32_t& v) {
        unsigned char b[4];
        if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
        v = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        return true;
    };
    auto infoName = [](const std::string& id) -> std::string {
        if (id == "INAM") return "Title";
        if (id == "IART") return "Artist";
        if (id == "IPRD") return "Album";
        if (id == "IGNR") return "Genre";
        if (id == "ITRK" || id == "IPRT") return "Track";
        if (id == "ICRD") return "Date";
        if (id == "ICMT") return "Comment";
        if (id == "ICOP") return "Copyright";
        if (id == "IENG") return "Engineer";
        if (id == "ISFT") return "Software";
        if (id == "ISRC") return "Source";
        return id;
    };
    auto setTag = [&](const std::string& id, const std::string& v) {
        if (v.empty()) return;
        if (id == "INAM") t.title = v;
        else if (id == "IART") t.artist = v;
        else if (id == "IPRD") t.album = v;
        else if (id == "IGNR") t.genre = v;
        else if (id == "ITRK" || id == "IPRT") t.trackNo = v;
        else if (id == "ICRD") t.date = v;
        addUniqueTag(t, infoName(id), v);
    };

    char id[4];
    std::uint32_t size = 0;
    while (f.read(id, 4) && rd32(size)) {
        std::streampos next = f.tellg() + static_cast<std::streamoff>(size + (size & 1));
        if (std::memcmp(id, "LIST", 4) == 0) {
            char lt[4];
            if (f.read(lt, 4) && std::memcmp(lt, "INFO", 4) == 0) {
                std::uint32_t rem = size >= 4 ? size - 4 : 0;
                while (rem >= 8) {
                    char sid[4];
                    std::uint32_t ssz;
                    if (!f.read(sid, 4) || !rd32(ssz)) break;
                    std::string val(ssz, '\0');
                    if (ssz && !f.read(val.data(), ssz)) break;
                    if (!val.empty() && val.back() == '\0') val.pop_back();
                    setTag(std::string(sid, 4), latin1ToUtf8(val.c_str()));
                    std::uint32_t adv = ssz + (ssz & 1);
                    if (adv & 1) f.get();
                    rem -= std::min<std::uint32_t>(rem, 8 + adv);
                }
            }
        } else if (std::memcmp(id, "id3 ", 4) == 0 || std::memcmp(id, "ID3 ", 4) == 0) {
            std::vector<unsigned char> buf(size);
            if (size && f.read(reinterpret_cast<char*>(buf.data()), size)) parseId3(buf, t);
        }
        f.seekg(next, std::ios::beg);
    }
}

// Extract the first value of an XML attribute (attr="value") at/after `from`.
std::string xmlAttr(const std::string& xml, const std::string& attr, std::size_t from = 0) {
    std::string key = attr + "=\"";
    std::size_t p = xml.find(key, from);
    if (p == std::string::npos) return "";
    p += key.size();
    std::size_t e = xml.find('"', p);
    if (e == std::string::npos) return "";
    return xml.substr(p, e - p);
}

// Standard bed speaker order for a given bed channel count (Dolby ADM bed layouts).
std::vector<std::string> bedOrder(int n) {
    switch (n) {
        case 1:  return {"M"};
        case 2:  return {"L", "R"};
        case 6:  return {"L", "R", "C", "LFE", "Ls", "Rs"};                  // 5.1
        case 8:  return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs"};  // 7.1
        case 10: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf"};  // 7.1.2
        case 12: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs",
                         "Ltf", "Rtf", "Ltr", "Rtr"};                        // 7.1.4
        case 14: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs",
                         "Ltf", "Rtf", "Ltm", "Rtm", "Ltr", "Rtr"};          // 7.1.6
        default: return {};
    }
}
std::string bedName(int n) {
    switch (n) {
        case 6: return "5.1"; case 8: return "7.1"; case 10: return "7.1.2";
        case 12: return "7.1.4"; case 14: return "7.1.6"; default: return std::to_string(n) + ".x";
    }
}

// From the chna track->pack assignments, label each channel as a bed speaker (incl. height)
// or "Object N". The pack shared by the most channels is taken as the bed; the rest are
// dynamic objects. Populates m.channelRoles and m.adm.bedLayout.
void resolveAdmRoles(FileMetadata& m, const std::vector<std::pair<int, std::string>>& chna) {
    if (chna.empty() || m.channels <= 0) return;
    // Count channels per pack ref; the largest pack is the bed.
    std::vector<std::pair<std::string, int>> counts;
    for (const auto& e : chna) {
        bool found = false;
        for (auto& c : counts)
            if (c.first == e.second) { ++c.second; found = true; break; }
        if (!found) counts.push_back({e.second, 1});
    }
    std::string bedPack;
    int bedCount = 0;
    for (const auto& c : counts)
        if (c.second > bedCount && !c.first.empty()) { bedCount = c.second; bedPack = c.first; }

    std::vector<std::string> bed = bedOrder(bedCount);
    m.channelRoles.assign(m.channels, "");
    int bedIdx = 0, objIdx = 0;
    for (const auto& e : chna) {
        int ch = e.first - 1;  // chna track index is 1-based
        if (ch < 0 || ch >= m.channels) continue;
        if (e.second == bedPack && bedCount > 1) {
            m.channelRoles[ch] = (bedIdx < static_cast<int>(bed.size())) ? bed[bedIdx]
                                                                         : "Bed " + std::to_string(bedIdx + 1);
            ++bedIdx;
        } else {
            m.channelRoles[ch] = "Object " + std::to_string(++objIdx);
        }
    }
    for (auto& r : m.channelRoles)
        if (r.empty()) r = "Ch ?";
    if (bedCount > 1) {
        m.adm.bedLayout = bedName(bedCount) + " bed";
        if (objIdx > 0) m.adm.bedLayout += " + " + std::to_string(objIdx) + " object" + (objIdx > 1 ? "s" : "");
    }
}

// Parse the WAVE channel mask (from a WAVE_FORMAT_EXTENSIBLE fmt chunk) and any ADM /
// Dolby Atmos metadata (axml / chna / dbmd) from a RIFF container.
void readAdmAndMask(const std::string& path, FileMetadata& m) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    char riff[4];
    if (!f.read(riff, 4)) return;
    if (std::memcmp(riff, "RIFF", 4) != 0 && std::memcmp(riff, "RF64", 4) != 0) return;
    f.seekg(8, std::ios::beg);
    char form[4];
    if (!f.read(form, 4) || std::memcmp(form, "WAVE", 4) != 0) return;

    auto rd32 = [&](std::uint32_t& v) {
        unsigned char b[4];
        if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
        v = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        return true;
    };

    std::vector<std::pair<int, std::string>> chna;  // (1-based channel, audioPackFormat ref)
    char id[4];
    std::uint32_t size = 0;
    while (f.read(id, 4) && rd32(size)) {
        std::streampos next = f.tellg() + static_cast<std::streamoff>(size + (size & 1));
        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 40) {
            std::vector<unsigned char> b(size);
            if (f.read(reinterpret_cast<char*>(b.data()), size)) {
                std::uint16_t tag = b[0] | (b[1] << 8);
                if (tag == 0xFFFE)  // WAVE_FORMAT_EXTENSIBLE -> dwChannelMask at offset 20
                    m.channelMask = b[20] | (b[21] << 8) | (b[22] << 16) |
                                    (static_cast<std::uint32_t>(b[23]) << 24);
            }
        } else if (std::memcmp(id, "chna", 4) == 0) {
            m.adm.present = true;
            std::vector<unsigned char> b(size);
            if (f.read(reinterpret_cast<char*>(b.data()), size) && size >= 4) {
                std::uint16_t numUIDs = b[2] | (b[3] << 8);
                // Each ID entry is 40 bytes: trackIndex(2) UID(12) trackRef(14) packRef(11) pad(1).
                for (int k = 0; k < numUIDs; ++k) {
                    std::size_t off = 4 + static_cast<std::size_t>(k) * 40;
                    if (off + 40 > size) break;
                    int tIdx = b[off] | (b[off + 1] << 8);
                    std::string pack(reinterpret_cast<char*>(&b[off + 28]), 11);
                    pack = std::string(pack.c_str());  // trim at first NUL
                    if (tIdx > 0) chna.push_back({tIdx, pack});
                }
            }
        } else if (std::memcmp(id, "dbmd", 4) == 0) {
            m.adm.hasDbmd = true;
        } else if (std::memcmp(id, "axml", 4) == 0) {
            std::uint32_t cap = std::min<std::uint32_t>(size, 1u << 20);  // up to 1 MB
            std::string xml(cap, '\0');
            if (cap && f.read(xml.data(), cap)) {
                m.adm.present = true;
                m.adm.programme = xmlAttr(xml, "audioProgrammeName");
                for (std::size_t p = xml.find("audioObjectName=\""); p != std::string::npos;
                     p = xml.find("audioObjectName=\"", p + 1)) {
                    std::string name = xmlAttr(xml, "audioObjectName", p);
                    if (!name.empty()) m.adm.objects.push_back(name);
                }
                // Best-effort bed layout from a pack-format name like "7.1.6".
                std::string pack = xmlAttr(xml, "audioPackFormatName");
                if (pack.find('.') != std::string::npos &&
                    pack.find_first_of("0123456789") != std::string::npos)
                    m.adm.bedLayout = pack;
            }
        }
        f.seekg(next, std::ios::beg);
    }
    // Resolve per-channel roles (bed speakers incl. height vs objects) from the chna map.
    if (m.adm.present) resolveAdmRoles(m, chna);
}

}  // namespace

DecodeResult decodeFile(const std::string& path) {
    DecodeResult r;
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        r.error = "File not found: " + path;
        return r;
    }

    SF_INFO info{};
    SNDFILE* snd = sf_open(path.c_str(), SFM_READ, &info);
    if (!snd) {
        // libsndfile can't handle this container (e.g. AAC / Apple Lossless in
        // .m4a/.mp4/.aac). Fall back to the platform codec stack.
        DecodeResult fallback;
        if (decodeCompressed(path, fallback)) return fallback;
        r.error = std::string("could not open file: ") + sf_strerror(nullptr);
        return r;
    }

    SubtypeInfo st = subtypeInfo(info.format);

    r.meta.path = path;
    r.meta.filename = fs::path(path).filename().string();
    r.meta.container = majorFormatName(info.format);
    r.meta.codec = st.codec;
    r.meta.bitDepth = st.bitDepth;
    r.meta.lossless = st.lossless;
    r.meta.sampleRate = info.samplerate;
    r.meta.channels = info.channels;
    r.meta.frames = static_cast<std::uint64_t>(info.frames);
    r.meta.durationSec = info.samplerate > 0
                             ? static_cast<double>(info.frames) / info.samplerate
                             : 0.0;
    r.meta.fileBytes = static_cast<std::uint64_t>(fs::file_size(path, ec));

    // Broadcast metadata (BWF bext via libsndfile; iXML via a direct RIFF scan).
    extractBroadcast(snd, r.meta.broadcast);
    std::string ixml = readIxmlChunk(path);
    if (!ixml.empty()) {
        r.meta.broadcast.present = true;
        r.meta.broadcast.ixml = ixml;
    }
    // Descriptive tags + embedded cover art (RIFF LIST/INFO + ID3).
    readRiffTags(path, r.meta.tags);
    // Channel layout: WAVE channel mask + ADM/Atmos (axml/chna/dbmd), then classify.
    readAdmAndMask(path, r.meta);
    classifyLayout(r.meta);

    if (info.channels <= 0 || info.samplerate <= 0) {
        sf_close(snd);
        r.error = "File has no decodable audio (channels/samplerate invalid)";
        return r;
    }

    AudioBuffer& buf = r.buffer;
    buf.sampleRate = info.samplerate;
    buf.channels = info.channels;
    buf.data.assign(info.channels, std::vector<float>());

    const sf_count_t kBlock = 1 << 16;  // frames per read
    std::vector<float> inter(static_cast<std::size_t>(kBlock) * info.channels);

    // Pre-reserve when frame count is known to avoid reallocations on long files.
    if (info.frames > 0) {
        for (auto& ch : buf.data) ch.reserve(static_cast<std::size_t>(info.frames));
    }

    sf_count_t got = 0;
    while ((got = sf_readf_float(snd, inter.data(), kBlock)) > 0) {
        for (sf_count_t f = 0; f < got; ++f) {
            for (int c = 0; c < info.channels; ++c) {
                buf.data[c].push_back(inter[f * info.channels + c]);
            }
        }
    }
    sf_close(snd);

    // Trust the decoded frame count over the header (catches truncation).
    r.meta.frames = static_cast<std::uint64_t>(buf.frames());
    r.meta.durationSec = buf.durationSec();

    if (buf.frames() == 0) {
        r.error = "Decoded zero audio frames";
        return r;
    }

    r.ok = true;
    return r;
}

}  // namespace argus
