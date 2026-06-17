// Self-contained PDF writer for the QA report. No external dependency: emits a
// PDF 1.4 with JetBrains Mono embedded as a TrueType FontFile2 (so the report is
// monospace and consistent on any machine), the full-file spectrogram, per-finding
// close-ups with the event boxed, a verdict badge, metadata grid, summary matrix
// and a paginated findings section.
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../Util.h"
#include "EmbeddedFonts.h"
#include "EmbeddedLogo.h"
#include "Exports.h"

#ifndef ARGUS_VERSION
#define ARGUS_VERSION "dev"
#endif

namespace argus {
namespace {

constexpr double kW = 792.0;  // US Letter, landscape
constexpr double kH = 612.0;
constexpr double kMargin = 42.0;
constexpr double kCharW = 0.6;  // JetBrains Mono advance (600/1000 em)

struct Rgb { double r, g, b; };
const Rgb kInk{0.13, 0.13, 0.16};
const Rgb kMuted{0.46, 0.46, 0.52};
const Rgb kRule{0.82, 0.82, 0.85};
const Rgb kPanel{0.95, 0.95, 0.965};
const Rgb kWhite{1, 1, 1};

Rgb severityRgb(Severity s) {
    switch (s) {
        case Severity::Pass: return {0.20, 0.58, 0.27};
        case Severity::Info: return {0.20, 0.42, 0.78};
        case Severity::Warn: return {0.85, 0.55, 0.05};
        case Severity::Fail: return {0.82, 0.20, 0.18};
    }
    return {0, 0, 0};
}

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '(' || c == ')' || c == '\\') o += '\\';
        if (c < 32 || c > 126) {
            o += '?';
            continue;
        }
        o += static_cast<char>(c);
    }
    return o;
}

std::string objStr(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", v);
    return b;
}

double freqFrac(int scale, double lo, double hi, double f) {
    f = std::min(hi, std::max(lo, f));
    if (scale == 0) {  // Mel
        auto mel = [](double x) { return 2595.0 * std::log10(1.0 + x / 700.0); };
        return (mel(f) - mel(lo)) / (mel(hi) - mel(lo));
    }
    if (scale == 1) return (std::log10(f) - std::log10(lo)) / (std::log10(hi) - std::log10(lo));
    return (f - lo) / (hi - lo);
}

// Box-average downscale of an RGBA raster to dw x dh DeviceRGB bytes.
std::string downsampleRgb(const std::vector<unsigned char>& src, int sw, int sh, int dw, int dh) {
    std::string out;
    out.resize(static_cast<std::size_t>(dw) * dh * 3);
    for (int y = 0; y < dh; ++y) {
        int sy0 = y * sh / dh, sy1 = std::max(sy0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            int sx0 = x * sw / dw, sx1 = std::max(sx0 + 1, (x + 1) * sw / dw);
            long r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx) {
                    std::size_t p = (static_cast<std::size_t>(sy) * sw + sx) * 4;
                    r += src[p];
                    g += src[p + 1];
                    b += src[p + 2];
                    ++n;
                }
            std::size_t q = (static_cast<std::size_t>(y) * dw + x) * 3;
            out[q + 0] = static_cast<char>(n ? r / n : 0);
            out[q + 1] = static_cast<char>(n ? g / n : 0);
            out[q + 2] = static_cast<char>(n ? b / n : 0);
        }
    }
    return out;
}

struct PdfImage {
    std::string name;
    int w, h;
    std::string rgb;
};

// Builds page content streams and accumulates image XObjects, paginating text flow.
struct Builder {
    std::vector<std::string> pages;
    std::string cur;
    std::vector<PdfImage> images;
    double yTop = kMargin;

    std::string addImage(int w, int h, const std::string& rgb) {
        PdfImage im;
        im.name = "Im" + fmtInt(static_cast<long long>(images.size()));
        im.w = w;
        im.h = h;
        im.rgb = rgb;
        images.push_back(std::move(im));
        return images.back().name;
    }

    void flush() {
        if (!cur.empty()) pages.push_back(cur);
        cur.clear();
    }
    void newPage() {
        flush();
        yTop = kMargin;
    }
    void ensure(double need) {
        if (yTop + need > kH - kMargin - 18) newPage();  // leave room for footer
    }
    double pdfY(double fromTop) const { return kH - fromTop; }
    static double textW(const std::string& s, double size) { return s.size() * size * kCharW; }

