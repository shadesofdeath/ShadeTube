#pragma once
// DirectWrite text: private variable-font collection (Bricolage Grotesque + JetBrains Mono embedded in
// the exe), typography tokens, and a cached text-layout holder.
//
// Typography tokens map 1:1 to tokens.dark.json -> typography.scale. Weight and optical size are set
// through variable-font axes (IDWriteFactory6::CreateTextFormat with DWRITE_FONT_AXIS_VALUEs).
#include "gfx/Device.h"
#include "gfx/Types.h"

#include <string>
#include <string_view>

namespace st::gfx {

struct TextStyle {
    float size = 14;
    float weight = 400;
    float lineHeight = 1.3f;   // multiplier of size
    float tracking = 0;        // em
    bool mono = false;
    bool uppercase = false;
    float opsz = 0;            // 0 = auto (clamp(size, 12, 96))

    TextStyle withWeight(float w) const { TextStyle s = *this; s.weight = w; return s; }
    TextStyle withSize(float px) const { TextStyle s = *this; s.size = px; return s; }
    bool operator==(const TextStyle&) const = default;
};

namespace type {
// Bricolage Grotesque
inline constexpr TextStyle hero{96, 800, 0.92f, -0.045f, false, false, 96};
inline constexpr TextStyle displayXL{88, 800, 0.90f, -0.05f, false, false, 96};
inline constexpr TextStyle displayL{64, 800, 1.0f, -0.04f, false, false, 96};
inline constexpr TextStyle displayM{56, 800, 1.0f, -0.04f, false, false, 96};
inline constexpr TextStyle displayS{36, 800, 1.0f, -0.04f};
inline constexpr TextStyle headline{28, 300, 1.1f, -0.02f};
inline constexpr TextStyle title{22, 700, 1.2f, -0.02f};
inline constexpr TextStyle sectionTitle{18, 700, 1.2f, -0.02f};
inline constexpr TextStyle bodyL{15, 400, 1.5f, 0};
inline constexpr TextStyle body{14, 600, 1.3f, 0};
inline constexpr TextStyle bodyRegular{14, 400, 1.3f, 0};
inline constexpr TextStyle secondary{13, 400, 1.3f, 0};
inline constexpr TextStyle caption{12, 400, 1.3f, 0};
inline constexpr TextStyle tiny{11, 400, 1.3f, 0};
inline constexpr TextStyle lyricActive{44, 800, 1.15f, -0.03f};
inline constexpr TextStyle lyricInactive{28, 500, 1.15f, -0.03f};
// JetBrains Mono
inline constexpr TextStyle monoLabel{10, 500, 1.3f, 0.14f, true, true};
inline constexpr TextStyle monoMeta{11, 400, 1.3f, 0.06f, true};
inline constexpr TextStyle monoDuration{12, 400, 1.0f, 0.04f, true};
inline constexpr TextStyle monoBadge{9, 700, 1.0f, 0.10f, true, true};
} // namespace type

enum class TextAlign { Leading, Center, Trailing };
enum class VAlign { Top, Center, Bottom };

struct TextOptions {
    TextAlign align = TextAlign::Leading;
    bool wrap = false;         // false = single line
    bool ellipsis = true;      // trim with "…" when it doesn't fit
    int maxLines = 0;          // with wrap: 0 = unlimited (clipped by maxHeight)
    bool operator==(const TextOptions&) const = default;
};

class Fonts {
public:
    static void init();       // builds the private collection from embedded fonts
    static IDWriteFontCollection2* collection();
    static const wchar_t* displayFamily();
    static const wchar_t* monoFamily();
    static IDWriteTextFormat3* format(const TextStyle& style, const TextOptions& opts);   // cached
};

// Creates a layout. maxHeight <= 0 means "one line tall" for single-line text or unbounded for wrap.
ComPtr<IDWriteTextLayout4> makeLayout(std::wstring_view text, const TextStyle& style, float maxWidth,
                                      const TextOptions& opts = {}, float maxHeight = 0);

// Cached layout: rebuilds only when text/style/width/options change. Cheap to keep per widget.
class Text {
public:
    Text() = default;
    Text(std::wstring text, TextStyle style, TextOptions opts = {})
        : text_(std::move(text)), style_(style), opts_(opts) {}

    void setText(std::wstring text);
    void setText(std::string_view utf8);
    void setStyle(const TextStyle& style);
    void setOptions(const TextOptions& opts);
    const std::wstring& text() const { return text_; }
    const TextStyle& style() const { return style_; }
    bool empty() const { return text_.empty(); }

    // Layout for the given width (cached).
    IDWriteTextLayout4* layout(float maxWidth, float maxHeight = 0);
    // Measured size of the text (width = actual ink advance up to maxWidth).
    Size measure(float maxWidth = 100000.f, float maxHeight = 0);
    bool isTrimmed(float maxWidth);
    void reset() { layout_.Reset(); }

private:
    std::wstring text_;
    TextStyle style_{};
    TextOptions opts_{};
    ComPtr<IDWriteTextLayout4> layout_;
    float layoutW_ = -1, layoutH_ = -1;
};

} // namespace st::gfx
