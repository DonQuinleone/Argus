// Argus - Dear ImGui desktop front-end.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "misc/cpp/imgui_stdlib.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "tinyfiledialogs.h"

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#include <unistd.h>  // readlink for executable-relative resource lookup
#endif

#include "Player.h"
#include "Settings.h"
#include "Texture.h"
#include "core/Decoder.h"
#include "core/Engine.h"
#include "core/Profile.h"
#include "core/Util.h"
#include "core/export/Exports.h"
#include "core/render/Colormap.h"
#include "core/render/Spectrogram.h"

#include "MacMenu.h"  // native macOS menu bar (no-op include on other platforms)

using namespace argus;

namespace {

// High-resolution full-file raster. The width is large so long programmes keep
// usable time detail when the image is scaled down for display; the GPU's linear
// filtering then keeps it crisp. Height covers the frequency axis generously.
constexpr int kSpecW = 4096;
constexpr int kSpecH = 1024;

enum JobKind { kAnalyze = 0, kLoadAudio = 1 };

ImVec4 severityColor(Severity s) {
    switch (s) {
        case Severity::Pass: return ImVec4(0.36f, 0.78f, 0.40f, 1.0f);
        case Severity::Info: return ImVec4(0.55f, 0.70f, 0.95f, 1.0f);
        case Severity::Warn: return ImVec4(0.96f, 0.74f, 0.24f, 1.0f);
        case Severity::Fail: return ImVec4(0.95f, 0.38f, 0.36f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

// Plain-English explanation of what each detector looks for (shown as a tooltip).
const char* checkHelp(const std::string& c) {
    auto has = [&](const char* k) { return c.find(k) != std::string::npos; };
    if (c == "File") return "Container, codec, bit depth, sample rate, channels and duration.";
    if (has("Format") || has("compliance"))
        return "Whether the file meets the selected delivery profile's bit-depth, channel, "
               "sample-rate and lossless rules.";
    if (has("Loudness")) return "Integrated loudness (LUFS) and loudness range (LRA) with per-DSP "
                                "target deltas, via ITU-R BS.1770.";
    if (has("True peak")) return "Inter-sample (true) peak in dBTP vs the delivery ceiling - the "
                                 "modern predictor of clipping after lossy encoding.";
    if (has("Clipping")) return "Runs of consecutive full-scale samples (on-sample clipping).";
    if (has("Dropout")) return "A sudden collapse of the high-frequency noise floor - the signature "
                               "of a dropped/missing audio frame, distinct from a musical pause.";
    if (has("Click")) return "Isolated sample-level discontinuities (interpolation-residual outliers).";
    if (has("ilence") || has("DC") || has("balance") || has("channel"))
        return "Leading/trailing silence, embedded digital-zero gaps, DC offset and channel "
               "balance / dead channels.";
    if (has("Phase") || has("mono")) return "Inter-channel correlation / mono compatibility and "
                                             "dual-mono detection.";
    if (has("Spectral") || has("bandwidth"))
        return "Bandwidth and brickwall-cutoff detection (upsampled 'fake hi-res' or lossy-sourced "
               "material).";
    return "";
}

void helpMarker(const char* desc) {
    if (!desc || !*desc) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(340.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Build a copyable plain-text representation of a finding.
std::string issueToText(const Issue& is) {
    std::string s = std::string(severityLabel(is.severity)) + "  " + is.check + " - " + is.summary;
    if (is.localised()) {
        s += "  [" + timecode(is.tStart);
        if (is.tEnd > is.tStart) s += " - " + timecode(is.tEnd);
        s += "]";
    }
    s += "\n";
    if (!is.detail.empty()) s += is.detail + "\n";
    for (const auto& f : is.fields) s += "  " + f.first + ": " + f.second + "\n";
    return s;
}

struct FileEntry {
    std::string path;
    std::string name;
    int state = 0;  // 0 pending, 1 analyzing, 2 done, 3 error
    Report report;
    Texture tex;
    bool texDirty = false;
    std::map<int, Texture> closeupTex;  // keyed by issue index
};

struct Job {
    int kind = kAnalyze;
    int index = 0;
    std::string path;
    SpectrogramSettings settings;
    Profile profile;
};
struct ResultMsg {
    int kind = kAnalyze;
    int index = 0;
    Report report;
    AudioBuffer audio;
    bool audioOk = false;
};

class App {
public:
    std::vector<FileEntry> files;
    int selected = -1;
    int selectedIssue = -1;
    SpectrogramSettings spec;
    bool showSettings = true;

    // QA / sign-off details for exported reports, and its floating window.
    ReportInfo reportInfo;
    bool showReportInfo = false;
    int themeMode = 0;      // 0 = dark, 1 = light
    int profileIndex = 0;   // selected delivery profile

    // Audition.
    Player player;
    int audioLoadedFor = -1;   // file index whose audio is in the player
    int audioPendingFor = -1;  // file index whose audio decode is in flight
    float preroll = 2.0f;
    float postroll = 2.0f;
    bool loopAudition = false;

    // Left pane layout.
    float listWidth = 280.0f;
    bool listCollapsed = false;
    std::string fileFilter;        // case-insensitive name filter
    bool sortByVerdict = false;    // sort the list worst-verdict-first
    std::string lastExportDir;     // remembered for export save dialogs

    // Deferred actions requested from the native menu (run on the UI thread).
    std::atomic<bool> reqOpenFile{false}, reqOpenFolder{false}, reqExportPdf{false};
    std::atomic<bool> reqExportCsv{false}, reqExportJson{false}, reqExportAll{false};
    std::atomic<bool> reqExportBatchPdf{false};

    // Worker.
    std::thread worker;
    std::mutex jobMx, resMx;
    std::condition_variable jobCv;
    std::queue<Job> jobs;
    std::queue<ResultMsg> results;
    std::atomic<bool> quit{false};
    std::atomic<int> pending{0};

    void start() { worker = std::thread([this] { workerLoop(); }); }
    void stop() {
        quit = true;
        jobCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void workerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(jobMx);
                jobCv.wait(lk, [this] { return quit || !jobs.empty(); });
                if (quit) return;
                job = jobs.front();
                jobs.pop();
            }
            ResultMsg msg;
            msg.kind = job.kind;
            msg.index = job.index;
            if (job.kind == kAnalyze) {
                msg.report = analyzeFileFull(job.path, kSpecW, kSpecH, job.settings, job.profile);
            } else {
                DecodeResult dec = decodeFile(job.path);
                msg.audioOk = dec.ok;
                if (dec.ok) msg.audio = std::move(dec.buffer);
            }
            std::lock_guard<std::mutex> lk(resMx);
            results.push(std::move(msg));
        }
    }

    const Profile& currentProfile() const {
        const auto& ps = builtinProfiles();
        int i = (profileIndex >= 0 && profileIndex < static_cast<int>(ps.size())) ? profileIndex : 0;
        return ps[i];
    }

    void enqueue(int index) {
        files[index].state = 1;
        ++pending;
        {
            std::lock_guard<std::mutex> lk(jobMx);
            Job j;
            j.kind = kAnalyze;
            j.index = index;
            j.path = files[index].path;
            j.settings = spec;
            j.profile = currentProfile();
            jobs.push(std::move(j));
        }
        jobCv.notify_one();
    }

    // Re-run analysis on every already-loaded file (e.g. after a profile change).
    void reanalyzeAll() {
        for (int i = 0; i < static_cast<int>(files.size()); ++i)
            if (files[i].state == 2 || files[i].state == 3) enqueue(i);
    }

    void enqueueAudio(int index) {
        if (index < 0 || index >= static_cast<int>(files.size())) return;
        audioPendingFor = index;
        {
            std::lock_guard<std::mutex> lk(jobMx);
            jobs.push({kLoadAudio, index, files[index].path, spec});
        }
        jobCv.notify_one();
    }

    void addPath(const std::string& path) {
        for (const auto& f : collectInputs(path)) {
            bool dup = false;
            for (auto& e : files)
                if (e.path == f) dup = true;
            if (dup) continue;
            FileEntry e;
            e.path = f;
            e.name = f.substr(f.find_last_of("/\\") + 1);
            files.push_back(std::move(e));
            int idx = static_cast<int>(files.size()) - 1;
            enqueue(idx);
            if (selected < 0) select(idx);
        }
    }

    void select(int idx) {
        if (idx == selected) return;
        selected = idx;
        selectedIssue = -1;
        player.stop();
        player.clear();
        audioLoadedFor = -1;
        if (idx >= 0 && idx < static_cast<int>(files.size()) && files[idx].state == 2)
            enqueueAudio(idx);
    }

    void drainResults() {
        std::lock_guard<std::mutex> lk(resMx);
        while (!results.empty()) {
            ResultMsg m = std::move(results.front());
            results.pop();
            if (m.index < 0 || m.index >= static_cast<int>(files.size())) continue;
            if (m.kind == kAnalyze) {
                files[m.index].report = std::move(m.report);
                files[m.index].state = files[m.index].report.ok() ? 2 : 3;
                files[m.index].texDirty = true;
                files[m.index].closeupTex.clear();
                --pending;
                // Pull audio for the visible file once analysis lands.
                if (m.index == selected && files[m.index].state == 2 && audioLoadedFor != selected &&
                    audioPendingFor != selected)
                    enqueueAudio(selected);
            } else {  // kLoadAudio
                if (m.index == selected && m.audioOk) {
                    player.load(m.audio);
                    player.setLoop(loopAudition);
                    audioLoadedFor = m.index;
                }
                if (audioPendingFor == m.index) audioPendingFor = -1;
            }
        }
    }

    void reRenderSelected() {
        if (selected >= 0 && files[selected].state == 2) enqueue(selected);
    }

    // Play the padded region around a finding (and select it).
    void auditionIssue(const Report& r, int issueIdx) {
        if (issueIdx < 0 || issueIdx >= static_cast<int>(r.issues.size())) return;
        selectedIssue = issueIdx;
        const Issue& is = r.issues[issueIdx];
        if (!is.localised() || !player.hasAudio()) return;
        double evEnd = is.tEnd > is.tStart ? is.tEnd : is.tStart;
        player.setLoop(loopAudition);
        player.play(is.tStart - preroll, evEnd + postroll);
    }

    // Index of the next/prev localised finding (Warn/Fail), for keyboard stepping.
    int stepIssue(const Report& r, int dir) const {
        std::vector<int> idx;
        for (int i = 0; i < static_cast<int>(r.issues.size()); ++i)
            if (r.issues[i].localised() && r.issues[i].severity >= Severity::Warn) idx.push_back(i);
        if (idx.empty()) return -1;
        int cur = -1;
        for (int k = 0; k < static_cast<int>(idx.size()); ++k)
            if (idx[k] == selectedIssue) cur = k;
        int next = cur < 0 ? (dir > 0 ? 0 : static_cast<int>(idx.size()) - 1) : cur + dir;
        next = std::max(0, std::min(static_cast<int>(idx.size()) - 1, next));
        return idx[next];
    }
};

// ---- Frequency axis mapping (mirror Spectrogram.cpp) ----
double freqToFrac(int scale, double lo, double hi, double f) {
    f = std::min(hi, std::max(lo, f));
    switch (scale) {
        case 0: {  // Mel
            auto mel = [](double x) { return 2595.0 * std::log10(1.0 + x / 700.0); };
            return (mel(f) - mel(lo)) / (mel(hi) - mel(lo));
        }
        case 1:  // Log
            return (std::log10(f) - std::log10(lo)) / (std::log10(hi) - std::log10(lo));
        default:  // Linear
            return (f - lo) / (hi - lo);
    }
}

// Draw frequency gridlines + labels into a plot rectangle.
void drawFreqAxis(ImDrawList* dl, ImVec2 axisOrigin, ImVec2 plotPos, float plotW, float h,
                  int scale, double minF, double maxF) {
    const double ticks[] = {100, 1000, 5000, 10000, 20000, 40000};
    for (double f : ticks) {
        if (f < minF || f > maxF) continue;
        double frac = freqToFrac(scale, minF, maxF, f);
        float y = plotPos.y + static_cast<float>((1.0 - frac) * h);
        dl->AddLine(ImVec2(plotPos.x, y), ImVec2(plotPos.x + plotW, y), IM_COL32(255, 255, 255, 22));
        char lbl[16];
        if (f >= 1000)
            std::snprintf(lbl, sizeof(lbl), "%gk", f / 1000.0);
        else
            std::snprintf(lbl, sizeof(lbl), "%g", f);
        dl->AddText(ImVec2(axisOrigin.x + 4, y - 6), IM_COL32(170, 170, 175, 255), lbl);
    }
}

void drawSpectrogram(App& app, FileEntry& fe) {
    const Report& r = fe.report;
    if (!fe.tex.valid() || r.specWidth <= 0) {
        ImGui::TextDisabled("(spectrogram unavailable)");
        return;
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float h = std::min(avail.y - 4.0f, 420.0f);
    if (h < 120.0f) h = 120.0f;
    float w = avail.x;
    const float axisW = 52.0f;
    const float plotW = w - axisW;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 plotPos = ImVec2(origin.x + axisW, origin.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorScreenPos(plotPos);
    ImGui::Image((ImTextureID)(intptr_t)fe.tex.id, ImVec2(plotW, h));

    drawFreqAxis(dl, origin, plotPos, plotW, h, r.specScale, r.specMinFreq, r.specMaxFreq);

    double dur = r.specDuration > 0 ? r.specDuration : 1.0;
    for (std::size_t i = 0; i < r.issues.size(); ++i) {
        const Issue& is = r.issues[i];
        if (!is.localised() || is.severity == Severity::Pass || is.severity == Severity::Info)
            continue;
        float x0 = plotPos.x + static_cast<float>(is.tStart / dur) * plotW;
        float x1 = is.tEnd > is.tStart ? plotPos.x + static_cast<float>(is.tEnd / dur) * plotW : x0;
        ImVec4 cc = severityColor(is.severity);
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(cc.x, cc.y, cc.z, 0.9f));
        if (x1 > x0 + 1)
            dl->AddRectFilled(ImVec2(x0, plotPos.y), ImVec2(x1, plotPos.y + h),
                              ImGui::ColorConvertFloat4ToU32(ImVec4(cc.x, cc.y, cc.z, 0.18f)));
        dl->AddLine(ImVec2(x0, plotPos.y), ImVec2(x0, plotPos.y + h), col, 1.5f);
        if (static_cast<int>(i) == app.selectedIssue)
            dl->AddLine(ImVec2(x0, plotPos.y), ImVec2(x0, plotPos.y + h), IM_COL32(255, 255, 255, 220),
                        1.0f);
    }

    // Playhead.
    if (app.player.playing() || app.player.paused()) {
        float px = plotPos.x + static_cast<float>(app.player.positionSec() / dur) * plotW;
        dl->AddLine(ImVec2(px, plotPos.y), ImVec2(px, plotPos.y + h), IM_COL32(255, 255, 255, 230),
                    1.5f);
    }

    for (int k = 0; k <= 6; ++k) {
        double t = dur * k / 6.0;
        float x = plotPos.x + static_cast<float>(t / dur) * plotW;
        dl->AddText(ImVec2(x + 2, plotPos.y + h + 2), IM_COL32(170, 170, 175, 255),
                    timecode(t).c_str());
    }

    // Click: near a marker -> audition that finding; elsewhere -> seek & play.
    ImGui::SetCursorScreenPos(plotPos);
    ImGui::InvisibleButton("##spechit", ImVec2(plotW, h));
    if (ImGui::IsItemClicked()) {
        float mx = ImGui::GetIO().MousePos.x;
        double tClick = (mx - plotPos.x) / plotW * dur;
        double best = 1e18;
        int bi = -1;
        for (std::size_t i = 0; i < r.issues.size(); ++i) {
            const Issue& is = r.issues[i];
            if (!is.localised() || is.severity < Severity::Warn) continue;
            double d = std::fabs(is.tStart - tClick);
            if (d < best) { best = d; bi = static_cast<int>(i); }
        }
        float markerPx = bi >= 0 ? std::fabs(static_cast<float>(r.issues[bi].tStart / dur) * plotW -
                                             (mx - plotPos.x))
                                 : 1e9f;
        if (bi >= 0 && markerPx < 7.0f) {
            app.auditionIssue(r, bi);
        } else if (app.player.hasAudio()) {
            app.player.setLoop(app.loopAudition);
            app.player.play(tClick, 0.0);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, plotPos.y + h + 20));
}

void drawWaveform(App& app, const Report& r) {
    if (r.overview.empty()) return;
    if (ImPlot::BeginPlot("##wave", ImVec2(-1, 90),
                          ImPlotFlags_NoLegend | ImPlotFlags_NoTitle | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, nullptr,
                          ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                          ImPlotAxisFlags_NoGridLines);
        double dur = r.specDuration > 0 ? r.specDuration : 1.0;
        ImPlot::SetupAxesLimits(0, dur, -1.0, 1.0, ImPlotCond_Always);
        std::vector<double> xs(r.overview.size()), top(r.overview.size()), bot(r.overview.size());
        for (std::size_t i = 0; i < r.overview.size(); ++i) {
            xs[i] = dur * i / r.overview.size();
            top[i] = r.overview[i];
            bot[i] = -r.overview[i];
        }
        ImPlot::SetNextFillStyle(ImVec4(0.30f, 0.55f, 0.85f, 0.8f));
        ImPlot::PlotShaded("amp", xs.data(), bot.data(), top.data(), static_cast<int>(xs.size()));
        for (const auto& is : r.issues) {
            if (!is.localised() || is.severity == Severity::Pass || is.severity == Severity::Info)
                continue;
            double x0 = is.tStart, x1 = is.tEnd > is.tStart ? is.tEnd : is.tStart + dur * 0.002;
            ImVec4 c = severityColor(is.severity);
            ImPlot::SetNextFillStyle(ImVec4(c.x, c.y, c.z, 0.30f));
            double xr[2] = {x0, x1};
            double yt[2] = {1, 1}, yb[2] = {-1, -1};
            ImPlot::PlotShaded("##rg", xr, yb, yt, 2);
        }
        // Playhead.
        if (app.player.playing() || app.player.paused()) {
            double pt = app.player.positionSec();
            double xr[2] = {pt, pt};
            double yr[2] = {-1, 1};
            ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.9f), 1.5f);
            ImPlot::PlotLine("##ph", xr, yr, 2);
        }
        ImPlot::EndPlot();
    }
}

// Draw a close-up raster with the event region boxed, inside the issue dropdown.
void drawCloseup(FileEntry& fe, const CloseupView& cv, int issueIdx) {
    Texture& t = fe.closeupTex[issueIdx];
    if (!t.valid()) t.upload(cv.rgba.data(), cv.width, cv.height);
    if (!t.valid()) return;

    float w = std::min(ImGui::GetContentRegionAvail().x, 560.0f);
    float h = w * cv.height / cv.width;
    const float axisW = 44.0f;
    float plotW = w - axisW;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 plotPos = ImVec2(origin.x + axisW, origin.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorScreenPos(plotPos);
    ImGui::Image((ImTextureID)(intptr_t)t.id, ImVec2(plotW, h));
    drawFreqAxis(dl, origin, plotPos, plotW, h, cv.scale, cv.minFreq, cv.maxFreq);

    double span = cv.winEnd - cv.winStart;
    if (span > 0) {
        float ex0 = plotPos.x + static_cast<float>((cv.evStart - cv.winStart) / span) * plotW;
        float ex1 = plotPos.x + static_cast<float>((cv.evEnd - cv.winStart) / span) * plotW;
        if (ex1 < ex0 + 2) ex1 = ex0 + 2;
        dl->AddRect(ImVec2(ex0, plotPos.y + 1), ImVec2(ex1, plotPos.y + h - 1),
                    IM_COL32(255, 70, 60, 255), 0, 0, 1.6f);
        dl->AddRectFilled(ImVec2(ex0, plotPos.y), ImVec2(ex1, plotPos.y + h),
                          IM_COL32(255, 70, 60, 36));
    }
    // Time labels at the window edges.
    dl->AddText(ImVec2(plotPos.x + 2, plotPos.y + h + 2), IM_COL32(150, 150, 155, 255),
                timecode(cv.winStart).c_str());
    std::string te = timecode(cv.winEnd);
    float tw = ImGui::CalcTextSize(te.c_str()).x;
    dl->AddText(ImVec2(plotPos.x + plotW - tw - 2, plotPos.y + h + 2), IM_COL32(150, 150, 155, 255),
                te.c_str());
    ImGui::SetCursorScreenPos(ImVec2(origin.x, plotPos.y + h + 18));
    ImGui::Dummy(ImVec2(w, 0));
}

void drawIssues(App& app, FileEntry& fe) {
    const Report& r = fe.report;
    for (std::size_t i = 0; i < r.issues.size(); ++i) {
        const Issue& is = r.issues[i];
        const CloseupView* cv = r.closeupFor(static_cast<int>(i));
        bool hasBody = !is.detail.empty() || !is.fields.empty() || cv != nullptr;
        ImVec4 col = severityColor(is.severity);
        ImGui::PushID(static_cast<int>(i));

        ImGui::TextColored(col, "%-5s", severityLabel(is.severity));
        ImGui::SameLine(70);
        std::string header = is.check + " - " + is.summary;
        if (is.localised())
            header += "  [" + timecode(is.tStart) +
                      (is.tEnd > is.tStart ? "-" + timecode(is.tEnd) : "") + "]";

        if (!hasBody) {
            // No extra info: render as a flat leaf with no disclosure arrow.
            ImGui::TreeNodeEx("##node",
                              ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                  ImGuiTreeNodeFlags_SpanAvailWidth,
                              "%s", header.c_str());
            if (ImGui::IsItemClicked()) app.selectedIssue = static_cast<int>(i);
            helpMarker(checkHelp(is.check));
            ImGui::PopID();
            continue;
        }

        if (static_cast<int>(i) == app.selectedIssue) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        bool open =
            ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", header.c_str());
        if (ImGui::IsItemClicked()) app.selectedIssue = static_cast<int>(i);
        helpMarker(checkHelp(is.check));
        if (open) {
            if (is.localised() && app.player.hasAudio()) {
                if (ImGui::SmallButton("Audition")) app.auditionIssue(r, static_cast<int>(i));
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(issueToText(is).c_str());
            if (is.localised() && app.player.hasAudio()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(␣ play/pause, ←→ step)");
            }
            if (!is.detail.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.80f, 1.0f));
                ImGui::TextWrapped("%s", is.detail.c_str());
                ImGui::PopStyleColor();
            }
            if (cv && cv->valid()) drawCloseup(fe, *cv, static_cast<int>(i));
            if (!is.fields.empty() &&
                ImGui::BeginTable("##fields", 2,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                for (const auto& kv : is.fields) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", kv.first.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kv.second.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void drawTransport(App& app) {
    Player& p = app.player;
    bool has = p.hasAudio();
    if (!has) ImGui::BeginDisabled();
    if (ImGui::Button(p.playing() ? "Pause" : "Play")) p.togglePause();
    ImGui::SameLine();
    if (ImGui::Button("Stop")) p.stop();
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &app.loopAudition)) p.setLoop(app.loopAudition);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("pre (s)", &app.preroll, 0.1f, 0.0f, 30.0f, "%.1f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("post (s)", &app.postroll, 0.1f, 0.0f, 30.0f, "%.1f");
    if (!has) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(app.audioPendingFor == app.selected ? "(loading audio...)"
                                                                : "(no audio)");
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("%s / %s", timecode(p.positionSec()).c_str(),
                            timecode(p.durationSec()).c_str());
    }
}

void drawSettings(App& app) {
    if (!app.showSettings) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 330, vp->WorkPos.y + 40),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(310, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Spectrogram Settings", &app.showSettings);
    bool changed = false;

    const char* cmaps[] = {"Cyan to orange", "Inferno", "Grayscale"};
    int cm = static_cast<int>(app.spec.colormap);
    if (ImGui::Combo("Color map", &cm, cmaps, IM_ARRAYSIZE(cmaps))) {
        app.spec.colormap = static_cast<Colormap>(cm);
        changed = true;
    }
    const char* scales[] = {"Mel", "Log", "Linear"};
    int sc = static_cast<int>(app.spec.freqScale);
    if (ImGui::Combo("Frequency scale", &sc, scales, IM_ARRAYSIZE(scales))) {
        app.spec.freqScale = static_cast<FreqScale>(sc);
        changed = true;
    }
    const char* ffts[] = {"1024", "2048", "4096", "8192"};
    int fftvals[] = {1024, 2048, 4096, 8192};
    int fi = 1;
    for (int k = 0; k < 4; ++k)
        if (fftvals[k] == app.spec.fftSize) fi = k;
    if (ImGui::Combo("FFT size", &fi, ffts, IM_ARRAYSIZE(ffts))) {
        app.spec.fftSize = fftvals[fi];
        changed = true;
    }

    float low = static_cast<float>(app.spec.dbLow), high = static_cast<float>(app.spec.dbHigh);
    ImGui::SliderFloat("Amplitude low (dB)", &low, -140.0f, -20.0f, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) { app.spec.dbLow = low; changed = true; }
    ImGui::SliderFloat("Amplitude high (dB)", &high, -20.0f, 12.0f, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) { app.spec.dbHigh = high; changed = true; }

    ImGui::Spacing();
    ImGui::TextDisabled("Legend (%.0f .. %.0f dB)", app.spec.dbLow, app.spec.dbHigh);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lw = ImGui::GetContentRegionAvail().x, lh = 16;
    for (int x = 0; x < static_cast<int>(lw); ++x) {
        unsigned char rr, gg, bb;
        colormap(app.spec.colormap, x / lw, rr, gg, bb);
        dl->AddLine(ImVec2(p.x + x, p.y), ImVec2(p.x + x, p.y + lh), IM_COL32(rr, gg, bb, 255));
    }
    ImGui::Dummy(ImVec2(lw, lh));

    if (changed) app.reRenderSelected();
    ImGui::End();
}

// Floating window collecting QA / sign-off details for exported reports. Each
// field has an "include" checkbox; a field is printed only when included and
// non-empty. Mirrors the Spectrogram Settings window's behaviour.
void drawReportInfo(App& app) {
    if (!app.showReportInfo) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 370, vp->WorkPos.y + 90),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("QA Report Details", &app.showReportInfo);
    ReportInfo& r = app.reportInfo;
    ImGui::TextDisabled("Shown on exported PDF / JSON reports.");
    ImGui::TextDisabled("Tick a row to include it; saved automatically.");
    ImGui::Spacing();

    auto field = [&](bool* show, const char* label, std::string* val, const char* hint) {
        ImGui::PushID(label);
        ImGui::Checkbox("##inc", show);
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        if (!*show) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##val", hint, val);
        if (!*show) ImGui::EndDisabled();
        ImGui::PopID();
    };

    field(&r.showEngineer, "QA engineer", &r.qaEngineer, "Full name");
    field(&r.showContact, "Contact", &r.contact, "Email or phone");
    field(&r.showStudio, "Studio / Company", &r.studio, "Organisation");
    field(&r.showClient, "Client", &r.client, "Delivered to");
    field(&r.showProject, "Project / Album", &r.project, "Release title");
    field(&r.showCatalog, "Catalog / Job no.", &r.catalog, "Reference");

    ImGui::PushID("notes");
    ImGui::Checkbox("##inc", &r.showNotes);
    ImGui::SameLine();
    ImGui::TextUnformatted("Notes");
    if (!r.showNotes) ImGui::BeginDisabled();
    ImGui::InputTextMultiline("##val", &r.notes, ImVec2(-1, 56));
    if (!r.showNotes) ImGui::EndDisabled();
    ImGui::PopID();

    ImGui::Spacing();
    ImGui::Checkbox("Include report date", &r.showDate);
    ImGui::Checkbox("Include Argus logo", &r.showLogo);
    ImGui::End();
}

void dropCallback(GLFWwindow* win, int count, const char** paths) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(win));
    for (int i = 0; i < count; ++i) app->addPath(paths[i]);
}

void openFileDialog(App& app) {
    const char* filt[] = {"*.wav", "*.aif", "*.aiff", "*.flac", "*.caf",
                          "*.mp3", "*.m4a", "*.mp4", "*.aac", "*.ogg"};
    const char* f = tinyfd_openFileDialog("Open audio file", "", 10, filt, "Audio files", 1);
    if (f) app.addPath(f);
}
void openFolderDialog(App& app) {
    const char* d = tinyfd_selectFolderDialog("Open folder of audio", "");
    if (d) app.addPath(d);
}

// Prefix a default filename with the last-used export directory, and remember the
// directory the user picks, so successive exports default to the same place.
std::string exportDefault(const App& app, const std::string& name) {
    return app.lastExportDir.empty() ? name : app.lastExportDir + "/" + name;
}
void rememberExportDir(App& app, const std::string& p) {
    auto slash = p.find_last_of("/\\");
    if (slash != std::string::npos) app.lastExportDir = p.substr(0, slash);
}

void exportPdfDialog(App& app) {
    if (app.selected < 0 || app.files[app.selected].state != 2) return;
    const Report& r = app.files[app.selected].report;
    std::string stem = app.files[app.selected].name.substr(0, app.files[app.selected].name.find_last_of('.'));
    std::string def = exportDefault(app, stem + "_QA.pdf");
    const char* filt[] = {"*.pdf"};
    const char* p = tinyfd_saveFileDialog("Export PDF report", def.c_str(), 1, filt, "PDF");
    if (p) { rememberExportDir(app, p); exportReportPdf(r, p, app.reportInfo); }
}
void exportCsvDialog(App& app) {
    if (app.selected < 0 || app.files[app.selected].state != 2) return;
    const Report& r = app.files[app.selected].report;
    std::string stem = app.files[app.selected].name.substr(0, app.files[app.selected].name.find_last_of('.'));
    std::string def = exportDefault(app, stem + "_QA.csv");
    const char* filt[] = {"*.csv"};
    const char* p = tinyfd_saveFileDialog("Export CSV", def.c_str(), 1, filt, "CSV");
    if (p) { rememberExportDir(app, p); exportReportCsv(r, p); }
}
void exportAllDialog(App& app) {
    const char* dir = tinyfd_selectFolderDialog("Export all reports (PDF) to folder",
                                                app.lastExportDir.c_str());
    if (!dir) return;
    app.lastExportDir = dir;
    int n = 0;
    for (auto& f : app.files) {
        if (f.state != 2) continue;
        std::string stem = f.name.substr(0, f.name.find_last_of('.'));
        std::string out = std::string(dir) + "/" + stem + "_QA.pdf";
        if (exportReportPdf(f.report, out, app.reportInfo)) ++n;
    }
    char msg[64];
    std::snprintf(msg, sizeof(msg), "Wrote %d PDF report(s).", n);
    tinyfd_messageBox("Export complete", msg, "ok", "info", 1);
}
void exportJsonDialog(App& app) {
    if (app.selected < 0 || app.files[app.selected].state != 2) return;
    const Report& r = app.files[app.selected].report;
    std::string stem = app.files[app.selected].name.substr(0, app.files[app.selected].name.find_last_of('.'));
    std::string def = exportDefault(app, stem + "_QA.json");
    const char* filt[] = {"*.json"};
    const char* p = tinyfd_saveFileDialog("Export JSON", def.c_str(), 1, filt, "JSON");
    if (p) { rememberExportDir(app, p); exportReportJson(r, p, app.reportInfo); }
}
void exportBatchPdfDialog(App& app) {
    std::vector<Report> reps;
    for (auto& f : app.files)
        if (f.state == 2) reps.push_back(f.report);
    if (reps.empty()) return;
    const char* filt[] = {"*.pdf"};
    std::string def = exportDefault(app, "QA_batch.pdf");
    const char* p = tinyfd_saveFileDialog("Export combined batch PDF", def.c_str(), 1, filt, "PDF");
    if (p) {
        rememberExportDir(app, p);
        bool ok = exportBatchPdf(reps, p, app.reportInfo);
        char msg[96];
        std::snprintf(msg, sizeof(msg), ok ? "Wrote a combined report for %d file(s)."
                                           : "Failed to write batch PDF.",
                      static_cast<int>(reps.size()));
        tinyfd_messageBox("Batch PDF", msg, "ok", ok ? "info" : "error", 1);
    }
}

void handleShortcuts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    bool mod = io.KeySuper || io.KeyCtrl;
    if (mod && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        openFileDialog(app);
        return;
    }
    if (app.selected < 0 || app.selected >= static_cast<int>(app.files.size())) return;
    FileEntry& fe = app.files[app.selected];
    if (fe.state != 2) return;
    const Report& r = fe.report;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (app.player.playing() || app.player.paused())
            app.player.togglePause();
        else if (app.selectedIssue >= 0)
            app.auditionIssue(r, app.selectedIssue);
        else if (app.player.hasAudio())
            app.player.play(0.0, 0.0);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        app.player.stop();
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        int n = app.stepIssue(r, +1);
        if (n >= 0) app.auditionIssue(r, n);
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
        int n = app.stepIssue(r, -1);
        if (n >= 0) app.auditionIssue(r, n);
    }
}

