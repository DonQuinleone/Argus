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

#include "ImageDecode.h"
#include "Player.h"
#include "Settings.h"
#include "Texture.h"
#include "core/Decoder.h"
#include "core/Engine.h"
#include "core/Profile.h"
#include "core/ProfileStore.h"
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

// User-customisable primary/accent colour (Appearance settings). Drives buttons, window
// headers, the waveform, markers and the splitter. Severity colours stay semantic.
ImVec4 gAccent = ImVec4(0.30f, 0.55f, 0.85f, 1.0f);
void applyStyle(int theme);  // defined below; used by the Appearance picker
inline ImVec4 accentCol() { return gAccent; }
inline ImU32 accentU32(float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, alpha));
}
// Pin a floating window's width (resizable vertically only).
inline void lockWindowWidth(float w) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 80.0f), ImVec2(w, 100000.0f));
}

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
    // The delivery profile for THIS file. Empty = auto-pick from the detected layout on the
    // next analysis; once resolved it is filled in with the profile actually used.
    std::string profileName;
    Report report;
    Texture tex;
    bool texDirty = false;
    std::map<int, Texture> closeupTex;  // keyed by issue index
    Texture artworkTex;                 // decoded embedded cover art
    bool artworkTried = false;          // decode attempted (success or not)
    Texture goniTex;                    // goniometer raster
    bool goniUploaded = false;
    Texture dcTex;                      // DC-offset bar meter raster
    bool dcUploaded = false;
    std::vector<Texture> channelSpecTex;  // per-channel spectrogram strips
    bool profileMismatchAck = false;      // user has acknowledged the layout-mismatch modal
};

struct Job {
    int kind = kAnalyze;
    int index = 0;
    std::string path;
    SpectrogramSettings settings;
    Profile profile;
    bool autoProfile = false;  // pick the profile from the detected layout
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
    // One-shot: when a finding is selected programmatically (audition, keyboard step,
    // auto-select) we force its disclosure open for a single frame. Header clicks do NOT
    // set this, so ImGui owns the open/close state and a second click can collapse it.
    int issueToExpand = -1;
    SpectrogramSettings spec;
    bool showSettings = false;

    // QA / sign-off details for exported reports, and its floating window.
    ReportInfo reportInfo;
    bool showReportInfo = false;
    int themeMode = 0;      // 0 = dark, 1 = light
    int profileIndex = 0;   // selected delivery profile (index into builtins + custom)

    // User-defined delivery profiles (loaded from disk), and the editor window state.
    std::vector<Profile> customProfiles;
    bool showProfiles = false;
    int profileEditSel = -1;       // index into customProfiles currently being edited
    std::string profileRatesBuf;   // comma-separated approved rates for the edited profile

    // Other floating tool windows.
    bool showChannels = false;  // multitrack / all-channels window
    bool showMetadata = false;  // embedded-metadata window
    bool showShortcuts = false; // keyboard-shortcuts window
    bool popOutResults = false; // results/findings in their own floating window
    Texture orgLogoTex;          // preview of the QA company logo
    std::string orgLogoTexFor;   // path the preview texture was built from

    // Audition.
    Player player;
    int audioLoadedFor = -1;   // file index whose audio is in the player
    int audioPendingFor = -1;  // file index whose audio decode is in flight
    float preroll = 2.0f;
    float postroll = 2.0f;
    bool loopAudition = false;

    // Left pane layout.
    float listWidth = 280.0f;
    std::string fileFilter;        // case-insensitive name filter
    bool sortByVerdict = false;    // sort the list worst-verdict-first
    std::string lastExportDir;     // remembered for export save dialogs

