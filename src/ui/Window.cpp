#include "ui/Window.h"

#include "core/Log.h"
#include "gfx/Icons.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Anim.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <typeinfo>

#pragma comment(lib, "dwmapi.lib")

namespace st::ui {

using gfx::ComPtr;

namespace {
constexpr wchar_t kClassName[] = L"ShadeTube.Window";
constexpr double kTooltipDelay = 400;

void registerClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        if (auto* self = Window::fromHwnd(h)) return self->handleMessageThunk(h, m, w, l);
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

MouseButton buttonFor(UINT msg, WPARAM wp) {
    switch (msg) {
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return MouseButton::Right;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return MouseButton::Middle;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? MouseButton::Back : MouseButton::Forward;
    default: return MouseButton::Left;
    }
}
} // namespace

Window* Window::fromHwnd(HWND hwnd) {
    return reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

Window::Window(const WindowOptions& options) : options_(options) {
    registerClass();
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!options.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    if (options.toolWindow) style &= ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX);   // a minimized tool window has no taskbar home
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;   // flip-model swap chain draws everything
    if (options.topmost) exStyle |= WS_EX_TOPMOST;
    if (options.toolWindow) exStyle |= WS_EX_TOOLWINDOW;

    hwnd_ = CreateWindowExW(exStyle, kClassName, options.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    const float s = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.f;
    const int w = static_cast<int>(options.width * s), h = static_cast<int>(options.height * s);
    int x = options.x, y = options.y;
    if (x == CW_USEDEFAULT || y == CW_USEDEFAULT) {   // center on the primary work area
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
    }
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    applyTheme();

    target_ = std::make_unique<gfx::WindowTarget>(hwnd_);
    resized();
}

Window::~Window() {
    overlays_.clear();
    root_.reset();
    graveyard_.clear();
    target_.reset();
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

LRESULT Window::handleMessageThunk(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    if (!hwnd_) hwnd_ = hwnd;   // messages arrive (WM_NCCREATE...) before CreateWindowEx returns
    return handleMessage(m, w, l);
}

void Window::show(int cmd) {
    ShowWindow(hwnd_, options_.maximized && cmd == SW_SHOW ? SW_SHOWMAXIMIZED : cmd);
    UpdateWindow(hwnd_);
}

void Window::close() { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }

void Window::applyTheme() {
    const BOOL dark = gfx::Theme::get().isLight() ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    const auto bg = gfx::colors().bgBase;
    const COLORREF border = RGB(static_cast<int>(bg.r * 255), static_cast<int>(bg.g * 255), static_cast<int>(bg.b * 255));
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof border);
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &border, sizeof border);
    scheduleLayout();
    invalidate();
}

void Window::setRoot(std::unique_ptr<Widget> root) {
    root_ = std::move(root);
    root_->window_ = this;
    scheduleLayout();
}

Widget* Window::pushOverlay(std::unique_ptr<Widget> overlay, bool modal) {
    overlay->window_ = this;
    overlays_.push_back({std::move(overlay), modal});
    auto* raw = overlays_.back().widget.get();
    raw->setRect(clientRect());
    if (modal) {
        if (capture_) {
            capture_->pressed_ = false;
            capture_ = nullptr;
            ReleaseCapture();
        }
        // Trap keyboard focus: remember who had it, take it off the layer below and give the overlay's first tab
        // stop focus after the next layout (a Dialog's content is added after open() returns). A widget the
        // opener focuses itself (box->focus()) wins over that.
        savedFocus_.push_back({raw, focus_});
        pendingOverlayFocus_ = raw;
        if (focus_) setFocus(nullptr);
        scheduleLayout();
        // A keyboard-focus tooltip (not cleared by a click here) would be painted over the menu / dialog.
        tipWidget_ = nullptr;
        tipText_.clear();
    }
    updateHover(nullptr);
    invalidate();
    return raw;
}

void Window::removeOverlay(Widget* overlay) {
    auto it = std::find_if(overlays_.begin(), overlays_.end(), [&](const auto& e) { return e.widget.get() == overlay; });
    if (it == overlays_.end()) return;
    // Focus to give back (kept valid by forget() while the overlay was open).
    Widget* previous = nullptr;
    bool restore = false;
    if (auto s = std::find_if(savedFocus_.begin(), savedFocus_.end(), [&](const SavedFocus& f) { return f.overlay == overlay; });
        s != savedFocus_.end()) {
        previous = s->previous;
        restore = true;
        savedFocus_.erase(s);
    }
    forget(overlay);
    auto owned = std::move(it->widget);
    overlays_.erase(it);
    owned->window_ = nullptr;
    deferDelete(std::move(owned));
    // setFocus() hands it on again if another modal overlay still covers `previous`.
    if (restore && !focus_ && previous) setFocus(previous);
    invalidate();
}

bool Window::hasModal() const {
    return std::any_of(overlays_.begin(), overlays_.end(), [](const auto& e) { return e.modal; });
}

void Window::invalidate() { dirty_ = true; }

void Window::invalidateAfter(double ms) { wakeAt_ = std::min(wakeAt_, frame::realNow() + ms); }

void Window::scheduleLayout() {
    layoutDirty_ = true;
    dirty_ = true;
}

bool Window::isShown() const { return IsWindowVisible(hwnd_) && !IsIconic(hwnd_); }

bool Window::needsFrame() const {
    if (!isShown()) return false;
    return dirty_ || animating_ || frame::realNow() >= wakeAt_;
}

// A stale wake-up of a hidden/minimized window would otherwise make the loop spin (timeout 0) forever.
double Window::nextWakeMs() const { return isShown() ? wakeAt_ : 1e300; }

bool Window::isMaximized() const { return IsZoomed(hwnd_) != FALSE; }
bool Window::isMinimized() const { return IsIconic(hwnd_) != FALSE; }
void Window::minimize() { ShowWindow(hwnd_, SW_MINIMIZE); }
void Window::toggleMaximize() { ShowWindow(hwnd_, isMaximized() ? SW_RESTORE : SW_MAXIMIZE); }

void Window::setFocus(Widget* w) {
    if (w == focus_) return;
    if (Widget* scope = focusScope(); w && scope != root_.get() && !inScope(w)) {
        // Focus trap: a widget below the open modal overlay (a page behind a Dialog focusing its search box after a
        // global shortcut navigated) gets focus when that overlay closes, not now.
        for (auto it = savedFocus_.rbegin(); it != savedFocus_.rend(); ++it) {
            if (it->overlay == scope) {
                it->previous = w;
                break;
            }
        }
        return;
    }
    Widget* old = focus_;
    focus_ = w;
    if (w) {
        navStart_ = w;
        lost_ = {};
    }
    if (old) old->onFocusChanged(false);
    if (w) w->onFocusChanged(true);
    if (w && focusVisible_) restartRing();
    invalidate();
}

void Window::restartRing() {
    // motion-spec "Focus ring (keyboard)": opacity 0 -> 1, 120 ms standard (instant with reduce-motion).
    ringAlpha_.snap(0);
    ringAlpha_.to(1, motion::fast, Ease::Standard);
}

void Window::focusByKeyboard(Widget* w) {
    const bool wasVisible = focusVisible_;
    focusVisible_ = true;
    if (w != focus_) setFocus(w);            // restarts the ring
    else if (!wasVisible && w) restartRing();
    if (w && w != focus_) return;            // below an open modal overlay: deferred by setFocus()
    // An icon button's tooltip is its only label: show it after the usual hover delay (rapid Tabbing never flashes
    // tooltips). The next mouse move over something else replaces it, a click or an activation key hides it.
    tipText_.clear();
    tipWidget_ = nullptr;
    if (w && !w->tooltip().empty()) {
        tipWidget_ = w;
        tipHoverStart_ = frame::realNow();
        invalidateAfter(kTooltipDelay + 10);
    }
    if (w) w->scrollIntoView();              // last: scroll listeners may rebuild content (forget() then cleans up)
    invalidate();
}

Widget* Window::focusScope() const {
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it)
        if (it->modal) return it->widget.get();
    return root_.get();
}

