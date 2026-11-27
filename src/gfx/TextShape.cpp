#include "gfx/TextShape.h"

#include "gfx/Canvas.h"

#include <algorithm>
#include <cmath>

namespace st::gfx {

// Every shape's state, weakly: the device release hook drops their realizations (one left behind would keep the whole
// device alive, 3.2).
struct TextShapeRegistry {
    static std::vector<std::weak_ptr<TextShape::State>>& list() {
        static std::vector<std::weak_ptr<TextShape::State>> l;
        return l;
    }
    static void add(const std::shared_ptr<TextShape::State>& s) {
        static const int hook = Device::get().addReleaseHook([] {
            for (auto& w : list())
                if (auto st = w.lock()) {
                    st->rows.clear();
                    st->usable = false;
                    st->generation = 0;
                }
            list().clear();
        });
        (void)hook;
        auto& l = list();
        if (l.size() >= 64) std::erase_if(l, [](const auto& w) { return w.expired(); });
        l.push_back(s);
    }
};

namespace {

uint64_t g_rowsBuilt = 0;

// Glyph outlines into a path geometry sink, moved by (dx, dy): DirectWrite writes them relative to the run's baseline
// origin.
class OffsetSink final : public IDWriteGeometrySink {
public:
    OffsetSink(ID2D1GeometrySink* s, float dx, float dy) : s_(s), dx_(dx), dy_(dy) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) {
            *out = static_cast<IDWriteGeometrySink*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // on the stack: nothing to count
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE m) override { s_->SetFillMode(m); }
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT f) override { s_->SetSegmentFlags(f); }
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN b) override {
        s_->BeginFigure({p.x + dx_, p.y + dy_}, b);
        any = true;
    }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* pts, UINT32 n) override {
        for (UINT32 i = 0; i < n; ++i) s_->AddLine({pts[i].x + dx_, pts[i].y + dy_});
    }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* b, UINT32 n) override {
        for (UINT32 i = 0; i < n; ++i)
            s_->AddBezier({{b[i].point1.x + dx_, b[i].point1.y + dy_},
                           {b[i].point2.x + dx_, b[i].point2.y + dy_},
                           {b[i].point3.x + dx_, b[i].point3.y + dy_}});
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END e) override { s_->EndFigure(e); }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
    bool any = false;

private:
    ID2D1GeometrySink* s_;
    float dx_, dy_;
};

// Collects a layout's glyph runs (and its trimming sign's) into one geometry per visual row.
class Collector final : public IDWriteTextRenderer {
public:
    struct Target {
        float baseline = 0;   // layout y of the row's baseline
        float snap = 0;       // added to move it onto the pixel grid
        ID2D1GeometrySink* sink = nullptr;
        bool any = false;
    };
    explicit Collector(std::vector<Target>& rows, float pxPerDip) : rows_(rows), pxPerDip_(pxPerDip) {}
    bool color = false;   // a run in a color font (emoji): not drawable as one-color geometry

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* off) override {
        *off = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override {
        *p = pxPerDip_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        if (!run || !run->fontFace || run->glyphCount == 0 || rows_.empty()) return S_OK;
        ComPtr<IDWriteFontFace2> face2;
        if (SUCCEEDED(run->fontFace->QueryInterface(IID_PPV_ARGS(&face2))) && face2->IsColorFont()) {
            color = true;
            return S_OK;
        }
        // The row whose baseline this run sits on.
        auto& t = *std::min_element(rows_.begin(), rows_.end(), [y](const Target& a, const Target& b) {
            return std::abs(a.baseline - y) < std::abs(b.baseline - y);
        });
        OffsetSink sink(t.sink, x, y + t.snap);
        const HRESULT hr = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances,
                                                             run->glyphOffsets, run->glyphCount, run->isSideways,
                                                             (run->bidiLevel & 1) != 0, &sink);
        t.any = t.any || sink.any;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* ctx, FLOAT x, FLOAT y, IDWriteInlineObject* obj, BOOL sideways,
                                               BOOL rtl, IUnknown* effect) override {
        // The ellipsis of a trimmed line draws its glyphs back through DrawGlyphRun.
        return obj ? obj->Draw(ctx, this, x, y, sideways, rtl, effect) : S_OK;
    }

private:
    std::vector<Target>& rows_;
    float pxPerDip_;
};

} // namespace

