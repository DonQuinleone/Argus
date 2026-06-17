#include "ChannelLayout.h"

#include "Util.h"

namespace argus {
namespace {

// Standard WAVE_FORMAT_EXTENSIBLE channel-mask bits, in bit order.
struct MaskRole {
    std::uint32_t bit;
    const char* role;
};
const MaskRole kMaskRoles[] = {
    {0x1, "L"},   {0x2, "R"},    {0x4, "C"},    {0x8, "LFE"},  {0x10, "Lrs"}, {0x20, "Rrs"},
    {0x40, "Lc"}, {0x80, "Rc"},  {0x100, "Cs"}, {0x200, "Lss"}, {0x400, "Rss"}, {0x800, "Tc"},
    {0x1000, "Ltf"}, {0x2000, "Tfc"}, {0x4000, "Rtf"}, {0x8000, "Ltr"}, {0x10000, "Tbc"},
    {0x20000, "Rtr"},
};

// Default role labels for common layouts when no mask/ADM is available.
std::vector<std::string> defaultRoles(int ch) {
    switch (ch) {
        case 1:  return {"M"};
        case 2:  return {"L", "R"};
        case 3:  return {"L", "R", "C"};
        case 4:  return {"L", "R", "Ls", "Rs"};
        case 6:  return {"L", "R", "C", "LFE", "Ls", "Rs"};                     // 5.1
        case 8:  return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs"};     // 7.1
        case 10: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs",
                         "Ltf", "Rtf"};                                          // 7.1.2
        case 12: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs",
                         "Ltf", "Rtf", "Ltr", "Rtr"};                            // 7.1.4
        case 14: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs",
                         "Ltf", "Rtf", "Ltm", "Rtm", "Ltr", "Rtr"};             // 7.1.6
        default: {
            std::vector<std::string> r;
            for (int i = 0; i < ch; ++i) r.push_back("Ch " + fmtInt(i + 1));
            return r;
        }
    }
}

// Atmos bed label by channel count (best-effort; the common bed for each count).
std::string atmosBed(int ch) {
    switch (ch) {
        case 8:  return "5.1.2";
        case 10: return "7.1.2";
        case 12: return "7.1.4";
        case 14: return "7.1.6";
        case 16: return "9.1.6";
        default: return fmtInt(ch) + " ch";
    }
}

std::string surroundName(int ch) {
    switch (ch) {
        case 6:  return "5.1";
        case 8:  return "7.1";
        case 10: return "7.1.2";
        case 12: return "7.1.4";
        case 14: return "7.1.6";
        default: return fmtInt(ch) + "-channel";
    }
}

}  // namespace

void classifyLayout(FileMetadata& m) {
    const int ch = m.channels;

    // Roles: keep ADM-resolved per-channel roles (bed speakers + objects) if already set;
    // otherwise prefer the explicit WAVE channel mask, else the standard layout map.
    if (static_cast<int>(m.channelRoles.size()) != ch) {
        m.channelRoles.clear();
        if (m.channelMask != 0) {
            for (const auto& mr : kMaskRoles)
                if (m.channelMask & mr.bit) m.channelRoles.push_back(mr.role);
            for (int i = static_cast<int>(m.channelRoles.size()); i < ch; ++i)
                m.channelRoles.push_back("Ch " + fmtInt(i + 1));
            if (static_cast<int>(m.channelRoles.size()) > ch) m.channelRoles.resize(ch);
        } else {
            m.channelRoles = defaultRoles(ch);
        }
    }

    if (m.adm.present || m.adm.hasDbmd) {
        m.layoutFamily = LayoutFamily::Atmos;
        std::string bed = m.adm.bedLayout.empty() ? atmosBed(ch) + " (Dolby Atmos)"
                                                  : m.adm.bedLayout + " (Dolby Atmos)";
        m.layoutName = bed;
    } else if (ch <= 0) {
        m.layoutFamily = LayoutFamily::Unknown;
        m.layoutName = "unknown";
    } else if (ch == 1) {
        m.layoutFamily = LayoutFamily::Mono;
        m.layoutName = "mono";
    } else if (ch == 2) {
        m.layoutFamily = LayoutFamily::Stereo;
        m.layoutName = "stereo";
    } else {
        m.layoutFamily = LayoutFamily::Surround;
        m.layoutName = surroundName(ch);
    }
}

}  // namespace argus