    // Deferred actions requested from the native menu (run on the UI thread).
    std::atomic<bool> reqOpenFile{false}, reqOpenFolder{false}, reqExportPdf{false};
    std::atomic<bool> reqExportCsv{false}, reqExportJson{false}, reqExportAll{false};
    std::atomic<bool> reqExportBatchPdf{false}, reqExportSpecPng{false};

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
                msg.report = analyzeFileFull(job.path, kSpecW, kSpecH, job.settings, job.profile,
                                             job.autoProfile);
            } else {
                DecodeResult dec = decodeFile(job.path);
                msg.audioOk = dec.ok;
                if (dec.ok) msg.audio = std::move(dec.buffer);
            }
            std::lock_guard<std::mutex> lk(resMx);
            results.push(std::move(msg));
        }
    }

    // Built-in profiles followed by the user's custom profiles, in dropdown order.
    int builtinCount() const { return static_cast<int>(builtinProfiles().size()); }
    int profileCount() const { return builtinCount() + static_cast<int>(customProfiles.size()); }

    const Profile& currentProfile() const {
        const auto& b = builtinProfiles();
        if (profileIndex >= 0 && profileIndex < builtinCount()) return b[profileIndex];
        int ci = profileIndex - builtinCount();
        if (ci >= 0 && ci < static_cast<int>(customProfiles.size())) return customProfiles[ci];
        return b[0];
    }

    const std::string& profileNameAt(int idx) const {
        static const std::string empty;
        const auto& b = builtinProfiles();
        if (idx >= 0 && idx < builtinCount()) return b[idx].name;
        int ci = idx - builtinCount();
        if (ci >= 0 && ci < static_cast<int>(customProfiles.size())) return customProfiles[ci].name;
        return empty;
    }

    // Resolve a profile by name across built-ins + customs (default if not found).
    Profile resolveProfile(const std::string& name) const {
        for (const auto& p : builtinProfiles())
            if (p.name == name) return p;
        for (const auto& p : customProfiles)
            if (p.name == name) return p;
        return defaultProfile();
    }
    // Index into the merged dropdown list for a profile name, or -1.
    int profileIndexForName(const std::string& name) const {
        for (int i = 0; i < profileCount(); ++i)
            if (profileNameAt(i) == name) return i;
        return -1;
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
            // Empty file profile => auto-pick from the detected layout; else use the chosen one.
            if (files[index].profileName.empty()) {
                j.autoProfile = true;
                j.profile = currentProfile();  // placeholder; engine overrides by layout
            } else {
                j.profile = resolveProfile(files[index].profileName);
            }
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

    // Remove a file from the list (after confirm). Frees its GPU textures first.
    void removeFile(int idx) {
        if (idx < 0 || idx >= static_cast<int>(files.size())) return;
        FileEntry& fe = files[idx];
        fe.tex.destroy();
        fe.artworkTex.destroy();
        fe.goniTex.destroy();
        for (auto& kv : fe.closeupTex) kv.second.destroy();
        for (auto& t : fe.channelSpecTex) t.destroy();
        files.erase(files.begin() + idx);
        if (selected == idx) {
            selected = -1;
            selectedIssue = -1;
            player.stop();
            player.clear();
        } else if (selected > idx) {
            --selected;
        }
        audioLoadedFor = -1;
        audioPendingFor = -1;
        if (selected < 0 && !files.empty()) select(0);
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
                // Lock in the profile the engine actually used (auto-picked or explicit).
                if (!files[m.index].report.usedProfileName.empty())
                    files[m.index].profileName = files[m.index].report.usedProfileName;
                files[m.index].texDirty = true;
                files[m.index].closeupTex.clear();
                files[m.index].artworkTex.destroy();
                files[m.index].artworkTried = false;
                files[m.index].goniTex.destroy();
                files[m.index].goniUploaded = false;
                files[m.index].dcTex.destroy();
                files[m.index].dcUploaded = false;
                for (auto& t : files[m.index].channelSpecTex) t.destroy();
                files[m.index].channelSpecTex.clear();
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
        issueToExpand = issueIdx;
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
    // When the results are popped out there's no findings list below, so let the
    // spectrogram grow to fill the view (reserving room for the waveform + transport).
    float h = app.popOutResults ? std::max(200.0f, avail.y - 150.0f)
                                : std::min(avail.y - 4.0f, 420.0f);
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
        // Extra recurring markers (e.g. every click location).
        for (double mt : is.marks) {
            float mx = plotPos.x + static_cast<float>(mt / dur) * plotW;
            dl->AddLine(ImVec2(mx, plotPos.y), ImVec2(mx, plotPos.y + h),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(cc.x, cc.y, cc.z, 0.7f)), 1.0f);
        }
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

// Drawn by hand (not ImPlot) so its plot area shares the spectrogram's exact geometry
// (same 52 px gutter, plot width and left edge) and therefore lines up pixel-for-pixel -
// issue markers and the playhead align vertically across both views.
void drawWaveform(App& app, const Report& r) {
    if (r.overview.empty()) return;
    const float h = 90.0f;
    const float axisW = 52.0f;  // matches drawSpectrogram
    float w = ImGui::GetContentRegionAvail().x;
    float plotW = w - axisW;
    if (plotW < 1.0f) return;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 plotPos = ImVec2(origin.x + axisW, origin.y);
    float yc = plotPos.y + h * 0.5f;
    float halfH = h * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Plot background + frame.
    dl->AddRectFilled(plotPos, ImVec2(plotPos.x + plotW, plotPos.y + h), IM_COL32(18, 18, 22, 255));

    // Amplitude gridlines + labels in the gutter (mirrors drawFreqAxis styling).
    const double amps[] = {1.0, 0.5, 0.0, -0.5, -1.0};
    for (double a : amps) {
        float y = yc - static_cast<float>(a) * halfH;
        dl->AddLine(ImVec2(plotPos.x, y), ImVec2(plotPos.x + plotW, y),
                    a == 0.0 ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 18));
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%+.1f", a);
        dl->AddText(ImVec2(origin.x + 4, y - 6), IM_COL32(150, 150, 155, 255), lbl);
    }

    double dur = r.specDuration > 0 ? r.specDuration : 1.0;

    // Warn/Fail issue regions (same colours/mapping as the spectrogram).
    for (const auto& is : r.issues) {
        if (!is.localised() || is.severity == Severity::Pass || is.severity == Severity::Info)
            continue;
        float x0 = plotPos.x + static_cast<float>(is.tStart / dur) * plotW;
        float x1 = is.tEnd > is.tStart ? plotPos.x + static_cast<float>(is.tEnd / dur) * plotW
                                       : x0 + 1.0f;
        ImVec4 c = severityColor(is.severity);
        dl->AddRectFilled(ImVec2(x0, plotPos.y), ImVec2(std::max(x1, x0 + 1.0f), plotPos.y + h),
                          ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.22f)));
        for (double mt : is.marks) {
            float mx = plotPos.x + static_cast<float>(mt / dur) * plotW;
            dl->AddLine(ImVec2(mx, plotPos.y), ImVec2(mx, plotPos.y + h),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.7f)), 1.0f);
        }
    }

    // Peak envelope: one vertical line per pixel column, sampled from the overview.
    const std::size_t n = r.overview.size();
    const ImU32 wcol = accentU32(0.92f);
    for (int px = 0; px < static_cast<int>(plotW); ++px) {
        std::size_t idx = static_cast<std::size_t>(px / plotW * n);
        if (idx >= n) idx = n - 1;
        float amp = r.overview[idx];
        float x = plotPos.x + px;
        dl->AddLine(ImVec2(x, yc - amp * halfH), ImVec2(x, yc + amp * halfH), wcol);
    }

    // Playhead.
    if (app.player.playing() || app.player.paused()) {
        float pxh = plotPos.x + static_cast<float>(app.player.positionSec() / dur) * plotW;
        dl->AddLine(ImVec2(pxh, plotPos.y), ImVec2(pxh, plotPos.y + h), IM_COL32(255, 255, 255, 230),
                    1.5f);
    }

    // Click to seek / audition, mirroring the spectrogram's hit behaviour.
    ImGui::SetCursorScreenPos(plotPos);
    ImGui::InvisibleButton("##wavehit", ImVec2(plotW, h));
    if (ImGui::IsItemClicked() && app.player.hasAudio()) {
        double tClick = (ImGui::GetIO().MousePos.x - plotPos.x) / plotW * dur;
        app.player.setLoop(app.loopAudition);
        app.player.play(std::max(0.0, tClick), 0.0);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, plotPos.y + h + 4));
    ImGui::Dummy(ImVec2(w, 0));
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

    // Clipping close-ups: a waveform strip below the spectrogram with the clip-level line,
    // so the flat-topped peaks and the ceiling they hit are directly visible.
    if (cv.hasWave && !cv.waveMax.empty()) {
        const float wh = 80.0f;
        // Extra gap so the strip clears the spectrogram's time-label row beneath it.
        ImVec2 wp = ImVec2(plotPos.x, plotPos.y + h + 24);
        float yc = wp.y + wh * 0.5f, half = wh * 0.5f - 2.0f;
        dl->AddRectFilled(wp, ImVec2(wp.x + plotW, wp.y + wh), IM_COL32(18, 18, 22, 255));
        dl->AddLine(ImVec2(wp.x, yc), ImVec2(wp.x + plotW, yc), IM_COL32(255, 255, 255, 30));
        float cl = static_cast<float>(cv.clipLevel);
        if (cl > 0.0f) {
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                float y = yc - sgn * cl * half;
                dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + plotW, y), IM_COL32(255, 80, 70, 210), 1.0f);
            }
            // Label inside the strip's top-left so it never collides with the time labels.
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "clip %.1f dBFS", toDb(cl));
            dl->AddText(ImVec2(wp.x + 4, wp.y + 2), IM_COL32(255, 120, 110, 255), lbl);
        }
        int cols = static_cast<int>(cv.waveMax.size());
        for (int px = 0; px < static_cast<int>(plotW); ++px) {
            int idx = px * cols / std::max(1, static_cast<int>(plotW));
            if (idx >= cols) idx = cols - 1;
            dl->AddLine(ImVec2(wp.x + px, yc - cv.waveMax[idx] * half),
                        ImVec2(wp.x + px, yc - cv.waveMin[idx] * half), accentU32(0.92f));
        }
        if (span > 0) {
            float ex0 = wp.x + static_cast<float>((cv.evStart - cv.winStart) / span) * plotW;
            float ex1 = wp.x + static_cast<float>((cv.evEnd - cv.winStart) / span) * plotW;
            if (ex1 < ex0 + 2) ex1 = ex0 + 2;
            dl->AddRect(ImVec2(ex0, wp.y), ImVec2(ex1, wp.y + wh), IM_COL32(255, 70, 60, 150));
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, wp.y + wh + 4));
        ImGui::Dummy(ImVec2(w, 0));
    } else {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, plotPos.y + h + 18));
        ImGui::Dummy(ImVec2(w, 0));
    }
}