void drawFileList(App& app) {
    float availY = ImGui::GetContentRegionAvail().y;
    if (app.listCollapsed) {
        ImGui::BeginChild("filelist", ImVec2(30, 0), ImGuiChildFlags_Border);
        if (ImGui::SmallButton(">")) app.listCollapsed = false;
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(app.files.size()); ++i) {
            FileEntry& e = app.files[i];
            ImVec4 vcol = e.state == 2 ? severityColor(e.report.verdict())
                          : e.state == 3 ? severityColor(Severity::Fail)
                                         : ImVec4(0.6f, 0.6f, 0.6f, 1);
            ImGui::PushStyleColor(ImGuiCol_Button, vcol);
            ImGui::PushID(i);
            if (ImGui::Button("##chip", ImVec2(16, 16))) app.select(i);
            ImGui::PopID();
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        return;
    }

    ImGui::BeginChild("filelist", ImVec2(app.listWidth, 0), ImGuiChildFlags_Border);
    ImGui::TextDisabled("FILES (%d)", static_cast<int>(app.files.size()));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 16);
    if (ImGui::SmallButton("<")) app.listCollapsed = true;

    // Delivery profile selector (re-runs analysis on change).
    {
        const auto& profs = builtinProfiles();
        std::vector<const char*> names;
        names.reserve(profs.size());
        for (const auto& p : profs) names.push_back(p.name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##profile", &app.profileIndex, names.data(),
                         static_cast<int>(names.size())))
            app.reanalyzeAll();
        ImGui::TextDisabled("Delivery profile");
    }
    // Search filter + verdict sort.
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter by name...", &app.fileFilter);
    ImGui::Checkbox("Sort by verdict", &app.sortByVerdict);
    ImGui::Separator();

    // Build the display order (filtered, optionally sorted worst-verdict-first).
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string needle = toLower(app.fileFilter);
    std::vector<int> order;
    order.reserve(app.files.size());
    for (int i = 0; i < static_cast<int>(app.files.size()); ++i) {
        if (!needle.empty() && toLower(app.files[i].name).find(needle) == std::string::npos)
            continue;
        order.push_back(i);
    }
    auto rank = [&](int i) {
        const FileEntry& e = app.files[i];
        return e.state == 3 ? 3 : (e.state == 2 ? static_cast<int>(e.report.verdict()) : -1);
    };
    if (app.sortByVerdict)
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return rank(a) > rank(b); });

    for (int i : order) {
        FileEntry& e = app.files[i];
        const char* badge = e.state == 0 ? "[..]" : e.state == 1 ? "[>>]" : e.state == 3 ? "[ER]" : "";
        ImVec4 vcol = ImVec4(0.7f, 0.7f, 0.7f, 1);
        if (e.state == 2) vcol = severityColor(e.report.verdict());
        else if (e.state == 3) vcol = severityColor(Severity::Fail);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, vcol);
        std::string label = std::string(e.state == 2 ? severityLabel(e.report.verdict()) : badge);
        ImGui::Text("%-4s", label.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(48);
        if (ImGui::Selectable(e.name.c_str(), app.selected == i)) app.select(i);
        ImGui::PopID();
    }
    if (order.empty() && !app.files.empty())
        ImGui::TextDisabled("(no files match)");
    ImGui::EndChild();

    // Draggable splitter.
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton("##vsplit", ImVec2(6, availY));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        app.listWidth += ImGui::GetIO().MouseDelta.x;
        app.listWidth = std::max(140.0f, std::min(600.0f, app.listWidth));
    }
    ImVec2 sp0 = ImGui::GetItemRectMin(), sp1 = ImGui::GetItemRectMax();
    float cx = (sp0.x + sp1.x) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cx, sp0.y), ImVec2(cx, sp1.y),
        ImGui::IsItemActive() ? IM_COL32(120, 150, 220, 255) : IM_COL32(70, 70, 80, 255), 1.0f);
    ImGui::SameLine(0, 0);
}

