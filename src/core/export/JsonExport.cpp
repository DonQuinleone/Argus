// Machine-readable JSON export for CI / pipeline integration. Self-contained (no
// JSON library): emits compact, well-formed UTF-8 JSON. Numbers (times in seconds,
// loudness fields, etc.) stay numeric where meaningful; everything else is a string.
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "Exports.h"

#ifndef ARGUS_VERSION
#define ARGUS_VERSION "dev"
#endif

namespace argus {
namespace {

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }
std::string kv(const std::string& k, const std::string& v) { return q(k) + ":" + q(v); }

std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

std::string numi(long long v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%lld", v);
    return b;
}

std::string isoNow() {
    std::time_t now = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    return b;
}

void appendQaBlock(std::string& o, const ReportInfo& info) {
    std::vector<std::string> parts;
    auto add = [&](bool show, const char* k, const std::string& v) {
        if (show && !v.empty()) parts.push_back(kv(k, v));
    };
    add(info.showEngineer, "engineer", info.qaEngineer);
    add(info.showContact, "contact", info.contact);
    add(info.showStudio, "studio", info.studio);
    add(info.showClient, "client", info.client);
    add(info.showProject, "project", info.project);
    add(info.showCatalog, "catalog", info.catalog);
    add(info.showNotes, "notes", info.notes);
    if (parts.empty()) return;
    o += q("qa") + ":{";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) o += ",";
        o += parts[i];
    }
    o += "},";
}

// Serialise one report as a JSON object (no trailing comma).
std::string reportObject(const Report& rep) {
    std::string o = "{";
    o += q("file") + ":{";
    o += kv("name", rep.meta.filename) + ",";
    o += kv("container", rep.meta.container) + ",";
    o += kv("codec", rep.meta.codec) + ",";
    o += q("sampleRate") + ":" + numi(rep.meta.sampleRate) + ",";
    o += q("bitDepth") + ":" + numi(rep.meta.bitDepth) + ",";
    o += q("channels") + ":" + numi(rep.meta.channels) + ",";
    o += q("durationSec") + ":" + num(rep.meta.durationSec) + ",";
    o += q("frames") + ":" + numi(static_cast<long long>(rep.meta.frames)) + ",";
    o += q("fileBytes") + ":" + numi(static_cast<long long>(rep.meta.fileBytes));
    o += "},";

    o += q("ok") + ":" + (rep.ok() ? "true" : "false") + ",";
    o += kv("verdict", rep.ok() ? severityLabel(rep.verdict()) : "ERROR") + ",";
    if (!rep.ok()) o += kv("decodeError", rep.decodeError) + ",";
    o += q("counts") + ":{" + q("fail") + ":" + numi(rep.count(Severity::Fail)) + "," + q("warn") +
         ":" + numi(rep.count(Severity::Warn)) + "," + q("info") + ":" +
         numi(rep.count(Severity::Info)) + "," + q("pass") + ":" + numi(rep.count(Severity::Pass)) +
         "},";

    o += q("issues") + ":[";
    for (std::size_t i = 0; i < rep.issues.size(); ++i) {
        const Issue& is = rep.issues[i];
        if (i) o += ",";
        o += "{";
        o += kv("check", is.check) + ",";
        o += kv("severity", severityLabel(is.severity)) + ",";
        o += kv("summary", is.summary);
        if (!is.detail.empty()) o += "," + kv("detail", is.detail);
        if (is.channel >= 0) o += "," + q("channel") + ":" + numi(is.channel);
        if (is.localised()) {
            o += "," + q("startSec") + ":" + num(is.tStart);
            if (is.tEnd > is.tStart) o += "," + q("endSec") + ":" + num(is.tEnd);
        }
        if (!is.fields.empty()) {
            o += "," + q("fields") + ":{";
            for (std::size_t k = 0; k < is.fields.size(); ++k) {
                if (k) o += ",";
                o += kv(is.fields[k].first, is.fields[k].second);
            }
            o += "}";
        }
        o += "}";
    }
    o += "]}";
    return o;
}

bool writeFile(const std::string& path, const std::string& data) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fwrite(data.data(), 1, data.size(), fp);
    std::fclose(fp);
    return true;
}

}  // namespace

bool exportReportJson(const Report& rep, const std::string& path, const ReportInfo& info) {
    std::string o = "{";
    o += kv("tool", "argus") + ",";
    o += kv("version", ARGUS_VERSION) + ",";
    o += kv("generated", isoNow()) + ",";
    appendQaBlock(o, info);
    o += q("report") + ":" + reportObject(rep);
    o += "}\n";
    return writeFile(path, o);
}

bool exportBatchJson(const std::vector<Report>& reps, const std::string& path,
                     const ReportInfo& info) {
    int fails = 0, warns = 0, errs = 0;
    for (const auto& r : reps) {
        if (!r.ok()) { ++errs; continue; }
        fails += r.count(Severity::Fail);
        warns += r.count(Severity::Warn);
    }
    std::string o = "{";
    o += kv("tool", "argus") + ",";
    o += kv("version", ARGUS_VERSION) + ",";
    o += kv("generated", isoNow()) + ",";
    appendQaBlock(o, info);
    o += q("summary") + ":{" + q("files") + ":" + numi(static_cast<long long>(reps.size())) + "," +
         q("fail") + ":" + numi(fails) + "," + q("warn") + ":" + numi(warns) + "," + q("error") +
         ":" + numi(errs) + "},";
    o += q("reports") + ":[";
    for (std::size_t i = 0; i < reps.size(); ++i) {
        if (i) o += ",";
        o += reportObject(reps[i]);
    }
    o += "]}\n";
    return writeFile(path, o);
}

}  // namespace argus
