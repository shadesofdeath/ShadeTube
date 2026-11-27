// "Söz senkronlayıcı": see LyricsSync.h.
#include "app/LyricsSync.h"

#include "app/AppContext.h"
#include "app/BrowseWidgets.h"
#include "app/InternetRadio.h"
#include "app/LyricsService.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "lyrics/LrclibPublish.h"
#include "lyrics/LyricsSync.h"
#include "ui/Anim.h"
#include "ui/Micro.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>

namespace st::app {

namespace {

namespace type = gfx::type;
namespace fs = std::filesystem;
using gfx::accent;
using gfx::colors;
using Microsoft::WRL::ComPtr;
using Draft = lyrics::SyncDraft;

constexpr float kBodyH = 452;          // the dialog's content
constexpr float kAreaTop = ChipTabs::kH + 14;
constexpr float kControlsH = 40;       // the step's control row
constexpr float kLegendH = 32;         // the key legend under it (two lines)
constexpr int kNudgeMs = 100;
constexpr int kSeekMs = 5000;
constexpr int kLookaheadMs = 150;      // the lyrics views highlight a line this much before its stamp (NowPlaying)
constexpr float kSpeeds[] = {0.5f, 0.75f, 1.f};

// "01:23.45" (a stamp), "--:--.--" for none.
std::wstring stampText(int ms) {
    if (ms < 0) return L"--:--.--";
    wchar_t b[24];
    swprintf(b, 24, L"%02d:%02d%c%02d", ms / 60000, ms / 1000 % 60, static_cast<wchar_t>(i18n::decimalSeparator()), ms % 1000 / 10);
    return b;
}

std::wstring clipboardText(HWND hwnd) {
    std::wstring out;
    if (!OpenClipboard(hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void setClipboardText(HWND hwnd, const std::wstring& s) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

// ---- The text step: a plain multi-line editor ------------------------------------------------------------------------
// Lines as typed (no wrapping; a long line scrolls sideways with the caret), caret and selection across lines, the
// clipboard, word moves. Keeps every key while focused (Tab still leaves it; Escape goes on to the dialog). It is a
// ui::TextBox only so that the app treats it as text input (App::handleKey runs no playback shortcuts while a TextBox
// has focus: Space types a space); none of the single-line box's own behaviour is used.
class TextArea : public ui::TextBox {
public:
    explicit TextArea(std::wstring placeholder)
        : ui::TextBox(ui::TextBox::Look::Field), placeholder_(std::move(placeholder), type::bodyRegular) {
        focusable = true;
        gfx::TextOptions wrap;
        wrap.wrap = true;
        placeholder_.setOptions(wrap);
        sync();
    }
    std::function<void()> onChange;

    void setText(std::wstring_view text) {
        lines_.clear();
        std::wstring cur;
        for (size_t i = 0; i < text.size(); ++i) {
            const wchar_t ch = text[i];
            if (ch == L'\r' || ch == L'\n') {
                lines_.push_back(std::move(cur));
                cur.clear();
                if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            } else if (ch == L'\t') {
                cur.push_back(L' ');
            } else if (ch >= 32 && ch != 127) {
                cur.push_back(ch);
            }
        }
        lines_.push_back(std::move(cur));
        caret_ = anchor_ = {};
        scroll_.snap(0);
        scrollX_ = 0;
        sync();
        invalidate();
    }
    std::wstring text() const {
        std::wstring out;
        for (size_t i = 0; i < lines_.size(); ++i) {
            if (i) out += L"\n";
            out += lines_[i];
        }
        return out;
    }
    bool blank() const {
        for (const auto& l : lines_)
            if (l.find_first_not_of(L' ') != std::wstring::npos) return false;
        return true;
    }

    LPCWSTR cursor() const override { return IDC_IBEAM; }
    ui::FocusShape focusShape() const override { return ui::FocusShape::None; }   // the accent border shows it
    float preferredHeight(float) override { return rect().h; }
    void onFocusChanged(bool f) override {
        focusA_.to(f ? 1.f : 0.f, ui::motion::fast);
        blinkStart_ = ui::frame::realNow();
        invalidate();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        caret_ = posAt(e.pos);
        if (!e.shift) anchor_ = caret_;
        if (e.clicks == 2) {   // a word
            anchor_ = {caret_.line, wordLeft(caret_).col};
            caret_ = {caret_.line, wordRightInLine(caret_)};
        }
        goalX_ = -1;
        blinkStart_ = ui::frame::realNow();
        invalidate();
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        if (!pressed()) return;
        caret_ = posAt(e.pos);
        reveal();
        invalidate();
    }
    bool onWheel(float dy, float, const ui::MouseEvent&) override {
        scroll_.setBounds(0, maxScroll());
        scroll_.wheel(dy, kLineH * 3, textRect().h * 0.9f);
        invalidate();
        return true;
    }

    bool onKeyDown(const ui::KeyEvent& e) override {
        const auto move = [&](Pos to) {
            caret_ = to;
            if (!e.shift) anchor_ = caret_;
            blinkStart_ = ui::frame::realNow();
            reveal();
            invalidate();
            return true;
        };
        const bool sel = hasSelection();
        switch (e.vk) {
        case VK_LEFT:
            goalX_ = -1;
            if (sel && !e.shift) return move(std::min(caret_, anchor_));
            return move(e.ctrl ? wordLeft(caret_) : stepLeft(caret_));
        case VK_RIGHT:
            goalX_ = -1;
            if (sel && !e.shift) return move(std::max(caret_, anchor_));
            return move(e.ctrl ? wordRight(caret_) : stepRight(caret_));
        case VK_UP:
        case VK_DOWN:
        case VK_PRIOR:
        case VK_NEXT: {
            if (goalX_ < 0) goalX_ = xOf(caret_);
            const int page = std::max(1, static_cast<int>(textRect().h / kLineH) - 1);
            const int delta = e.vk == VK_UP ? -1 : e.vk == VK_DOWN ? 1 : e.vk == VK_PRIOR ? -page : page;
            const int line = std::clamp(static_cast<int>(caret_.line) + delta, 0, static_cast<int>(lines_.size()) - 1);
            const float keep = goalX_;
            move({static_cast<size_t>(line), colAt(static_cast<size_t>(line), keep)});
            goalX_ = keep;
            return true;
        }
        case VK_HOME: goalX_ = -1; return move(e.ctrl ? Pos{} : Pos{caret_.line, 0});
        case VK_END:
            goalX_ = -1;
            return move(e.ctrl ? Pos{lines_.size() - 1, lines_.back().size()} : Pos{caret_.line, lines_[caret_.line].size()});
        case VK_DELETE:
            if (sel) eraseSelection();
            else if (caret_.col < lines_[caret_.line].size())
                lines_[caret_.line].erase(caret_.col, e.ctrl ? wordRightInLine(caret_) - caret_.col : 1);
            else if (caret_.line + 1 < lines_.size()) {
                lines_[caret_.line] += lines_[caret_.line + 1];
                lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(caret_.line) + 1);
            }
            changed();
            return true;
        case VK_RETURN:
            if (e.ctrl || e.alt) return false;
            insert(L"\n");
            return true;
        case VK_ESCAPE: return false;   // the dialog's
        default: break;
        }
        if (e.ctrl) {
            HWND hwnd = window() ? window()->hwnd() : nullptr;
            switch (e.vk) {
            case 'A':
                anchor_ = {};
                caret_ = {lines_.size() - 1, lines_.back().size()};
                invalidate();
                return true;
            case 'C':
                if (sel) setClipboardText(hwnd, selectedText());
                return true;
            case 'X':
                if (sel) {
                    setClipboardText(hwnd, selectedText());
                    eraseSelection();
                    changed();
                }
                return true;
            case 'V': insert(clipboardText(hwnd)); return true;
            default: break;
            }
        }
        return false;
    }

    bool onChar(wchar_t ch) override {
        if (ch == 8 || ch == 127) {   // Backspace (Ctrl+Backspace: a word; some layouts send DEL for it)
            if (hasSelection()) {
                eraseSelection();
            } else if (caret_.col > 0) {
                const bool word = ch == 127 || GetKeyState(VK_CONTROL) < 0;
                const size_t from = word ? wordLeft(caret_).col : caret_.col - 1;
                lines_[caret_.line].erase(from, caret_.col - from);
                caret_.col = from;
            } else if (caret_.line > 0) {
                const size_t col = lines_[caret_.line - 1].size();
                lines_[caret_.line - 1] += lines_[caret_.line];
                lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(caret_.line));
                caret_ = {caret_.line - 1, col};
            }
            anchor_ = caret_;
            changed();
            return true;
        }
        if (ch < 32) return false;
        insert(std::wstring(1, ch));
        return true;
    }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect r = rect();
        const float f = focusA_, h = hover_;
        c.fillRounded(r, gfx::radii().field, col.bgRaised.mulAlpha(0.6f));
        c.strokeRounded(r, gfx::radii().field, Color::lerp(Color::lerp(col.hairDefault, col.hairStrong, h), accent().base, f));
        const Rect tr = textRect();
        scroll_.setBounds(0, maxScroll());
        const float sy = scroll_.value();
        c.pushClip({r.x + 1, r.y + 1, r.w - 2, r.h - 2});
        if (lines_.size() == 1 && lines_[0].empty()) {
            c.text(placeholder_, {tr.x, tr.y, tr.w, tr.h}, col.fgTertiary);
        }
        const size_t first = static_cast<size_t>(std::max(0.f, sy / kLineH));
        const size_t last = std::min(lines_.size(), static_cast<size_t>((sy + tr.h) / kLineH) + 2);
        const Pos a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
        for (size_t i = first; i < last; ++i) {
            const float y = tr.y + i * kLineH - sy;
            if (hasSelection() && i >= a.line && i <= b.line) {
                const float x0 = i == a.line ? xOf({i, a.col}) : 0.f;
                const float x1 = i == b.line ? xOf({i, b.col}) : xOf({i, lines_[i].size()}) + 6.f;   // the line break
                if (x1 > x0) c.fillRect({tr.x + x0 - scrollX_, y, x1 - x0, kLineH}, accent().tint25);
            }
            if (!lines_[i].empty())
                if (auto* l = texts_[i].layout(100000.f)) c.text(l, tr.x - scrollX_, y + (kLineH - lineBox()) * 0.5f, col.fgPrimary);
        }
        if (focused()) {
            const double t = std::fmod(ui::frame::realNow() - blinkStart_, 1060.0);
            const float y = tr.y + caret_.line * kLineH - sy;
            if (t < 530) c.fillRect({tr.x + xOf(caret_) - scrollX_, y + 3, 1.5f, kLineH - 6}, accent().base);
            if (auto* w = window()) w->invalidateAfter(t < 530 ? 530 - t : 1060 - t);
        }
        // A thin thumb while the text is taller than the box.
        if (const float total = lines_.size() * kLineH; total > tr.h + 1) {
            const float th = std::max(24.f, r.h * tr.h / total);
            const float ty = r.y + (r.h - th) * (sy / std::max(1.f, total - tr.h));
            c.fillRounded({r.right() - 5, ty + 2, 3, th - 4}, gfx::radii().scrollbarThumb, col.hairStrong);
        }
        c.popClip();
    }

private:
    struct Pos {
        size_t line = 0, col = 0;
        auto operator<=>(const Pos&) const = default;
    };
    static constexpr float kPad = 12, kLineH = 22;

    float lineBox() const { return type::bodyRegular.size * type::bodyRegular.lineHeight; }
    Rect textRect() const {
        const Rect r = rect();
        return {r.x + kPad, r.y + 10, std::max(0.f, r.w - kPad * 2), std::max(0.f, r.h - 20)};
    }
    float maxScroll() const { return std::max(0.f, lines_.size() * kLineH - textRect().h); }
    void sync() {
        texts_.resize(lines_.size());
        for (size_t i = 0; i < lines_.size(); ++i)
            if (texts_[i].text() != lines_[i] || texts_[i].style() != type::bodyRegular) texts_[i] = gfx::Text(lines_[i], type::bodyRegular);
    }
    float xOf(Pos p) {
        if (p.line >= lines_.size() || p.col == 0 || lines_[p.line].empty()) return 0;
        auto* l = texts_[p.line].layout(100000.f);
        if (!l) return 0;
        DWRITE_HIT_TEST_METRICS m{};
        float x = 0, y = 0;
        l->HitTestTextPosition(static_cast<UINT32>(std::min(p.col, lines_[p.line].size())), FALSE, &x, &y, &m);
        return x;
    }
    size_t colAt(size_t line, float x) {
        if (line >= lines_.size() || lines_[line].empty()) return 0;
        auto* l = texts_[line].layout(100000.f);
        if (!l) return 0;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        l->HitTestPoint(x, 1, &trailing, &inside, &m);
        return std::min(lines_[line].size(), static_cast<size_t>(m.textPosition + (trailing ? m.length : 0)));
    }
    Pos posAt(gfx::Point p) {
        const Rect tr = textRect();
        const float y = p.y - tr.y + scroll_.value();
        const size_t line = static_cast<size_t>(std::clamp(static_cast<int>(std::floor(y / kLineH)), 0, static_cast<int>(lines_.size()) - 1));
        return {line, colAt(line, p.x - tr.x + scrollX_)};
    }
    void reveal() {
        const Rect tr = textRect();
        const float top = caret_.line * kLineH;
        float target = scroll_.target();
        if (top < target) target = top;
        if (top + kLineH > target + tr.h) target = top + kLineH - tr.h;
        scroll_.setBounds(0, maxScroll());
        if (target != scroll_.target()) scroll_.snap(std::clamp(target, 0.f, maxScroll()));
        const float x = xOf(caret_);
        if (x - scrollX_ > tr.w - 4) scrollX_ = x - tr.w + 4;
        if (x - scrollX_ < 0) scrollX_ = x;
    }
    bool hasSelection() const { return caret_ != anchor_; }
    std::wstring selectedText() const {
        const Pos a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
        if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
        std::wstring s = lines_[a.line].substr(a.col);
        for (size_t i = a.line + 1; i < b.line; ++i) s += L"\r\n" + lines_[i];
        return s + L"\r\n" + lines_[b.line].substr(0, b.col);
    }
    void eraseSelection() {
        if (!hasSelection()) return;
        const Pos a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
        lines_[a.line] = lines_[a.line].substr(0, a.col) + lines_[b.line].substr(b.col);
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(a.line) + 1, lines_.begin() + static_cast<std::ptrdiff_t>(b.line) + 1);
        caret_ = anchor_ = a;
    }
    void insert(std::wstring_view s) {
        eraseSelection();
        std::vector<std::wstring> pieces{{}};
        for (size_t i = 0; i < s.size(); ++i) {
            const wchar_t ch = s[i];
            if (ch == L'\r' || ch == L'\n') {
                pieces.emplace_back();
                if (ch == L'\r' && i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            } else if (ch == L'\t') {
                pieces.back().push_back(L' ');
            } else if (ch >= 32 && ch != 127) {
                pieces.back().push_back(ch);
            }
        }
        std::wstring& line = lines_[caret_.line];
        const std::wstring tail = line.substr(caret_.col);
        line.erase(caret_.col);
        line += pieces[0];
        size_t at = caret_.line;
        for (size_t k = 1; k < pieces.size(); ++k) lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(++at), pieces[k]);
        caret_ = {at, lines_[at].size()};
        lines_[at] += tail;
        anchor_ = caret_;
        changed();
    }
    void changed() {
        sync();
        goalX_ = -1;
        blinkStart_ = ui::frame::realNow();
        reveal();
        if (onChange) onChange();
        invalidate();
    }
    Pos stepLeft(Pos p) const {
        if (p.col > 0) return {p.line, p.col - 1};
        return p.line > 0 ? Pos{p.line - 1, lines_[p.line - 1].size()} : p;
    }
    Pos stepRight(Pos p) const {
        if (p.col < lines_[p.line].size()) return {p.line, p.col + 1};
        return p.line + 1 < lines_.size() ? Pos{p.line + 1, 0} : p;
    }
    Pos wordLeft(Pos p) const {
        if (p.col == 0) return stepLeft(p);
        const std::wstring& s = lines_[p.line];
        size_t i = std::min(p.col, s.size());
        while (i > 0 && std::iswspace(s[i - 1])) --i;
        while (i > 0 && !std::iswspace(s[i - 1])) --i;
        return {p.line, i};
    }
    size_t wordRightInLine(Pos p) const {
        const std::wstring& s = lines_[p.line];
        size_t i = p.col;
        while (i < s.size() && !std::iswspace(s[i])) ++i;
        while (i < s.size() && std::iswspace(s[i])) ++i;
        return i;
    }
    Pos wordRight(Pos p) const {
        if (p.col >= lines_[p.line].size()) return stepRight(p);
        return {p.line, wordRightInLine(p)};
    }