void renderUI(App& app) {
    // Drain menu requests.
    if (app.reqOpenFile.exchange(false)) openFileDialog(app);
    if (app.reqOpenFolder.exchange(false)) openFolderDialog(app);
    if (app.reqExportPdf.exchange(false)) exportPdfDialog(app);
    if (app.reqExportCsv.exchange(false)) exportCsvDialog(app);
    if (app.reqExportJson.exchange(false)) exportJsonDialog(app);
    if (app.reqExportAll.exchange(false)) exportAllDialog(app);
    if (app.reqExportBatchPdf.exchange(false)) exportBatchPdfDialog(app);

    handleShortcuts(app);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
#ifndef __APPLE__
    wf |= ImGuiWindowFlags_MenuBar;
#endif
    ImGui::Begin("##main", nullptr, wf);

#ifndef __APPLE__
    // In-app menu bar (Windows/Linux). On macOS the native NSMenu is used instead.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open File...", "Ctrl+O")) openFileDialog(app);
            if (ImGui::MenuItem("Open Folder...")) openFolderDialog(app);
            ImGui::Separator();
            if (ImGui::MenuItem("Export PDF...")) exportPdfDialog(app);
            if (ImGui::MenuItem("Export CSV...")) exportCsvDialog(app);
            if (ImGui::MenuItem("Export JSON...")) exportJsonDialog(app);
            ImGui::Separator();
            if (ImGui::MenuItem("Export All Reports (PDF)...")) exportAllDialog(app);
            if (ImGui::MenuItem("Export Combined Batch PDF...")) exportBatchPdfDialog(app);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Spectrogram Settings", nullptr, &app.showSettings);
            ImGui::MenuItem("QA Report Details", nullptr, &app.showReportInfo);
            bool light = app.themeMode == 1;
            if (ImGui::MenuItem("Light theme", nullptr, &light)) app.themeMode = light ? 1 : 0;
            ImGui::EndMenu();
        }
        if (app.pending > 0) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.96f, 0.74f, 0.24f, 1.0f), "Analyzing %d...",
                               app.pending.load());
        }
        ImGui::EndMenuBar();
    }
