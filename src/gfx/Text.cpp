#include "gfx/Text.h"

#include "core/Log.h"
#include "core/Resources.h"
#include "core/Utf.h"

#include <unordered_map>

namespace st::gfx {

namespace {

ComPtr<IDWriteFontCollection2> g_collection;
ComPtr<IDWriteFontCollection2> g_system;   // fallback families (Segoe UI Variable, Cascadia Mono)
std::wstring g_display = L"Bricolage Grotesque";
std::wstring g_mono = L"JetBrains Mono";

struct FormatKey {
    TextStyle style;
    TextOptions opts;
    bool operator==(const FormatKey&) const = default;
};
struct FormatKeyHash {
    size_t operator()(const FormatKey& k) const noexcept {
        size_t h = std::hash<float>()(k.style.size);
        auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix(std::hash<float>()(k.style.weight));
        mix(std::hash<float>()(k.style.lineHeight));
        mix(std::hash<float>()(k.style.opsz));
        mix(static_cast<size_t>(k.style.mono) | (static_cast<size_t>(k.opts.align) << 1) |
            (static_cast<size_t>(k.opts.wrap) << 3) | (static_cast<size_t>(k.opts.ellipsis) << 4));
        return h;
    }
};
std::unordered_map<FormatKey, ComPtr<IDWriteTextFormat3>, FormatKeyHash> g_formats;

bool familyExists(IDWriteFontCollection2* c, const wchar_t* name) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    return c && SUCCEEDED(c->FindFamilyName(name, &index, &exists)) && exists;
}

} // namespace

void Fonts::init() {
    auto* factory = Device::get().dwrite();
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(factory->CreateInMemoryFontFileLoader(&loader))) return;
    factory->RegisterFontFileLoader(loader.Get());

    ComPtr<IDWriteFontSetBuilder2> builder;
    factory->CreateFontSetBuilder(&builder);
    for (const wchar_t* name : {L"fonts/BricolageGrotesque.ttf", L"fonts/JetBrainsMono.ttf"}) {
        const auto data = res::load(name);
        if (data.empty()) {
            ST_LOG_WARN("fonts", "embedded font missing: {}", toUtf8(name));
            continue;
        }
        ComPtr<IDWriteFontFile> file;
        // No owner object: DirectWrite copies the (resource) bytes. Resources outlive the process anyway.
        if (SUCCEEDED(loader->CreateInMemoryFontFileReference(factory, data.data(), static_cast<UINT32>(data.size()),
                                                              nullptr, &file)))
            builder->AddFontFile(file.Get());
    }
    ComPtr<IDWriteFontSet> set;
    if (SUCCEEDED(builder->CreateFontSet(&set)))
        factory->CreateFontCollectionFromFontSet(set.Get(), DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &g_collection);

    if (!familyExists(g_collection.Get(), g_display.c_str())) {
        ST_LOG_WARN("fonts", "Bricolage Grotesque unavailable, falling back to Segoe UI Variable");
        g_display = L"Segoe UI Variable Display";
    }
    if (!familyExists(g_collection.Get(), g_mono.c_str())) {
        ST_LOG_WARN("fonts", "JetBrains Mono unavailable, falling back to Cascadia Mono");
        g_mono = L"Cascadia Mono";
    }
    factory->GetSystemFontCollection(FALSE, DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &g_system);
}

IDWriteFontCollection2* Fonts::collection() { return g_collection.Get(); }
const wchar_t* Fonts::displayFamily() { return g_display.c_str(); }
const wchar_t* Fonts::monoFamily() { return g_mono.c_str(); }