bool Window::inScope(const Widget* w) const {
    if (!w) return false;
    while (w->parent_) w = w->parent_;
    return w == focusScope();
}

namespace {
// `w` and all its ancestors are visible.
bool shown(const Widget* w) {
    for (; w; w = w->parent())
        if (!w->visible()) return false;
    return true;
}

// Window position of `p`'s content space origin (where a child at {0, 0} would be).
Point contentOrigin(const Widget* p) {
    const Rect r = p->toWindow(p->rect());
    const Point o = p->contentOffset();
    return {r.x + o.x, r.y + o.y};
}

// Pre-order walk collecting tab stops. Hidden / disabled subtrees are still walked (eligible = false) so that the
// position of an anchor inside them is known. anchorPos = number of stops that precede `anchor`.
void collectStops(Widget* node, bool eligible, const Widget* anchor, std::vector<Widget*>& stops, int& anchorPos) {
    eligible = eligible && node->visible() && node->enabled();
    const bool empty = node->rect().w <= 0 || node->rect().h <= 0;
    if (node == anchor) anchorPos = static_cast<int>(stops.size());
    if (eligible && node->focusable && !empty) stops.push_back(node);
    if (empty && node->clipsChildren) eligible = false;   // nothing inside can be seen
    for (const auto& c : node->children()) collectStops(c.get(), eligible, anchor, stops, anchorPos);
}
} // namespace