bool TextShape::build(ID2D1DeviceContext5* dc, IDWriteTextLayout* layout, float pxPerDip, float maxZoom) {
    state_ = std::make_shared<State>();
    auto& st = *state_;
    st.layout = layout;
    st.pxPerDip = pxPerDip;
    st.generation = Device::get().generation();
    TextShapeRegistry::add(state_);
    if (!dc || !layout || pxPerDip <= 0) return false;
    st.width = layout->GetMaxWidth();

    UINT32 count = 0;
    layout->GetLineMetrics(nullptr, 0, &count);
    if (count == 0) return false;
    std::vector<DWRITE_LINE_METRICS> lines(count);
    if (FAILED(layout->GetLineMetrics(lines.data(), count, &count))) return false;

    auto* factory = Device::get().d2dFactory();
    std::vector<ComPtr<ID2D1PathGeometry>> geos(count);
    std::vector<ComPtr<ID2D1GeometrySink>> sinks(count);
    std::vector<Collector::Target> targets(count);
    float top = 0;
    st.rows.resize(count);
    for (UINT32 i = 0; i < count; ++i) {
        if (FAILED(factory->CreatePathGeometry(&geos[i])) || FAILED(geos[i]->Open(&sinks[i]))) return false;
        sinks[i]->SetFillMode(D2D1_FILL_MODE_WINDING);
        const float baseline = top + lines[i].baseline;
        targets[i] = {baseline, (std::round(baseline * pxPerDip) - baseline * pxPerDip) / pxPerDip, sinks[i].Get()};
        st.rows[i].top = top;
        st.rows[i].height = lines[i].height;
        top += lines[i].height;
    }
    Collector collector(targets, pxPerDip);
    const HRESULT drawn = layout->Draw(nullptr, &collector, 0, 0);
    for (auto& s : sinks) s->Close();
    if (FAILED(drawn) || collector.color) {
        st.rows.clear();
        return false;
    }
    const float tolerance =
        D2D1::ComputeFlatteningTolerance(D2D1::Matrix3x2F::Identity(), 96.f * pxPerDip, 96.f * pxPerDip, std::max(1.f, maxZoom));
    const auto aa = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    bool ok = true;
    for (UINT32 i = 0; i < count && ok; ++i) {
        if (!targets[i].any) continue;
        ok = SUCCEEDED(dc->CreateFilledGeometryRealization(geos[i].Get(), tolerance, &st.rows[i].fill));
        ++g_rowsBuilt;
    }
    dc->SetAntialiasMode(aa);
    if (!ok) {
        st.rows.clear();
        return false;
    }
    st.usable = true;
    return true;
}

bool TextShape::built() const { return state_ && state_->usable && state_->generation == Device::get().generation(); }

bool TextShape::builtFor(IDWriteTextLayout* layout, float pxPerDip) const {
    // A width change keeps the layout object (Text::layout sets a new max width), so it is compared too.
    return state_ && state_->generation == Device::get().generation() && state_->layout.Get() == layout && layout &&
           state_->width == layout->GetMaxWidth() && std::abs(state_->pxPerDip - pxPerDip) < 0.01f;
}

void TextShape::drawRow(Canvas& c, size_t row, Point origin, ID2D1Brush* brush, float scale, Point anchor) const {
    if (!built() || row >= state_->rows.size() || !brush) return;
    const Row& r = state_->rows[row];
    if (!r.fill) return;
    // Off the visible area: nothing to draw.
    const float y0 = origin.y + anchor.y + (r.top - anchor.y) * scale;
    const Rect& vis = c.clip();
    if (y0 > vis.bottom() || y0 + r.height * scale < vis.y) return;
    auto* dc = c.dc();
    D2D1_MATRIX_3X2_F cur;
    dc->GetTransform(&cur);
    const auto local = D2D1::Matrix3x2F::Scale(scale, scale, {anchor.x, anchor.y}) * D2D1::Matrix3x2F::Translation(origin.x, origin.y);
    dc->SetTransform(local * *D2D1::Matrix3x2F::ReinterpretBaseType(&cur));
    dc->DrawGeometryRealization(r.fill.Get(), brush);
    dc->SetTransform(cur);
}

void TextShape::draw(Canvas& c, Point origin, ID2D1Brush* brush, float scale, Point anchor) const {
    if (!built() || !brush) return;
    auto* dc = c.dc();
    D2D1_MATRIX_3X2_F cur;
    dc->GetTransform(&cur);
    const auto local = D2D1::Matrix3x2F::Scale(scale, scale, {anchor.x, anchor.y}) * D2D1::Matrix3x2F::Translation(origin.x, origin.y);
    dc->SetTransform(local * *D2D1::Matrix3x2F::ReinterpretBaseType(&cur));
    const Rect& vis = c.clip();
    for (const Row& r : state_->rows) {
        if (!r.fill) continue;
        const float y0 = origin.y + anchor.y + (r.top - anchor.y) * scale;
        if (y0 > vis.bottom() || y0 + r.height * scale < vis.y) continue;
        dc->DrawGeometryRealization(r.fill.Get(), brush);
    }
    dc->SetTransform(cur);
}

size_t TextShape::liveShapes() {
    size_t n = 0;
    for (const auto& w : TextShapeRegistry::list())
        if (auto s = w.lock(); s && s->usable) ++n;
    return n;
}

uint64_t TextShape::rowsBuilt() { return g_rowsBuilt; }

} // namespace st::gfx
