#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cli/ConsoleReport.h"
#include "core/Engine.h"
#include "core/Profile.h"
#include "core/export/Exports.h"

namespace {

// QA / sign-off details for exported reports, read from the environment so CI
// pipelines can stamp reports (e.g. ARGUS_QA_ENGINEER, ARGUS_QA_STUDIO, ...).
argus::ReportInfo qaInfoFromEnv() {
    argus::ReportInfo info;
    auto get = [](const char* k) -> std::string {
        const char* v = std::getenv(k);
        return v ? v : "";
    };
    info.qaEngineer = get("ARGUS_QA_ENGINEER");
    info.contact = get("ARGUS_QA_CONTACT");
    info.studio = get("ARGUS_QA_STUDIO");
    info.client = get("ARGUS_QA_CLIENT");
    info.project = get("ARGUS_QA_PROJECT");
    info.catalog = get("ARGUS_QA_CATALOG");
    info.notes = get("ARGUS_QA_NOTES");
    return info;
}

void usage(const char* argv0) {
    std::printf(
        "Argus (argus) - audio master QA\n"
        "Usage: %s [options] <file-or-directory> [more paths...]\n\n"
        "Runs the QA analysis suite on each audio file and prints a report.\n\n"
        "Per-file exports:\n"
        "  --pdf <dir>          write a <name>_QA.pdf report per file into <dir>\n"
        "  --csv <dir>          write a <name>_QA.csv per file into <dir>\n"
        "  --json <dir>         write a <name>_QA.json per file into <dir>\n\n"
        "Batch exports:\n"
        "  --batch-csv <file>   write a one-row-per-file summary CSV\n"
        "  --batch-json <file>  write a single JSON document for the whole batch\n"
        "  --batch-pdf <file>   write one combined PDF for the whole batch\n\n"
        "Other:\n"
        "  --profile <name|idx> delivery profile for spec checks (default: 0)\n"
        "  --list-profiles      print the built-in delivery profiles and exit\n"
        "  --spectrogram <ppm>  render the first file's spectrogram to a PPM\n"
        "  --spectrogram-png <png>  render the first file's spectrogram to a PNG\n\n"
        "Exit code: 0 clean/info, 1 warnings, 2 failures.\n",
        argv0);
}

void listProfiles() {
    std::printf("Delivery profiles:\n");
    const auto ps = argus::allProfiles();
    const std::size_t builtins = argus::builtinProfiles().size();
    for (std::size_t i = 0; i < ps.size(); ++i)
        std::printf("  %zu  %s%s\n", i, ps[i].name.c_str(), i >= builtins ? "  (custom)" : "");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    std::string specOut, specPngOut, pdfDir, csvDir, jsonDir, batchCsv, batchJson, batchPdf,
        profileName;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--list-profiles") { listProfiles(); return 0; }
        if (a == "--spectrogram" && i + 1 < argc) { specOut = argv[++i]; continue; }
        if (a == "--spectrogram-png" && i + 1 < argc) { specPngOut = argv[++i]; continue; }
        if (a == "--pdf" && i + 1 < argc) { pdfDir = argv[++i]; continue; }
        if (a == "--csv" && i + 1 < argc) { csvDir = argv[++i]; continue; }
        if (a == "--json" && i + 1 < argc) { jsonDir = argv[++i]; continue; }
        if (a == "--batch-csv" && i + 1 < argc) { batchCsv = argv[++i]; continue; }
        if (a == "--batch-json" && i + 1 < argc) { batchJson = argv[++i]; continue; }
        if (a == "--batch-pdf" && i + 1 < argc) { batchPdf = argv[++i]; continue; }
        if (a == "--profile" && i + 1 < argc) { profileName = argv[++i]; continue; }
        auto found = argus::collectInputs(a);
        if (found.empty())
            std::fprintf(stderr, "warning: no audio files at '%s'\n", a.c_str());
        files.insert(files.end(), found.begin(), found.end());
    }

