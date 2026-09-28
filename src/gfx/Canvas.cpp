#include "gfx/Canvas.h"

#include "gfx/Icons.h"
#include "gfx/Theme.h"

#include <cmath>
#include <numbers>

namespace st::gfx {

namespace {

// Device-dependent objects shared by all canvases; rebuilt after device loss.
struct Shared {
    uint64_t generation = 0;
    ComPtr<ID2D1BitmapBrush1> bitmapBrush;
    ComPtr<ID2D1Effect> shadow, blur, saturation, scale;
    ComPtr<ID2D1StrokeStyle1> roundCaps;

    void ensure(ID2D1DeviceContext5* dc) {
        const uint64_t gen = Device::get().generation();
        if (gen == generation) return;
        generation = gen;
        bitmapBrush.Reset();
        dc->CreateBitmapBrush(nullptr, D2D1::BitmapBrushProperties1(), D2D1::BrushProperties(), &bitmapBrush);
        dc->CreateEffect(CLSID_D2D1Shadow, shadow.ReleaseAndGetAddressOf());
        dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.ReleaseAndGetAddressOf());
        dc->CreateEffect(CLSID_D2D1Saturation, saturation.ReleaseAndGetAddressOf());
        dc->CreateEffect(CLSID_D2D1Scale, scale.ReleaseAndGetAddressOf());
        const D2D1_STROKE_STYLE_PROPERTIES1 props{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                  D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_SOLID, 0.f,
                                                  D2D1_STROKE_TRANSFORM_TYPE_NORMAL};
        Device::get().d2dFactory()->CreateStrokeStyle(props, nullptr, 0, roundCaps.ReleaseAndGetAddressOf());
    }
};
Shared g_shared;

} // namespace

Canvas::Canvas(ID2D1DeviceContext5* dc, float scale) : dc_(dc), scale_(scale) {
    g_shared.ensure(dc);
    dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
    D2D1_SIZE_F size = dc_->GetSize();
    clips_.push_back({{0, 0, size.width, size.height}, false});
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

Canvas::~Canvas() {
    while (clips_.size() > 1) {
        if (clips_.back().axisClip) popClip();
        else popTransform();
    }
}

ID2D1SolidColorBrush* Canvas::brush(const Color& c) {
    brush_->SetColor(c.d2d());
    return brush_.Get();
}

void Canvas::clear(const Color& c) { dc_->Clear(c.d2d()); }

void Canvas::fillRect(const Rect& r, const Color& c) {
    if (c.a <= 0.001f || r.empty()) return;
    dc_->FillRectangle(r.d2d(), brush(c));
}

void Canvas::fillRounded(const Rect& r, float radius, const Color& c) {
    if (c.a <= 0.001f || r.empty()) return;
    if (radius <= 0.01f) return fillRect(r, c);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(c));
}

void Canvas::fillCircle(Point p, float radius, const Color& c) {
    if (c.a <= 0.001f || radius <= 0) return;
    dc_->FillEllipse(D2D1::Ellipse({p.x, p.y}, radius, radius), brush(c));
}

void Canvas::strokeRounded(const Rect& r, float radius, const Color& c, float width) {
    if (c.a <= 0.001f || r.empty()) return;
    // Stroke centered on a half-pixel inset so 1px borders stay crisp.
    const float inset = width * 0.5f;
    const Rect s = r.inset(inset);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(s.d2d(), std::max(0.f, radius - inset), std::max(0.f, radius - inset)),
                              brush(c), width);
}

void Canvas::strokeCircle(Point p, float radius, const Color& c, float width) {
    if (c.a <= 0.001f) return;
    dc_->DrawEllipse(D2D1::Ellipse({p.x, p.y}, radius - width * 0.5f, radius - width * 0.5f), brush(c), width);
}

void Canvas::hline(float x0, float x1, float y, const Color& c, float width) {
    const float px = 1.f / scale_;
    const float h = std::max(px, std::round(width * scale_) / scale_);
    const float yy = std::floor(y * scale_) / scale_;
    fillRect({x0, yy, x1 - x0, h}, c);
}

