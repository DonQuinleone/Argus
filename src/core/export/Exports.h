#pragma once

#include <string>
#include <vector>

#include "../Report.h"
#include "../ReportInfo.h"

namespace argus {

// Per-issue CSV for a single report (one row per finding).
bool exportReportCsv(const Report& rep, const std::string& path);

// One-row-per-file summary CSV for a batch.
bool exportBatchCsv(const std::vector<Report>& reps, const std::string& path);

// Full PDF report: header (with optional logo + QA sign-off block), metadata,
// embedded spectrogram with issue markers, and the findings table. Self-contained
// writer (no external PDF library).
bool exportReportPdf(const Report& rep, const std::string& path, const ReportInfo& info = {});

// One combined, delivery-ready PDF for a whole batch: a summary matrix page
// followed by each file's full report section.
bool exportBatchPdf(const std::vector<Report>& reps, const std::string& path,
                    const ReportInfo& info = {});

// Machine-readable JSON for a single report (CI / pipeline integration).
bool exportReportJson(const Report& rep, const std::string& path, const ReportInfo& info = {});

// One JSON document describing a whole batch (array of file results + summary).
bool exportBatchJson(const std::vector<Report>& reps, const std::string& path,
                     const ReportInfo& info = {});

// PNG of the rendered spectrogram raster (the same RGBA8 image shown in the app).
// Requires a report produced by analyzeFileFull (raster populated); false otherwise.
bool exportSpectrogramPng(const Report& rep, const std::string& path);

}  // namespace argus