    std::vector<std::wstring> lines_{std::wstring()};
    std::vector<gfx::Text> texts_;
    gfx::Text placeholder_;
    Pos caret_, anchor_;
    float goalX_ = -1, scrollX_ = 0;
    ui::SmoothScroll scroll_;
    double blinkStart_ = 0;
    ui::Anim hover_, focusA_;
};

// ---- The tap step's list ----------------------------------------------------------------------------------------------
// Every line with its stamp: the cursor line (where the next tap goes) in the accent, the line playing now marked with
// a dot, stamps out of order in the warning color. Follows the cursor unless the wheel moved it a moment ago.
class StampList : public ui::Widget {
public:
    explicit StampList(const Draft& d) : d_(d) { focusable = true; }
    std::function<void(size_t)> onPick;     // a click: the cursor goes to that line
    std::function<void(size_t)> onReplay;   // a double click: play from its stamp
    int playing = -1;                       // the line playing now (the body sets it each frame)

    void rebuild() {
        texts_.clear();
        texts_.reserve(d_.size());
        for (const auto& l : d_.lines())
            texts_.emplace_back(l.text.empty() ? std::wstring(L"♪") : toWide(l.text), type::bodyRegular);
        invalidate();
    }
    void follow() { manualUntil_ = 0; }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        const int i = rowAt(e.pos);
        if (i < 0) return true;
        if (e.clicks == 2) {
            if (onReplay) onReplay(static_cast<size_t>(i));
        } else if (onPick) {
            onPick(static_cast<size_t>(i));
        }
        manualUntil_ = 0;
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const int i = rowAt(e.pos);
        if (i != hover_) {
            hover_ = i;
            invalidate();
        }
    }
    void onMouseLeave() override {
        hover_ = -1;
        invalidate();
    }
    bool onWheel(float dy, float, const ui::MouseEvent&) override {
        scroll_.setBounds(0, maxScroll());
        scroll_.wheel(dy, kRowH * 2, rect().h * 0.9f);
        manualUntil_ = ui::frame::realNow() + 2500;
        invalidate();
        return true;
    }
    LPCWSTR cursor() const override { return IDC_HAND; }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect r = rect();
        c.strokeRounded(r, gfx::radii().field, col.hairDefault);
        if (texts_.size() != d_.size()) rebuild();
        // Keep the cursor line in view, a little above the middle.
        scroll_.setBounds(0, maxScroll());
        if (ui::frame::realNow() >= manualUntil_) {
            const float target = std::clamp(kPadY + std::min(d_.cursor(), d_.size() ? d_.size() - 1 : 0) * kRowH - r.h * 0.4f, 0.f, maxScroll());
            if (std::abs(target - scroll_.target()) > 0.5f) scroll_.glideTo(target, ui::ScrollFeel::Follow);
        } else if (auto* w = window()) {
            w->invalidateAfter(manualUntil_ - ui::frame::realNow() + 1);
        }
        const float sy = scroll_.value();
        const int disorder = d_.firstOutOfOrder();
        c.pushClip({r.x + 1, r.y + 1, r.w - 2, r.h - 2});
        const size_t first = static_cast<size_t>(std::max(0.f, (sy - kPadY) / kRowH));
        const size_t last = std::min(d_.size(), static_cast<size_t>((sy + r.h) / kRowH) + 2);
        for (size_t i = first; i < last; ++i) {
            const Rect row{r.x + 1, r.y + kPadY + i * kRowH - sy, r.w - 2, kRowH};
            const auto& line = d_.line(i);
            const bool cursor = i == d_.cursor();
            if (cursor) {
                c.fillRect(row, accent().tint12);
                c.fillRect({row.x, row.y, 2, row.h}, accent().base);
            } else if (static_cast<int>(i) == hover_) {
                c.fillRect(row, col.overlayHover);
            }
            if (static_cast<int>(i) == playing) c.fillCircle({row.x + 12, row.cy()}, 3, accent().base);
            const bool late = static_cast<int>(i) == disorder;   // stamped before the line above it
            const Color timeColor = late ? col.warning : line.timeMs >= 0 ? col.fgSecondary : col.fgTertiary;
            c.text(stampText(line.timeMs), type::monoDuration, {row.x + 22, row.y, 80, row.h}, timeColor, gfx::TextAlign::Leading,
                   gfx::VAlign::Center);
            const Color textColor = line.text.empty() ? col.fgTertiary : cursor || line.timeMs >= 0 ? col.fgPrimary : col.fgSecondary;
            c.text(texts_[i], {row.x + 108, row.y, row.w - 120, row.h}, textColor, gfx::VAlign::Center);
            if (cursor) {   // where the next tap goes
                const std::wstring tag = toUpperTr(tr(L"Sıradaki"));
                c.text(tag, type::monoBadge, {row.right() - 92, row.y, 80, row.h}, accent().base, gfx::TextAlign::Trailing,
                       gfx::VAlign::Center);
            }
        }
        if (d_.cursor() >= d_.size() && !d_.empty()) {   // past the last line
            const Rect row{r.x + 1, r.y + kPadY + d_.size() * kRowH - sy, r.w - 2, kRowH};
            c.fillRect({row.x, row.y, 2, row.h}, accent().base);
            c.text(tr(L"Bütün satırlar damgalandı. Önizle'ye geç ya da bir satıra tıklayıp yeniden damgala."), type::caption,
                   {row.x + 22, row.y, row.w - 34, row.h}, col.fgSecondary, gfx::TextAlign::Leading, gfx::VAlign::Center);
        }
        c.popClip();
    }