void Canvas::vline(float x, float y0, float y1, const Color& c, float width) {
    const float px = 1.f / scale_;
    const float w = std::max(px, std::round(width * scale_) / scale_);
    const float xx = std::floor(x * scale_) / scale_;
    fillRect({xx, y0, w, y1 - y0}, c);
}

void Canvas::line(Point a, Point b, const Color& c, float width, bool roundCaps) {
    dc_->DrawLine({a.x, a.y}, {b.x, b.y}, brush(c), width, roundCaps ? g_shared.roundCaps.Get() : nullptr);
}

void Canvas::arc(Point center, float radius, float startDeg, float sweepDeg, const Color& c, float width) {
    if (std::abs(sweepDeg) < 0.1f) return;
    ComPtr<ID2D1PathGeometry> geo;
    Device::get().d2dFactory()->CreatePathGeometry(&geo);
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    auto pt = [&](float deg) {
        const float rad = (deg - 90.f) * std::numbers::pi_v<float> / 180.f;
        return D2D1::Point2F(center.x + radius * std::cos(rad), center.y + radius * std::sin(rad));
    };
    sink->BeginFigure(pt(startDeg), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(pt(startDeg + sweepDeg), D2D1::SizeF(radius, radius), 0,
                                  sweepDeg > 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                  std::abs(sweepDeg) > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    dc_->DrawGeometry(geo.Get(), brush(c), width, g_shared.roundCaps.Get());
}

void Canvas::fillLinearGradient(const Rect& r, Point from, Point to, const Color& a, const Color& b) {
    if (r.empty()) return;
    const D2D1_GRADIENT_STOP stops[] = {{0.f, a.d2d()}, {1.f, b.d2d()}};
    ComPtr<ID2D1GradientStopCollection> coll;
    dc_->CreateGradientStopCollection(stops, 2, &coll);
    ComPtr<ID2D1LinearGradientBrush> br;
    dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({from.x, from.y}, {to.x, to.y}), coll.Get(), &br);
    dc_->FillRectangle(r.d2d(), br.Get());
}

void Canvas::fillRadialGradient(const Rect& r, Point c, float radius, const Color& inner, const Color& outer) {
    if (r.empty()) return;
    const D2D1_GRADIENT_STOP stops[] = {{0.f, inner.d2d()}, {1.f, outer.d2d()}};
    ComPtr<ID2D1GradientStopCollection> coll;
    dc_->CreateGradientStopCollection(stops, 2, &coll);
    ComPtr<ID2D1RadialGradientBrush> br;
    dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({c.x, c.y}, {0, 0}, radius, radius), coll.Get(),
                                   &br);
    dc_->FillRectangle(r.d2d(), br.Get());
}

// --- text ----------------------------------------------------------------------------------------