bool Window::moveFocus(bool forward) {
    Widget* scope = focusScope();
    if (!scope) return false;
    // Start from the focused widget, else from the last click / the parent of a destroyed focused widget.
    const Widget* anchor = inScope(focus_) ? focus_ : inScope(navStart_) ? navStart_ : nullptr;
    std::vector<Widget*> stops;
    int anchorPos = -1;
    collectStops(scope, true, anchor, stops, anchorPos);
    if (stops.empty()) return false;
    const int n = static_cast<int>(stops.size());
    int next = forward ? 0 : n - 1;
    if (anchorPos >= 0) {
        const bool anchorIsStop = anchorPos < n && stops[anchorPos] == anchor;
        next = forward ? (anchorIsStop ? anchorPos + 1 : anchorPos) : anchorPos - 1;
        next = (next % n + n) % n;   // wrap around
    }
    focusByKeyboard(stops[next]);
    return true;
}

void Window::applyPendingOverlayFocus() {
    Widget* overlay = pendingOverlayFocus_;
    if (!overlay) return;
    pendingOverlayFocus_ = nullptr;
    if (overlay != focusScope() || inScope(focus_)) return;   // closed meanwhile, or the opener focused a field
    std::vector<Widget*> stops;
    int unused = -1;
    collectStops(overlay, true, nullptr, stops, unused);
    if (stops.empty()) return;   // Menu: keys go to the overlay itself
    setFocus(stops.front());     // Dialog: first field, else first button (ring only in keyboard modality)
    if (focusVisible_) stops.front()->scrollIntoView();
}

void Window::applyFocusRestore() {
    if (!lost_.parent) return;
    if (focus_ || !focusVisible_ || frame::realNow() > lost_.until || !inScope(lost_.parent)) {
        lost_ = {};
        return;
    }
    std::vector<Widget*> stops;
    int unused = -1;
    collectStops(lost_.parent, true, nullptr, stops, unused);
    const Point o = contentOrigin(lost_.parent);
    for (Widget* s : stops) {
        if (typeid(*s) != *lost_.type) continue;
        const Rect r = s->toWindow(s->focusRect());
        const Rect& l = lost_.rect;
        if (std::abs(r.x - o.x - l.x) < 1.5f && std::abs(r.y - o.y - l.y) < 1.5f && std::abs(r.w - l.w) < 1.5f &&
            std::abs(r.h - l.h) < 1.5f) {
            lost_ = {};
            setFocus(s);
            ringAlpha_.snap(1);   // same spot as before: no fade
            return;
        }
    }
    // No match yet: the rebuild may still be queued (Dispatcher::post); retried after the next layout until `until`.
}

void Window::forget(Widget* w) {
    // Also forget descendants (they die with w).
    std::function<bool(const Widget*)> contains = [&](const Widget* node) {
        if (!node) return false;
        for (const Widget* p = node; p; p = p->parent_)
            if (p == w) return true;
        return false;
    };
    if (contains(hover_)) hover_ = nullptr;
    if (contains(capture_)) {
        capture_ = nullptr;
        ReleaseCapture();
    }
    // A destroyed focused widget (e.g. its page was replaced) leaves Tab starting at its surviving parent. In the
    // destructor path the parent is forgotten first, so navStart_ never points into a dying subtree.
    if (contains(focus_)) {
        if (focusVisible_ && w->parent_) {   // release path: everything is still attached and alive here
            const Point o = contentOrigin(w->parent_);
            const Rect fr = focus_->toWindow(focus_->focusRect());
            lost_ = {w->parent_, &typeid(*focus_), {fr.x - o.x, fr.y - o.y, fr.w, fr.h}, frame::realNow() + 1000};
        }
        focus_ = nullptr;
        navStart_ = w->parent_;
    }
    if (contains(navStart_)) navStart_ = w->parent_;
    if (contains(lost_.parent)) lost_ = {};
    for (auto& s : savedFocus_)
        if (contains(s.previous)) s.previous = nullptr;
    std::erase_if(savedFocus_, [&](const SavedFocus& s) { return s.overlay == w; });
    if (contains(pendingOverlayFocus_)) pendingOverlayFocus_ = nullptr;
    if (contains(tipWidget_)) {
        tipWidget_ = nullptr;
        tipText_.clear();
    }
}