private:
    static constexpr float kRowH = 34, kPadY = 6;
    float maxScroll() const { return std::max(0.f, kPadY * 2 + (d_.size() + 1) * kRowH - rect().h); }
    int rowAt(gfx::Point p) const {
        const Rect r = rect();
        if (!r.contains(p)) return -1;
        const int i = static_cast<int>(std::floor((p.y - r.y - kPadY + scroll_.value()) / kRowH));
        return i >= 0 && i < static_cast<int>(d_.size()) ? i : -1;
    }

    const Draft& d_;
    std::vector<gfx::Text> texts_;
    ui::SmoothScroll scroll_;
    double manualUntil_ = 0;
    int hover_ = -1;
};

// ---- The preview step: the result as the lyrics views show it ---------------------------------------------------------
// The line playing now large and bright, the others smaller and dimmer by distance, the playing one kept a little above
// the middle (Now Playing's lyrics: same lookahead, sizes of its compact layout scaled to the dialog).
class PreviewView : public ui::Widget {
public:
    PreviewView() { focusable = true; }       // keeps the keys while focused (they go on to the body)
    std::function<int()> clock;               // the song position (ms)
    std::function<void(int)> onSeek;          // a click on a line: there

    // `keepPlace`: the same lines with other times (a nudge, an undo): the view stays where it is.
    void setLyrics(lyrics::Lyrics l, bool keepPlace = false) {
        keepPlace = keepPlace && l.lines.size() == l_.lines.size();
        l_ = std::move(l);
        lines_.clear();
        gfx::TextOptions wrap;
        wrap.wrap = true;
        for (const auto& x : l_.lines) lines_.emplace_back(x.text.empty() ? std::wstring(L"♪") : toWide(x.text), inactiveStyle(), wrap);
        heights_.assign(lines_.size(), -1.f);
        active_ = -2;
        activeText_ = gfx::Text({}, activeStyle(), wrap);
        if (!keepPlace) scroll_.snap(0);
        invalidate();
    }
    int active() const { return active_; }
    const lyrics::Lyrics& lyrics() const { return l_; }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        for (size_t i = 0; i < rects_.size(); ++i)
            if (rects_[i].contains(e.pos) && i < l_.lines.size() && onSeek) {
                onSeek(l_.lines[i].timeMs);
                return true;
            }
        return true;
    }
    LPCWSTR cursor() const override { return IDC_HAND; }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect r = rect();
        c.strokeRounded(r, gfx::radii().field, col.hairDefault);
        if (l_.lines.empty()) {
            c.text(tr(L"Önizlemek için önce birkaç satırı damgala."), type::bodyRegular, r, col.fgTertiary, gfx::TextAlign::Center,
                   gfx::VAlign::Center);
            return;
        }
        const Rect area{r.x + 24, r.y + 12, r.w - 48, r.h - 24};
        const int pos = clock ? clock() : 0;
        const int active = lyrics::activeLine(l_, pos + kLookaheadMs);
        if (active != active_) {
            active_ = active;
            activeText_.setText(active >= 0 ? lines_[static_cast<size_t>(active)].text() : L"");
        }
        if (heightsW_ != area.w) {
            heights_.assign(lines_.size(), -1.f);
            heightsW_ = area.w;
        }
        rects_.resize(lines_.size());
        float y = 0, activeTop = 0, activeH = 0;
        for (size_t i = 0; i < lines_.size(); ++i) {
            float h;
            if (static_cast<int>(i) == active_) {
                h = activeText_.measure(area.w).h;
                activeTop = y;
                activeH = h;
            } else {
                if (heights_[i] < 0) {
                    heights_[i] = lines_[i].measure(area.w).h;
                    lines_[i].reset();
                }
                h = heights_[i];
            }
            rects_[i] = {area.x, y, area.w, h};
            y += h + 12;
        }
        const float lo = -area.h * 0.5f, hi = std::max(0.f, y - area.h * 0.5f);
        scroll_.setBounds(lo, hi);
        const float target = std::clamp(active_ >= 0 ? activeTop + activeH * 0.5f - area.h * 0.42f : -area.h * 0.3f, lo, hi);
        if (std::abs(target - scroll_.target()) > 0.5f) scroll_.glideTo(target, ui::ScrollFeel::Follow);
        const float off = scroll_.value();
        const float edge = std::min(48.f, area.h * 0.18f);
        c.pushClip(area);
        for (size_t i = 0; i < lines_.size(); ++i) {
            Rect lr = rects_[i].offset(0, area.y - off);
            rects_[i] = lr;
            if (lr.bottom() < area.y || lr.y > area.bottom()) {
                lines_[i].reset();
                continue;
            }
            Color colr = col.fgPrimary;
            if (static_cast<int>(i) != active_) {
                const int dist = std::abs(static_cast<int>(i) - active_);
                const float a = static_cast<int>(i) < active_ ? 0.38f : 0.26f;
                colr = col.fgPrimary.withAlpha(dist >= 3 ? a * 0.65f : a);
            }
            const float edgeA = std::clamp((lr.cy() - area.y) / edge, 0.f, 1.f) * std::clamp((area.bottom() - lr.cy()) / edge, 0.f, 1.f);
            colr = colr.mulAlpha(0.15f + 0.85f * edgeA);
            if (static_cast<int>(i) == active_) c.text(activeText_, lr, colr);
            else c.text(lines_[i], lr, colr);
        }
        c.popClip();
    }