void Canvas::text(IDWriteTextLayout* layout, float x, float y, const Color& c) {
    if (!layout || c.a <= 0.001f) return;
    // Snap the origin to whole pixels: sharper glyphs and no shimmer while scrolling.
    x = std::round(x * scale_) / scale_;
    y = std::round(y * scale_) / scale_;
    dc_->DrawTextLayout({x, y}, layout, brush(c), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void Canvas::text(Text& t, const Rect& r, const Color& c, VAlign valign) {
    if (t.empty() || r.w <= 0) return;
    auto* l = t.layout(r.w);
    if (!l) return;
    float y = r.y;
    if (valign != VAlign::Top) {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        y = valign == VAlign::Center ? r.y + (r.h - m.height) * 0.5f : r.bottom() - m.height;
    }
    text(l, r.x, y, c);
}

void Canvas::text(std::wstring_view s, const TextStyle& style, const Rect& r, const Color& c, TextAlign align,
                  VAlign valign) {
    TextOptions o;
    o.align = align;
    auto l = makeLayout(s, style, r.w, o);
    if (!l) return;
    float y = r.y;
    if (valign != VAlign::Top) {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        y = valign == VAlign::Center ? r.y + (r.h - m.height) * 0.5f : r.bottom() - m.height;
    }
    text(l.Get(), r.x, y, c);
}

// --- icons & images -------------------------------------------------------------------------------

void Canvas::icon(std::string_view name, const Rect& r, const Color& c) { Icons::draw(dc_, name, r, c, scale_); }

void Canvas::iconColored(std::string_view name, const Rect& r, float opacity) {
    Icons::drawColored(dc_, name, r, scale_, opacity);
}

void Canvas::image(ID2D1Bitmap1* bmp, const Rect& r, float radius, float opacity) {
    if (!bmp || r.empty()) return;
    const auto sz = bmp->GetSize();   // DIPs at 96 dpi == pixels
    // Center-crop source to the destination aspect.
    const float dstAspect = r.w / r.h, srcAspect = sz.width / sz.height;
    D2D1_RECT_F src{0, 0, sz.width, sz.height};
    if (srcAspect > dstAspect) {
        const float w = sz.height * dstAspect;
        src.left = (sz.width - w) * 0.5f;
        src.right = src.left + w;
    } else if (srcAspect < dstAspect) {
        const float h = sz.width / dstAspect;
        src.top = (sz.height - h) * 0.5f;
        src.bottom = src.top + h;
    }
    if (radius <= 0.5f) {
        dc_->DrawBitmap(bmp, r.d2d(), opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src);
        return;
    }
    // Rounded: bitmap brush mapped onto the rect (antialiased edges, no layer).
    auto* br = g_shared.bitmapBrush.Get();
    br->SetBitmap(bmp);
    br->SetInterpolationMode1(D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    br->SetOpacity(opacity);
    const float sx = r.w / (src.right - src.left), sy = r.h / (src.bottom - src.top);
    br->SetTransform(D2D1::Matrix3x2F::Translation(-src.left, -src.top) * D2D1::Matrix3x2F::Scale(sx, sy) *
                     D2D1::Matrix3x2F::Translation(r.x, r.y));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), br);
    br->SetBitmap(nullptr);
}

void Canvas::imageCircle(ID2D1Bitmap1* bmp, const Rect& r, float opacity) {
    if (!bmp) return;
    image(bmp, r, std::min(r.w, r.h) * 0.5f, opacity);
}

void Canvas::skeleton(const Rect& r, float radius, double nowMs) {
    const auto& c = colors();
    fillRounded(r, radius, c.skeletonBase);
    // 30%-wide shine sweeping left -> right over 1400 ms.
    const float t = static_cast<float>(std::fmod(nowMs, 1400.0) / 1400.0);
    const float span = std::max(r.w, 120.f);
    const float cx = r.x - span * 0.3f + (span * 1.6f) * t;
    const D2D1_GRADIENT_STOP stops[] = {{0.f, c.skeletonBase.withAlpha(0).d2d()},
                                        {0.5f, (c.skeletonShine.mulAlpha(0.6f)).d2d()},
                                        {1.f, c.skeletonBase.withAlpha(0).d2d()}};
    ComPtr<ID2D1GradientStopCollection> coll;
    dc_->CreateGradientStopCollection(stops, 3, &coll);
    ComPtr<ID2D1LinearGradientBrush> br;
    dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({cx - span * 0.15f, 0}, {cx + span * 0.15f, 0}),
                                   coll.Get(), &br);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), br.Get());
}

// --- effects ---------------------------------------------------------------------------------------