    void rawText(const std::string& s, double x, double yFromTop, double size, Rgb c, bool bold) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "BT /%s %.1f Tf %.3f %.3f %.3f rg %.2f %.2f Td (",
                      bold ? "F2" : "F1", size, c.r, c.g, c.b, x, pdfY(yFromTop + size));
        cur += buf;
        cur += esc(s);
        cur += ") Tj ET\n";
    }
    // Advancing line of text from x.
    void line(const std::string& s, double x, double size, Rgb c, bool bold = false) {
        ensure(size + 4);
        rawText(s, x, yTop, size, c, bold);
        yTop += size + 4;
    }
    void gap(double h) { yTop += h; }

    void fillRect(double x, double yFromTop, double w, double h, Rgb c) {
        char b[160];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f rg %.2f %.2f %.2f %.2f re f\n", c.r, c.g, c.b, x,
                      pdfY(yFromTop + h), w, h);
        cur += b;
    }
    void strokeRect(double x, double yFromTop, double w, double h, Rgb c, double lw) {
        char b[200];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f RG %.2f w %.2f %.2f %.2f %.2f re S\n", c.r, c.g,
                      c.b, lw, x, pdfY(yFromTop + h), w, h);
        cur += b;
    }
    void hline(double x0, double x1, double yFromTop, Rgb c, double lw) {
        char b[200];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f RG %.2f w %.2f %.2f m %.2f %.2f l S\n", c.r, c.g,
                      c.b, lw, x0, pdfY(yFromTop), x1, pdfY(yFromTop));
        cur += b;
    }
    void vline(double x, double y0FromTop, double y1FromTop, Rgb c, double lw) {
        char b[200];
        std::snprintf(b, sizeof(b), "%.3f %.3f %.3f RG %.2f w %.2f %.2f m %.2f %.2f l S\n", c.r, c.g,
                      c.b, lw, x, pdfY(y0FromTop), x, pdfY(y1FromTop));
        cur += b;
    }
    void image(const std::string& name, double x, double yFromTop, double w, double h) {
        char b[160];
        std::snprintf(b, sizeof(b), "q %.2f 0 0 %.2f %.2f %.2f cm /%s Do Q\n", w, h, x,
                      pdfY(yFromTop + h), name.c_str());
        cur += b;
    }

    // Colored status chip with label.
    void chip(double x, double yFromTop, Severity sev) {
        const char* lbl = severityLabel(sev);
        double w = textW(lbl, 8) + 10, h = 13;
        fillRect(x, yFromTop, w, h, severityRgb(sev));
        rawText(lbl, x + 5, yFromTop + 2.5, 8, kWhite, true);
    }

    void wrapped(const std::string& s, double x, double size, Rgb c) {
        double maxW = kW - kMargin - x;
        std::size_t cpl = static_cast<std::size_t>(maxW / (size * kCharW));
        if (cpl < 8) cpl = 8;
        std::string word, ln;
        auto emit = [&]() {
            if (!ln.empty()) { line(ln, x, size, c); ln.clear(); }
        };
        for (std::size_t i = 0; i <= s.size(); ++i) {
            char ch = (i < s.size()) ? s[i] : ' ';
            if (ch == ' ' || ch == '\n') {
                if (ln.size() + word.size() + 1 > cpl) emit();
                if (!ln.empty()) ln += ' ';
                ln += word;
                word.clear();
                if (ch == '\n') emit();
            } else {
                word += ch;
            }
        }
        emit();
    }
};