private:
    static gfx::TextStyle inactiveStyle() { return type::lyricInactive.withSize(20); }
    static gfx::TextStyle activeStyle() { return type::lyricActive.withSize(28); }
    lyrics::Lyrics l_;
    std::vector<gfx::Text> lines_;
    std::vector<float> heights_;
    std::vector<Rect> rects_;
    float heightsW_ = -1;
    gfx::Text activeText_;
    int active_ = -2;
    ui::SmoothScroll scroll_;
};

// ---- Files ---------------------------------------------------------------------------------------------------------

std::wstring safeFileName(std::wstring w) {
    for (auto& ch : w)
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    if (w.empty()) w = L"lyrics";
    if (w.size() > 120) w.resize(120);
    return w;
}

// The .lrc save dialog owned by the main window. Dev / tests: SHADETUBE_LYRICS_EXPORT=<file> answers it. Modal: call
// it posted, outside a widget event. Empty = cancelled.
fs::path askLrcFile(const std::wstring& name) {
    wchar_t dev[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"SHADETUBE_LYRICS_EXPORT", dev, MAX_PATH) > 0) return dev;
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    const std::wstring lrc = tr(L"Senkronlanmış sözler (.lrc)");
    const COMDLG_FILTERSPEC types[] = {{lrc.c_str(), L"*.lrc"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"lrc");
    dlg->SetFileName(safeFileName(name).c_str());
    const std::wstring title = tr(L"Sözleri .lrc olarak dışa aktar");
    dlg->SetTitle(title.c_str());
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    fs::path file = path;
    CoTaskMemFree(path);
    return file;
}