void Canvas::shadow(const Rect& r, float radius, float blur, float offsetY, const Color& c) {
    if (c.a <= 0.001f) return;
    ComPtr<ID2D1CommandList> list;
    if (FAILED(dc_->CreateCommandList(&list))) return;
    ComPtr<ID2D1Image> old;
    dc_->GetTarget(&old);
    D2D1_MATRIX_3X2_F oldT;
    dc_->GetTransform(&oldT);
    dc_->SetTarget(list.Get());
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    ComPtr<ID2D1SolidColorBrush> black;
    dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &black);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), black.Get());
    list->Close();
    dc_->SetTarget(old.Get());
    dc_->SetTransform(oldT);

    auto* fx = g_shared.shadow.Get();
    fx->SetInput(0, list.Get());
    fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur / 3.f);
    fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(c.r, c.g, c.b, c.a));
    dc_->DrawImage(fx, D2D1::Point2F(0, offsetY));
    fx->SetInput(0, nullptr);
}

void Canvas::blurredImage(ID2D1Bitmap1* bmp, const Rect& r, float blurDip, float scale, float saturation) {
    if (!bmp || r.empty()) return;
    const auto sz = bmp->GetSize();
    // Scale the (square) art to cover r * scale, then blur + saturate.
    const float cover = std::max(r.w / sz.width, r.h / sz.height) * scale;
    auto* sc = g_shared.scale.Get();
    sc->SetInput(0, bmp);
    sc->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(cover, cover));
    sc->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_LINEAR);
    auto* sat = g_shared.saturation.Get();
    sat->SetInputEffect(0, sc);
    sat->SetValue(D2D1_SATURATION_PROP_SATURATION, saturation);
    auto* bl = g_shared.blur.Get();
    bl->SetInputEffect(0, sat);
    bl->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blurDip / 3.f);
    bl->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    bl->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
    const float w = sz.width * cover, h = sz.height * cover;
    pushClip(r);
    dc_->DrawImage(bl, D2D1::Point2F(r.cx() - w * 0.5f, r.cy() - h * 0.5f));
    popClip();
    sc->SetInput(0, nullptr);
}

// --- state -------------------------------------------------------------------------------------------

void Canvas::pushClip(const Rect& r) {
    dc_->PushAxisAlignedClip(r.d2d(), D2D1_ANTIALIAS_MODE_ALIASED);
    clips_.push_back({clips_.back().r.intersect(r), true});
}

void Canvas::popClip() {
    if (clips_.size() <= 1 || !clips_.back().axisClip) return;
    dc_->PopAxisAlignedClip();
    clips_.pop_back();
}

void Canvas::pushRoundedClip(const Rect& r, float radius) {
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    Device::get().d2dFactory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r.d2d(), radius, radius), &geo);
    dc_->PushLayer(D2D1::LayerParameters1(r.d2d(), geo.Get()), nullptr);
    ++layerDepth_;
}

void Canvas::popLayer() {
    if (layerDepth_ <= 0) return;
    dc_->PopLayer();
    --layerDepth_;
}

void Canvas::pushOpacity(float opacity) {
    dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), opacity),
                   nullptr);
    ++layerDepth_;
}

void Canvas::pushTransform(const D2D1_MATRIX_3X2_F& m) {
    D2D1_MATRIX_3X2_F cur;
    dc_->GetTransform(&cur);
    transforms_.push_back(cur);
    const auto local = *D2D1::Matrix3x2F::ReinterpretBaseType(&m);
    dc_->SetTransform(local * *D2D1::Matrix3x2F::ReinterpretBaseType(&cur));
    // Map the visible rect into the new local space (inverse transform, axis-aligned bounds).
    auto inv = local;
    Rect vis = clips_.back().r;
    if (inv.Invert()) {
        const D2D1_POINT_2F a = inv.TransformPoint({vis.x, vis.y});
        const D2D1_POINT_2F b = inv.TransformPoint({vis.right(), vis.bottom()});
        vis = {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
    }
    clips_.push_back({vis, false});
}

void Canvas::popTransform() {
    if (transforms_.empty() || clips_.back().axisClip) return;
    dc_->SetTransform(transforms_.back());
    transforms_.pop_back();
    clips_.pop_back();
}

void Canvas::pushScale(float s, Point c) { pushTransform(D2D1::Matrix3x2F::Scale(s, s, {c.x, c.y})); }

} // namespace st::gfx