    const argus::Profile& profile =
        profileName.empty() ? argus::defaultProfile() : argus::profileByName(profileName);
    const argus::ReportInfo info = qaInfoFromEnv();

    // Debug/utility: render the spectrogram of the first file to a binary PPM.
    if (!specOut.empty() && !files.empty()) {
        argus::Report rep = argus::analyzeFileFull(files[0], 1200, 512, {}, profile);
        if (rep.specWidth > 0) {
            FILE* fp = std::fopen(specOut.c_str(), "wb");
            if (fp) {
                std::fprintf(fp, "P6\n%d %d\n255\n", rep.specWidth, rep.specHeight);
                for (int p = 0; p < rep.specWidth * rep.specHeight; ++p) {
                    std::fputc(rep.specRGBA[p * 4 + 0], fp);
                    std::fputc(rep.specRGBA[p * 4 + 1], fp);
                    std::fputc(rep.specRGBA[p * 4 + 2], fp);
                }
                std::fclose(fp);
                std::printf("wrote spectrogram %dx%d to %s\n", rep.specWidth, rep.specHeight,
                            specOut.c_str());
            }
        }
        return 0;
    }

    // Render the spectrogram of the first file to a PNG.
    if (!specPngOut.empty() && !files.empty()) {
        argus::Report rep = argus::analyzeFileFull(files[0], 1200, 512, {}, profile);
        if (argus::exportSpectrogramPng(rep, specPngOut))
            std::printf("wrote spectrogram %dx%d to %s\n", rep.specWidth, rep.specHeight,
                        specPngOut.c_str());
        else
            std::fprintf(stderr, "error: could not render spectrogram for '%s'\n",
                         files[0].c_str());
        return 0;
    }

    if (files.empty()) {
        std::fprintf(stderr, "error: no audio files to analyse\n");
        return 1;
    }

    namespace fs = std::filesystem;
    const bool wantPerFileExport = !pdfDir.empty() || !csvDir.empty() || !jsonDir.empty();
    const bool wantSpec = !pdfDir.empty() || !batchPdf.empty();
    const bool keepReports = !batchCsv.empty() || !batchJson.empty() || !batchPdf.empty();
    std::vector<argus::Report> allReports;

    int worst = 0;  // track highest severity for exit code
    for (const auto& f : files) {
        argus::Report rep = wantSpec ? argus::analyzeFileFull(f, 1200, 512, {}, profile)
                                     : argus::analyzeFile(f, profile);
        argus::printReport(rep);

        if (wantPerFileExport) {
            std::string stem = fs::path(f).stem().string();
            if (!pdfDir.empty())
                argus::exportReportPdf(rep, (fs::path(pdfDir) / (stem + "_QA.pdf")).string(), info);
            if (!csvDir.empty())
                argus::exportReportCsv(rep, (fs::path(csvDir) / (stem + "_QA.csv")).string());
            if (!jsonDir.empty())
                argus::exportReportJson(rep, (fs::path(jsonDir) / (stem + "_QA.json")).string(),
                                        info);
        }
        if (keepReports) allReports.push_back(std::move(rep));
        else if (!rep.ok()) worst = std::max(worst, 3);
        else worst = std::max(worst, static_cast<int>(rep.verdict()));
    }

    if (keepReports) {
        if (!batchCsv.empty()) argus::exportBatchCsv(allReports, batchCsv);
        if (!batchJson.empty()) argus::exportBatchJson(allReports, batchJson, info);
        if (!batchPdf.empty()) argus::exportBatchPdf(allReports, batchPdf, info);
        for (const auto& rep : allReports) {
            if (!rep.ok()) worst = std::max(worst, 3);
            else worst = std::max(worst, static_cast<int>(rep.verdict()));
        }
    }

    // Exit code: 0 clean/info, 1 warnings, 2 failures (handy for CI / scripting).
    if (worst >= static_cast<int>(argus::Severity::Fail)) return 2;
    if (worst >= static_cast<int>(argus::Severity::Warn)) return 1;
    return 0;
}