bool writeText(const fs::path& file, const std::string& text) {
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

void showFileToast(const std::wstring& message, const fs::path& file) {
    ui::Toasts::show(ctx().window, message, ui::ToastKind::Success, tr(L"Dosyayı göster"), [file] {
        const std::wstring arg = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    });
}

// Worker: the .lrc next to a download / local file. One already there that says something else is kept as .lrc.bak
// (it may be the user's own).
bool writeSidecarKeepingOld(const fs::path& audio, const lyrics::Lyrics& l, const catalog::Track& t) {
    const fs::path side = lyrics::sidecarPath(audio);
    std::error_code ec;
    if (fs::exists(side, ec)) {
        auto bak = side;
        bak += L".bak";
        fs::copy_file(side, bak, fs::copy_options::overwrite_existing, ec);
        if (ec) ST_LOG_WARN("lyrics", "could not keep the old .lrc: {}", ec.message());
    }
    return writeLyricsSidecar(audio.wstring(), l, t);
}

Lifetime& publishLife() {
    static Lifetime life;
    return life;
}

class SyncBody;
SyncBody* g_open = nullptr;   // the synchronizer on screen (its player changes are undone at exit too)

// ---- The dialog's content --------------------------------------------------------------------------------------------

class SyncBody : public ui::Widget {
public:
    enum class Step { Text, Tap, Preview };

    explicit SyncBody(const catalog::Track& t) : track_(t) {
        auto* p = ctx().player;
        speedBefore_ = p ? p->speed() : 1.f;
        repeatBefore_ = p ? p->repeat() : RepeatMode::Off;
        steps_ = add<ChipTabs>(std::vector<std::wstring>{tr(L"Metin"), tr(L"Senkronla"), tr(L"Önizle")}, 0);
        steps_->onSelect = [this](int i) {
            Dispatcher::post([this, i, ref = life_.ref()] {
                if (!ref.expired()) go(static_cast<Step>(i));
            });
        };
        text_ = add<TextArea>(tr(L"Sözleri buraya yapıştır ya da yaz: her satır bir söz satırı olur. Boş satırlar atlanır; "
                                 L"yalnızca ♪ yazan satır müzik arası olarak damgalanır."));
        text_->onChange = [this] { textChanged(); };
        list_ = add<StampList>(draft_);
        list_->onPick = [this](size_t i) {
            draft_.setCursor(i);
            changed(false);
        };
        list_->onReplay = [this](size_t i) {
            const int t = draft_.line(i).timeMs >= 0 ? draft_.line(i).timeMs : (i > 0 ? draft_.line(i - 1).timeMs : 0);
            draft_.setCursor(i);
            seekSong(std::max(0, t - 1000));
            changed(false);
        };
        preview_ = add<PreviewView>();
        preview_->clock = [this] { return songNow(); };
        preview_->onSeek = [this](int ms) { seekSong(std::max(0, ms - 300)); };

        start_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Senkronlamaya başla"), "chevron-right");
        start_->onClick = [this] { go(Step::Tap); };
        stamp_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Damgala"), "clock");
        stamp_->setTooltip(tr(L"Sıradaki satırı şu ana damgala\tBoşluk"));
        stamp_->onClick = [this] { stampNow(); };
        back5_ = add<ui::Button>(ui::ButtonKind::Ghost, i18n::format(tr(L"{} sn"), {L"−5"}));
        back5_->setTooltip(tr(L"5 saniye geri sar\t←"));
        back5_->onClick = [this] { seekBy(-kSeekMs); };
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Outline);
        play_->onClick = [this] { togglePlay(); };
        for (size_t i = 0; i < std::size(kSpeeds); ++i) {
            speeds_[i] = add<ui::Button>(ui::ButtonKind::Chip, speedLabel(kSpeeds[i]));
            speeds_[i]->setTooltip(i + 1 < std::size(kSpeeds) ? tr(L"Daha yavaş çal: daha hassas damgalarsın")
                                                               : tr(L"Normal hızda çal"));
            speeds_[i]->onClick = [this, i] { setSpeed(kSpeeds[i]); };
        }
        restart_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Baştan oynat"), "prev");
        restart_->onClick = [this] {
            const auto l = draft_.toLyrics();
            seekSong(l.lines.empty() ? 0 : std::max(0, l.lines.front().timeMs - 2000));
            if (auto* p = ctx().player; p && !p->isPlaying()) p->play();
        };
        const std::wstring tenth = L"0" + std::wstring(1, i18n::decimalSeparator()) + L"1";
        earlier_ = add<ui::Button>(ui::ButtonKind::Chip, i18n::format(tr(L"{} sn"), {L"−" + tenth}));
        earlier_->setTooltip(tr(L"Çalan satırı biraz erkene al\t⇧←"));
        earlier_->onClick = [this] { nudgeShown(-kNudgeMs); };
        later_ = add<ui::Button>(ui::ButtonKind::Chip, i18n::format(tr(L"{} sn"), {L"+" + tenth}));
        later_->setTooltip(tr(L"Çalan satırı biraz geciktir\t⇧→"));
        later_->onClick = [this] { nudgeShown(kNudgeMs); };
        playSong_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Şarkıyı çal"), "play");
        playSong_->onClick = [this] {
            if (auto* p = ctx().player) {
                p->playNext(track_);
                p->next();
            }
        };
        load();
        go(Step::Text);
        g_open = this;
    }
    ~SyncBody() override {
        life_.renew();
        if (g_open == this) g_open = nullptr;
    }

    std::function<void()> onStateChanged;   // readiness / dirty changed: the footer follows
    bool dirty() const { return dirty_; }
    bool ready() const { return draft_.ready(); }
    const Draft& draft() const { return draft_; }
    const catalog::Track& track() const { return track_; }
    TextArea* textArea() const { return text_; }

    // Before the dialog goes: the player as it was (speed, repeat).
    void restorePlayer() {
        auto* p = ctx().player;
        if (!p) return;
        if (speedChanged_) p->setSpeed(speedBefore_);
        if (repeatChanged_) p->setRepeat(repeatBefore_);
        speedChanged_ = repeatChanged_ = false;
    }
    void markSaved() {
        dirty_ = false;
        if (onStateChanged) onStateChanged();
    }

    float preferredHeight(float) override { return kBodyH; }

    void layout() override {
        const Rect r = rect();
        steps_->setRect({0, 0, r.w, ChipTabs::kH});
        const float controlsY = r.h - kLegendH - 8 - kControlsH;
        const Rect area{0, kAreaTop, r.w, std::max(0.f, controlsY - 14 - kAreaTop)};
        text_->setRect(area);
        list_->setRect(area);
        preview_->setRect(area);
        // Controls from the left, chips from the right; the position fills what is between them.
        float x = 0, right = r.w;
        auto left = [&](ui::Widget* w, float width, float h = kControlsH) {
            w->setRect({x, controlsY + (kControlsH - h) * 0.5f, width, h});
            x += width + 8;
        };
        auto fromRight = [&](ui::Widget* w, float width, float h, float gap) {
            right -= width;
            w->setRect({right, controlsY + (kControlsH - h) * 0.5f, width, h});
            right -= gap;
        };
        auto natural = [](ui::Button* b) { return std::ceil(b->naturalWidth()); };
        if (step_ == Step::Text) {
            fromRight(start_, natural(start_), kControlsH, 8);
        } else if (!isCurrent()) {
            left(playSong_, natural(playSong_));
        } else if (step_ == Step::Tap) {
            left(stamp_, std::max(150.f, natural(stamp_) + 24));
            left(play_, kControlsH);
            left(back5_, natural(back5_));
            for (size_t i = std::size(kSpeeds); i-- > 0;) fromRight(speeds_[i], natural(speeds_[i]), ChipTabs::kH, 6);
        } else {
            left(restart_, natural(restart_));
            left(play_, kControlsH);
            left(back5_, natural(back5_));
            fromRight(later_, natural(later_), ChipTabs::kH, 6);
            fromRight(earlier_, natural(earlier_), ChipTabs::kH, 6);
        }
        positionRect_ = {x, controlsY, std::max(0.f, right - x), kControlsH};
        legendRect_ = {0, r.h - kLegendH, r.w, kLegendH};
    }

    // Which widgets the step shows (and whether the song is the one playing).
    void syncVisibility() {
        const bool cur = isCurrent();
        wasCurrent_ = cur;
        const bool tap = step_ == Step::Tap, preview = step_ == Step::Preview, text = step_ == Step::Text;
        text_->setVisible(text);
        list_->setVisible(tap);
        preview_->setVisible(preview);
        start_->setVisible(text);
        playSong_->setVisible(!text && !cur);
        stamp_->setVisible(tap && cur);
        for (auto* s : speeds_) s->setVisible(tap && cur);
        play_->setVisible(!text && cur);
        back5_->setVisible(!text && cur);
        for (ui::Button* b : {restart_, earlier_, later_}) b->setVisible(preview && cur);
        requestLayout();
        invalidate();
    }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect r = rect();
        auto* p = ctx().player;
        const bool current = isCurrent();
        if (current != wasCurrent_) {   // the song changed under the dialog (or came back): other controls
            Dispatcher::post([this, ref = life_.ref()] {
                if (!ref.expired()) syncVisibility();
            });
        }
        // The counter beside the steps.
        if (loaded_ && !draft_.empty()) {
            const std::wstring counter = i18n::format(tr(L"{} / {} satır damgalandı"),
                                                      {std::to_wstring(draft_.stampedCount()), std::to_wstring(draft_.size())});
            c.text(toUpperTr(counter), type::monoLabel, {r.x + r.w * 0.5f, r.y, r.w * 0.5f, ChipTabs::kH},
                   draft_.ready() ? accent().base : col.fgTertiary, gfx::TextAlign::Trailing, gfx::VAlign::Center);
        }
        if (step_ == Step::Text && !loaded_) {   // the song's lyrics on their way
            const Rect area = text_->rect();
            for (int i = 0; i < 6; ++i)
                ui::paintSkeleton(c, this, {area.x + 14, area.y + 14 + i * 26.f, area.w * (0.35f + 0.1f * ((i * 7) % 4)), 14},
                                  gfx::radii().badge);
        }
        if (step_ == Step::Tap) list_->playing = current ? draft_.activeAt(songNow() + kLookaheadMs) : -1;
        if (p) play_->setPlaying(current && p->isPlaying());
        for (size_t i = 0; i < std::size(kSpeeds); ++i)
            speeds_[i]->setActive(p && std::abs(p->speed() - kSpeeds[i]) < 0.01f);
        paintChildren(c);
        // The position, between the controls; a note when the song isn't playing here.
        const Rect pos{r.x + positionRect_.x + 4, r.y + positionRect_.y, std::max(0.f, positionRect_.w - 8), kControlsH};
        if (step_ != Step::Text) {
            if (!current) {
                c.text(tr(L"Bu şarkı şu an çalmıyor: damgalamak için çal."), type::secondary, pos, col.fgSecondary,
                       gfx::TextAlign::Leading, gfx::VAlign::Center);
            } else if (p) {
                const int64_t dur = std::max<int64_t>(0, p->durationMs());
                std::wstring s = stampText(songNow()) + L" / " + ui::formatDuration(dur);
                c.text(s, type::monoDuration, pos, col.fgSecondary, gfx::TextAlign::Leading, gfx::VAlign::Center);
            }
        }
        // The keys of the step (two lines at most).
        const wchar_t* legend = step_ == Step::Text
                                    ? tr(L"Metni düzenle, sonra şarkıyla birlikte satır satır damgala · Ctrl+Enter başla")
                                : step_ == Step::Tap
                                    ? tr(L"Boşluk / Enter damgala · ⌫ bir satır geri · ↑ ↓ satır seç · Ctrl+Z geri al\n"
                                         L"⇧← ⇧→ ±0,1 sn · ← → 5 sn · P duraklat · Ctrl+Enter önizle")
                                    : tr(L"Boşluk duraklat · ← → 5 sn · ⇧← ⇧→ çalan satır ±0,1 sn · Ctrl+Z geri al\n"
                                         L"Bir satıra tıkla: oradan çalar · Enter kaydet");
        if (legend_.text() != legend) {
            gfx::TextOptions wrap;
            wrap.wrap = true;
            wrap.maxLines = 2;
            legend_ = gfx::Text(legend, type::caption, wrap);
        }
        c.text(legend_, {r.x + legendRect_.x, r.y + legendRect_.y, legendRect_.w, legendRect_.h}, col.fgTertiary, gfx::VAlign::Center);
        // Playing: the position and the highlight move (Now Playing's synced lyrics pace: 20 fps).
        if (step_ != Step::Text && current && p && p->isPlaying())
            if (auto* w = window()) w->invalidateAfter(50);
    }

    // Keys of the tap and preview steps (from the focused widget's chain, or the dialog when nothing took them).
    bool key(const ui::KeyEvent& e) {
        // Ctrl+Enter: on to the next step (Metin -> Senkronla -> Önizle).
        if (e.ctrl && !e.alt && e.vk == VK_RETURN && loaded_ && step_ != Step::Preview) {
            if (!e.repeat) {
                const Step next = step_ == Step::Text ? Step::Tap : Step::Preview;
                Dispatcher::post([this, next, ref = life_.ref()] {
                    if (!ref.expired()) go(next);
                });
            }
            return true;
        }
        if (step_ == Step::Text || !loaded_) return false;
        if (e.ctrl && e.vk == 'Z') {
            if (draft_.undo()) changed(true);
            return true;
        }
        if (e.ctrl || e.alt) return false;
        const bool tap = step_ == Step::Tap;
        switch (e.vk) {
        case VK_SPACE:
            if (e.repeat) return true;
            if (tap) stampNow();
            else togglePlay();
            return true;
        case VK_RETURN:
            if (!tap) return false;   // the preview's Enter is the dialog's (Kaydet)
            if (!e.repeat) stampNow();
            return true;
        case VK_BACK:
            if (!tap) return false;
            seekSong(draft_.back());
            changed(true);
            return true;
        case VK_UP:
        case VK_DOWN:
            if (!tap) return false;
            if (e.vk == VK_UP) draft_.setCursor(draft_.cursor() > 0 ? draft_.cursor() - 1 : 0);
            else draft_.skip();
            list_->follow();
            changed(false);
            return true;
        case VK_LEFT:
        case VK_RIGHT: {
            const int dir = e.vk == VK_LEFT ? -1 : 1;
            if (e.shift) {
                if (tap) nudgeLast(dir * kNudgeMs);
                else nudgeShown(dir * kNudgeMs);
            } else {
                seekBy(dir * kSeekMs);
            }
            return true;
        }
        case VK_DELETE:
            if (!tap) return false;
            if (draft_.cursor() < draft_.size()) {
                draft_.clear(draft_.cursor());
                changed(true);
            }
            return true;
        case 'P':
            togglePlay();
            return true;
        default: return false;
        }
    }
    bool onKeyDown(const ui::KeyEvent& e) override { return key(e); }

    void go(Step s) {
        if (s != Step::Text && step_ == Step::Text) applyText();
        if (s != Step::Text && draft_.empty()) {   // nothing to time yet
            s = Step::Text;
            toast(tr(L"Önce sözleri yaz ya da yapıştır"));
        }
        const bool entering = s != step_;
        step_ = s;
        steps_->select(static_cast<int>(s), false);
        if (s != Step::Text) holdSong();
        if (s == Step::Tap && entering) {
            list_->rebuild();
            list_->follow();
            // A fresh start: from the top of the song.
            if (auto* p = ctx().player; p && isCurrent() && draft_.stampedCount() == 0) {
                seekSong(0);
                if (!p->isPlaying()) p->play();
            }
        }
        if (s == Step::Preview) preview_->setLyrics(draft_.toLyrics());
        syncVisibility();
        if (auto* w = window()) {
            ui::Widget* target = text_;
            if (s == Step::Tap) target = list_;
            else if (s == Step::Preview) target = preview_;
            w->setFocus(target);
        }
    }