// Draw a spectrogram block (image + frequency axis + time ticks + event overlays).
// `regions` are highlighted spans in seconds with a colour; `box` selects a hollow
// box (close-up) vs. thin marker lines (full file).
struct Region { double t0, t1; Rgb c; };
void drawSpectro(Builder& b, const std::string& name, double drawW, double drawH, double winStart,
                 double winEnd, double minF, double maxF, int scale,
                 const std::vector<Region>& regions, bool box) {
    const double axisW = 30.0;
    double plotX = kMargin + axisW;
    double plotW = drawW - axisW;
    double y0 = b.yTop;  // top of plot

    b.image(name, plotX, y0, plotW, drawH);
    b.strokeRect(plotX, y0, plotW, drawH, kRule, 0.6);

    // Frequency gridlines + labels.
    const double ticks[] = {100, 1000, 5000, 10000, 20000, 40000};
    for (double f : ticks) {
        if (f < minF || f > maxF) continue;
        double frac = freqFrac(scale, minF, maxF, f);
        double yy = y0 + (1.0 - frac) * drawH;
        char lbl[16];
        if (f >= 1000) std::snprintf(lbl, sizeof(lbl), "%gk", f / 1000.0);
        else std::snprintf(lbl, sizeof(lbl), "%g", f);
        b.rawText(lbl, kMargin - 2, yy - 3, 6.5, kMuted, false);
    }

    // Event overlays.
    double span = winEnd - winStart;
    if (span > 0) {
        for (const auto& r : regions) {
            double fx0 = plotX + (r.t0 - winStart) / span * plotW;
            double fx1 = plotX + (r.t1 - winStart) / span * plotW;
            if (box) {
                if (fx1 < fx0 + 2) fx1 = fx0 + 2;
                b.strokeRect(fx0, y0 + 1, fx1 - fx0, drawH - 2, r.c, 1.3);
            } else {
                b.vline(fx0, y0, y0 + drawH, r.c, 1.0);
            }
        }
    }

    // Time ticks under the plot (fewer on narrow close-ups to avoid collisions).
    int nticks = plotW > 480 ? 6 : 4;
    for (int k = 0; k <= nticks; ++k) {
        double t = winStart + span * k / nticks;
        double xx = plotX + (span > 0 ? (t - winStart) / span : 0) * plotW;
        std::string tc = timecode(t);
        double tw = Builder::textW(tc, 6);
        double tx = (k == nticks) ? plotX + plotW - tw : xx;  // right-align final tick
        b.rawText(tc, std::max(plotX, tx), y0 + drawH + 2, 6, kMuted, false);
    }
    b.yTop = y0 + drawH + 12;
}

// FontFile2 object body (uncompressed): << /Length n /Length1 n >> stream <bytes>.
std::string fontFileObj(const unsigned char* data, unsigned int len) {
    std::string s(reinterpret_cast<const char*>(data), len);
    std::string n = fmtInt(static_cast<long long>(len));
    return "<< /Length " + n + " /Length1 " + n + " >>\nstream\n" + s + "\nendstream";
}

std::string widthsArray() {
    std::string w = "[";
    for (int i = 32; i <= 126; ++i) w += fmtInt(kJetBrainsMonoAdvance) + " ";
    w += "]";
    return w;
}

}  // namespace