// Goniometer (vectorscope) + correlation-over-time timeline, shown under the Phase finding.
void drawPhaseViz(FileEntry& fe) {
    const Report& r = fe.report;
    if (r.goniW <= 0 || r.goniRGBA.empty()) return;
    if (!fe.goniUploaded) {
        fe.goniUploaded = true;
        fe.goniTex.upload(r.goniRGBA.data(), r.goniW, r.goniH);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float gon = 240.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (fe.goniTex.valid())
        ImGui::Image((ImTextureID)(intptr_t)fe.goniTex.id, ImVec2(gon, gon));
    ImGui::SameLine();

    // Correlation timeline to the right of the scope (capped so it isn't huge).
    ImVec2 cp = ImGui::GetCursorScreenPos();
    float cw = std::max(120.0f, std::min(420.0f, ImGui::GetContentRegionAvail().x - 8.0f));
    float chh = gon;
    dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + chh), IM_COL32(18, 18, 22, 255));
    float midY = cp.y + chh * 0.5f;
    dl->AddLine(ImVec2(cp.x, midY), ImVec2(cp.x + cw, midY), IM_COL32(255, 255, 255, 40));  // 0
    dl->AddLine(ImVec2(cp.x, cp.y + 1), ImVec2(cp.x + cw, cp.y + 1), IM_COL32(120, 200, 120, 50));  // +1
    dl->AddLine(ImVec2(cp.x, cp.y + chh - 1), ImVec2(cp.x + cw, cp.y + chh - 1),
                IM_COL32(220, 120, 120, 50));  // -1
    const auto& cor = r.correlation;
    for (std::size_t k = 0; k + 1 < cor.size(); ++k) {
        float x0 = cp.x + cw * k / cor.size();
        float x1 = cp.x + cw * (k + 1) / cor.size();
        float y0 = midY - cor[k] * (chh * 0.5f - 2);
        float y1 = midY - cor[k + 1] * (chh * 0.5f - 2);
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), accentU32(0.9f), 1.2f);
    }
    ImGui::Dummy(ImVec2(cw, chh));
    ImGui::TextDisabled("Goniometer (vertical = in phase, horizontal = anti-phase) + correlation "
                        "over time (+1 mono → −1 inverted).");
}

// Per-channel DC-offset bar meter, shown under the DC offset finding.
void drawDcViz(FileEntry& fe) {
    const Report& r = fe.report;
    if (r.dcMeterW <= 0 || r.dcMeterRGBA.empty()) return;
    if (!fe.dcUploaded) {
        fe.dcUploaded = true;
        fe.dcTex.upload(r.dcMeterRGBA.data(), r.dcMeterW, r.dcMeterH);
    }
    if (fe.dcTex.valid())
        ImGui::Image((ImTextureID)(intptr_t)fe.dcTex.id,
                     ImVec2(static_cast<float>(r.dcMeterW), static_cast<float>(r.dcMeterH)));
    ImGui::TextDisabled("Per-channel DC bias (centre = 0; bars past the amber ticks are flagged).");
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

        // Force open only as a one-shot for programmatic selection; clearing it lets
        // ImGui own the state afterwards so the user can collapse the node again.
        if (static_cast<int>(i) == app.issueToExpand) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            app.issueToExpand = -1;
        }
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
            if (is.check == "Phase / mono") drawPhaseViz(fe);
            if (is.check == "DC offset") drawDcViz(fe);
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
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 400, vp->WorkPos.y + 40),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    lockWindowWidth(380);
    ImGui::Begin("Spectrogram Settings", &app.showSettings);
    // Reserve room on the right for the (long) labels so they aren't clipped.
    ImGui::PushItemWidth(-150.0f);
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

    // ---- Appearance: theme + UI accent colour ----
    ImGui::SeparatorText("Appearance");
    {
        int th = app.themeMode;
        if (ImGui::RadioButton("Dark", &th, 0)) { app.themeMode = 0; applyStyle(0); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Light", &th, 1)) { app.themeMode = 1; applyStyle(1); }
    }
    float acc[3] = {gAccent.x, gAccent.y, gAccent.z};
    if (ImGui::ColorEdit3("Accent colour", acc, ImGuiColorEditFlags_NoInputs)) {
        gAccent = ImVec4(acc[0], acc[1], acc[2], 1.0f);
        applyStyle(app.themeMode);
    }
    ImGui::TextDisabled("Presets:");
    struct Preset { const char* name; ImVec4 col; };
    static const Preset presets[] = {
        {"Argus blue", ImVec4(0.30f, 0.55f, 0.85f, 1)}, {"Teal", ImVec4(0.20f, 0.66f, 0.62f, 1)},
        {"Violet", ImVec4(0.55f, 0.42f, 0.85f, 1)},     {"Amber", ImVec4(0.85f, 0.62f, 0.25f, 1)},
        {"Rose", ImVec4(0.85f, 0.40f, 0.50f, 1)},       {"Green", ImVec4(0.36f, 0.72f, 0.42f, 1)},
    };
    for (int i = 0; i < IM_ARRAYSIZE(presets); ++i) {
        ImGui::PushID(i);
        if (ImGui::ColorButton(presets[i].name, presets[i].col, 0, ImVec2(22, 22))) {
            gAccent = presets[i].col;
            applyStyle(app.themeMode);
        }
        ImGui::PopID();
        if ((i + 1) % 6 != 0) ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::PopItemWidth();
    if (changed) app.reRenderSelected();
    ImGui::End();
}

// Pick an image file for the organisation logo.
std::string uiOpenImage() {
#if defined(__APPLE__)
    auto v = macOpenFiles("Choose company logo", {"png", "jpg", "jpeg"}, false);
    return v.empty() ? std::string() : v[0];
#else
    const char* filt[] = {"*.png", "*.jpg", "*.jpeg"};
    const char* p = tinyfd_openFileDialog("Choose company logo", "", 3, filt, "Images", 0);
    return p ? std::string(p) : std::string();
#endif
}

// Decode the chosen logo to RGB (composited on white) and store it on the ReportInfo so the
// PDF exporter can embed it without needing an image decoder in core.
bool loadOrgLogo(ReportInfo& r, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::fseek(fp, 0, SEEK_END);
    long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<unsigned char> data(n > 0 ? static_cast<std::size_t>(n) : 0);
    if (n > 0) { if (std::fread(data.data(), 1, data.size(), fp) != data.size()) { /*best effort*/ } }
    std::fclose(fp);
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!decodeImageRGBA(data.data(), static_cast<int>(data.size()), rgba, w, h)) return false;
    r.orgLogoRGB.assign(static_cast<std::size_t>(w) * h * 3, 255);
    for (int i = 0; i < w * h; ++i) {
        float a = rgba[i * 4 + 3] / 255.0f;
        for (int k = 0; k < 3; ++k)
            r.orgLogoRGB[i * 3 + k] =
                static_cast<unsigned char>(rgba[i * 4 + k] * a + 255.0f * (1.0f - a));
    }
    r.orgLogoW = w;
    r.orgLogoH = h;
    r.orgLogoPath = path;
    return true;
}