private:
    bool isCurrent() const {
        const auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;
        return t && t->id == track_.id;
    }
    // The song's timeline (SponsorBlock cuts out), without the user's timing offset: what a stamp is.
    int songNow() const { return lyricsClockMs(track_.id) + lyricsOffset(track_.id); }
    void seekSong(int ms) {
        if (!isCurrent()) return;
        seekToLyricsTime(track_.id, std::max(0, ms) - lyricsOffset(track_.id));
        invalidate();
    }
    void seekBy(int delta) { seekSong(std::max(0, songNow() + delta)); }
    void togglePlay() {
        if (auto* p = ctx().player; p && isCurrent()) p->togglePause();
        invalidate();
    }
    void setSpeed(float s) {
        auto* p = ctx().player;
        if (!p) return;
        p->setSpeed(s);
        speedChanged_ = std::abs(s - speedBefore_) > 0.001f || speedChanged_;
        invalidate();
    }
    // While timing, the song repeats instead of moving on to the next one (put back when the dialog closes).
    void holdSong() {
        auto* p = ctx().player;
        if (!p || !isCurrent() || p->repeat() == RepeatMode::One) return;
        if (!repeatChanged_) repeatBefore_ = p->repeat();
        p->setRepeat(RepeatMode::One);
        repeatChanged_ = true;
    }

    void stampNow() {
        if (!isCurrent() || draft_.cursor() >= draft_.size()) return;
        draft_.stamp(songNow());
        list_->follow();
        changed(true);
    }
    void nudgeLast(int delta) {
        const int i = draft_.lastStamped();
        if (i >= 0 && draft_.nudge(static_cast<size_t>(i), delta)) changed(true);
    }
    // The preview: the line playing now (else the next one), and the preview follows.
    void nudgeShown(int delta) {
        int i = preview_->active();
        if (i < 0) i = 0;
        if (static_cast<size_t>(i) < draft_.size() && draft_.nudge(static_cast<size_t>(i), delta)) changed(true);
    }

    void changed(bool stamps) {
        if (stamps) dirty_ = true;
        if (stamps && step_ == Step::Preview) preview_->setLyrics(draft_.toLyrics(), true);
        if (onStateChanged) onStateChanged();
        invalidate();
    }
    void textChanged() {
        if (!loaded_) return;
        dirty_ = true;
        if (onStateChanged) onStateChanged();
    }
    // The text step's lines become the draft's (stamps of the unchanged lines kept).
    void applyText() {
        const std::wstring t = text_->text();
        if (t == appliedText_) return;
        appliedText_ = t;
        draft_.setText(toUtf8(t));
        list_->rebuild();
        if (onStateChanged) onStateChanged();
    }

    // The song's lyrics: plain ones to time, synced ones to re-time (their stamps kept), nothing = an empty text.
    void load() {
        const lyrics::Query q = lyricsQueryFor(track_);
        async(Priority::High, life_.ref(), [q] { return lyrics::fetch(q); },
              [this](Result<std::optional<lyrics::Lyrics>> r) {
                  loaded_ = true;
                  if (r && *r && !(*r)->instrumental && !(*r)->lines.empty()) {
                      draft_ = Draft::fromLyrics(**r);
                      text_->setText(toWide(draft_.text()));
                      appliedText_ = text_->text();
                      list_->rebuild();
                  }
                  if (onStateChanged) onStateChanged();
                  invalidate();
              });
    }

    catalog::Track track_;
    Draft draft_;
    Step step_ = Step::Text;
    ChipTabs* steps_ = nullptr;
    TextArea* text_ = nullptr;
    StampList* list_ = nullptr;
    PreviewView* preview_ = nullptr;
    ui::Button *start_ = nullptr, *stamp_ = nullptr, *back5_ = nullptr, *restart_ = nullptr, *earlier_ = nullptr,
               *later_ = nullptr, *playSong_ = nullptr;
    ui::Button* speeds_[std::size(kSpeeds)] = {};
    ui::PlayButton* play_ = nullptr;
    Rect positionRect_{}, legendRect_{};
    gfx::Text legend_;
    std::wstring appliedText_;
    bool loaded_ = false, dirty_ = false, wasCurrent_ = true;
    float speedBefore_ = 1;
    RepeatMode repeatBefore_ = RepeatMode::Off;
    bool speedChanged_ = false, repeatChanged_ = false;
    Lifetime life_;
};