// Append one report's full body (header, optional logo + QA block, metadata,
// summary matrix, spectrogram and findings) to the shared builder, starting at
// the current yTop. Shared by the single-file and combined-batch PDF writers.
static void renderReportBody(Builder& b, const Report& rep, const ReportInfo& info) {
    const double contentW = kW - 2 * kMargin;

    // ---- Header band ----
    b.fillRect(kMargin, b.yTop, contentW, 46, kPanel);
    double textX = kMargin + 10;
    if (info.showLogo) {
        const double sz = 34, lx = kMargin + 8, ly = b.yTop + 6;
        std::string ln = b.addImage(
            kArgusLogoW, kArgusLogoH,
            std::string(reinterpret_cast<const char*>(kArgusLogoRGB),
                        static_cast<std::size_t>(kArgusLogoW) * kArgusLogoH * 3));
        b.image(ln, lx, ly, sz, sz);
        textX = lx + sz + 10;
    }
    b.rawText("ARGUS  AUDIO QA", textX, b.yTop + 8, 9, kMuted, true);
    b.rawText(rep.meta.filename, textX, b.yTop + 20, 14, kInk, true);
    {
        Severity v = rep.ok() ? rep.verdict() : Severity::Fail;
        std::string vt = rep.ok() ? std::string("  ") + severityLabel(v) + "  " : "  DECODE ERROR  ";
        double bw = Builder::textW(vt, 13) + 6, bh = 22;
        double bx = kW - kMargin - bw - 10, by = b.yTop + 12;
        b.fillRect(bx, by, bw, bh, severityRgb(v));
        b.rawText(vt, bx, by + 4, 13, kWhite, true);
        std::string sub = fmtInt(rep.count(Severity::Fail)) + " fail / " +
                          fmtInt(rep.count(Severity::Warn)) + " warn";
        b.rawText(sub, kW - kMargin - Builder::textW(sub, 7) - 10, b.yTop + 36, 7, kMuted, false);
    }
    b.yTop += 56;

    // ---- QA sign-off block (optional; honours per-field toggles) ----
    if (info.anyText() || info.showDate) {
        std::vector<std::pair<std::string, std::string>> qa;
        auto add = [&](bool show, const char* k, const std::string& v) {
            if (show && !v.empty()) qa.emplace_back(k, v);
        };
        add(info.showEngineer, "QA engineer", info.qaEngineer);
        add(info.showContact, "Contact", info.contact);
        add(info.showStudio, "Studio", info.studio);
        add(info.showClient, "Client", info.client);
        add(info.showProject, "Project", info.project);
        add(info.showCatalog, "Catalog/Job", info.catalog);
        if (info.showDate) {
            std::time_t now = std::time(nullptr);
            char d[32];
            std::strftime(d, sizeof(d), "%Y-%m-%d", std::localtime(&now));
            qa.emplace_back("Date", d);
        }
        if (!qa.empty()) {
            b.line("PREPARED BY", kMargin, 9, kMuted, true);
            double colW = contentW / 2;
            double startY = b.yTop;
            for (std::size_t i = 0; i < qa.size(); ++i) {
                double cx = kMargin + (i % 2) * colW;
                double cy = startY + (i / 2) * 13.0;
                b.rawText(qa[i].first, cx, cy, 8, kMuted, false);
                b.rawText(qa[i].second, cx + 80, cy, 8, kInk, true);
            }
            b.yTop = startY + ((qa.size() + 1) / 2) * 13.0 + 4;
        }
        if (info.showNotes && !info.notes.empty()) {
            b.rawText("Notes", kMargin, b.yTop, 8, kMuted, false);
            b.yTop += 11;
            b.wrapped(info.notes, kMargin + 8, 8, kInk);
        }
        b.hline(kMargin, kW - kMargin, b.yTop, kRule, 0.6);
        b.yTop += 6;
    }

    // ---- Metadata grid (two columns of key:value) ----
    {
        std::vector<std::pair<std::string, std::string>> kv = {
            {"Container", rep.meta.container},
            {"Codec", rep.meta.codec},
            {"Sample rate", fmtInt(rep.meta.sampleRate) + " Hz"},
            {"Bit depth", rep.meta.bitDepth ? fmtInt(rep.meta.bitDepth) + "-bit" : "compressed"},
            {"Channels", fmtInt(rep.meta.channels)},
            {"Duration", timecode(rep.meta.durationSec)},
            {"Frames", fmtInt(static_cast<long long>(rep.meta.frames))},
            {"File size", fmtInt(static_cast<long long>(rep.meta.fileBytes / 1024)) + " KiB"},
        };
        double colW = contentW / 2;
        double startY = b.yTop;
        for (std::size_t i = 0; i < kv.size(); ++i) {
            double cx = kMargin + (i % 2) * colW;
            double cy = startY + (i / 2) * 13.0;
            b.rawText(kv[i].first, cx, cy, 8, kMuted, false);
            b.rawText(kv[i].second, cx + 80, cy, 8, kInk, true);
        }
        b.yTop = startY + ((kv.size() + 1) / 2) * 13.0 + 8;
    }

    // ---- Summary matrix (one row per check, worst status chip) ----
    b.hline(kMargin, kW - kMargin, b.yTop, kRule, 0.6);
    b.yTop += 6;
    b.line("SUMMARY", kMargin, 9, kMuted, true);
    {
        // Group issues by check, keep worst severity + last summary.
        std::vector<std::string> order;
        std::vector<Severity> worst;
        std::vector<std::string> summ;
        for (const auto& is : rep.issues) {
            int found = -1;
            for (std::size_t k = 0; k < order.size(); ++k)
                if (order[k] == is.check) found = static_cast<int>(k);
            if (found < 0) {
                order.push_back(is.check);
                worst.push_back(is.severity);
                summ.push_back(is.summary);
            } else if (is.severity >= worst[found]) {
                worst[found] = is.severity;
                summ[found] = is.summary;
            }
        }
        for (std::size_t k = 0; k < order.size(); ++k) {
            b.ensure(14);
            double y = b.yTop;
            b.chip(kMargin, y, worst[k]);
            b.rawText(order[k], kMargin + 52, y + 2.5, 8.5, kInk, true);
            std::string s = summ[k];
            double sx = kMargin + 160;
            std::size_t maxc = static_cast<std::size_t>((kW - kMargin - sx) / (8 * kCharW));
            if (s.size() > maxc && maxc > 3) s = s.substr(0, maxc - 1) + "...";
            b.rawText(s, sx, y + 2.5, 8, kMuted, false);
            b.yTop += 14;
        }
    }
    b.gap(4);

    // ---- Full-file spectrogram ----
    if (rep.specWidth > 0 && rep.specHeight > 0 && !rep.specRGBA.empty()) {
        double drawW = contentW;
        double drawH = 180;
        b.ensure(drawH + 24);
        b.hline(kMargin, kW - kMargin, b.yTop, kRule, 0.6);
        b.yTop += 6;
        b.line("SPECTROGRAM", kMargin, 9, kMuted, true);
        int dw = 1700, dh = std::max(1, dw * rep.specHeight / rep.specWidth);
        std::string rgb = downsampleRgb(rep.specRGBA, rep.specWidth, rep.specHeight, dw, dh);
        std::string nm = b.addImage(dw, dh, rgb);
        std::vector<Region> regions;
        for (const auto& is : rep.issues) {
            if (!is.localised() || is.severity < Severity::Warn) continue;
            regions.push_back({is.tStart, is.tEnd > is.tStart ? is.tEnd : is.tStart, severityRgb(is.severity)});
        }
        drawSpectro(b, nm, drawW, drawH, 0.0, rep.specDuration > 0 ? rep.specDuration : 1.0,
                    rep.specMinFreq, rep.specMaxFreq, rep.specScale, regions, false);
    }

    // ---- Findings ----
    b.hline(kMargin, kW - kMargin, b.yTop, kRule, 0.6);
    b.yTop += 6;
    b.line("FINDINGS", kMargin, 9, kMuted, true);
    for (std::size_t i = 0; i < rep.issues.size(); ++i) {
        const Issue& is = rep.issues[i];
        Rgb c = severityRgb(is.severity);
        b.ensure(16);
        double y = b.yTop;
        b.chip(kMargin, y, is.severity);
        std::string head = is.check + " - " + is.summary;
        if (is.localised()) {
            head += "  [" + timecode(is.tStart);
            if (is.tEnd > is.tStart) head += " - " + timecode(is.tEnd);
            head += "]";
        }
        b.rawText(head, kMargin + 52, y + 2.5, 9, kInk, true);
        b.yTop += 16;

        if (!is.detail.empty()) b.wrapped(is.detail, kMargin + 8, 8, kMuted);

        // Per-finding close-up.
        if (const CloseupView* cv = rep.closeupFor(static_cast<int>(i))) {
            if (cv->valid()) {
                double drawW = contentW * 0.62;
                double drawH = drawW * cv->height / cv->width;
                if (drawH > 150) { drawH = 150; drawW = drawH * cv->width / cv->height; }
                b.ensure(drawH + 16);
                std::string nm = b.addImage(cv->width, cv->height,
                                            downsampleRgb(cv->rgba, cv->width, cv->height,
                                                          cv->width, cv->height));
                std::vector<Region> reg = {{cv->evStart, cv->evEnd, severityRgb(Severity::Fail)}};
                drawSpectro(b, nm, drawW + 30, drawH, cv->winStart, cv->winEnd, cv->minFreq,
                            cv->maxFreq, cv->scale, reg, true);
            }
        }

        // Fields as aligned key/value lines (monospace lets us pad cleanly).
        std::size_t kwid = 0;
        for (const auto& f : is.fields) kwid = std::max(kwid, f.first.size());
        for (const auto& f : is.fields) {
            std::string k = f.first;
            k.resize(kwid, ' ');
            b.line(k + "   " + f.second, kMargin + 8, 8, kInk, false);
        }
        b.gap(6);
    }
}