void Window::widgetHidden(Widget* w) {
    for (const Widget* p = tipWidget_; p; p = p->parent_) {
        if (p != w) continue;
        tipWidget_ = nullptr;   // e.g. a keyboard-focus tooltip of a title-bar button hidden by Now Playing
        tipText_.clear();
        break;
    }
    for (const Widget* p = focus_; p; p = p->parent_) {
        if (p != w) continue;
        Widget* was = focus_;
        setFocus(nullptr);
        navStart_ = was;   // kept valid by forget()
        return;
    }
}

void Window::deferDelete(std::unique_ptr<Widget> w) {
    graveyard_.push_back(std::move(w));
    invalidate();
}

void Window::runLayout() {
    if (!layoutDirty_) return;
    layoutDirty_ = false;
    const Rect r = clientRect();
    if (root_) {
        root_->layoutPending_ = true;
        root_->setRect(r);
    }
    for (auto& o : overlays_) {
        o.widget->layoutPending_ = true;
        o.widget->setRect(r);
    }
    applyPendingOverlayFocus();
    applyFocusRestore();
}

void Window::render() {
    graveyard_.clear();
    if (!target_ || isMinimized()) return;
    frame::begin();
    runLayout();
    // Widgets may request layout while painting (e.g. text measured late): run it before drawing.
    auto* dc = target_->begin();
    if (!dc) return;
    gfx::ImageCache::get().beginFrame();
    const bool accentAnimating = gfx::Theme::get().tick(frame::now());
    {
        Canvas c(dc, target_->scale());
        c.clear(gfx::colors().bgBase);
        if (root_) root_->paint(c);
        for (auto& o : overlays_) o.widget->paint(c);
        paintFocusRing(c);
        paintTooltip(c);
    }
    const bool more = frame::consumeRequest() || accentAnimating;
    dirty_ = false;
    animating_ = more;
    if (frame::realNow() >= wakeAt_) wakeAt_ = 1e300;
    if (!target_->end()) dirty_ = true;   // device lost: redraw with recreated resources
    if (layoutDirty_) dirty_ = true;
}

bool Window::renderToPng(const std::wstring& path) {
    if (!target_) return false;
    auto& dev = gfx::Device::get();
    auto* dc = dev.resourceContext();
    const float s = scale();
    const UINT w = static_cast<UINT>(widthDip_ * s), h = static_cast<UINT>(heightDip_ * s);
    ST_LOG_INFO("ui", "renderToPng {}x{} px", w, h);
    if (!w || !h) return false;
    ComPtr<ID2D1Bitmap1> target, cpu;
    const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const float dpi = 96.f * s;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt, dpi, dpi), &target))) {
        ST_LOG_ERROR("ui", "renderToPng: target bitmap");
        return false;
    }
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt, dpi, dpi), &cpu))) {
        ST_LOG_ERROR("ui", "renderToPng: cpu bitmap");
        return false;
    }
    frame::begin();
    runLayout();
    dc->SetTarget(target.Get());
    dc->SetDpi(dpi, dpi);
    dc->BeginDraw();
    {
        Canvas c(dc, s);
        c.clear(gfx::colors().bgBase);
        if (root_) root_->paint(c);
        for (auto& o : overlays_) o.widget->paint(c);
        paintFocusRing(c);
    }
    if (HRESULT hr = dc->EndDraw(); FAILED(hr)) {
        ST_LOG_ERROR("ui", "renderToPng EndDraw 0x{:08X}", static_cast<unsigned>(hr));
        dc->SetTarget(nullptr);
        return false;
    }
    dc->SetTarget(nullptr);
    D2D1_POINT_2U origin{0, 0};
    D2D1_RECT_U src{0, 0, w, h};
    cpu->CopyFromBitmap(&origin, target.Get(), &src);
    D2D1_MAPPED_RECT mapped{};
    if (HRESULT hr = cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped); FAILED(hr)) {
        ST_LOG_ERROR("ui", "renderToPng Map 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    auto* wic = dev.wic();
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frameEnc;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frameEnc, nullptr)) && SUCCEEDED(frameEnc->Initialize(nullptr)) &&
              SUCCEEDED(frameEnc->SetSize(w, h));
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppPBGRA;
    ok = ok && SUCCEEDED(frameEnc->SetPixelFormat(&pf)) &&
         SUCCEEDED(frameEnc->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits)) && SUCCEEDED(frameEnc->Commit()) &&
         SUCCEEDED(enc->Commit());
    cpu->Unmap();
    if (!ok) ST_LOG_ERROR("ui", "renderToPng: PNG encode failed");
    return ok;
}

