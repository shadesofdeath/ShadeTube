#include "ui/TextBox.h"

#include "gfx/Theme.h"
#include "ui/Window.h"

#include <cwctype>

namespace st::ui {

using gfx::colors;
using gfx::accent;

TextBox::TextBox(Look look, std::wstring placeholder) : look_(look) {
    focusable = true;
    const auto style = look == Look::Search ? gfx::type::bodyL : gfx::type::bodyRegular;
    placeholder_ = gfx::Text(std::move(placeholder), style);
    shown_ = gfx::Text({}, style);
}

void TextBox::setText(std::wstring text, bool notify) {
    text_ = std::move(text);
    caret_ = anchor_ = text_.size();
    shown_.setText(text_);
    if (notify && onChange) onChange(text_);
    invalidate();
}

void TextBox::selectAll() {
    anchor_ = 0;
    caret_ = text_.size();
    invalidate();
}

Rect TextBox::textRect() const {
    const Rect r = rect();
    if (look_ == Look::Search) return {r.x + 20 + 18 + 12, r.y, r.w - 20 - 18 - 12 - 44, r.h};
    return {r.x + 14, r.y, r.w - 28, r.h};
}

Rect TextBox::clearRect() const {
    const Rect r = rect();
    return {r.right() - 12 - 24, r.cy() - 12, 24, 24};
}

size_t TextBox::indexAt(float x) {
    auto* l = shown_.layout(100000.f);
    if (!l) return 0;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    l->HitTestPoint(x - textRect().x + scrollX_, 1, &trailing, &inside, &m);
    return std::min(text_.size(), static_cast<size_t>(m.textPosition + (trailing ? m.length : 0)));
}

bool TextBox::onMouseDown(const MouseEvent& e) {
    if (e.button != MouseButton::Left) return false;
    if (look_ == Look::Search && !text_.empty() && clearRect().contains(e.pos)) {
        setText({}, true);
        return true;
    }
    caret_ = indexAt(e.pos.x);
    if (!e.shift) anchor_ = caret_;
    if (e.clicks == 2) {
        anchor_ = wordLeft(caret_);
        caret_ = wordRight(caret_);
    }
    blinkStart_ = frame::realNow();
    invalidate();
    return true;
}

void TextBox::onMouseMove(const MouseEvent& e) {
    if (!pressed()) return;
    caret_ = indexAt(e.pos.x);
    invalidate();
}

void TextBox::onFocusChanged(bool f) {
    focusA_.to(f ? 1.f : 0.f, motion::fast);
    blinkStart_ = frame::realNow();
    if (!f) anchor_ = caret_;
    invalidate();
}

void TextBox::changed() {
    shown_.setText(text_);
    blinkStart_ = frame::realNow();
    if (onChange) onChange(text_);
    invalidate();
}

void TextBox::eraseSelection() {
    if (!hasSelection()) return;
    const size_t a = std::min(anchor_, caret_), b = std::max(anchor_, caret_);
    text_.erase(a, b - a);
    caret_ = anchor_ = a;
}

void TextBox::insert(const std::wstring& s) {
    eraseSelection();
    std::wstring clean;
    for (wchar_t ch : s)
        if (ch >= 32 && ch != 127) clean.push_back(ch);
    text_.insert(caret_, clean);
    caret_ += clean.size();
    anchor_ = caret_;
    changed();
}

size_t TextBox::wordLeft(size_t i) const {
    while (i > 0 && std::iswspace(text_[i - 1])) --i;
    while (i > 0 && !std::iswspace(text_[i - 1])) --i;
    return i;
}

size_t TextBox::wordRight(size_t i) const {
    while (i < text_.size() && !std::iswspace(text_[i])) ++i;
    while (i < text_.size() && std::iswspace(text_[i])) ++i;
    return i;
}

static std::wstring clipboardText(HWND hwnd) {
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

static void setClipboardText(HWND hwnd, const std::wstring& s) {
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

bool TextBox::onKeyDown(const KeyEvent& e) {
    if (onKey && onKey(e)) return true;
    const size_t a = std::min(anchor_, caret_), b = std::max(anchor_, caret_);
    auto move = [&](size_t to) {
        caret_ = to;
        if (!e.shift) anchor_ = caret_;
        blinkStart_ = frame::realNow();
        invalidate();
    };
    switch (e.vk) {
    case VK_LEFT: move(e.ctrl ? wordLeft(caret_) : (hasSelection() && !e.shift ? a : (caret_ ? caret_ - 1 : 0))); return true;
    case VK_RIGHT:
        move(e.ctrl ? wordRight(caret_) : (hasSelection() && !e.shift ? b : std::min(text_.size(), caret_ + 1)));
        return true;
    case VK_HOME: move(0); return true;
    case VK_END: move(text_.size()); return true;
    case VK_DELETE:
        if (hasSelection()) eraseSelection();
        else if (caret_ < text_.size()) text_.erase(caret_, e.ctrl ? wordRight(caret_) - caret_ : 1);
        changed();
        return true;
    case VK_RETURN:
        // Without a submit handler Enter goes on to the ancestors (a Dialog presses its default button).
        if (!onSubmit) return false;
        onSubmit(text_);
        return true;
    case VK_ESCAPE: {
        if (onEscape) {
            onEscape();
            return true;
        }
        // Inside an overlay (Dialog) Escape cancels the overlay rather than just leaving the field.
        const Widget* top = this;
        while (top->parent()) top = top->parent();
        auto* w = window();
        if (!w || top != w->root()) return false;
        w->setFocus(nullptr);
        return true;
    }
    default: break;
    }
    if (e.ctrl) {
        auto* w = window();
        HWND hwnd = w ? w->hwnd() : nullptr;
        switch (e.vk) {
        case 'A': selectAll(); return true;
        case 'C': if (hasSelection()) setClipboardText(hwnd, text_.substr(a, b - a)); return true;
        case 'X':
            if (hasSelection()) {
                setClipboardText(hwnd, text_.substr(a, b - a));
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

bool TextBox::onChar(wchar_t ch) {
    if (ch == 8) {   // backspace
        if (hasSelection()) eraseSelection();
        else if (caret_ > 0) {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            const size_t from = ctrl ? wordLeft(caret_) : caret_ - 1;
            text_.erase(from, caret_ - from);
            caret_ = anchor_ = from;
        }
        changed();
        return true;
    }
    if (ch == 127) {   // ctrl+backspace generates DEL on some layouts
        const size_t from = wordLeft(caret_);
        text_.erase(from, caret_ - from);
        caret_ = anchor_ = from;
        changed();
        return true;
    }
    if (ch < 32) return false;
    insert(std::wstring(1, ch));
    return true;
}

void TextBox::paint(Canvas& c) {
    const auto& col = colors();
    const auto& acc = accent();
    const Rect r = rect();
    const float f = focusA_, h = hover_;

    if (look_ == Look::Search) {
        c.fillPill(r, col.bgRaised);
        c.fillPill(r, acc.tint06.mulAlpha(f));
        c.strokePill(r, Color::lerp(Color::lerp(col.hairSubtle, col.hairStrong, h), acc.base, f));
        c.icon("search", {r.x + 20, r.cy() - 9, 18, 18}, Color::lerp(col.fgTertiary, col.fgSecondary, f));
        if (!text_.empty()) {
            const Rect cr = clearRect();
            c.icon("close", cr.center(14, 14), col.fgSecondary);
        }
    } else {
        c.fillRounded(r, 2, col.bgRaised.mulAlpha(0.6f));
        const Color border = error_ ? col.error : Color::lerp(Color::lerp(col.hairDefault, col.hairStrong, h), acc.base, f);
        c.strokeRounded(r, 2, border);
    }

    const Rect tr = textRect();
    c.pushClip(tr);
    auto* l = shown_.layout(100000.f);
    float caretX = 0;
    if (l && !text_.empty()) {
        DWRITE_HIT_TEST_METRICS m{};
        float px, py;
        l->HitTestTextPosition(static_cast<UINT32>(caret_), FALSE, &px, &py, &m);
        caretX = px;
        // Keep the caret visible.
        if (caretX - scrollX_ > tr.w - 2) scrollX_ = caretX - tr.w + 2;
        if (caretX - scrollX_ < 0) scrollX_ = caretX;
    } else {
        scrollX_ = 0;
    }
    DWRITE_TEXT_METRICS tm{};
    if (l) l->GetMetrics(&tm);
    const float lineH = shown_.style().size * shown_.style().lineHeight;
    const float ty = r.cy() - lineH * 0.5f;
    if (hasSelection() && l) {
        const UINT32 a = static_cast<UINT32>(std::min(anchor_, caret_)), b = static_cast<UINT32>(std::max(anchor_, caret_));
        UINT32 count = 0;
        l->HitTestTextRange(a, b - a, 0, 0, nullptr, 0, &count);
        std::vector<DWRITE_HIT_TEST_METRICS> ms(count);
        l->HitTestTextRange(a, b - a, 0, 0, ms.data(), count, &count);
        for (auto& m : ms) c.fillRect({tr.x + m.left - scrollX_, ty, m.width, lineH}, acc.tint25);
    }
    if (text_.empty()) {
        c.text(placeholder_, {tr.x, r.y, tr.w, r.h}, col.fgTertiary, gfx::VAlign::Center);
    } else if (l) {
        c.text(l, tr.x - scrollX_, ty, col.fgPrimary);
    }
    if (focused()) {
        const double t = std::fmod(frame::realNow() - blinkStart_, 1060.0);
        if (t < 530) c.fillRect({tr.x + caretX - scrollX_, ty + 1, 1.5f, lineH - 2}, acc.base);
        if (auto* w = window()) w->invalidateAfter(t < 530 ? 530 - t : 1060 - t);
    }
    c.popClip();
}

} // namespace st::ui