#endif

    drawFileList(app);

    // Right: detail.
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Border);
    if (app.selected < 0 || app.selected >= static_cast<int>(app.files.size())) {
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1.0f), "Argus - audio master QA");
        ImGui::Spacing();
        ImGui::BulletText("Drag audio files or a folder onto this window");
        ImGui::BulletText("or use File > Open File / Open Folder");
        ImGui::BulletText("Pick a delivery profile on the left before analysing");
        ImGui::BulletText("Click a finding or a spectrogram marker to audition it");
        ImGui::BulletText("Export PDF / CSV / JSON, or a combined batch PDF");
        ImGui::Spacing();
        ImGui::TextDisabled("Supported: WAV, AIFF, FLAC, CAF, Ogg, MP3, and AAC/ALAC (.m4a).");
        if (app.pending > 0) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.96f, 0.74f, 0.24f, 1.0f), "Analyzing %d...",
                               app.pending.load());
        }
    } else {
        FileEntry& fe = app.files[app.selected];
        ImGui::Text("%s", fe.name.c_str());
        if (fe.state == 1) {
            ImGui::TextColored(ImVec4(0.96f, 0.74f, 0.24f, 1.0f), "Analyzing...");
        } else if (fe.state == 3) {
            ImGui::TextColored(severityColor(Severity::Fail), "Decode error: %s",
                               fe.report.decodeError.c_str());
        } else if (fe.state == 2) {
            const Report& r = fe.report;
            ImVec4 vc = severityColor(r.verdict());
            ImGui::SameLine();
            ImGui::TextColored(vc, "  [ %s ]", severityLabel(r.verdict()));
            ImGui::TextDisabled("%s  |  %d-bit  |  %.1f kHz  |  %d ch  |  %s  |  %d fail, %d warn",
                                r.meta.container.c_str(), r.meta.bitDepth, r.meta.sampleRate / 1000.0,
                                r.meta.channels, timecode(r.meta.durationSec).c_str(),
                                r.count(Severity::Fail), r.count(Severity::Warn));
            if (app.selectedIssue < 0) {
                for (std::size_t i = 0; i < r.issues.size(); ++i)
                    if (r.issues[i].severity >= Severity::Warn && r.issues[i].localised()) {
                        app.selectedIssue = static_cast<int>(i);
                        break;
                    }
            }

            if (ImGui::Button("Export PDF")) exportPdfDialog(app);
            ImGui::SameLine();
            if (ImGui::Button("Export CSV")) exportCsvDialog(app);
            ImGui::SameLine();
            if (ImGui::Button("Export JSON")) exportJsonDialog(app);
            ImGui::SameLine();
            if (ImGui::Button("Export All (PDF)")) exportAllDialog(app);
            ImGui::SameLine();
            if (ImGui::Button("Batch PDF")) exportBatchPdfDialog(app);
            ImGui::Separator();
            drawSpectrogram(app, fe);
            drawWaveform(app, r);
            drawTransport(app);
            ImGui::Separator();
            ImGui::BeginChild("issues");
            drawIssues(app, fe);
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
    ImGui::End();

    drawSettings(app);
    drawReportInfo(app);
}

void uploadTextures(App& app) {
    for (auto& e : app.files) {
        if (e.texDirty && e.state == 2 && !e.report.specRGBA.empty()) {
            e.tex.upload(e.report.specRGBA.data(), e.report.specWidth, e.report.specHeight);
            e.texDirty = false;
        }
    }
}

void applyStyle(int theme) {
    if (theme == 1) ImGui::StyleColorsLight();
    else ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    ImVec4* c = s.Colors;
    if (theme == 1) {
        c[ImGuiCol_WindowBg] = ImVec4(0.96f, 0.96f, 0.97f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.99f, 0.99f, 1.00f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.78f, 0.84f, 0.94f, 1.0f);
    } else {
        c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.13f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
    }
}

// Load a bundled TTF at native Retina density so text is crisp instead of the
// blurry built-in bitmap font. Searches bundle Resources then dev build dirs.
void loadFont(float contentScale) {
    ImGuiIO& io = ImGui::GetIO();
    std::vector<std::string> candidates;
#if defined(__APPLE__)
    std::string bundled = macResourcePath("JetBrainsMono-Regular.ttf");
    if (!bundled.empty()) candidates.push_back(bundled);
#endif
#if defined(__linux__)
    // Installed layout (/usr/bin/argus-gui -> /usr/share/argus/resources) and the
    // AppImage layout both resolve relative to the executable.
    {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            std::string d(exe);
            auto s = d.find_last_of('/');
            if (s != std::string::npos) {
                std::string bin = d.substr(0, s);
                candidates.push_back(bin + "/../share/argus/resources/JetBrainsMono-Regular.ttf");
                candidates.push_back(bin + "/resources/JetBrainsMono-Regular.ttf");
            }
        }
    }
#endif
    candidates.push_back("src/ui/resources/JetBrainsMono-Regular.ttf");
    candidates.push_back("../src/ui/resources/JetBrainsMono-Regular.ttf");
    candidates.push_back("../../src/ui/resources/JetBrainsMono-Regular.ttf");
    candidates.push_back("resources/JetBrainsMono-Regular.ttf");

    float px = 15.0f * contentScale;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    for (const std::string& path : candidates) {
        if (FILE* fp = std::fopen(path.c_str(), "rb")) {
            std::fclose(fp);
            if (io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg)) {
                io.FontGlobalScale = 1.0f / contentScale;
                return;
            }
        }
    }
    // Fall back to the built-in font scaled up so it is at least legible.
    io.FontGlobalScale = 1.0f;
}

}  // namespace