void Window::resized() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    target_->resize(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top), dpi);
    widthDip_ = (rc.right - rc.left) * 96.f / dpi;
    heightDip_ = (rc.bottom - rc.top) * 96.f / dpi;
    ST_LOG_DEBUG("ui", "resized: {}x{} px @ {} dpi", rc.right - rc.left, rc.bottom - rc.top, dpi);
    scheduleLayout();
}

Point Window::toDip(LPARAM lp) const {
    const float s = scale();
    return {GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s};
}

Widget* Window::hitTest(Point p) {
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
        if (Widget* hit = it->widget->hitTest(p)) return hit;
        if (it->modal) return nullptr;
    }
    return root_ ? root_->hitTest(p) : nullptr;
}

void Window::updateHover(Widget* hit) {
    if (hit && !hit->enabled_) hit = nullptr;
    if (hit == hover_) return;
    // Leave/enter along the ancestor chains (so a card stays "hovered" while over its play button).
    std::vector<Widget*> oldChain, newChain;
    for (Widget* w = hover_; w; w = w->parent_) oldChain.push_back(w);
    for (Widget* w = hit; w; w = w->parent_) newChain.push_back(w);
    for (Widget* w : oldChain) {
        if (std::find(newChain.begin(), newChain.end(), w) == newChain.end()) {
            w->hovered_ = false;
            w->onMouseLeave();
        }
    }
    for (auto it = newChain.rbegin(); it != newChain.rend(); ++it) {
        if (!(*it)->hovered_) {
            (*it)->hovered_ = true;
            (*it)->onMouseEnter();
        }
    }
    hover_ = hit;
    // Tooltip: restart the delay for the new widget.
    tipText_.clear();
    tipWidget_ = nullptr;
    for (Widget* w = hit; w; w = w->parent_) {
        if (!w->tooltip().empty()) {
            tipWidget_ = w;
            tipHoverStart_ = frame::realNow();
            invalidateAfter(kTooltipDelay + 10);
            break;
        }
    }
    invalidate();
}

void Window::showTooltipFor(Widget* w) {
    tipWidget_ = w;
    tipHoverStart_ = frame::realNow() - kTooltipDelay;
    invalidate();
}

void Window::paintTooltip(Canvas& c) {
    if (!tipWidget_ || capture_) return;
    const double elapsed = frame::realNow() - tipHoverStart_;
    if (elapsed < kTooltipDelay) return;
    const std::wstring text = tipWidget_->tooltip();
    if (text.empty()) return;
    const float t = std::min(1.f, static_cast<float>((elapsed - kTooltipDelay) / 120.0));
    if (t < 1.f) frame::requestNext();
    const auto& col = gfx::colors();
    auto layout = gfx::makeLayout(text, gfx::type::caption, 320);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const Rect anchor = tipWidget_->toWindow(tipWidget_->rect());
    const float w = std::ceil(m.width) + 20, h = 28;
    float x = std::clamp(anchor.cx() - w * 0.5f, 8.f, widthDip_ - w - 8);
    float y = anchor.bottom() + 8;
    if (y + h > heightDip_ - 8) y = anchor.y - 8 - h;
    y += (1 - ease(Ease::Standard, t)) * 4;
    const Rect box{x, y, w, h};
    c.pushOpacity(ease(Ease::Standard, t));
    c.fillRounded(box, 2, col.bgElevated);
    c.strokeRounded(box, 2, col.hairStrong);
    c.text(layout.Get(), x + 10, y + (h - m.height) * 0.5f, col.fgPrimary);
    c.popLayer();
}

