#include "app/HeaderCollapse.h"

#include <algorithm>
#include <cmath>

namespace st::app::collapse {

using gfx::Color;

namespace {
float clamp01(float t) { return std::clamp(t, 0.f, 1.f); }
} // namespace

float soft(float d, float k) { return d >= k ? d : k * std::exp(d / k - 1.f); }

float smoothstep(float t) {
    t = clamp01(t);
    return t * t * (3.f - 2.f * t);
}

Frame compute(const Spec& sp, float s) {
    Frame f;
    s = std::max(0.f, s);
    f.s = s;
    const float barCy = kBarH * 0.5f;
    const float compactX = sp.left + kBarCover + kBarTitleGap;
    const bool hasCover = !sp.cover.empty();

    // Cover: its top and bottom edges are each pinned to the bar's cover slot. The top settles first (it starts near
    // it), the bottom keeps moving with the content until the cover is the bar's size: the cover shrinks exactly as
    // fast as the content below it comes up, so the two never meet.
    if (hasCover) {
        const float top = kBarCoverY + soft(sp.cover.y - s - kBarCoverY);
        const float bottom = kBarCoverY + kBarCover + soft(sp.cover.bottom() - s - (kBarCoverY + kBarCover));
        const float size = std::clamp(bottom - top, kBarCover, std::max(kBarCover, sp.cover.h));
        f.cover = {sp.cover.x, top, size, size};
        f.coverProgress = sp.cover.h > kBarCover ? clamp01((sp.cover.h - size) / (sp.cover.h - kBarCover)) : 1.f;
    } else {
        f.cover = {sp.left, kBarCoverY, kBarCover, kBarCover};
    }

    // Title: its vertical center rides with the content until it settles on the bar's center line; on the way it
    // scales down to the compact size (smoothstep of the distance travelled) and cross-fades into the compact layout.
    const float cy0 = sp.title.y + sp.title.h * 0.5f;
    const float travel = std::max(1.f, cy0 - barCy);
    const float q = clamp01(s / travel);
    const float e = smoothstep(q);
    f.titleProgress = q;
    f.titleAnchor.y = barCy + soft(cy0 - s - barCy);
    f.titleScale = 1.f + (sp.titleScale - 1.f) * e;
    // The cross-fade into the one-line layout happens once the title is within 35 % of its compact size, so the
    // one-line layout never shows much wider than the bar has room for.
    const float remaining = f.titleScale / std::max(0.01f, sp.titleScale) - 1.f;
    f.titleMix = 1.f - smoothstep(remaining / 0.35f);
    if (hasCover) {
        // Next to the cover, the gap narrowing as the cover shrinks.
        const float gap0 = sp.title.x - sp.cover.right();
        f.titleAnchor.x = f.cover.right() + gap0 + (kBarTitleGap - gap0) * f.coverProgress;
    } else {
        // No cover (artist): the title makes room for the avatar that fades in.
        f.avatar = smoothstep((q - 0.35f) / 0.65f);
        f.titleAnchor.x = sp.title.x + (compactX - sp.title.x) * e;
    }

    // Header content dissolves into the bar: transparent above maskTop, opaque below maskBottom. At offset 0 the band
    // lies above the topmost content (nothing fades); it moves down to just above the bar's line as the page scrolls.
    f.maskTop = std::min(kBarH - 8.f, sp.contentTop - kFade - 8.f + s);
    f.maskBottom = f.maskTop + kFade;

    // Docking: the list (its column header) reaches the bar's bottom edge.
    f.dockOffset = sp.dockY - kBarH;
    f.docked = s >= f.dockOffset - 0.25f;
    f.barBottom = kBarH + sp.columnHeaderH;
    f.hairline = sp.columnHeaderH > 0 ? 0.f : clamp01((s - f.dockOffset) / 24.f);
    // The bar's own play / shuffle come in while the header's buttons dissolve (they sit right above the list).
    f.buttons = smoothstep((s - (f.dockOffset - 110.f)) / 80.f);
    f.inset = std::min(s, f.barBottom);

    // Backdrop: anchored to the page's top, its end rides with the content and settles a little below the bar.
    const float settled = f.barBottom + kTint;
    f.backdropBottom = settled + soft(sp.backdropH - s - settled, 24.f);
    return f;
}

float contrast(Color a, Color b) {
    const float la = a.luminance(), lb = b.luminance();
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

Color over(Color base, Color top) {
    const float t = clamp01(top.a);
    return {base.r + (top.r - base.r) * t, base.g + (top.g - base.g) * t, base.b + (top.b - base.b) * t, 1.f};
}

Color wash(Color source, const Ink& ink, float strength) {
    // A wash may cost a text color at most 6 % of the contrast it has on the bare background when that is already
    // below the target.
    const float needPrimary = std::min(7.f, contrast(ink.primary, ink.bg) * 0.94f);
    const float needSecondary = std::min(4.5f, contrast(ink.secondary, ink.bg) * 0.94f);
    float a = std::max(0.f, strength);
    for (; a > 0.f; a -= 0.005f) {
        const Color bg = over(ink.bg, source.withAlpha(a));
        if (contrast(ink.primary, bg) >= needPrimary && contrast(ink.secondary, bg) >= needSecondary) break;
    }
    return source.withAlpha(std::max(0.f, a));
}

} // namespace st::app::collapse