// Finalise a builder (footer on every page, object table, xref) and write it out.
static bool writePdf(Builder& b, const std::string& path) {
    b.flush();
    if (b.pages.empty()) b.pages.push_back("");

    // ---- Footer on each page ----
    std::time_t now = std::time(nullptr);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", std::localtime(&now));
    for (std::size_t i = 0; i < b.pages.size(); ++i) {
        std::string& pg = b.pages[i];
        char foot[256];
        std::string left = std::string("argus ") + ARGUS_VERSION + "  -  " + ts;
        std::snprintf(foot, sizeof(foot),
                      "BT /F1 7 Tf 0.55 0.55 0.6 rg %.2f %.2f Td (%s) Tj ET\n", kMargin,
                      kH - (kH - kMargin + 6), left.c_str());
        pg += foot;
        std::string pn = "Page " + fmtInt(static_cast<long long>(i + 1)) + " of " +
                         fmtInt(static_cast<long long>(b.pages.size()));
        char pr[160];
        std::snprintf(pr, sizeof(pr), "BT /F1 7 Tf 0.55 0.55 0.6 rg %.2f %.2f Td (%s) Tj ET\n",
                      kW - kMargin - pn.size() * 7 * kCharW, kH - (kH - kMargin + 6), pn.c_str());
        pg += pr;
    }

    // ---- Assemble objects ----
    // 1 Catalog, 2 Pages, 3 F1, 4 F2, 5 FD1, 6 FD2, 7 FF2-reg, 8 FF2-bold,
    // 9.. images, then content streams, then page dicts.
    std::vector<std::string> all(8);
    const int imgBase = 9;
    const int nImg = static_cast<int>(b.images.size());
    const int contentBase = imgBase + nImg;
    const int nPages = static_cast<int>(b.pages.size());
    const int pageBase = contentBase + nPages;

    all[2] = "<< /Type /Font /Subtype /TrueType /BaseFont /JetBrainsMono /FirstChar 32 "
             "/LastChar 126 /Widths " + widthsArray() +
             " /FontDescriptor 5 0 R /Encoding /WinAnsiEncoding >>";
    all[3] = "<< /Type /Font /Subtype /TrueType /BaseFont /JetBrainsMono-Bold /FirstChar 32 "
             "/LastChar 126 /Widths " + widthsArray() +
             " /FontDescriptor 6 0 R /Encoding /WinAnsiEncoding >>";
    all[4] = "<< /Type /FontDescriptor /FontName /JetBrainsMono /Flags 33 "
             "/FontBBox [-100 -350 700 1050] /ItalicAngle 0 /Ascent 1020 /Descent -300 "
             "/CapHeight 730 /StemV 90 /FontFile2 7 0 R >>";
    all[5] = "<< /Type /FontDescriptor /FontName /JetBrainsMono-Bold /Flags 33 "
             "/FontBBox [-100 -350 700 1050] /ItalicAngle 0 /Ascent 1020 /Descent -300 "
             "/CapHeight 730 /StemV 160 /FontFile2 8 0 R >>";
    all[6] = fontFileObj(kJetBrainsMonoRegular, kJetBrainsMonoRegular_len);
    all[7] = fontFileObj(kJetBrainsMonoBold, kJetBrainsMonoBold_len);

    // Image objects + shared XObject resource dict.
    std::string xobjects;
    for (int i = 0; i < nImg; ++i) {
        const PdfImage& im = b.images[i];
        std::string hdr = "<< /Type /XObject /Subtype /Image /Width " + fmtInt(im.w) + " /Height " +
                          fmtInt(im.h) +
                          " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length " +
                          fmtInt(static_cast<long long>(im.rgb.size())) + " >>\nstream\n";
        all.push_back(hdr + im.rgb + "\nendstream");
        xobjects += "/" + im.name + " " + fmtInt(imgBase + i) + " 0 R ";
    }

    std::string resources = "<< /Font << /F1 3 0 R /F2 4 0 R >> /XObject << " + xobjects + ">> >>";

    std::string kids;
    std::vector<std::string> contentObjs, pageObjs;
    for (int i = 0; i < nPages; ++i) {
        std::string& s = b.pages[i];
        contentObjs.push_back("<< /Length " + fmtInt(static_cast<long long>(s.size())) +
                              " >>\nstream\n" + s + "\nendstream");
        pageObjs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + objStr(kW) + " " +
                           objStr(kH) + "] /Contents " + fmtInt(contentBase + i) +
                           " 0 R /Resources " + resources + " >>");
        kids += fmtInt(pageBase + i) + " 0 R ";
    }

    all[0] = "<< /Type /Catalog /Pages 2 0 R >>";
    all[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + fmtInt(nPages) + " >>";
    for (auto& c : contentObjs) all.push_back(c);
    for (auto& p : pageObjs) all.push_back(p);

    // ---- Write file with xref ----
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets(all.size() + 1, 0);
    for (std::size_t i = 0; i < all.size(); ++i) {
        offsets[i + 1] = out.size();
        out += fmtInt(static_cast<long long>(i + 1)) + " 0 obj\n" + all[i] + "\nendobj\n";
    }
    std::size_t xrefPos = out.size();
    out += "xref\n0 " + fmtInt(static_cast<long long>(all.size() + 1)) + "\n";
    out += "0000000000 65535 f \n";
    for (std::size_t i = 1; i <= all.size(); ++i) {
        char b10[24];
        std::snprintf(b10, sizeof(b10), "%010zu 00000 n \n", offsets[i]);
        out += b10;
    }
    out += "trailer\n<< /Size " + fmtInt(static_cast<long long>(all.size() + 1)) +
           " /Root 1 0 R >>\nstartxref\n" + fmtInt(static_cast<long long>(xrefPos)) + "\n%%EOF\n";

    std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    return true;
}