// Floating window collecting QA / sign-off details for exported reports. Each
// field has an "include" checkbox; a field is printed only when included and
// non-empty. Mirrors the Spectrogram Settings window's behaviour.
void drawReportInfo(App& app) {
    if (!app.showReportInfo) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Below the Spectrogram Settings window so the two don't overlap on first open.
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 360, vp->WorkPos.y + 430),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
    lockWindowWidth(350);
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
    ImGui::Checkbox("Include all diagrams (not just warn/fail)", &r.allDiagrams);
    ImGui::Spacing();
    ImGui::TextDisabled("Company logo (shown in the report header)");
    // Preview the chosen logo (build an RGBA texture from the stored RGB, once per path).
    if (!r.orgLogoRGB.empty() && r.orgLogoW > 0) {
        if (app.orgLogoTexFor != r.orgLogoPath || !app.orgLogoTex.valid()) {
            std::vector<unsigned char> rgba(static_cast<std::size_t>(r.orgLogoW) * r.orgLogoH * 4);
            for (int i = 0; i < r.orgLogoW * r.orgLogoH; ++i) {
                rgba[i * 4 + 0] = r.orgLogoRGB[i * 3 + 0];
                rgba[i * 4 + 1] = r.orgLogoRGB[i * 3 + 1];
                rgba[i * 4 + 2] = r.orgLogoRGB[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
            app.orgLogoTex.upload(rgba.data(), r.orgLogoW, r.orgLogoH);
            app.orgLogoTexFor = r.orgLogoPath;
        }
        if (app.orgLogoTex.valid()) {
            float hh = 56.0f, ww = hh * r.orgLogoW / r.orgLogoH;
            ImGui::Image((ImTextureID)(intptr_t)app.orgLogoTex.id, ImVec2(ww, hh));
        }
    }
    if (ImGui::Button("Choose logo...")) {
        std::string p = uiOpenImage();
        if (!p.empty() && !loadOrgLogo(r, p))
            tinyfd_messageBox("Logo", "Could not load that image.", "ok", "error", 1);
    }
    if (!r.orgLogoPath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            r.orgLogoPath.clear();
            r.orgLogoRGB.clear();
            r.orgLogoW = r.orgLogoH = 0;
            app.orgLogoTexFor.clear();
        }
    }
    ImGui::End();
}

void dropCallback(GLFWwindow* win, int count, const char** paths) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(win));
    for (int i = 0; i < count; ++i) app->addPath(paths[i]);
}

// ---- Platform file dialogs ----
// On macOS use native Cocoa panels (focused + in front); elsewhere fall back to
// tinyfiledialogs. Extensions are bare ("pdf"); the save helper builds the filter.
std::string uiSaveFile(const char* title, const std::string& def, const char* ext) {
#if defined(__APPLE__)
    return macSaveFile(title, def, ext);
#else
    std::string pat = std::string("*.") + ext;
    const char* filt[] = {pat.c_str()};
    const char* p = tinyfd_saveFileDialog(title, def.c_str(), 1, filt, ext);
    return p ? std::string(p) : std::string();
#endif
}
std::vector<std::string> uiOpenAudioFiles() {
#if defined(__APPLE__)
    return macOpenFiles("Open audio file(s)",
                        {"wav", "wave", "aif", "aiff", "aifc", "flac", "caf", "w64", "rf64",
                         "ogg", "oga", "opus", "mp3", "m4a", "mp4", "aac", "m4b"},
                        true);
#else
    const char* filt[] = {"*.wav", "*.aif", "*.aiff", "*.flac", "*.caf",
                          "*.mp3", "*.m4a", "*.mp4", "*.aac", "*.ogg"};
    const char* f = tinyfd_openFileDialog("Open audio file", "", 10, filt, "Audio files", 0);
    std::vector<std::string> out;
    if (f) out.push_back(f);
    return out;
#endif
}
std::string uiOpenFolder(const char* title, const std::string& defDir) {
#if defined(__APPLE__)
    return macOpenFolder(title, defDir);
#else
    const char* d = tinyfd_selectFolderDialog(title, defDir.c_str());
    return d ? std::string(d) : std::string();
#endif
}

