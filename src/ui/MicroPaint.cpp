// Drawing half of ui/Micro.h (the logic lives in Micro.cpp, which tests compile without a device).
#include "gfx/Canvas.h"
#include "gfx/Theme.h"
#include "ui/Micro.h"
#include "ui/Widget.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace st::ui {

using gfx::Color;
using gfx::Point;
using gfx::Rect;

void paintEqualizer(gfx::Canvas& c, Widget* owner, const Rect& box, const Color& color, bool playing) {
    auto& clk = equalizer::clock();
    const double t = clk.advance(frame::now(), playing && motion::micro());
    float lv[4];
    equalizer::levels(t, lv);
    const float s = c.scale();
    const float barW = std::max(2.f, std::round(std::min(box.w, box.h) / 7.f));   // 14 / 16 DIP boxes: 2 DIP bars
    const float gap = std::clamp((box.w - 4 * barW) / 3.f, 1.f, barW);
    const float total = 4 * barW + 3 * gap;
    const float maxH = std::round(box.h * 0.8f), minH = std::max(2.f, std::round(box.h * 0.2f));
    float x = gfx::snap(box.cx() - total * 0.5f, s);
    const float bottom = gfx::snap(box.cy() + maxH * 0.5f, s);
    for (int i = 0; i < 4; ++i) {
        const float h = minH + (maxH - minH) * lv[i];
        c.fillRounded({x, bottom - h, barW, h}, barW * 0.5f, color);
        x += barW + gap;
    }
    if (clk.moving() && motion::micro() && owner)
        if (auto* w = owner->window()) w->invalidateAfter(equalizer::kFrameMs);
}

void HeartPop::draw(gfx::Canvas& c, const Rect& box, bool liked, std::string_view outline, std::string_view filled,
                    const Color& outlineColor, const Color& fillColor) const {
    const Pose p = pose(liked);
    const Point center{box.cx(), box.cy()};
    const bool scaled = std::abs(p.scale - 1.f) > 0.001f;
    if (scaled) c.pushScale(p.scale, center);
    if (p.outline > 0.001f)
        c.icon(outline, box, (liked ? Color::lerp(outlineColor, fillColor, p.fill) : outlineColor).mulAlpha(p.outline));
    if (p.fill > 0.001f) {
        const float fs = 0.35f + 0.65f * p.fill;   // the fill grows out of (or back into) the outline
        if (fs < 0.999f) {
            c.pushScale(fs, center);
            c.icon(filled, box, fillColor.mulAlpha(p.fill));
            c.popTransform();
        } else {
            c.icon(filled, box, fillColor);
        }
    }
    if (scaled) c.popTransform();
    if (p.burst >= 0.f && p.burst < 1.f) {
        // Six thin ticks fly out from just outside the glyph and fade.
        const float e = ease(Ease::Decelerate, p.burst);
        const float size = std::min(box.w, box.h);
        const float r0 = size * (0.62f + 0.34f * e), len = size * 0.2f * (1.f - e) + 0.5f;
        const float fade = 1.f - p.burst;
        const Color tick = fillColor.mulAlpha(0.9f * fade * fade);
        for (int k = 0; k < 6; ++k) {
            const float a = (-90.f + 60.f * static_cast<float>(k)) * std::numbers::pi_v<float> / 180.f;
            const float dx = std::cos(a), dy = std::sin(a);
            c.line({center.x + dx * r0, center.y + dy * r0}, {center.x + dx * (r0 + len), center.y + dy * (r0 + len)}, tick,
                   1.25f);
        }
    }
}

namespace card {

void paintBack(gfx::Canvas& c, const Rect& art, float hover, bool circle) {
    if (hover <= 0.01f) return;
    c.shadow(art, circle ? art.w * 0.5f : 2.f, 24, 12, gfx::colors().shadowCard.mulAlpha(hover));   // cardHoverShadow
}

void paintEdge(gfx::Canvas& c, const Rect& art, float hover, bool circle) {
    if (hover <= 0.01f) return;
    if (circle) c.strokeCircle({art.cx(), art.cy()}, art.w * 0.5f, gfx::accent().base.mulAlpha(hover * 0.8f), 1.5f);
    else c.strokeRounded(art.inset(0.5f), 2, gfx::colors().hairStrong.mulAlpha(hover));
}

void paintPlay(gfx::Canvas& c, const Rect& r, const Play& p, const Color& fill, const Color& fillHot, const Color& glyph) {
    if (p.show <= 0.01f) return;
    const float e = ease(Ease::Decelerate, std::clamp(p.show, 0.f, 1.f));
    const Rect pr = r.offset(0, 8.f * (1.f - e));
    const Point center{pr.cx(), pr.cy()};
    const float s = (0.9f + 0.1f * e) * (1.f + 0.04f * p.hot) * (1.f - 0.08f * p.press);
    const bool faded = p.show < 0.999f;
    if (faded) c.pushOpacity(p.show);
    c.pushScale(s, center);
    c.fillCircle(center, pr.w * 0.5f, Color::lerp(fill, fillHot, std::clamp(p.hot, 0.f, 1.f)));
    c.icon(p.pause ? "pause" : "play", pr.center(p.iconSize, p.iconSize).offset(p.pause ? 0.f : 1.f, 0), glyph);
    c.popTransform();
    if (faded) c.popLayer();
}

} // namespace card

} // namespace st::ui