void Window::paintFocusRing(Canvas& c) {
    Widget* f = focus_;
    if (!f || !focusVisible_ || !inScope(f)) return;
    const FocusShape shape = f->focusShape();
    if (shape == FocusShape::None) return;
    for (const Widget* w = f; w; w = w->parent_)
        if (!w->visible_ || !w->enabled_) return;
    const Rect fr = f->toWindow(f->focusRect());
    if (fr.empty()) return;
    // Clip like the tree does (a row scrolled half out of its ScrollView shows only the visible part)...
    Rect clip = clientRect();
    for (const Widget* p = f->parent_; p; p = p->parent_)
        if (p->clipsChildren) clip = clip.intersect(p->toWindow(p->rect_));
    if (!fr.intersects(clip)) return;
    // ...but on a side where the widget itself is not cut, the ring may pass the clip edge by its own extent, so a
    // row flush with its list's edge still gets a complete ring.
    constexpr float kOffset = 2, kWidth = 2, kExtent = kOffset + kWidth + 1;
    const float l = fr.x >= clip.x ? fr.x - kExtent : clip.x;
    const float t = fr.y >= clip.y ? fr.y - kExtent : clip.y;
    const float r = fr.right() <= clip.right() ? fr.right() + kExtent : clip.right();
    const float b = fr.bottom() <= clip.bottom() ? fr.bottom() + kExtent : clip.bottom();
    const Rect allowed = Rect{l, t, r - l, b - t}.intersect(clientRect());
    if (allowed.empty()) return;
    const Color col = gfx::accent().base.mulAlpha(ringAlpha_.value());
    const float mid = kOffset + kWidth * 0.5f;   // the stroke is centered on its path
    c.pushClip(allowed);
    switch (shape) {
    case FocusShape::Circle: {
        const float d = std::min(fr.w, fr.h);
        c.strokeCircle({fr.cx(), fr.cy()}, d * 0.5f + mid, col, kWidth);
        break;
    }
    case FocusShape::Pill: c.strokePill(fr.inset(-mid), col, kWidth); break;
    default: c.strokeRounded(fr.inset(-mid), 2, col, kWidth); break;
    }
    c.popClip();
}

bool Window::dispatchKey(const KeyEvent& e) {
    // Tab always belongs to the Window (text fields never swallow it).
    if (e.vk == VK_TAB && !e.ctrl && !e.alt) {
        if (!moveFocus(!e.shift)) {
            // A modal scope without tab stops (Menu) gets the key itself: Tab / Shift+Tab walk its items.
            if (Widget* scope = focusScope(); scope && scope != root_.get()) scope->onKeyDown(e);
        }
        return true;
    }
    const bool activation = (e.vk == VK_RETURN || e.vk == VK_SPACE) && !e.ctrl && !e.alt;
    const bool navigation = e.vk == VK_LEFT || e.vk == VK_RIGHT || e.vk == VK_UP || e.vk == VK_DOWN || e.vk == VK_HOME ||
                            e.vk == VK_END || e.vk == VK_PRIOR || e.vk == VK_NEXT;
    Widget* f = inScope(focus_) && shown(focus_) ? focus_ : nullptr;
    // A control focused by a mouse click takes no keys: Space still reaches the global play/pause etc.
    if (f && f->activatable() && !focusVisible_) f = nullptr;
    bool handled = false;
    const Widget* chainTop = nullptr;
    for (Widget* w = f; w && !handled; w = w->parent_) {
        handled = w->onKeyDown(e);
        if (!handled && w == f && activation && focusVisible_)
            handled = e.repeat ? f->activatable() : f->onActivate();   // holding Enter/Space never re-fires
        if (handled && w == f && navigation) {
            // Arrow keys inside a focused list / control are keyboard interaction: show the ring, follow it.
            if (!focusVisible_) {
                focusVisible_ = true;
                restartRing();
            }
            if (focus_) focus_->scrollIntoView();
        }
        chainTop = w;
    }
    if (handled && activation) {   // like a click: the keyboard-focus tooltip goes away
        tipWidget_ = nullptr;
        tipText_.clear();
    }
    if (!handled) {
        // The open modal overlay (Menu / Dialog), else the topmost overlay, unless it already saw the key above.
        Widget* scope = focusScope();
        Widget* target = scope != root_.get() ? scope : (!overlays_.empty() ? overlays_.back().widget.get() : nullptr);
        if (target && target != chainTop) handled = target->onKeyDown(e);
    }
    if (!handled && onKey) handled = onKey(e);
    return handled;
}

MouseEvent Window::makeEvent(Widget* target, Point windowPos, MouseButton b, WPARAM wp) const {
    MouseEvent e;
    e.windowPos = windowPos;
    e.pos = target ? target->fromWindow(windowPos) : windowPos;
    e.button = b;
    e.ctrl = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
    e.shift = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
    e.alt = GetKeyState(VK_MENU) < 0;
    return e;
}