bool exportReportPdf(const Report& rep, const std::string& path, const ReportInfo& info) {
    Builder b;
    renderReportBody(b, rep, info);
    return writePdf(b, path);
}

bool exportBatchPdf(const std::vector<Report>& reps, const std::string& path,
                    const ReportInfo& info) {
    Builder b;
    const double contentW = kW - 2 * kMargin;

    // ---- Summary cover page ----
    b.fillRect(kMargin, b.yTop, contentW, 46, kPanel);
    double textX = kMargin + 10;
    if (info.showLogo) {
        const double sz = 34, lx = kMargin + 8, ly = b.yTop + 6;
        std::string ln = b.addImage(
            kArgusLogoW, kArgusLogoH,
            std::string(reinterpret_cast<const char*>(kArgusLogoRGB),
                        static_cast<std::size_t>(kArgusLogoW) * kArgusLogoH * 3));
        b.image(ln, lx, ly, sz, sz);
        textX = lx + sz + 10;
    }
    b.rawText("ARGUS  AUDIO QA", textX, b.yTop + 8, 9, kMuted, true);
    b.rawText("Batch report - " + fmtInt(static_cast<long long>(reps.size())) + " file(s)", textX,
              b.yTop + 20, 14, kInk, true);
    {
        int fails = 0, warns = 0, errs = 0;
        for (const auto& r : reps) {
            if (!r.ok()) { ++errs; continue; }
            fails += r.count(Severity::Fail);
            warns += r.count(Severity::Warn);
        }
        std::string sub = fmtInt(fails) + " fail / " + fmtInt(warns) + " warn / " +
                          fmtInt(errs) + " err";
        b.rawText(sub, kW - kMargin - Builder::textW(sub, 8) - 10, b.yTop + 18, 8, kMuted, false);
    }
    b.yTop += 56;

    if (info.anyText() || info.showDate) {
        std::time_t now = std::time(nullptr);
        char d[32];
        std::strftime(d, sizeof(d), "%Y-%m-%d", std::localtime(&now));
        std::string who = info.showEngineer && !info.qaEngineer.empty() ? info.qaEngineer : "";
        if (info.showStudio && !info.studio.empty())
            who += (who.empty() ? "" : ", ") + info.studio;
        if (info.showDate) who += (who.empty() ? "" : "  -  ") + std::string(d);
        if (!who.empty()) b.line(who, kMargin, 8, kMuted, false);
    }

    b.hline(kMargin, kW - kMargin, b.yTop, kRule, 0.6);
    b.yTop += 6;
    b.line("FILES", kMargin, 9, kMuted, true);
    for (const auto& r : reps) {
        b.ensure(15);
        double y = b.yTop;
        Severity v = r.ok() ? r.verdict() : Severity::Fail;
        b.chip(kMargin, y, v);
        b.rawText(r.meta.filename, kMargin + 52, y + 2.5, 8.5, kInk, true);
        std::string s = r.ok() ? fmtInt(r.count(Severity::Fail)) + " fail / " +
                                     fmtInt(r.count(Severity::Warn)) + " warn"
                               : std::string("decode error");
        b.rawText(s, kW - kMargin - Builder::textW(s, 8) - 4, y + 2.5, 8, kMuted, false);
        b.yTop += 15;
    }

    // ---- One full report section per file ----
    for (const auto& r : reps) {
        b.newPage();
        renderReportBody(b, r, info);
    }
    return writePdf(b, path);
}

}  // namespace argus