IDWriteTextFormat3* Fonts::format(const TextStyle& style, const TextOptions& opts) {
    FormatKey key{style, opts};
    key.style.tracking = 0;     // tracking/uppercase are layout-level; share formats
    key.style.uppercase = false;
    if (auto it = g_formats.find(key); it != g_formats.end()) return it->second.Get();

    auto* factory = Device::get().dwrite();
    const float opsz = style.opsz > 0 ? style.opsz : std::clamp(style.size, 12.f, 96.f);
    DWRITE_FONT_AXIS_VALUE axes[] = {
        {DWRITE_FONT_AXIS_TAG_WEIGHT, style.weight},
        {DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE, opsz},
    };
    const UINT32 axisCount = style.mono ? 1 : 2;   // JetBrains Mono has no opsz axis
    ComPtr<IDWriteTextFormat3> fmt;
    const std::wstring& family = style.mono ? g_mono : g_display;
    const bool ours = familyExists(g_collection.Get(), family.c_str());
    HRESULT hr = factory->CreateTextFormat(family.c_str(), ours ? g_collection.Get() : g_system.Get(), axes,
                                           axisCount, style.size, L"tr-TR", &fmt);
    if (FAILED(hr)) {
        ComPtr<IDWriteTextFormat> legacy;
        factory->CreateTextFormat(L"Segoe UI", nullptr, static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(style.weight)),
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, style.size, L"tr-TR", &legacy);
        legacy.As(&fmt);
    }
    fmt->SetWordWrapping(opts.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(opts.align == TextAlign::Center     ? DWRITE_TEXT_ALIGNMENT_CENTER
                          : opts.align == TextAlign::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                              : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const float lineH = style.size * style.lineHeight;
    fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, lineH * 0.8f);
    if (opts.ellipsis) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        factory->CreateEllipsisTrimmingSign(fmt.Get(), &sign);
        fmt->SetTrimming(&trimming, sign.Get());
    }
    auto* raw = fmt.Get();
    g_formats.emplace(key, std::move(fmt));
    return raw;
}

ComPtr<IDWriteTextLayout4> makeLayout(std::wstring_view text, const TextStyle& style, float maxWidth,
                                      const TextOptions& opts, float maxHeight) {
    auto* fmt = Fonts::format(style, opts);
    const float lineH = style.size * style.lineHeight;
    float h = maxHeight;
    if (h <= 0) h = opts.wrap ? 100000.f : lineH;
    if (opts.wrap && opts.maxLines > 0) h = std::min(h, lineH * static_cast<float>(opts.maxLines) + 0.5f);

    std::wstring upper;
    if (style.uppercase) {
        upper = toUpperTr(text);
        text = upper;
    }
    ComPtr<IDWriteTextLayout> base;
    Device::get().dwrite()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt,
                                             std::max(1.f, maxWidth), h, &base);
    ComPtr<IDWriteTextLayout4> layout;
    if (!base || FAILED(base.As(&layout))) return nullptr;
    if (style.tracking != 0 && !text.empty()) {
        const float half = style.tracking * style.size * 0.5f;
        layout->SetCharacterSpacing(half, half, 0, {0, static_cast<UINT32>(text.size())});
    }
    return layout;
}

// ---------------------------------------------------------------------------------------------------

void Text::setText(std::wstring text) {
    if (text == text_) return;
    text_ = std::move(text);
    layout_.Reset();
}
void Text::setText(std::string_view utf8) { setText(toWide(utf8)); }
void Text::setStyle(const TextStyle& style) {
    if (style == style_) return;
    style_ = style;
    layout_.Reset();
}
void Text::setOptions(const TextOptions& opts) {
    if (opts == opts_) return;
    opts_ = opts;
    layout_.Reset();
}

IDWriteTextLayout4* Text::layout(float maxWidth, float maxHeight) {
    if (!layout_ || layoutW_ != maxWidth || layoutH_ != maxHeight) {
        if (layout_ && layoutH_ == maxHeight && layoutW_ != maxWidth) {
            // Width-only change: reuse the layout (much cheaper than re-shaping).
            layout_->SetMaxWidth(std::max(1.f, maxWidth));
        } else {
            layout_ = makeLayout(text_, style_, maxWidth, opts_, maxHeight);
        }
        layoutW_ = maxWidth;
        layoutH_ = maxHeight;
    }
    return layout_.Get();
}

Size Text::measure(float maxWidth, float maxHeight) {
    auto* l = layout(maxWidth, maxHeight);
    if (!l) return {};
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

bool Text::isTrimmed(float maxWidth) {
    auto* l = layout(maxWidth);
    if (!l) return false;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace > maxWidth + 0.5f;
}

} // namespace st::gfx