void Window::onMouseMessage(UINT msg, WPARAM wp, LPARAM lp) {
    const Point p = toDip(lp);
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!trackingLeave_) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            trackingLeave_ = true;
        }
        if (capture_) {
            capture_->onMouseMove(makeEvent(capture_, p, MouseButton::Left, wp));
            // Keep hover consistent with what's under the cursor for visual feedback.
            Widget* hit = hitTest(p);
            capture_->hovered_ = (hit == capture_);
        } else {
            Widget* hit = hitTest(p);
            updateHover(hit);
            if (hover_) hover_->onMouseMove(makeEvent(hover_, p, MouseButton::Left, wp));
        }
        break;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: {
        const MouseButton b = buttonFor(msg, wp);
        Widget* hit = hitTest(p);
        updateHover(hit);
        tipWidget_ = nullptr;
        // Pointer interaction hides the keyboard focus ring; Tab then continues from the clicked spot.
        focusVisible_ = false;
        navStart_ = hit;
        lost_ = {};
        // Clicking anywhere that isn't focusable clears the text focus.
        if (focus_ && hit != focus_) setFocus(nullptr);
        bool handled = false;
        for (Widget* w = hit; w && !handled; w = w->parent_) {
            if (!w->enabled_) continue;
            MouseEvent e = makeEvent(w, p, b, wp);
            e.clicks = (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK) ? 2 : 1;
            if (w->onMouseDown(e)) {
                handled = true;
                capture_ = w;
                w->pressed_ = true;
                SetCapture(hwnd_);
                // Still attached (a handler may have removed it: forget() already ran, it must not become focus_).
                if (w->focusable && w->window() == this) setFocus(w);
            }
        }
        if (!handled && (b == MouseButton::Back || b == MouseButton::Forward) && onNavButton) onNavButton(b);
        invalidate();
        break;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
        if (Widget* w = capture_) {
            capture_ = nullptr;
            w->pressed_ = false;
            ReleaseCapture();
            w->onMouseUp(makeEvent(w, p, buttonFor(msg, wp), wp));
            updateHover(hitTest(p));
        }
        invalidate();
        break;
    }
    default: break;
    }
}

