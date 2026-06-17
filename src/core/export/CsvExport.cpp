#include <cstdio>
#include <string>

#include "../Util.h"
#include "Exports.h"

namespace argus {
namespace {

std::string csvEscape(const std::string& s) {
    bool needQuote = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!needQuote) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string fieldsJoined(const Issue& is) {
    std::string s;
    for (std::size_t i = 0; i < is.fields.size(); ++i) {
        if (i) s += "; ";
        s += is.fields[i].first + "=" + is.fields[i].second;
    }
    return s;
}

}  // namespace

bool exportReportCsv(const Report& rep, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fprintf(fp, "file,check,severity,start,end,channel,summary,detail,technical\n");
    for (const auto& is : rep.issues) {
        std::fprintf(
            fp, "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", csvEscape(rep.meta.filename).c_str(),
            csvEscape(is.check).c_str(), severityLabel(is.severity),
            is.localised() ? timecode(is.tStart).c_str() : "",
            (is.tEnd > is.tStart) ? timecode(is.tEnd).c_str() : "",
            is.channel >= 0 ? fmtInt(is.channel).c_str() : "",
            csvEscape(is.summary).c_str(), csvEscape(is.detail).c_str(),
            csvEscape(fieldsJoined(is)).c_str());
    }
    std::fclose(fp);
    return true;
}

bool exportBatchCsv(const std::vector<Report>& reps, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fprintf(fp,
                 "file,verdict,fails,warns,container,codec,bit_depth,sample_rate_hz,"
                 "channels,duration,decode_error\n");
    for (const auto& r : reps) {
        std::fprintf(fp, "%s,%s,%d,%d,%s,%s,%d,%d,%d,%s,%s\n",
                     csvEscape(r.meta.filename).c_str(),
                     r.ok() ? severityLabel(r.verdict()) : "ERROR",
                     r.count(Severity::Fail), r.count(Severity::Warn),
                     csvEscape(r.meta.container).c_str(), csvEscape(r.meta.codec).c_str(),
                     r.meta.bitDepth, r.meta.sampleRate, r.meta.channels,
                     timecode(r.meta.durationSec).c_str(),
                     csvEscape(r.decodeError).c_str());
    }
    std::fclose(fp);
    return true;
}

}  // namespace argus