int main(int argc, char** argv) {
    std::string shotPath;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc)
            shotPath = argv[++i];
        else
            inputs.push_back(a);
    }

    // Restore persisted state (window geometry, settings, layout, audition prefs).
    AppState st;
    bool haveState = !shotPath.empty() ? false : loadState(st);

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    if (!shotPath.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    int winW = (haveState && st.winW > 0) ? st.winW : 1500;
    int winH = (haveState && st.winH > 0) ? st.winH : 950;
    GLFWwindow* win = glfwCreateWindow(winW, winH, "Argus", nullptr, nullptr);
    if (!win) {
        std::fprintf(stderr, "window creation failed\n");
        glfwTerminate();
        return 1;
    }
    if (haveState && st.winX >= 0 && st.winY >= 0) glfwSetWindowPos(win, st.winX, st.winY);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(win, &xscale, &yscale);
    if (xscale < 1.0f) xscale = 1.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    applyStyle(haveState ? st.theme : 0);
    loadFont(xscale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    App app;
    if (haveState) {
        app.spec.colormap = static_cast<Colormap>(st.colormap);
        app.spec.freqScale = static_cast<FreqScale>(st.freqScale);
        app.spec.fftSize = st.fftSize;
        app.spec.dbLow = st.dbLow;
        app.spec.dbHigh = st.dbHigh;
        app.listWidth = static_cast<float>(st.listWidth);
        app.listCollapsed = st.listCollapsed;
        app.showSettings = st.showSettings;
        app.preroll = static_cast<float>(st.preroll);
        app.postroll = static_cast<float>(st.postroll);
        app.loopAudition = st.loop;
        app.showReportInfo = st.showReportInfo;
        app.reportInfo = st.reportInfo;
        app.themeMode = st.theme;
        app.profileIndex = st.profile;
        app.lastExportDir = st.lastFolder;
    }
    app.start();
    glfwSetWindowUserPointer(win, &app);
    glfwSetDropCallback(win, dropCallback);

#if defined(__APPLE__)
    {
        MacMenuCallbacks cb;
        cb.openFile = [&app] { app.reqOpenFile = true; };
        cb.openFolder = [&app] { app.reqOpenFolder = true; };
        cb.exportPdf = [&app] { app.reqExportPdf = true; };
        cb.exportCsv = [&app] { app.reqExportCsv = true; };
        cb.exportJson = [&app] { app.reqExportJson = true; };
        cb.exportAll = [&app] { app.reqExportAll = true; };
        cb.exportBatchPdf = [&app] { app.reqExportBatchPdf = true; };
        cb.toggleSettings = [&app] { app.showSettings = !app.showSettings; };
        cb.toggleReportInfo = [&app] { app.showReportInfo = !app.showReportInfo; };
        cb.toggleTheme = [&app] { app.themeMode ^= 1; };
        cb.quit = [win] { glfwSetWindowShouldClose(win, GLFW_TRUE); };
        installMacMenu(cb);
    }
#endif

    for (const auto& in : inputs) app.addPath(in);

    // Screenshot composition hooks (only used with --shot, for the README images).
    if (!shotPath.empty()) {
        if (std::getenv("ARGUS_SHOT_NOSETTINGS")) app.showSettings = false;
        if (std::getenv("ARGUS_SHOT_QA")) {
            app.showReportInfo = true;
            app.reportInfo.qaEngineer = "Alex Rivera";
            app.reportInfo.contact = "alex@northgate.audio";
            app.reportInfo.studio = "Northgate Mastering";
            app.reportInfo.client = "Cadenza Records";
            app.reportInfo.project = "Nocturnes, Vol. II";
            app.reportInfo.catalog = "CAD-0142";
            app.reportInfo.notes = "Approved for delivery; dropout at 0:05 flagged to recording eng.";
        }
    }

    int framesSinceDone = 0;
    int appliedTheme = haveState ? st.theme : 0;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        app.drainResults();
        uploadTextures(app);
        if (app.themeMode != appliedTheme) {
            applyStyle(app.themeMode);
            appliedTheme = app.themeMode;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        renderUI(app);
        ImGui::Render();

        int dw, dh;
        glfwGetFramebufferSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!shotPath.empty()) {
            bool ready = app.pending == 0 && !app.files.empty() && app.files[0].state >= 2;
            if (ready) ++framesSinceDone;
            if (ready && framesSinceDone >= 4) {
                std::vector<unsigned char> px(static_cast<std::size_t>(dw) * dh * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadBuffer(GL_BACK);
                glReadPixels(0, 0, dw, dh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                FILE* fp = std::fopen(shotPath.c_str(), "wb");
                if (fp) {
                    std::fprintf(fp, "P6\n%d %d\n255\n", dw, dh);
                    for (int y = dh - 1; y >= 0; --y)
                        std::fwrite(&px[static_cast<std::size_t>(y) * dw * 3], 1, dw * 3, fp);
                    std::fclose(fp);
                    std::printf("wrote screenshot %dx%d to %s\n", dw, dh, shotPath.c_str());
                }
                glfwSwapBuffers(win);
                break;
            }
        }
        glfwSwapBuffers(win);
    }

    // Persist state (skip in headless screenshot mode so we don't clobber it).
    if (shotPath.empty()) {
        glfwGetWindowPos(win, &st.winX, &st.winY);
        glfwGetWindowSize(win, &st.winW, &st.winH);
        st.colormap = static_cast<int>(app.spec.colormap);
        st.freqScale = static_cast<int>(app.spec.freqScale);
        st.fftSize = app.spec.fftSize;
        st.dbLow = app.spec.dbLow;
        st.dbHigh = app.spec.dbHigh;
        st.listWidth = app.listWidth;
        st.listCollapsed = app.listCollapsed;
        st.showSettings = app.showSettings;
        st.preroll = app.preroll;
        st.postroll = app.postroll;
        st.loop = app.loopAudition;
        st.showReportInfo = app.showReportInfo;
        st.reportInfo = app.reportInfo;
        st.theme = app.themeMode;
        st.profile = app.profileIndex;
        st.lastFolder = app.lastExportDir;
        saveState(st);
    }

    app.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