LRESULT Window::hitTestNc(POINT screen) {
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (options_.resizable && !isMaximized()) {
        const int border = GetSystemMetricsForDpi(SM_CXFRAME, GetDpiForWindow(hwnd_)) +
                           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(hwnd_));
        const bool left = pt.x < border, right = pt.x >= rc.right - border;
        const bool top = pt.y < border, bottom = pt.y >= rc.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    const float s = scale();
    Widget* hit = hitTest({pt.x / s, pt.y / s});
    if (hit) {
        if (hit->isMaximizeButton && options_.resizable) return HTMAXBUTTON;
        if (hit->isDragRegion) return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT Window::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (onMessage) onMessage(msg, wp, lp);
    switch (msg) {
    case WM_NCCALCSIZE:
        if (options_.customChrome && wp) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            if (isMaximized()) {
                // A maximized window extends beyond the monitor by the frame size: pull the client back in.
                const UINT dpi = GetDpiForWindow(hwnd_);
                const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                params->rgrc[0].left += fx;
                params->rgrc[0].right -= fx;
                params->rgrc[0].top += fy;
                params->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (options_.customChrome) return hitTestNc({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        break;
    case WM_NCMOUSEMOVE:
        if (wp == HTMAXBUTTON) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
            TrackMouseEvent(&tme);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            updateHover(hitTest({pt.x / scale(), pt.y / scale()}));
            maxButtonHot_ = true;
        } else if (maxButtonHot_) {
            maxButtonHot_ = false;
            updateHover(nullptr);
        }
        break;
    case WM_NCMOUSELEAVE:
        if (maxButtonHot_) {
            maxButtonHot_ = false;
            if (hover_) hover_->pressed_ = false;
            updateHover(nullptr);
        }
        break;
    case WM_NCLBUTTONDOWN:
        if (wp == HTMAXBUTTON) {
            if (hover_) hover_->pressed_ = true;
            invalidate();
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) {
            if (hover_) hover_->pressed_ = false;
            toggleMaximize();
            return 0;
        }
        break;
    case WM_NCRBUTTONUP:
        if (wp == HTCAPTION) {   // system menu on right-click in the title bar
            HMENU menu = GetSystemMenu(hwnd_, FALSE);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0,
                                            hwnd_, nullptr);
            if (cmd) PostMessageW(hwnd_, WM_SYSCOMMAND, cmd, 0);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        const float s = hwnd_ ? static_cast<float>(GetDpiForWindow(hwnd_)) / 96.f : 1.f;
        mmi->ptMinTrackSize = {static_cast<LONG>(options_.minWidth * s), static_cast<LONG>(options_.minHeight * s)};
        return 0;
    }
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        gfx::Icons::clear();
        // The constructor's SetWindowPos can move a new window (mini player at its remembered spot) onto a
        // monitor with another DPI before target_ exists; the constructor's own resized() covers that case.
        if (target_) resized();
        return 0;
    }
    case WM_SIZE:
        if (target_) {
            const bool minimized = wp == SIZE_MINIMIZED;
            if (onMinimizeChanged) onMinimizeChanged(minimized);
            if (!minimized) {
                resized();
                render();   // live resize runs a modal loop: paint synchronously
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        dirty_ = true;
        render();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        active_ = LOWORD(wp) != WA_INACTIVE;
        if (!active_) consumedVk_ = 0;   // its key-up goes to another window
        invalidate();
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            Widget* w = capture_ ? capture_ : hover_;
            SetCursor(LoadCursorW(nullptr, w ? w->cursor() : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        onMouseMessage(msg, wp, lp);
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (!capture_) updateHover(nullptr);
        return 0;
    case WM_CAPTURECHANGED:
        if (capture_ && reinterpret_cast<HWND>(lp) != hwnd_) {
            capture_->pressed_ = false;
            capture_ = nullptr;
            invalidate();
        }
        return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        const Point p{pt.x / scale(), pt.y / scale()};
        const float delta = GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
        const float dy = msg == WM_MOUSEWHEEL ? delta : 0, dx = msg == WM_MOUSEHWHEEL ? delta : 0;
        for (Widget* w = hitTest(p); w; w = w->parent_) {
            if (w->enabled_ && w->onWheel(dy, dx, makeEvent(w, p, MouseButton::Middle, wp))) break;
        }
        invalidate();
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        KeyEvent e;
        e.vk = static_cast<UINT>(wp);
        e.ctrl = GetKeyState(VK_CONTROL) < 0;
        e.shift = GetKeyState(VK_SHIFT) < 0;
        e.alt = GetKeyState(VK_MENU) < 0;
        e.repeat = (lp & (1 << 30)) != 0;
        const bool activationKey = e.vk == VK_RETURN || e.vk == VK_SPACE;
        bool handled = false;
        if (e.repeat && activationKey && e.vk == consumedVk_) {
            // Holding Enter / Space after its press was handled (it may have opened a dialog whose field now has
            // focus) must not submit that field or type into it.
            handled = true;
        } else {
            try {
                handled = dispatchKey(e);
            } catch (const std::exception& ex) {   // a key handler must never unwind through the window procedure
                ST_LOG_ERROR("ui", "key handler failed: {}", ex.what());
                handled = true;
            }
            if (activationKey && !e.repeat) consumedVk_ = handled ? e.vk : 0;
        }
        if (handled) {
            // TranslateMessage already queued this key's WM_CHAR: a handled key must not also type (Space on a
            // focused button that opens a dialog would put a space into the dialog's text field).
            MSG pending;
            PeekMessageW(&pending, hwnd_, WM_CHAR, WM_CHAR, PM_REMOVE | PM_NOYIELD);
        }
        invalidate();
        if (handled) return 0;
        if (msg == WM_SYSKEYDOWN && e.vk != VK_F4) return 0;   // no menu-bar beep on Alt combos
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP:
        if (static_cast<UINT>(wp) == consumedVk_) consumedVk_ = 0;
        break;
    case WM_CHAR: {
        // Only a shown widget of the focus scope types (never one below an open modal overlay).
        Widget* f = inScope(focus_) && shown(focus_) ? focus_ : nullptr;
        if (f && wp >= 32 && f->onChar(static_cast<wchar_t>(wp))) {
            invalidate();
            return 0;
        }
        if (f && (wp == 8 || wp == 13 || wp == 27 || wp == 1 || wp == 3 || wp == 22 || wp == 24 || wp == 26)) {
            f->onChar(static_cast<wchar_t>(wp));
            invalidate();
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        if (onCloseRequested) {
            onCloseRequested();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (onDestroyed) onDestroyed();
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace st::ui
