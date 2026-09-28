#pragma once
// Single-line text input (search pill / text field): caret, selection, clipboard, word navigation.
#include "gfx/Text.h"
#include "ui/Anim.h"
#include "ui/Widget.h"

#include <functional>
#include <string>

namespace st::ui {

class TextBox : public Widget {
public:
    enum class Look { Search, Field };
    explicit TextBox(Look look = Look::Search, std::wstring placeholder = {});

    std::function<void(const std::wstring&)> onChange;
    std::function<void(const std::wstring&)> onSubmit;
    std::function<void()> onEscape;
    std::function<bool(const KeyEvent&)> onKey;   // e.g. arrow keys for suggestion lists

    void setText(std::wstring text, bool notify = false);
    const std::wstring& text() const { return text_; }
    void setPlaceholder(std::wstring p) { placeholder_.setText(std::move(p)); }
    void selectAll();
    void setError(bool e) { error_ = e; invalidate(); }

    float preferredHeight(float) override { return look_ == Look::Search ? 48.f : 44.f; }
    void paint(Canvas& c) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseMove(const MouseEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onChar(wchar_t ch) override;
    void onFocusChanged(bool focused) override;
    void onMouseEnter() override { hover_.to(1, motion::fast); }
    void onMouseLeave() override { hover_.to(0, motion::fast); }
    LPCWSTR cursor() const override { return IDC_IBEAM; }
    // Owns its keys whenever focused (not activatable); its accent border is the focus indicator, so the Window
    // draws no ring. Tab / Shift+Tab always leave it (handled by the Window).
    FocusShape focusShape() const override { return FocusShape::None; }

private:
    Rect textRect() const;
    Rect clearRect() const;
    size_t indexAt(float x);
    void insert(const std::wstring& s);
    void eraseSelection();
    void changed();
    size_t wordLeft(size_t i) const;
    size_t wordRight(size_t i) const;
    bool hasSelection() const { return anchor_ != caret_; }

    Look look_;
    std::wstring text_;
    gfx::Text placeholder_;
    gfx::Text shown_;
    size_t caret_ = 0, anchor_ = 0;
    float scrollX_ = 0;
    double blinkStart_ = 0;
    bool error_ = false;
    Anim hover_, focusA_;
};

} // namespace st::ui