void openFileDialog(App& app) {
    for (const auto& f : uiOpenAudioFiles()) app.addPath(f);
}
void openFolderDialog(App& app) {
    std::string d = uiOpenFolder("Open folder of audio", "");
    if (!d.empty()) app.addPath(d);
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
    std::string p = uiSaveFile("Export PDF report", exportDefault(app, stem + "_QA.pdf"), "pdf");
    if (!p.empty()) { rememberExportDir(app, p); exportReportPdf(r, p, app.reportInfo); }
}
void exportCsvDialog(App& app) {
    if (app.selected < 0 || app.files[app.selected].state != 2) return;
    const Report& r = app.files[app.selected].report;
    std::string stem = app.files[app.selected].name.substr(0, app.files[app.selected].name.find_last_of('.'));
    std::string p = uiSaveFile("Export CSV", exportDefault(app, stem + "_QA.csv"), "csv");
    if (!p.empty()) { rememberExportDir(app, p); exportReportCsv(r, p); }
}
void exportAllDialog(App& app) {
    std::string dir = uiOpenFolder("Export all reports (PDF) to folder", app.lastExportDir);
    if (dir.empty()) return;
    app.lastExportDir = dir;
    int n = 0;
    for (auto& f : app.files) {
        if (f.state != 2) continue;
        std::string stem = f.name.substr(0, f.name.find_last_of('.'));
        std::string out = dir + "/" + stem + "_QA.pdf";
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
    std::string p = uiSaveFile("Export JSON", exportDefault(app, stem + "_QA.json"), "json");
    if (!p.empty()) { rememberExportDir(app, p); exportReportJson(r, p, app.reportInfo); }
}
void exportSpectrogramPngDialog(App& app) {
    if (app.selected < 0 || app.files[app.selected].state != 2) return;
    const Report& r = app.files[app.selected].report;
    if (r.specRGBA.empty()) {
        tinyfd_messageBox("Export Spectrogram", "No spectrogram available for this file.", "ok",
                          "warning", 1);
        return;
    }
    std::string stem = app.files[app.selected].name.substr(0, app.files[app.selected].name.find_last_of('.'));
    std::string p =
        uiSaveFile("Export Spectrogram PNG", exportDefault(app, stem + "_spectrogram.png"), "png");
    if (!p.empty()) { rememberExportDir(app, p); exportSpectrogramPng(r, p); }
}
void exportBatchPdfDialog(App& app) {
    std::vector<Report> reps;
    for (auto& f : app.files)
        if (f.state == 2) reps.push_back(f.report);
    if (reps.empty()) return;
    std::string p = uiSaveFile("Export combined batch PDF", exportDefault(app, "QA_batch.pdf"), "pdf");
    if (!p.empty()) {
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

// A full-width toggle button that shows an explicit open/closed state and tints while its
// window is open.
void windowToggleButton(const char* label, bool& flag) {
    // Cache the pushed state: Button() may flip `flag`, so the pop must match the push.
    const bool tinted = flag;
    if (tinted) ImGui::PushStyleColor(ImGuiCol_Button, accentCol());
    std::string lbl = std::string(flag ? "[x] " : "[ ] ") + label;
    if (ImGui::Button(lbl.c_str(), ImVec2(-1, 0))) flag = !flag;
    if (tinted) ImGui::PopStyleColor();
}

void drawFileList(App& app) {
    float availY = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("filelist", ImVec2(app.listWidth, 0), ImGuiChildFlags_Border);

    // Open controls (no menu bar).
    float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Open File", ImVec2(halfW, 0))) openFileDialog(app);
    ImGui::SameLine();
    if (ImGui::Button("Open Folder", ImVec2(halfW, 0))) openFolderDialog(app);
    ImGui::TextDisabled("FILES (%d)", static_cast<int>(app.files.size()));
    if (app.pending > 0) {
        ImGui::SameLine();
        ImGui::TextColored(accentCol(), "analyzing %d...", app.pending.load());
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

    // Scrolling file rows, leaving room for the current-profile line + window-toggle bar.
    const int kToggleCount = 7;
    const float kBarH = kToggleCount * ImGui::GetFrameHeightWithSpacing() +
                        ImGui::GetTextLineHeightWithSpacing() + 3 * ImGui::GetStyle().ItemSpacing.y;
    int toRemove = -1;
    ImGui::BeginChild("##rows", ImVec2(0, -kBarH), false);
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
        float selW = std::max(40.0f, ImGui::GetContentRegionAvail().x - 22.0f);
        if (ImGui::Selectable(e.name.c_str(), app.selected == i, 0, ImVec2(selW, 0))) app.select(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) ImGui::OpenPopup("rm");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from list");
        if (ImGui::BeginPopup("rm")) {  // confirm fly-out
            ImGui::TextUnformatted("Remove from list?");
            if (ImGui::SmallButton("Remove")) { toRemove = i; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (order.empty() && !app.files.empty())
        ImGui::TextDisabled("(no files match)");
    ImGui::EndChild();
    if (toRemove >= 0) app.removeFile(toRemove);

    // Current profile for the selected file.
    ImGui::Separator();
    if (app.selected >= 0 && app.selected < static_cast<int>(app.files.size()) &&
        !app.files[app.selected].profileName.empty())
        ImGui::TextDisabled("Profile: %s", app.files[app.selected].profileName.c_str());
    else
        ImGui::TextDisabled("Profile: -");

    // Window-toggle bar: open/close the floating tool windows (stacked, with state).
    windowToggleButton("Spectrogram", app.showSettings);
    windowToggleButton("QA Details", app.showReportInfo);
    windowToggleButton("Delivery Profiles", app.showProfiles);
    windowToggleButton("Channels", app.showChannels);
    windowToggleButton("Metadata", app.showMetadata);
    windowToggleButton("Pop out results", app.popOutResults);
    windowToggleButton("Shortcuts", app.showShortcuts);
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
        ImGui::IsItemActive() ? accentU32(1.0f) : IM_COL32(70, 70, 80, 255), 1.0f);
    ImGui::SameLine(0, 0);
}

// Sync the rates edit buffer from the currently-selected custom profile.
void syncProfileRatesBuf(App& app) {
    app.profileRatesBuf.clear();
    if (app.profileEditSel < 0 || app.profileEditSel >= static_cast<int>(app.customProfiles.size()))
        return;
    const auto& rates = app.customProfiles[app.profileEditSel].approvedRates;
    for (std::size_t i = 0; i < rates.size(); ++i) {
        if (i) app.profileRatesBuf += ", ";
        app.profileRatesBuf += std::to_string(rates[i]);
    }
}

// Persist custom profiles and re-run analysis so changes take effect immediately.
void saveAndApplyProfiles(App& app) {
    saveCustomProfiles(app.customProfiles);
    app.reanalyzeAll();
}

// Floating window showing all embedded metadata for the selected file: cover artwork,
// every descriptive tag (RIFF INFO + ID3), Broadcast Wave (bext) fields and ADM/Atmos info.
void drawMetadataWindow(App& app) {
    if (!app.showMetadata) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 420, vp->WorkPos.y + 120), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_FirstUseEver);
    lockWindowWidth(460);
    if (!ImGui::Begin("Metadata", &app.showMetadata)) {
        ImGui::End();
        return;
    }
    if (app.selected < 0 || app.selected >= static_cast<int>(app.files.size()) ||
        app.files[app.selected].state != 2) {
        ImGui::TextDisabled("Select an analysed file to see its embedded metadata.");
        ImGui::End();
        return;
    }
    FileEntry& fe = app.files[app.selected];
    const FileMetadata& m = fe.report.meta;
    const TagInfo& tg = m.tags;
    const BroadcastInfo& bi = m.broadcast;

    if (!tg.any() && !bi.present && m.adm.present == false) {
        ImGui::TextDisabled("No embedded metadata in this file.");
        ImGui::End();
        return;
    }

    // Cover art (decoded once into a texture, cached on the FileEntry).
    if (!tg.artwork.empty()) {
        if (!fe.artworkTried) {
            fe.artworkTried = true;
            std::vector<unsigned char> rgba;
            int iw = 0, ih = 0;
            if (decodeImageRGBA(tg.artwork.data(), static_cast<int>(tg.artwork.size()), rgba, iw, ih))
                fe.artworkTex.upload(rgba.data(), iw, ih);
        }
        if (fe.artworkTex.valid()) {
            float side = 200.0f;
            float aspect = static_cast<float>(fe.artworkTex.h) / fe.artworkTex.w;
            ImGui::Image((ImTextureID)(intptr_t)fe.artworkTex.id, ImVec2(side, side * aspect));
            ImGui::TextDisabled("Cover art - %d x %d %s", fe.artworkTex.w, fe.artworkTex.h,
                                tg.artworkMime.c_str());
            ImGui::Spacing();
        }
    }

    auto row = [](const std::string& k, const std::string& v) {
        if (v.empty()) return;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", k.c_str());
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", v.c_str());
    };

    if (!tg.allTags.empty()) {
        ImGui::SeparatorText("Tags");
        if (ImGui::BeginTable("##tags", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& kv : tg.allTags) row(kv.first, kv.second);
            ImGui::EndTable();
        }
    }

    if (bi.present) {
        ImGui::SeparatorText("Broadcast Wave (bext)");
        if (ImGui::BeginTable("##bext", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            row("Description", bi.description);
            row("Originator", bi.originator);
            row("Reference", bi.originatorReference);
            if (!bi.originationDate.empty() || !bi.originationTime.empty())
                row("Origination", bi.originationDate + " " + bi.originationTime);
            if (bi.timeReference > 0)
                row("Time reference", std::to_string(bi.timeReference) + " samples");
            row("UMID", bi.umid);
            row("Coding history", bi.codingHistory);
            if (!bi.ixml.empty()) row("iXML", std::to_string(bi.ixml.size()) + " bytes embedded");
            ImGui::EndTable();
        }
    }

    if (m.adm.present || m.adm.hasDbmd) {
        ImGui::SeparatorText("ADM / Dolby Atmos");
        if (ImGui::BeginTable("##adm", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            row("Layout", m.layoutName);
            row("Programme", m.adm.programme);
            if (!m.adm.objects.empty())
                row("Objects", std::to_string(m.adm.objects.size()));
            if (m.adm.hasDbmd) row("Dolby metadata", "present (dbmd)");
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// Keyboard-shortcuts reference window.
void drawShortcutsWindow(App& app) {
    if (!app.showShortcuts) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
    lockWindowWidth(440);
    if (!ImGui::Begin("Keyboard shortcuts", &app.showShortcuts)) {
        ImGui::End();
        return;
    }
    struct SC { const char* key; const char* desc; };
    // Listed in the order you'd reach for them: transport, then navigation, then file ops.
    const SC shortcuts[] = {
        {"Space", "Play / pause"},
        {"Esc", "Stop playback"},
        {"\xE2\x86\x90", "Previous finding"},
        {"\xE2\x86\x92", "Next finding"},
#if defined(__APPLE__)
        {"\xE2\x8C\x98 O", "Open file(s)"},
#else
        {"Ctrl O", "Open file(s)"},
#endif
        {"Drag & drop", "Add files or a folder to the list"},
    };
    if (ImGui::BeginTable("##sc", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& s : shortcuts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(accentCol(), "%s", s.key);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.desc);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// Centre-screen modal shown when the selected file's chosen profile doesn't match its
// detected layout (e.g. the user forced a stereo profile onto an Atmos master).
void drawProfileMismatchModal(App& app) {
    if (app.selected < 0 || app.selected >= static_cast<int>(app.files.size())) return;
    FileEntry& fe = app.files[app.selected];
    if (fe.state != 2 || fe.profileMismatchAck || fe.report.suggestedProfile.empty()) return;

    const char* id = "Profile / layout mismatch";
    if (!ImGui::IsPopupOpen(id)) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup(id);
    }
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
                          ImVec4(gAccent.x * 0.30f, gAccent.y * 0.30f, gAccent.z * 0.35f + 0.06f, 1.0f));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("\"%s\" looks like a %s file, but the \"%s\" profile is selected.",
                           fe.name.c_str(), fe.report.meta.layoutName.c_str(),
                           fe.profileName.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Switch to the matching \"%s\" profile?",
                           fe.report.suggestedProfile.c_str());
        ImGui::Spacing();
        std::string sw = "Switch to " + fe.report.suggestedProfile;
        if (ImGui::Button(sw.c_str())) {
            fe.profileName = fe.report.suggestedProfile;
            fe.profileMismatchAck = true;
            app.enqueue(app.selected);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep current")) {
            fe.profileMismatchAck = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

// Multitrack window: per-channel waveform overview + mini-spectrogram + role/silent state.
// Makes a dead channel (or an Atmos bed/objects) obvious at a glance.
void drawChannelsWindow(App& app) {
    if (!app.showChannels) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 120, vp->WorkPos.y + 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(820, 600), ImGuiCond_FirstUseEver);
    lockWindowWidth(820);
    if (!ImGui::Begin("Channels", &app.showChannels)) {
        ImGui::End();
        return;
    }
    if (app.selected < 0 || app.selected >= static_cast<int>(app.files.size()) ||
        app.files[app.selected].state != 2) {
        ImGui::TextDisabled("Select an analysed file to see its channels.");
        ImGui::End();
        return;
    }
    FileEntry& fe = app.files[app.selected];
    const Report& r = fe.report;
    const FileMetadata& m = r.meta;
    const int ch = static_cast<int>(r.channelOverviews.size());
    if (ch == 0) {
        ImGui::TextDisabled("No channel data.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s  -  %d channel(s)", m.layoutName.c_str(), ch);
    if (static_cast<int>(fe.channelSpecTex.size()) != ch) fe.channelSpecTex.resize(ch);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;

    for (int c = 0; c < ch; ++c) {
        const auto& ov = r.channelOverviews[c];
        float peak = 0.0f;
        for (float v : ov) peak = std::max(peak, v);
        bool silent = peak < 1e-4f;
        std::string role = c < static_cast<int>(m.channelRoles.size()) ? m.channelRoles[c] : "";
        auto starts = [&](const char* p) { return role.rfind(p, 0) == 0; };
        bool isObject = starts("Object");
        bool isHeight = starts("Lt") || starts("Rt") || starts("Tf") || starts("Tt");
        bool isLfe = role == "LFE";
        ImGui::PushID(c);
        ImGui::Text("ch %d", c);
        if (!role.empty()) {
            ImGui::SameLine();
            ImVec4 rc = isObject ? accentCol()
                        : isHeight ? ImVec4(0.70f, 0.85f, 0.55f, 1.0f)
                        : isLfe ? ImVec4(0.90f, 0.65f, 0.45f, 1.0f)
                                : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(rc, "%s%s", role.c_str(), isHeight ? " (height)" : "");
        }
        // Silent is only a concern for the bed; objects/LFE are routinely silent.
        if (silent) {
            ImGui::SameLine();
            if (isObject || isLfe)
                ImGui::TextDisabled("(silent)");
            else
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.3f, 1.0f), "[SILENT]");
        }
        // Waveform overview strip.
        const float wh = 34.0f;
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + wh), IM_COL32(18, 18, 22, 255));
        float yc = p.y + wh * 0.5f;
        for (int px = 0; px < static_cast<int>(w); ++px) {
            int idx = static_cast<int>(static_cast<std::size_t>(px) * ov.size() / std::max(1, (int)w));
            if (idx >= static_cast<int>(ov.size())) idx = static_cast<int>(ov.size()) - 1;
            float a = ov[idx] * (wh * 0.5f - 1);
            dl->AddLine(ImVec2(p.x + px, yc - a), ImVec2(p.x + px, yc + a), accentU32(0.9f));
        }
        ImGui::Dummy(ImVec2(w, wh));
        // Mini-spectrogram.
        if (c < static_cast<int>(r.channelSpecs.size()) && !r.channelSpecs[c].empty()) {
            if (!fe.channelSpecTex[c].valid())
                fe.channelSpecTex[c].upload(r.channelSpecs[c].data(), r.chanSpecW, r.chanSpecH);
            if (fe.channelSpecTex[c].valid())
                ImGui::Image((ImTextureID)(intptr_t)fe.channelSpecTex[c].id,
                             ImVec2(w, static_cast<float>(r.chanSpecH)));
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::End();
}

void drawProfileEditor(App& app) {
    if (!app.showProfiles) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 360, vp->WorkPos.y + 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 500), ImGuiCond_FirstUseEver);
    lockWindowWidth(560);
    if (!ImGui::Begin("Delivery Profiles", &app.showProfiles)) {
        ImGui::End();
        return;
    }

    // Active delivery profile for the SELECTED file (profiles are per-file). Changing it
    // re-analyses just that file.
    ImGui::TextDisabled("PROFILE FOR SELECTED FILE");
    {
        std::vector<const char*> names;
        names.reserve(app.profileCount());
        for (int i = 0; i < app.profileCount(); ++i) names.push_back(app.profileNameAt(i).c_str());
        bool hasSel = app.selected >= 0 && app.selected < static_cast<int>(app.files.size());
        int idx = hasSel ? app.profileIndexForName(app.files[app.selected].profileName) : -1;
        if (idx < 0) idx = 0;
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(!hasSel);
        if (ImGui::Combo("##activeprofile", &idx, names.data(), static_cast<int>(names.size())) &&
            hasSel) {
            app.files[app.selected].profileName = app.profileNameAt(idx);
            app.enqueue(app.selected);  // re-analyse just this file with the chosen profile
        }
        ImGui::EndDisabled();
        if (!hasSel) ImGui::TextDisabled("(no file selected)");
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Built-in profiles are read-only. Create custom profiles for your own "
                       "delivery specs; they are saved to disk and available on the CLI.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // List of custom profiles.
    ImGui::TextDisabled("CUSTOM PROFILES");
    if (ImGui::BeginListBox("##customlist", ImVec2(-1, 110))) {
        for (int i = 0; i < static_cast<int>(app.customProfiles.size()); ++i) {
            ImGui::PushID(i);
            bool sel = (i == app.profileEditSel);
            if (ImGui::Selectable(app.customProfiles[i].name.c_str(), sel)) {
                app.profileEditSel = i;
                syncProfileRatesBuf(app);
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }

    if (ImGui::Button("New")) {
        Profile p;
        p.name = "Custom " + std::to_string(app.customProfiles.size() + 1);
        app.customProfiles.push_back(p);
        app.profileEditSel = static_cast<int>(app.customProfiles.size()) - 1;
        syncProfileRatesBuf(app);
        saveCustomProfiles(app.customProfiles);
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate current")) {
        // Seed a new custom profile from whatever profile is currently active.
        Profile p = app.currentProfile();
        p.name += " (copy)";
        app.customProfiles.push_back(p);
        app.profileEditSel = static_cast<int>(app.customProfiles.size()) - 1;
        syncProfileRatesBuf(app);
        saveCustomProfiles(app.customProfiles);
    }
    ImGui::SameLine();
    bool hasSel = app.profileEditSel >= 0 &&
                  app.profileEditSel < static_cast<int>(app.customProfiles.size());
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button("Delete")) {
        app.customProfiles.erase(app.customProfiles.begin() + app.profileEditSel);
        // Keep the active selection pointing at a valid profile.
        if (app.profileIndex >= app.profileCount()) app.profileIndex = 0;
        app.profileEditSel = -1;
        syncProfileRatesBuf(app);
        saveAndApplyProfiles(app);
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (hasSel) {
        Profile& p = app.customProfiles[app.profileEditSel];
        ImGui::InputText("Name", &p.name);

        ImGui::Text("Required bit depth (0 = any)");
        ImGui::InputInt("##bitdepth", &p.requiredBitDepth);
        ImGui::Text("Required channels (0 = any)");
        ImGui::InputInt("##channels", &p.requiredChannels);
        ImGui::Checkbox("Require lossless", &p.requireLossless);

        ImGui::Text("Approved sample rates (comma-separated; empty = any)");
        if (ImGui::InputText("##rates", &app.profileRatesBuf)) {
            p.approvedRates.clear();
            std::string tok;
            for (char c : app.profileRatesBuf + ",") {
                if (c == ',') {
                    if (!tok.empty()) {
                        int r = std::atoi(tok.c_str());
                        if (r > 0) p.approvedRates.push_back(r);
                    }
                    tok.clear();
                } else if (!std::isspace(static_cast<unsigned char>(c))) {
                    tok += c;
                }
            }
        }

        ImGui::Separator();
        ImGui::Checkbox("Enforce true-peak ceiling", &p.enforceTruePeak);
        double ceil = p.truePeakCeiling;
        if (ImGui::InputDouble("Ceiling (dBTP)", &ceil, 0.1, 1.0, "%.1f"))
            p.truePeakCeiling = ceil;
        ImGui::Checkbox("Check loudness target", &p.checkLoudnessTarget);
        ImGui::BeginDisabled(!p.checkLoudnessTarget);
        double tgt = p.lufsTarget, tol = p.lufsTolerance;
        if (ImGui::InputDouble("Target (LUFS)", &tgt, 0.5, 1.0, "%.1f")) p.lufsTarget = tgt;
        if (ImGui::InputDouble("Tolerance (LU)", &tol, 0.5, 1.0, "%.1f")) p.lufsTolerance = tol;
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::Button("Save & Apply")) saveAndApplyProfiles(app);
        ImGui::SameLine();
        ImGui::TextDisabled("Saved to %s", customProfilesPath().c_str());
    } else {
        ImGui::TextDisabled("Select or create a custom profile to edit.");
    }

    ImGui::End();
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
    if (app.reqExportSpecPng.exchange(false)) exportSpectrogramPngDialog(app);

    handleShortcuts(app);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    // No menu bar: all actions live in the UI (Open buttons in the sidebar, Export buttons
    // by the file title, View toggles + theme in the sidebar / Spectrogram Settings).
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##main", nullptr, wf);

    drawFileList(app);

    // Right: detail.
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Border);
    if (app.selected < 0 || app.selected >= static_cast<int>(app.files.size())) {
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1.0f), "Argus - audio master QA");
        ImGui::Spacing();
        ImGui::BulletText("Drag audio files or a folder onto this window");
        ImGui::BulletText("or use the Open File / Open Folder buttons (top-left)");
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

            // Export controls, right-aligned on the title line (no separate row / menu bar).
            {
                struct Exp { const char* lbl; const char* tip; void (*fn)(App&); };
                const Exp exps[] = {
                    {"PDF", "PDF report for this file", exportPdfDialog},
                    {"CSV", "CSV findings for this file", exportCsvDialog},
                    {"JSON", "JSON report for this file", exportJsonDialog},
                    {"PDFs/folder", "One PDF per loaded file, into a folder", exportAllDialog},
                    {"Combined PDF", "Single PDF covering all loaded files", exportBatchPdfDialog},
                    {"Spectrogram", "Spectrogram of this file as a PNG", exportSpectrogramPngDialog}};
                const ImGuiStyle& st = ImGui::GetStyle();
                const char* prefix = "Export:";
                float total = ImGui::CalcTextSize(prefix).x + st.ItemSpacing.x;
                for (const auto& e : exps)
                    total += ImGui::CalcTextSize(e.lbl).x + st.FramePadding.x * 2 + st.ItemSpacing.x;
                total -= st.ItemSpacing.x;
                ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionMax().x - total));
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", prefix);
                for (const auto& e : exps) {
                    ImGui::SameLine();
                    if (ImGui::Button(e.lbl)) e.fn(app);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.tip);
                }
            }

            std::string layoutStr = r.meta.layoutName.empty() ? std::to_string(r.meta.channels) + " ch"
                                                              : r.meta.layoutName;
            ImGui::TextDisabled("%s  |  %d-bit  |  %.1f kHz  |  %s  |  %s  |  %d fail, %d warn",
                                r.meta.container.c_str(), r.meta.bitDepth, r.meta.sampleRate / 1000.0,
                                layoutStr.c_str(), timecode(r.meta.durationSec).c_str(),
                                r.count(Severity::Fail), r.count(Severity::Warn));

            // Profile mismatch is surfaced via a centre-screen modal (see drawProfileMismatchModal).
            if (app.selectedIssue < 0) {
                for (std::size_t i = 0; i < r.issues.size(); ++i)
                    if (r.issues[i].severity >= Severity::Warn && r.issues[i].localised()) {
                        app.selectedIssue = static_cast<int>(i);
                        app.issueToExpand = static_cast<int>(i);
                        break;
                    }
            }
            ImGui::Separator();
            drawSpectrogram(app, fe);
            drawWaveform(app, r);
            drawTransport(app);
            // Findings inline, unless popped out into their own window (below).
            if (!app.popOutResults) {
                ImGui::Separator();
                ImGui::BeginChild("issues");
                drawIssues(app, fe);
                ImGui::EndChild();
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();

    // Pop-out results window (so the spectrogram/waveform can fill the main view, e.g. on
    // a second monitor).
    if (app.popOutResults) {
        const ImGuiViewport* vpr = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vpr->WorkPos.x + 200, vpr->WorkPos.y + 120),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Results", &app.popOutResults)) {
            if (app.selected >= 0 && app.selected < static_cast<int>(app.files.size()) &&
                app.files[app.selected].state == 2)
                drawIssues(app, app.files[app.selected]);
            else
                ImGui::TextDisabled("Select an analysed file.");
        }
        ImGui::End();
    }

    drawSettings(app);
    drawReportInfo(app);
    drawProfileEditor(app);
    drawMetadataWindow(app);
    drawChannelsWindow(app);
    drawShortcutsWindow(app);
    drawProfileMismatchModal(app);
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
    s.ScrollbarSize = 11.0f;        // slimmer so window text isn't crowded
    s.ScrollbarRounding = 3.0f;
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
    // Apply the user accent colour across all the interactive chrome.
    const ImVec4 a = gAccent;
    auto mul = [](ImVec4 v, float f) { return ImVec4(v.x * f, v.y * f, v.z * f, 1.0f); };
    auto al = [](ImVec4 v, float alpha) { return ImVec4(v.x, v.y, v.z, alpha); };
    c[ImGuiCol_Button] = mul(a, 0.65f);
    c[ImGuiCol_ButtonHovered] = mul(a, 0.85f);
    c[ImGuiCol_ButtonActive] = a;
    c[ImGuiCol_Header] = mul(a, 0.55f);
    c[ImGuiCol_HeaderHovered] = mul(a, 0.75f);
    c[ImGuiCol_HeaderActive] = mul(a, 0.9f);
    c[ImGuiCol_CheckMark] = a;
    c[ImGuiCol_SliderGrab] = mul(a, 0.85f);
    c[ImGuiCol_SliderGrabActive] = a;
    c[ImGuiCol_FrameBg] = mul(a, theme == 1 ? 0.22f : 0.18f);  // inputs / combos / sliders
    c[ImGuiCol_FrameBgHovered] = al(a, 0.28f);
    c[ImGuiCol_FrameBgActive] = al(a, 0.42f);
    c[ImGuiCol_TitleBg] = mul(a, 0.22f);
    c[ImGuiCol_TitleBgActive] = mul(a, 0.5f);
    c[ImGuiCol_Tab] = mul(a, 0.45f);
    c[ImGuiCol_TabHovered] = mul(a, 0.8f);
    c[ImGuiCol_TabActive] = mul(a, 0.65f);
    c[ImGuiCol_ScrollbarGrab] = mul(a, 0.5f);
    c[ImGuiCol_ScrollbarGrabHovered] = mul(a, 0.7f);
    c[ImGuiCol_ScrollbarGrabActive] = mul(a, 0.9f);
    c[ImGuiCol_ResizeGrip] = al(a, 0.30f);
    c[ImGuiCol_ResizeGripHovered] = al(a, 0.6f);
    c[ImGuiCol_ResizeGripActive] = al(a, 0.9f);
    c[ImGuiCol_SeparatorHovered] = al(a, 0.6f);
    c[ImGuiCol_SeparatorActive] = a;
    c[ImGuiCol_TextSelectedBg] = al(a, 0.35f);
    c[ImGuiCol_NavHighlight] = a;
    c[ImGuiCol_DragDropTarget] = a;
    c[ImGuiCol_TextLink] = a;
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
    // Multi-monitor: let tool windows be dragged out onto other displays as OS windows.
    // (Skipped in headless screenshot mode.)
    if (shotPath.empty()) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    if (haveState) gAccent = ImVec4(st.accentR, st.accentG, st.accentB, 1.0f);
    applyStyle(haveState ? st.theme : 0);
    loadFont(xscale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    App app;
    app.customProfiles = loadCustomProfiles();
    if (haveState) {
        app.spec.colormap = static_cast<Colormap>(st.colormap);
        app.spec.freqScale = static_cast<FreqScale>(st.freqScale);
        app.spec.fftSize = st.fftSize;
        app.spec.dbLow = st.dbLow;
        app.spec.dbHigh = st.dbHigh;
        app.listWidth = static_cast<float>(st.listWidth);
        app.showSettings = st.showSettings;
        app.preroll = static_cast<float>(st.preroll);
        app.postroll = static_cast<float>(st.postroll);
        app.loopAudition = st.loop;
        app.showReportInfo = st.showReportInfo;
        app.reportInfo = st.reportInfo;
        // Re-decode the persisted company logo (only the path is stored).
        if (!app.reportInfo.orgLogoPath.empty())
            loadOrgLogo(app.reportInfo, app.reportInfo.orgLogoPath);
        app.themeMode = st.theme;
        // Resolve the saved profile by name (stable across custom-profile edits), falling
        // back to the legacy index when no name was stored or it no longer exists.
        app.profileIndex = st.profile;
        if (!st.profileName.empty()) {
            for (int i = 0; i < app.profileCount(); ++i)
                if (app.profileNameAt(i) == st.profileName) { app.profileIndex = i; break; }
        }
        if (app.profileIndex < 0 || app.profileIndex >= app.profileCount()) app.profileIndex = 0;
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
        cb.exportSpecPng = [&app] { app.reqExportSpecPng = true; };
        cb.exportAll = [&app] { app.reqExportAll = true; };
        cb.exportBatchPdf = [&app] { app.reqExportBatchPdf = true; };
        cb.toggleSettings = [&app] { app.showSettings = !app.showSettings; };
        cb.toggleReportInfo = [&app] { app.showReportInfo = !app.showReportInfo; };
        cb.toggleProfiles = [&app] { app.showProfiles = !app.showProfiles; };
        cb.toggleTheme = [&app] { app.themeMode ^= 1; };
        cb.quit = [win] { glfwSetWindowShouldClose(win, GLFW_TRUE); };
        installMacMenu(cb);
    }
#endif

    // Screenshot look (applied before analysis so the spectrogram colormap takes effect).
    if (!shotPath.empty()) {
        if (std::getenv("ARGUS_SHOT_INFERNO")) app.spec.colormap = Colormap::Inferno;
        if (const char* acc = std::getenv("ARGUS_SHOT_ACCENT")) {
            float r = 0, g = 0, bl = 0;
            if (std::sscanf(acc, "%f,%f,%f", &r, &g, &bl) == 3) {
                gAccent = ImVec4(r, g, bl, 1.0f);
                applyStyle(app.themeMode);
            }
        }
    }
    for (const auto& in : inputs) app.addPath(in);

    // Screenshot composition hooks (only used with --shot, for the README images).
    if (!shotPath.empty()) {
        if (std::getenv("ARGUS_SHOT_NOSETTINGS")) app.showSettings = false;
        if (std::getenv("ARGUS_SHOT_CHANNELS")) app.showChannels = true;
        if (std::getenv("ARGUS_SHOT_METADATA")) app.showMetadata = true;
        if (std::getenv("ARGUS_SHOT_SETTINGS")) app.showSettings = true;
        if (std::getenv("ARGUS_SHOT_SHORTCUTS")) app.showShortcuts = true;
        if (std::getenv("ARGUS_SHOT_POPOUT")) app.popOutResults = true;
        if (std::getenv("ARGUS_SHOT_PROFILES")) app.showProfiles = true;
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

        // Render any windows that were dragged out to other monitors.
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup);
        }

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
        st.showSettings = app.showSettings;
        st.preroll = app.preroll;
        st.postroll = app.postroll;
        st.loop = app.loopAudition;
        st.showReportInfo = app.showReportInfo;
        st.reportInfo = app.reportInfo;
        st.theme = app.themeMode;
        st.accentR = gAccent.x;
        st.accentG = gAccent.y;
        st.accentB = gAccent.z;
        st.profile = app.profileIndex;
        st.profileName = app.profileNameAt(app.profileIndex);
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