// ---- The dialog --------------------------------------------------------------------------------------------------------

class SyncDialog : public ui::Dialog {
public:
    explicit SyncDialog(const catalog::Track& t)
        : ui::Dialog(tr(L"Söz senkronlayıcı"), toWide(t.name) + (t.artists.empty() ? L"" : L" — " + toWide(t.artistLine())), 680) {
        body_ = body()->add<SyncBody>(t);
        exportBtn_ = addLeadingButton(tr(L"Dışa aktar…"), ui::ButtonKind::Ghost, [this] { exportLrc(); }, false);
        exportBtn_->setTooltip(tr(L"Sonucu bir .lrc dosyası olarak kaydet"));
        publishBtn_ = addLeadingButton(tr(L"LRCLIB'e gönder…"), ui::ButtonKind::Ghost, [this] { askPublish(); }, false);
        publishBtn_->setTooltip(tr(L"Herkesin kullanabilmesi için LRCLIB'de yayımla"));
        addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, [this] { requestClose(); }, false);
        save_ = addButton(tr(L"Kaydet"), ui::ButtonKind::Primary, [this] { save(); }, false);
        save_->setTooltip(tr(L"Bu şarkının sözleri olarak kaydet"));
        body_->onStateChanged = [this] { syncButtons(); };
        onClosed = [this] { body_->restorePlayer(); };
        syncButtons();
    }
    SyncBody* content() const { return body_; }

    bool onKeyDown(const ui::KeyEvent& e) override {
        if (body_->key(e)) return true;
        if (e.vk == VK_ESCAPE) {
            requestClose();
            return true;
        }
        return ui::Dialog::onKeyDown(e);
    }
    // A click on the scrim doesn't throw away unsaved stamps.
    bool onMouseDown(const ui::MouseEvent& e) override { return body_->dirty() ? true : ui::Dialog::onMouseDown(e); }

private:
    void syncButtons() {
        const bool ready = body_->ready();
        save_->setEnabled(ready);
        exportBtn_->setEnabled(ready);
        publishBtn_->setEnabled(ready && publishable());
    }
    lyrics::lrclib::Submission submission() const {
        const auto& t = body_->track();
        const lyrics::Lyrics l = body_->draft().toLyrics();
        lyrics::lrclib::Submission s;
        s.title = t.name;
        s.artist = t.artistLine();
        s.album = t.album.name;
        int64_t ms = t.durationMs;
        if (const auto* p = ctx().player; p && p->current() && p->current()->id == t.id && p->durationMs() > 0) ms = p->durationMs();
        s.durationSec = static_cast<int>((ms + 500) / 1000);
        s.syncedLyrics = lyrics::toLrc(l);
        s.plainLyrics = lyrics::toPlainText(l);
        return s;
    }
    bool publishable() const { return lyrics::lrclib::publishable(submission()); }

    void requestClose() {
        if (!body_->dirty()) {
            close();
            return;
        }
        ui::Dialog::confirm(window(), tr(L"Zamanlamalar kaydedilmedi"),
                            tr(L"Kapatırsan damgaladığın zamanlar kaybolur. Kaydetmek için Kaydet'i kullan."),
                            tr(L"Kaydetmeden kapat"), [ref = life_.ref(), this] {
                                if (!ref.expired()) close();
                            },
                            true);
    }

    void save() {
        if (!body_->ready()) return;
        const catalog::Track t = body_->track();
        const lyrics::Lyrics l = body_->draft().toLyrics();
        const std::string lrc = "\xEF\xBB\xBF" + lyrics::toLrc(l, t.name, t.artistLine(), t.album.name);
        const fs::path audio = lyricsQueryFor(t).audioPath;   // a download or a local file: the .lrc goes beside it
        struct Saved {
            std::string name;
            bool sidecar = false;
        };
        body_->markSaved();
        close();
        auto keep = std::make_shared<Lifetime>();
        async(Priority::High, keep->ref(),
              [t, l, lrc, audio] {
                  Saved s;
                  s.name = lyrics::Offsets::get().storeText(t.id, lrc);
                  if (!s.name.empty() && !audio.empty()) s.sidecar = writeSidecarKeepingOld(audio, l, t);
                  return s;
              },
              [keep, id = t.id](Result<Saved> r) {
                  if (!r || r->name.empty()) {
                      toast(tr(L"Sözler kaydedilemedi"), true);
                      return;
                  }
                  lyrics::Choice c;
                  c.kind = lyrics::Choice::Kind::File;
                  c.file = r->name;
                  setLyricsChoice(id, c);
                  toast(r->sidecar ? tr(L"Senkronlanmış sözler kaydedildi; .lrc dosyası şarkının yanına da yazıldı")
                                   : tr(L"Senkronlanmış sözler kaydedildi"));
                  ST_LOG_INFO("lyrics", "synchronizer: saved {}{}", r->name, r->sidecar ? " and the sidecar .lrc" : "");
              });
    }

    void exportLrc() {
        if (!body_->ready()) return;
        const catalog::Track t = body_->track();
        const std::string lrc = "\xEF\xBB\xBF" + lyrics::toLrc(body_->draft().toLyrics(), t.name, t.artistLine(), t.album.name);
        const std::wstring name = (t.artists.empty() ? L"" : toWide(t.artistLine()) + L" - ") + toWide(t.name);
        Dispatcher::post([lrc, name] {
            const fs::path file = askLrcFile(name);
            if (file.empty()) return;
            auto keep = std::make_shared<Lifetime>();
            async(Priority::High, keep->ref(), [file, lrc] { return writeText(file, lrc); },
                  [keep, file](Result<bool> r) {
                      if (r && *r) showFileToast(tr(L"Sözler .lrc olarak dışa aktarıldı"), file);
                      else toast(tr(L"Dosya yazılamadı"), true);
                  });
        });
    }

    // "LRCLIB'e gönder": what goes public, the user's explicit yes, then the challenge, the solve and the publish on a
    // worker (the dialog may close meanwhile; the result comes as a toast).
    void askPublish() {
        if (!body_->ready()) return;
        const auto s = submission();
        if (!lyrics::lrclib::publishable(s)) return;
        const std::wstring what = toWide(s.title) + L" — " + toWide(s.artist) + (s.album.empty() ? L"" : L" · " + toWide(s.album)) + L" · " +
                                  ui::formatDuration(int64_t{s.durationSec} * 1000) + L" · " +
                                  i18n::plural(L"{} satır", static_cast<long long>(body_->draft().size()));
        auto* d = ui::Dialog::open(window(), tr(L"LRCLIB'e gönderilsin mi?"),
                                   tr(L"Bu senkronlanmış sözler LRCLIB'de herkese açık olarak yayımlanır: herkes görebilir ve "
                                      L"başka uygulamalar da kullanır. Gönderdiğini buradan geri alamazsın; yalnızca doğru "
                                      L"sözleri ve zamanlamaları gönder."),
                                   520);
        if (!d) return;
        auto* line = d->body()->add<ui::Label>(what, type::secondary, ui::Tone::Primary);
        line->setWrap(true);
        d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
        d->addButton(tr(L"Herkese açık olarak gönder"), ui::ButtonKind::Primary, [s] { publishNow(s); });
    }

    static void publishNow(const lyrics::lrclib::Submission& s) {
        toast(tr(L"LRCLIB'e gönderiliyor…"));
        async(Priority::Low, publishLife().ref(), [s] { return lyrics::lrclib::publish(s); },
              [](Result<lyrics::lrclib::Outcome> r) {
                  using K = lyrics::lrclib::Outcome::Kind;
                  if (!r) {
                      toast(tr(L"LRCLIB'e ulaşılamadı. Daha sonra tekrar dene."), true);
                      return;
                  }
                  switch (r->kind) {
                  case K::Published: toast(tr(L"Sözler LRCLIB'de yayımlandı. Teşekkürler!")); break;
                  case K::BadToken: toast(tr(L"LRCLIB doğrulamayı kabul etmedi. Biraz sonra tekrar dene."), true); break;
                  case K::Rejected:
                      toast(r->message.empty() ? std::wstring(tr(L"LRCLIB gönderimi reddetti"))
                                               : i18n::format(tr(L"LRCLIB gönderimi reddetti: {}"), {toWide(r->message)}),
                            true);
                      break;
                  case K::Failed: toast(tr(L"LRCLIB'e ulaşılamadı. Daha sonra tekrar dene."), true); break;
                  case K::Cancelled: break;
                  }
              });
    }

    SyncBody* body_ = nullptr;
    ui::Button *save_ = nullptr, *exportBtn_ = nullptr, *publishBtn_ = nullptr;
    Lifetime life_;
};

} // namespace

bool lyricsSyncAvailable(const catalog::Track& t) {
    return !t.id.empty() && !radio::isStationId(t.id) && !catalog::isPodcastId(t.id);
}

void showLyricsSync(const catalog::Track& t) {
    auto* w = ctx().window;
    if (!w || !lyricsSyncAvailable(t)) return;
    // The repeat and speed it changed are the user's settings: put back before they are saved at exit, too.
    static const bool hooked = [] {
        ctx().persistHooks.push_back([] {
            if (g_open) g_open->restorePlayer();
        });
        return true;
    }();
    (void)hooked;
    auto* d = static_cast<SyncDialog*>(w->pushOverlay(std::make_unique<SyncDialog>(t), true));
    if (d) d->content()->textArea()->focus();
}

} // namespace st::app
