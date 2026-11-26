"""ShadeTube's icon set: writes assets/icons/<grid>/<name>.svg for every grid from the drawings in this file.

One thin-outline language for the whole app (docs/DEVELOPMENT.md, "Icons"):

- Every icon is drawn once on a 24-unit master grid (2-unit padding, stroke centers between 3 and 21) and written
  for six pixel grids: 12, 14, 16, 18, 20 and 24 px. Each file's viewBox is its own pixel grid (0 0 N N), so one unit
  is one device pixel when the icon is drawn at that size.
- Stroke: max(1, N / 16) px (1 px up to 16, 1.125 / 1.25 / 1.5 at 18 / 20 / 24): one sixteenth of the icon, never
  under a pixel. Round caps, round joins. gfx::Icons keeps at least 1 px when it draws a grid smaller than its size.
- Hinting: stroke centers sit on pixel centers (x.5), so a 1 px horizontal or vertical stroke covers whole pixels;
  lengths are whole pixels; filled shapes end on pixel edges. Points on an axis of symmetry use the mean of their
  snapped neighbours, so chevrons, triangles and roofs stay symmetric. When the grid's center is a pixel edge, a shape
  that needs a crisp centre line sits half a pixel right / down of the box's centre.
- Shapes: circles r 9, squares 16, containers corner radius 1.5 (1 px at 16), inner details 0.75. Dots (more, list
  bullets, the i of info) are filled circles of radius = the stroke, on pixel centers.
- State: outline = off / neutral. On is the accent color; an "owned" state with a body is the filled glyph
  (heart-filled = liked, downloaded = on disk), and the playing indicator is the filled equalizer. Fills are never
  decoration. A filled glyph covers exactly its outline's silhouette (fill + the same stroke), so the outline and the
  fill swap without a size change; details inside a filled glyph are cut out of it (even-odd), never painted white.

    python tools/icons.py                 # rewrite assets/icons (and remove files of icons no longer drawn here)
    python tools/icons.py --design DIR    # also write DIR/icons (the design package's copy, ShadeTube-Design)
    python tools/icons.py --check         # exit 1 when assets/icons differs from these drawings

Names: <name>.svg; the player bar's old "<name>-thin" names are aliases resolved by gfx::Icons (gfx/IconGrid.h).
"""
import argparse
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GRIDS = (12, 14, 16, 18, 20, 24)
EPS = 1e-6


def fmt(v):
    v = round(v + 0.0, 3)
    if abs(v) < 5e-4:
        return "0"
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return s


def snapc(v):
    """A pixel value -> the center of the pixel it falls in."""
    return math.floor(v + EPS) + 0.5


def below(v):
    """The largest pixel center <= v."""
    return math.floor(v - 0.5 + EPS) + 0.5


def above(v):
    """The smallest pixel center >= v."""
    return math.ceil(v - 0.5 - EPS) + 0.5


def snape(v):
    """A pixel value -> the nearest pixel edge."""
    return math.floor(v + 0.5 + EPS)


def rnd(v):
    return math.floor(v + 0.5 + EPS)


# ---- path data ------------------------------------------------------------------------------------------------------

_TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?")
_ARGS = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2, "A": 7, "Z": 0}


def parse_path(d):
    """SVG path data -> list of (command, [absolute args]) with M L C Q A Z only."""
    tokens = _TOKEN.findall(d)
    out, i, cmd = [], 0, None
    cx = cy = sx = sy = 0.0
    last_c2 = None
    while i < len(tokens):
        t = tokens[i]
        if t.isalpha():
            cmd = t
            i += 1
            if cmd in "Zz":
                out.append(("Z", []))
                cx, cy = sx, sy
                last_c2 = None
                continue
        n = _ARGS[cmd.upper()]
        a = [float(x) for x in tokens[i:i + n]]
        i += n
        rel = cmd.islower()
        C = cmd.upper()
        if C == "M":
            x, y = (cx + a[0], cy + a[1]) if rel else (a[0], a[1])
            out.append(("M", [x, y]))
            cx, cy, sx, sy = x, y, x, y
            cmd = "l" if rel else "L"
            last_c2 = None
        elif C == "L":
            x, y = (cx + a[0], cy + a[1]) if rel else (a[0], a[1])
            out.append(("L", [x, y]))
            cx, cy = x, y
            last_c2 = None
        elif C == "H":
            x = cx + a[0] if rel else a[0]
            out.append(("L", [x, cy]))
            cx = x
            last_c2 = None
        elif C == "V":
            y = cy + a[0] if rel else a[0]
            out.append(("L", [cx, y]))
            cy = y
            last_c2 = None
        elif C in "CS":
            if C == "C":
                p = [cx + a[k] if rel and k % 2 == 0 else cy + a[k] if rel else a[k] for k in range(6)]
            else:
                c1 = (2 * cx - last_c2[0], 2 * cy - last_c2[1]) if last_c2 else (cx, cy)
                q = [cx + a[k] if rel and k % 2 == 0 else cy + a[k] if rel else a[k] for k in range(4)]
                p = [c1[0], c1[1]] + q
            out.append(("C", p))
            last_c2 = (p[2], p[3])
            cx, cy = p[4], p[5]
        elif C == "Q":
            p = [cx + a[k] if rel and k % 2 == 0 else cy + a[k] if rel else a[k] for k in range(4)]
            out.append(("Q", p))
            cx, cy = p[2], p[3]
            last_c2 = None
        elif C == "A":
            x, y = (cx + a[5], cy + a[6]) if rel else (a[5], a[6])
            out.append(("A", [a[0], a[1], a[2], a[3], a[4], x, y]))
            cx, cy = x, y
            last_c2 = None
        else:
            raise ValueError(f"path command {cmd} not supported")
    return out


def write_path(segs):
    parts = []
    for c, a in segs:
        if c == "Z":
            parts.append("Z")
        elif c == "A":
            parts.append(f"A{fmt(a[0])} {fmt(a[1])} {fmt(a[2])} {int(a[3])} {int(a[4])} {fmt(a[5])} {fmt(a[6])}")
        else:
            parts.append(c + " ".join(fmt(v) for v in a))
    return "".join(parts)


# ---- geometry helpers (pixel space) ---------------------------------------------------------------------------------

def norm(x, y):
    l = math.hypot(x, y)
    return (x / l, y / l) if l > 1e-9 else (0.0, 0.0)


def polyline_d(pts, close=False, r=0.0):
    """Path data through `pts` (pixels); corners rounded with radius r (fillets) when r > 0."""
    n = len(pts)
    if r <= 0 or n < 3:
        d = "M" + "L".join(f"{fmt(x)} {fmt(y)}" for x, y in pts)
        return d + ("Z" if close else "")
    corners = range(n) if close else range(1, n - 1)
    fil = {}
    for i in corners:
        P, A, B = pts[i], pts[i - 1], pts[(i + 1) % n]
        u = norm(A[0] - P[0], A[1] - P[1])
        v = norm(B[0] - P[0], B[1] - P[1])
        cosang = max(-1.0, min(1.0, u[0] * v[0] + u[1] * v[1]))
        ang = math.acos(cosang)
        if ang < 1e-3 or abs(ang - math.pi) < 1e-3:
            continue
        t = r / math.tan(ang / 2)
        lim = min(math.dist(A, P), math.dist(B, P)) / 2
        rr = r
        if t > lim:
            t = lim
            rr = t * math.tan(ang / 2)
        d1 = (P[0] - A[0], P[1] - A[1])
        d2 = (B[0] - P[0], B[1] - P[1])
        sweep = 1 if d1[0] * d2[1] - d1[1] * d2[0] > 0 else 0
        fil[i] = ((P[0] + u[0] * t, P[1] + u[1] * t), (P[0] + v[0] * t, P[1] + v[1] * t), rr, sweep)
    out = []
    if close:
        start = fil[0][1] if 0 in fil else pts[0]
        out.append(f"M{fmt(start[0])} {fmt(start[1])}")
        order = list(range(1, n)) + [0]
    else:
        out.append(f"M{fmt(pts[0][0])} {fmt(pts[0][1])}")
        order = list(range(1, n))
    for i in order:
        if i in fil:
            t1, t2, rr, sw = fil[i]
            out.append(f"L{fmt(t1[0])} {fmt(t1[1])}A{fmt(rr)} {fmt(rr)} 0 0 {sw} {fmt(t2[0])} {fmt(t2[1])}")
        elif not (close and i == 0):
            out.append(f"L{fmt(pts[i][0])} {fmt(pts[i][1])}")
    if close:
        out.append("Z")
    return "".join(out)


def circle_d(cx, cy, r):
    return (f"M{fmt(cx - r)} {fmt(cy)}A{fmt(r)} {fmt(r)} 0 1 0 {fmt(cx + r)} {fmt(cy)}"
            f"A{fmt(r)} {fmt(r)} 0 1 0 {fmt(cx - r)} {fmt(cy)}Z")


def rrect_d(x0, y0, x1, y1, r=0.0):
    r = max(0.0, min(r, (x1 - x0) / 2, (y1 - y0) / 2))
    if r <= 1e-6:
        return f"M{fmt(x0)} {fmt(y0)}H{fmt(x1)}V{fmt(y1)}H{fmt(x0)}Z"
    a = f"A{fmt(r)} {fmt(r)} 0 0 1 "
    return (f"M{fmt(x0 + r)} {fmt(y0)}H{fmt(x1 - r)}{a}{fmt(x1)} {fmt(y0 + r)}V{fmt(y1 - r)}{a}{fmt(x1 - r)} {fmt(y1)}"
            f"H{fmt(x0 + r)}{a}{fmt(x0)} {fmt(y1 - r)}V{fmt(y0 + r)}{a}{fmt(x0 + r)} {fmt(y0)}Z")


def arc_d(cx, cy, r, a0, a1):
    """Arc of a circle from angle a0 to a1 (degrees, 0 = right, 90 = down; a1 > a0 runs clockwise)."""
    p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
    p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
    large = 1 if abs(a1 - a0) > 180 else 0
    sweep = 1 if a1 > a0 else 0
    return f"M{fmt(p0[0])} {fmt(p0[1])}A{fmt(r)} {fmt(r)} 0 {large} {sweep} {fmt(p1[0])} {fmt(p1[1])}"


def stroke_outline(pts, hw):
    """Closed outline of a polyline stroked with half-width hw (round caps, round outer joins): a cut-out's shape.

    Traversed clockwise: one side forward, the end cap, the other side back, the start cap."""
    n = len(pts)
    dirs = [norm(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]) for i in range(n - 1)]
    nrm = [(d[1], -d[0]) for d in dirs]   # the left side in screen coordinates (y down)

    def off(p, v, s):
        return (p[0] + v[0] * hw * s, p[1] + v[1] * hw * s)

    def isect(p, d, q, e):
        den = d[0] * e[1] - d[1] * e[0]
        if abs(den) < 1e-9:
            return q
        t = ((q[0] - p[0]) * e[1] - (q[1] - p[1]) * e[0]) / den
        return (p[0] + d[0] * t, p[1] + d[1] * t)

    def turn(i):   # > 0: the path turns clockwise at vertex i
        a, b = dirs[i - 1], dirs[i]
        return a[0] * b[1] - a[1] * b[0]

    arc = f"A{fmt(hw)} {fmt(hw)} 0 0 1 "
    pt = lambda p: f"{fmt(p[0])} {fmt(p[1])}"
    out = ["M" + pt(off(pts[0], nrm[0], 1))]
    for i in range(1, n - 1):   # the left side, forward
        a, b = off(pts[i], nrm[i - 1], 1), off(pts[i], nrm[i], 1)
        if turn(i) > 0:
            out.append("L" + pt(a) + arc + pt(b))
        else:
            out.append("L" + pt(isect(a, dirs[i - 1], b, dirs[i])))
    out.append("L" + pt(off(pts[-1], nrm[-1], 1)) + arc + pt(off(pts[-1], nrm[-1], -1)))
    for i in range(n - 2, 0, -1):   # the right side, back
        a, b = off(pts[i], nrm[i], -1), off(pts[i], nrm[i - 1], -1)
        if turn(i) < 0:
            out.append("L" + pt(a) + arc + pt(b))
        else:
            out.append("L" + pt(isect(a, dirs[i], b, dirs[i - 1])))
    out.append("L" + pt(off(pts[0], nrm[0], -1)) + arc + pt(off(pts[0], nrm[0], 1)) + "Z")
    return "".join(out)


# ---- the grid -------------------------------------------------------------------------------------------------------

class G:
    """One pixel grid. Master-unit helpers (lowercase) scale + snap; p-prefixed ones take pixels."""

    def __init__(self, n):
        self.n = n
        self.s = n / 24
        self.w = max(1.0, n / 16)
        self.hw = self.w / 2
        self.strokes = []
        self.fills = []   # (d, evenodd, stroked)

    # snapping
    def X(self, v):
        """Master coordinate -> stroke center (pixel center)."""
        return snapc(v * self.s)

    def E(self, v):
        """Master coordinate -> pixel edge."""
        return snape(v * self.s)

    def L(self, v, lo=1):
        """Master length -> whole pixels."""
        return max(lo, rnd(v * self.s))

    def U(self, v):
        return v * self.s

    def half(self, axis, h):
        """Half-width (master) around `axis` (pixels) so that axis +- result are pixel centers."""
        frac = 0.0 if abs(axis - math.floor(axis) - 0.5) < 1e-6 else 0.5
        hp = h * self.s
        best = rnd(hp - frac) + frac
        return max(best, frac if frac > 0 else 1.0)

    def rows(self, a, b, k):
        """k evenly spaced stroke centers from master a to b (spacing in whole pixels)."""
        y0, y1 = self.X(a), self.X(b)
        step = rnd((y1 - y0) / (k - 1))
        return [y0 + i * step for i in range(k)]

    @property
    def dot_r(self):
        """Dots are as wide as two strokes (a 2 px dot at 16), a little smaller on the smallest grids."""
        return max(0.75, self.w * min(1.0, self.n / 16))

    @property
    def small(self):
        """12 and 14 px: too few pixels for secondary details (they are left out there)."""
        return self.n <= 14

    # stroked, pixels
    def pline(self, *pts, close=False, r=0.0):
        self.strokes.append(polyline_d(list(pts), close, r))

    def pcircle(self, cx, cy, r):
        self.strokes.append(circle_d(cx, cy, r))

    def prect(self, x0, y0, x1, y1, r=0.0):
        self.strokes.append(rrect_d(x0, y0, x1, y1, r))

    def parc(self, cx, cy, r, a0, a1):
        self.strokes.append(arc_d(cx, cy, r, a0, a1))

    def pd(self, d):
        self.strokes.append(d)

    # stroked, master units
    def line(self, x1, y1, x2, y2):
        self.pline((self.X(x1), self.X(y1)), (self.X(x2), self.X(y2)))

    def poly(self, *pts, close=False, r=0.0):
        self.pline(*[(self.X(x), self.X(y)) for x, y in pts], close=close, r=r * self.s)

    def rect(self, x, y, w, h, r=0.0):
        self.prect(self.X(x), self.X(y), self.X(x + w), self.X(y + h), r * self.s)

    def circle_at(self, cx, cy, r):
        """Snapped circle (center on a pixel center, whole-pixel radius) without drawing it."""
        return self.X(cx), self.X(cy), max(1, math.ceil(r * self.s - 0.5 - EPS))

    def circle(self, cx, cy, r):
        c = self.circle_at(cx, cy, r)
        self.pcircle(*c)
        return c

    def path(self, d, hint=False):
        """Master path data, scaled; with hint the end points snap to pixel centers (control points follow)."""
        segs = parse_path(d)
        out = []
        cur = (0.0, 0.0)
        cur_snap = (0.0, 0.0)
        for c, a in segs:
            if c == "Z":
                out.append((c, a))
                continue
            if c == "A":
                x, y = a[5] * self.s, a[6] * self.s
                if hint:
                    x, y = snapc(x), snapc(y)
                out.append(("A", [a[0] * self.s, a[1] * self.s, a[2], a[3], a[4], x, y]))
                cur, cur_snap = (a[5], a[6]), (x, y)
                continue
            pts = [(a[k] * self.s, a[k + 1] * self.s) for k in range(0, len(a), 2)]
            end = pts[-1]
            end_s = (snapc(end[0]), snapc(end[1])) if hint else end
            if hint and c in "CQ":
                ds = (cur_snap[0] - cur[0] * self.s, cur_snap[1] - cur[1] * self.s)
                de = (end_s[0] - end[0], end_s[1] - end[1])
                moved = []
                for k, p in enumerate(pts[:-1]):
                    dd = ds if (c == "C" and k == 0) else de
                    moved.append((p[0] + dd[0], p[1] + dd[1]))
                pts = moved + [end_s]
            else:
                pts = pts[:-1] + [end_s]
            out.append((c, [v for p in pts for v in p]))
            cur, cur_snap = (a[-2], a[-1]), end_s
        self.strokes.append(write_path(out))

    def dot(self, cx, cy, r=None, free=False):
        if free:   # not aligned with a stroke: an even diameter sits on a pixel corner (a crisp 2 x 2 dot)
            r = self.dot_r if r is None else r
            if rnd(2 * r) % 2 == 0:
                self.pdot(self.E(cx), self.E(cy), r)
                return
        self.pdot(self.X(cx), self.X(cy), r)

    def pdot(self, x, y, r=None):
        self.fills.append((circle_d(x, y, self.dot_r if r is None else r), False, False))

    # filled
    def fd(self, d, evenodd=False, stroked=False):
        self.fills.append((d, evenodd, stroked))

    # output
    def svg(self):
        n = self.n
        parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{n}" height="{n}" viewBox="0 0 {n} {n}">']
        sw = fmt(self.w)
        stroke = f'stroke="#000" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round"'
        if self.strokes:
            parts.append(f'<path fill="none" {stroke} d="{"".join(self.strokes)}"/>')
        for d, evenodd, stroked in self.fills:
            rule = ' fill-rule="evenodd"' if evenodd else ""
            extra = f" {stroke}" if stroked else ""
            parts.append(f'<path fill="#000"{rule}{extra} d="{d}"/>')
        parts.append("</svg>")
        return "".join(parts)


ICONS = {}


def icon(*names):
    def deco(fn):
        for nm in names:
            ICONS[nm] = fn
        return fn
    return deco


def mid(a, b):
    return (a + b) / 2


# ---- the icons ------------------------------------------------------------------------------------------------------
# Master coordinates are stroke centers on the 24 grid; everything below is also correct at 12..20 px.

@icon("album")
def _(g):
    cx, cy, r = g.circle(12, 12, 9)
    g.pcircle(cx, cy, max(1, g.L(2.5)))


def arrow_h(g, right):
    y = g.X(12)
    x0, x1 = g.X(4), g.X(20)
    h = g.L(6)
    g.pline((x0, y), (x1, y))
    if right:
        g.pline((x1 - h, y - h), (x1, y), (x1 - h, y + h))
    else:
        g.pline((x0 + h, y - h), (x0, y), (x0 + h, y + h))


ICONS["arrow-back"] = lambda g: arrow_h(g, False)
ICONS["arrow-forward"] = lambda g: arrow_h(g, True)


@icon("arrow-up-right")
def _(g):
    a, b = g.X(7), g.X(17)
    g.pline((a, b), (b, a))
    g.pline((g.X(8), a), (b, a), (b, g.X(16)))


@icon("artist")
def _(g):
    cx, cy, r = g.circle(9.5, 8, 3.5)
    shoulders(g, cx, cy + r, g.L(6.5), g.X(20.5))
    # a person with a note: the stem and flag stroked, the head a dot
    stem = g.X(19.5)
    top, foot = g.X(3), g.X(10)
    g.pline((stem, foot), (stem, top), (stem + max(1, g.L(2)), top + max(1, g.L(1.5))))
    hr = g.dot_r * 1.25
    g.pdot(stem - hr + g.hw, foot + 0.25, hr)


def shoulders(g, cx, head_bottom, half_w, base_y):
    """The bust under a head: an arc from (cx - half_w, base_y) to (cx + half_w, base_y), clear of the head."""
    top = head_bottom + max(2 * g.w, g.U(2))
    sag = max(1.0, base_y - top)
    R = (half_w * half_w + sag * sag) / (2 * sag)
    large = 1 if sag > half_w else 0
    g.pd(f"M{fmt(cx - half_w)} {fmt(base_y)}A{fmt(R)} {fmt(R)} 0 {large} 1 {fmt(cx + half_w)} {fmt(base_y)}")
    return base_y - sag


@icon("cache")
def _(g):
    L, R = g.X(4), g.X(20)
    cx, rx = mid(L, R), (R - L) / 2
    ry = max(1, g.L(3))
    top = g.X(3.5)
    yt = top + ry
    ym = g.X(12)
    yb = max(g.X(17.5), ym + ry)
    g.pd(f"M{fmt(L)} {fmt(yt)}A{fmt(rx)} {fmt(ry)} 0 1 1 {fmt(R)} {fmt(yt)}A{fmt(rx)} {fmt(ry)} 0 1 1 {fmt(L)} {fmt(yt)}Z")
    g.pd(f"M{fmt(L)} {fmt(ym)}A{fmt(rx)} {fmt(ry)} 0 0 0 {fmt(R)} {fmt(ym)}")
    g.pd(f"M{fmt(L)} {fmt(yt)}V{fmt(yb)}A{fmt(rx)} {fmt(ry)} 0 0 0 {fmt(R)} {fmt(yb)}V{fmt(yt)}")


@icon("calendar")
def _(g):
    x0, x1, y0, y1 = g.X(4), g.X(20), g.X(5), g.X(20)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    yd = g.X(9.5)
    g.pline((x0, yd), (x1, yd))
    t0, t1 = g.X(3), g.X(7)
    for xr in (g.X(8), g.X(16)):
        g.pline((xr, t0), (xr, t1))


def check_pts(g, vx, vy, short, long):
    """A check mark with 45-degree legs from its vertex (pixels)."""
    a, b = g.L(short), g.L(long)
    return [(vx - a, vy - a), (vx, vy), (vx + b, vy - b)]


@icon("check")
def _(g):
    g.pline(*check_pts(g, g.X(9.25), g.X(17.25), 4.75, 10.25))


def chevron(g, direction):
    a, b = g.X(6), g.X(18)
    h = (b - a) / 2
    c = mid(a, b)
    if direction == "down":
        y = g.X(9)
        g.pline((a, y), (c, y + h), (b, y))
    elif direction == "up":
        y = g.X(15)
        g.pline((a, y), (c, y - h), (b, y))
    elif direction == "right":
        x = g.X(9)
        g.pline((x, a), (x + h, c), (x, b))
    else:
        x = g.X(15)
        g.pline((x, a), (x - h, c), (x, b))


for _d in ("down", "up", "left", "right"):
    ICONS[f"chevron-{_d}"] = (lambda d: lambda g: chevron(g, d))(_d)


@icon("clock")
def _(g):
    cx, cy, r = g.circle(12, 12, 9)
    g.pline((cx, g.X(7)), (cx, cy), (cx + g.L(3.5), cy + g.L(2)))


@icon("close")
def _(g):
    a, b = g.X(6), g.X(18)
    g.pline((a, a), (b, b))
    g.pline((b, a), (a, b))


@icon("collapse")
def _(g):
    g.poly((20, 10), (14, 10), (14, 4))
    g.line(14, 10, 21, 3)
    g.poly((4, 14), (10, 14), (10, 20))
    g.line(10, 14, 3, 21)


@icon("expand")
def _(g):
    g.poly((14, 4), (20, 4), (20, 10))
    g.line(20, 4, 13, 11)
    g.poly((10, 20), (4, 20), (4, 14))
    g.line(4, 20, 11, 13)


@icon("download")
def _(g):
    x = g.X(12)
    top, tip = g.X(4), g.X(15)
    h = g.L(4.5)
    g.pline((x, top), (x, tip))
    g.pline((x - h, tip - h), (x, tip), (x + h, tip - h))
    hb = g.L(7)
    yb = g.X(20)
    g.pline((x - hb, yb), (x + hb, yb))


@icon("downloaded")
def _(g):
    cx, cy, r = g.circle_at(12, 12, 9)
    body = circle_d(cx, cy, r + g.hw)
    check = check_pts(g, cx - g.L(1.25), cy + g.L(3.25), 2.75, 5.5)
    g.fd(body + stroke_outline(check, g.hw), evenodd=True)


@icon("drag-handle")
def _(g):
    for x in (9, 15):
        for y in (6, 12, 18):
            g.dot(x, y, free=True)


@icon("edit")
def _(g):
    g.poly((4, 20), (8.5, 20), (20, 8.5), (15.5, 4), (4, 15.5), close=True)
    g.line(13.5, 6, 18, 10.5)


@icon("equalizer")
def _(g):
    # The playing indicator at rest: four pill bars, as ui::paintEqualizer draws them.
    n = g.n
    bar = max(2, rnd(n / 7))
    gap = max(1, min(bar, (n - 4 * bar) // 3))
    total = 4 * bar + 3 * gap
    x = (n - total) // 2
    max_h = rnd(n * 0.8)
    min_h = max(2, rnd(n * 0.2))
    bottom = (n + max_h) // 2
    for lv in (0.55, 1.0, 0.7, 0.35):
        h = rnd(min_h + (max_h - min_h) * lv)
        g.fd(rrect_d(x, bottom - h, x + bar, bottom, bar / 2))
        x += bar + gap


@icon("sliders")
def _(g):
    # Audio settings: three faders, each knob (a ring) at a different height, the track broken around it.
    top, bot = g.X(4), g.X(20)
    rk = max(1, g.L(2))
    gap = rk + g.hw + 1 + g.hw
    for x, knob in zip(g.rows(6, 18, 3), (14.5, 8, 12)):
        y = g.X(knob)
        if y - gap > top:
            g.pline((x, top), (x, y - gap))
        if y + gap < bot:
            g.pline((x, y + gap), (x, bot))
        g.pcircle(x, y, rk)


@icon("error")
def _(g):
    cx, cy, r = g.circle(12, 12, 9)
    k = max(1, min(g.L(3), r - 2.5))
    g.pline((cx - k, cy - k), (cx + k, cy + k))
    g.pline((cx + k, cy - k), (cx - k, cy + k))


@icon("explicit")
def _(g):
    # A content flag: the filled tag with the E cut out.
    x0, x1 = g.E(3.25), g.E(20.75)
    body = rrect_d(x0, x0, x1, x1, max(1.0, g.U(2)))
    t = max(1, rnd(g.w))
    ex0, ex1 = g.E(9), g.E(15)
    ey0, ey1 = g.E(7), g.E(17)
    if (ey1 - ey0 - 3 * t) % 2:
        ey1 -= 1
    ym = ey0 + (ey1 - ey0 - t) // 2
    xm = ex1 - max(1, rnd((ex1 - ex0) * 0.2))
    e = [(ex0, ey0), (ex1, ey0), (ex1, ey0 + t), (ex0 + t, ey0 + t), (ex0 + t, ym), (xm, ym), (xm, ym + t),
         (ex0 + t, ym + t), (ex0 + t, ey1 - t), (ex1, ey1 - t), (ex1, ey1), (ex0, ey1)]
    g.fd(body + "M" + "L".join(f"{fmt(x)} {fmt(y)}" for x, y in e) + "Z", evenodd=True)


def box_arrow(g):
    x0, y0, x1, y1 = g.X(4), g.X(5), g.X(19), g.X(20)
    g.pline((g.X(10), y0), (x0, y0), (x0, y1), (x1, y1), (x1, g.X(14)), r=g.U(1.5))
    ax, ay = g.X(20), g.X(4)
    g.pline((g.X(14), ay), (ax, ay), (ax, g.X(10)))
    d = ax - g.X(11)
    g.pline((ax, ay), (ax - d, ay + d))


ICONS["external-link"] = box_arrow
ICONS["pop-out"] = box_arrow


@icon("filter")
def _(g):
    g.poly((3.5, 5.5), (20.5, 5.5), (14, 13.5), (14, 19.5), (10, 17.5), (10, 13.5), close=True, r=0.75)


@icon("folder")
def _(g):
    g.poly((3.5, 5.5), (9.5, 5.5), (11.5, 7.5), (20.5, 7.5), (20.5, 18.5), (3.5, 18.5), close=True, r=1.5)


@icon("grid")
def _(g):
    a, b = g.X(4), g.X(20)
    total = b - a
    gap = max(2, g.L(3))
    if (total - gap) % 2:
        gap += 1
    s = (total - gap) / 2
    r = g.U(1)
    for x in (a, b - s):
        for y in (a, b - s):
            g.prect(x, y, x + s, y + s, r)


HEART = "M12 20.5S4 15.1 4 9.5A4.6 4.6 0 0 1 12 7A4.6 4.6 0 0 1 20 9.5C20 15.1 12 20.5 12 20.5Z"


@icon("heart")
def _(g):
    g.path(HEART)


@icon("heart-filled")
def _(g):
    g.path(HEART)
    d = g.strokes.pop()
    g.fd(d, stroked=True)


@icon("home")
def _(g):
    x0, x1 = g.X(4), g.X(20)
    ax = mid(x0, x1)
    eave, apex, y1 = g.X(10.5), g.X(3.5), g.X(20)
    g.pline((x0, y1), (x0, eave), (ax, apex), (x1, eave), (x1, y1), close=True, r=g.U(1))
    dh = g.half(ax, 2)
    dt = g.X(14.5)
    g.pline((ax - dh, y1), (ax - dh, dt), (ax + dh, dt), (ax + dh, y1))


@icon("info")
def _(g):
    cx, cy, r = g.circle(12, 12, 9)
    dy = g.X(7.5)
    top = snapc(dy + g.dot_r + g.w + 0.5)
    g.pline((cx, top), (cx, max(top, g.X(16.5))))
    g.pdot(cx, dy)


@icon("keyboard")
def _(g):
    x0, x1, y0, y1 = g.X(2.5), g.X(21.5), g.X(5.5), g.X(18.5)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    rows = g.rows(9.5, 14.5, 2)
    fc = mid(x0, x1)
    tick = max(0.5, g.w * 0.62)
    if g.small:   # three keys over a space bar
        sp = 2 if fc % 1 else 3
        kx = [fc - sp, fc, fc + sp] if fc % 1 else [fc - 1.5, fc + 1.5]
        for x in kx:
            g.pdot(x, rows[0], tick)
        h = g.half(fc, 2.5)
        g.pline((fc - h, rows[1]), (fc + h, rows[1]))
        return
    sp = max(2, rnd(g.U(3.67)))
    if (fc % 1 == 0) != (sp % 2 == 1):   # keep the four keys on pixel centers, symmetric about the frame
        sp += 1
    kx = [fc + k * sp for k in (-1.5, -0.5, 0.5, 1.5)]
    for x in kx:
        g.pdot(x, rows[0], tick)
    g.pdot(kx[0], rows[1], tick)
    g.pdot(kx[3], rows[1], tick)
    g.pline((kx[1], rows[1]), (kx[2], rows[1]))


@icon("library")
def _(g):
    t, b = g.X(3.5), g.X(20.5)
    for x in (g.X(4), g.X(9)):
        g.pline((x, t), (x, b))
    g.pline((g.X(13.5), g.X(5.5)), (g.X(19), b))


@icon("link")
def _(g):
    yc = g.X(12)
    r = max(2, g.L(4.5))
    yt, yb = yc - r, yc + r
    la, ra = g.X(7), g.X(17)
    li, ri = g.X(10), g.X(14)
    g.pd(f"M{fmt(li)} {fmt(yt)}H{fmt(la)}A{fmt(r)} {fmt(r)} 0 0 0 {fmt(la)} {fmt(yb)}H{fmt(li)}")
    g.pd(f"M{fmt(ri)} {fmt(yt)}H{fmt(ra)}A{fmt(r)} {fmt(r)} 0 0 1 {fmt(ra)} {fmt(yb)}H{fmt(ri)}")
    g.pline((g.X(8.5), yc), (g.X(15.5), yc))


@icon("list")
def _(g):
    x0, x1 = g.X(9), g.X(20.5)
    bx = g.X(4.5)
    for y in g.rows(6, 18, 3):
        g.pline((x0, y), (x1, y))
        g.pdot(bx, y)


@icon("lock")
def _(g):
    x0, x1, y0, y1 = g.X(5), g.X(19), g.X(10.5), g.X(20.5)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    a, b = g.X(8), g.X(16)
    r = (b - a) / 2
    ys = g.X(7.5)
    g.pd(f"M{fmt(a)} {fmt(y0)}V{fmt(ys)}A{fmt(r)} {fmt(r)} 0 0 1 {fmt(b)} {fmt(ys)}V{fmt(y0)}")


@icon("logout")
def _(g):
    g.poly((10, 4.5), (4.5, 4.5), (4.5, 19.5), (10, 19.5), r=1.5)
    y = g.X(12)
    tip = g.X(20)
    h = g.L(4)
    g.pline((g.X(9.5), y), (tip, y))
    g.pline((tip - h, y - h), (tip, y), (tip - h, y + h))


@icon("lyrics")
def _(g):
    x0 = g.X(3.5)
    for y, x1 in zip(g.rows(7, 17, 3), (20.5, 14.5, 17.5)):
        g.pline((x0, y), (g.X(x1), y))


@icon("maximize")
def _(g):
    g.prect(g.X(4.5), g.X(4.5), g.X(19.5), g.X(19.5), g.U(1))


@icon("minimize")
def _(g):
    y = g.X(12)
    g.pline((g.X(4.5), y), (g.X(19.5), y))


@icon("restore")
def _(g):
    a, b = g.X(4.5), g.X(19.5)
    o = max(2, g.L(3))
    s = (b - a) - o
    fx0, fy1 = a, b
    fx1, fy0 = fx0 + s, fy1 - s
    r = g.U(1)
    g.prect(fx0, fy0, fx1, fy1, r)
    g.pline((fx0 + o, fy0), (fx0 + o, fy0 - o), (fx1 + o, fy0 - o), (fx1 + o, fy1 - o), (fx1, fy1 - o), r=r)


@icon("mini-player")
def _(g):
    x0, x1, y0, y1 = g.X(2.5), g.X(21.5), g.X(4.5), g.X(19.5)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    ix1, iy1 = x1 - g.L(3), y1 - g.L(3)
    g.prect(ix1 - g.L(6), iy1 - g.L(4.5), ix1, iy1, g.U(0.75))


@icon("mini-square")
def _(g):
    a, b = g.X(4.5), g.X(19.5)
    g.prect(a, a, b, b, g.U(1.5))
    y = b - g.L(4)
    g.pline((a + g.L(3), y), (b - g.L(3), y))


@icon("mini-strip")
def _(g):
    x0, x1, y0, y1 = g.X(2.5), g.X(21.5), g.X(7.5), g.X(16.5)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    xd = g.X(8)
    g.pline((xd, y0), (xd, y1))
    yc = mid(y0, y1)
    if yc % 1 == 0:
        yc += 0.5
    g.pline((g.X(11), yc), (g.X(15.5), yc))


@icon("mini-wide")
def _(g):
    x0, x1, y0, y1 = g.X(2.5), g.X(21.5), g.X(5.5), g.X(18.5)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    m = g.L(2.5)
    g.prect(x0 + m, y0 + m, g.X(12), y1 - m, g.U(0.75))
    ra, rb = g.rows(10.5, 13.5, 2)
    g.pline((g.X(14.5), ra), (g.X(18.5), ra))
    g.pline((g.X(14.5), rb), (g.X(17), rb))


def plus_minus(g, plus):
    c = g.X(12)
    h = g.L(7)
    g.pline((c - h, c), (c + h, c))
    if plus:
        g.pline((c, c - h), (c, c + h))


ICONS["plus"] = lambda g: plus_minus(g, True)
ICONS["minus"] = lambda g: plus_minus(g, False)


def dots3(g, vertical):
    for k in (-1, 0, 1):
        g.dot(12, 12 + 7 * k, free=True) if vertical else g.dot(12 + 7 * k, 12, free=True)


ICONS["more"] = lambda g: dots3(g, False)
ICONS["more-vertical"] = lambda g: dots3(g, True)


@icon("music-note")
def _(g):
    s1, s2 = g.X(9), g.X(19)
    r = max(1, g.L(2.5))
    y1, y2 = g.X(18.5), g.X(16)
    g.pline((s1, y1), (s1, g.X(5.5)), (s2, g.X(3.5)), (s2, y2))
    g.pcircle(s1 - r, y1, r)
    g.pcircle(s2 - r, y2, r)


def speaker(g):
    cy = g.X(12)
    hb, hc = g.half(cy, 2.5), g.half(cy, 6.5)
    x0, x1, xc = g.X(3), g.X(6.5), g.X(10.5)
    g.pline((x0, cy - hb), (x1, cy - hb), (xc, cy - hc), (xc, cy + hc), (x1, cy + hb), (x0, cy + hb), close=True,
            r=g.U(0.75))
    return xc, cy


def volume(g, level):
    xc, cy = speaker(g)
    for r in (4.5, 7.5, 10.5)[:level]:
        g.parc(g.U(10), cy, g.U(r), -40, 40)


for _lv in range(4):
    ICONS[f"volume-{_lv}"] = (lambda lv: lambda g: volume(g, lv))(_lv)


@icon("mute")
def _(g):
    xc, cy = speaker(g)
    k = g.L(2.5)
    x = g.X(17.5)
    g.pline((x - k, cy - k), (x + k, cy + k))
    g.pline((x + k, cy - k), (x - k, cy + k))


def skip(g, forward):
    top, bot = g.X(6), g.X(18)
    c = mid(top, bot)
    if forward:
        x0, ax, bx = g.X(6), g.X(15), g.X(18)
    else:
        x0, ax, bx = g.X(18), g.X(9), g.X(6)
    g.pline((x0, top), (ax, c), (x0, bot), close=True)
    g.pline((bx, top), (bx, bot))


ICONS["next"] = lambda g: skip(g, True)
ICONS["prev"] = lambda g: skip(g, False)


WIFI_C = (12, 18.5)
WIFI_R = (4.5, 8.75, 13)


def wifi_arcs(g, gap_line=None):
    cx, cy = g.U(WIFI_C[0]), g.U(WIFI_C[1])
    for rm in (WIFI_R[0] * 1.25, WIFI_R[2]) if g.small else WIFI_R:
        r = g.U(rm)
        a0, a1 = -135.0, -45.0
        spans = [(a0, a1)]
        if gap_line:
            # cut the arc where the slash crosses it (the slash runs along y - x = const)
            (px0, py0), _ = gap_line
            k = g.U(py0 - px0)          # line: y = x + k
            v = (cx - cy + k) / (math.sqrt(2) * r)
            if abs(v) < 1:
                hit = 45 + math.degrees(math.asin(v)) + 180
                hit = (hit + 180) % 360 - 180
                gap = math.degrees((g.w * 1.5 + 0.75) / r)
                spans = [(a0, hit - gap), (hit + gap, a1)]
        for s0, s1 in spans:
            if s1 - s0 > 4:
                g.parc(cx, cy, r, s0, s1)
    return cx, cy


@icon("wifi")
def _(g):
    cx, cy = wifi_arcs(g)
    g.pdot(snapc(cx), snapc(cy))


@icon("offline")
def _(g):
    line = ((3.5, 3.5), (20.5, 20.5))
    cx, cy = wifi_arcs(g, line)
    g.pdot(snapc(cx), snapc(cy))
    g.line(3.5, 3.5, 20.5, 20.5)


@icon("palette")
def _(g):
    g.path("M12 3A9 9 0 1 0 12 21C13.7 21 14 20 13.5 19S13 17 14.5 16S21 17 21 12A9 9 0 0 0 12 3Z")
    for x, y in ((7.75, 11), (10, 7.25), (14.5, 7.25)):
        g.dot(x, y, free=True)


def play_tri(g):
    x0, ax = g.X(7.5), g.X(18.5)
    top, bot = g.X(5.5), g.X(18.5)
    g.pline((x0, top), (ax, mid(top, bot)), (x0, bot), close=True)


@icon("play")
def _(g):
    play_tri(g)


@icon("pause")
def _(g):
    # As ui::PlayButton's outline pause: two bars, 4.5 wide, 3 apart.
    a, b = g.X(6), g.X(18)
    top, bot = g.X(5.5), g.X(18.5)
    total = b - a
    gap = max(2, g.L(3))
    bar = (total - gap) // 2
    gap = total - 2 * bar
    r = g.U(1)
    g.prect(a, top, a + bar, bot, r)
    g.prect(b - bar, top, b, bot, r)


@icon("stop")
def _(g):
    a, b = g.X(6), g.X(18)
    g.prect(a, a, b, b, g.U(1.5))


@icon("pin")
def _(g):
    cx = g.X(12)
    w1, w2, w3 = g.half(cx, 3), g.half(cx, 2), g.half(cx, 5)
    y0, y1, y2, y3 = g.X(3.5), g.X(9.5), g.X(12.5), g.X(14)
    g.pline((cx - w1, y0), (cx + w1, y0), (cx + w2, y1), (cx + w3, y2), (cx + w3, y3), (cx - w3, y3), (cx - w3, y2),
            (cx - w2, y1), close=True)
    g.pline((cx, y3), (cx, g.X(20.5)))


@icon("playlist")
def _(g):
    x0 = g.X(3.5)
    rows = g.rows(6, 16, 3)
    for y, x1 in zip(rows, (15, 15, 10)):
        g.pline((x0, y), (g.X(x1), y))
    stem = g.X(19)
    r = max(1, g.L(2.5))
    hy = g.X(17.5)
    g.pline((stem, hy), (stem, g.X(6.5)), (g.X(21.5), g.X(8)))
    g.pcircle(stem - r, hy, r)


@icon("podcast")
def _(g):
    cx = g.X(12)
    hw = g.half(cx, 2.5)
    y0, y1 = g.X(3.5), g.X(13)
    g.prect(cx - hw, y0, cx + hw, y1, hw)
    yc = g.X(10.5)
    R = g.half(cx, 5.5)
    g.pd(f"M{fmt(cx - R)} {fmt(yc)}A{fmt(R)} {fmt(R)} 0 0 0 {fmt(cx + R)} {fmt(yc)}")
    yb = g.X(20.5)
    g.pline((cx, yc + R), (cx, yb))
    hb = g.half(cx, 3)
    g.pline((cx - hb, yb), (cx + hb, yb))


@icon("queue")
def _(g):
    x0 = g.X(3.5)
    rows = g.rows(6, 18, 3)
    for y, x1 in zip(rows, (20.5, 20.5, 12.5)):
        g.pline((x0, y), (g.X(x1), y))
    yc = rows[2]
    h = g.half(yc, 2.75)
    tx0, tx1 = g.X(16), g.X(20.5)
    g.pline((tx0, yc - h), (tx1, yc), (tx0, yc + h), close=True)


@icon("radio")
def _(g):
    x0, x1, y0, y1 = g.X(3.5), g.X(20.5), g.X(9.5), g.X(20)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    g.pline((g.X(7), y0), (g.X(17), g.X(4.5)))
    yc = mid(y0, y1)
    if g.n < 20:   # the speaker as a dot: a ring that small fills in
        g.pdot(g.X(8.5) if yc % 1 else rnd(g.U(8.5)), yc, g.dot_r * 1.25)
    else:
        rs = g.half(yc, 2.25)   # crisp top and bottom
        g.pcircle(g.X(8.5) if rs % 1 == 0 else rnd(g.U(8.5)), yc, rs)
    k = g.half(yc, 1.5)
    for y in (yc - k, yc + k):
        g.pline((g.X(13.5), y), (g.X(17.5), y))


@icon("refresh")
def _(g):
    cx, cy, r = g.circle_at(12, 12, 8)
    g.parc(cx, cy, r, 0, 315)
    tx, ty = g.X(20), g.X(9)
    g.pline((tx, g.X(4)), (tx, ty), (g.X(15), ty))


def repeat(g, one):
    yt, yb = g.X(6), g.X(18)
    xl, xr = g.X(4), g.X(20)
    h = g.L(3)
    R = g.L(4)
    g.pd(f"M{fmt(xr)} {fmt(yt)}H{fmt(xl + R)}A{fmt(R)} {fmt(R)} 0 0 0 {fmt(xl)} {fmt(yt + R)}V{fmt(max(yt + R, g.X(11)))}")
    g.pline((xr - h, yt - h), (xr, yt), (xr - h, yt + h))
    g.pd(f"M{fmt(xl)} {fmt(yb)}H{fmt(xr - R)}A{fmt(R)} {fmt(R)} 0 0 0 {fmt(xr)} {fmt(yb - R)}V{fmt(min(yb - R, g.X(13)))}")
    g.pline((xl + h, yb - h), (xl, yb), (xl + h, yb + h))
    if one:
        x = g.X(12.5)
        t, b = g.X(9), g.X(15)
        g.pline((x - g.L(1.5), t + g.L(1)), (x, t), (x, b))


ICONS["repeat"] = lambda g: repeat(g, False)
ICONS["repeat-one"] = lambda g: repeat(g, True)


@icon("search")
def _(g):
    cx, cy, r = g.circle(11, 11, 6.5)
    k = r / math.sqrt(2)
    e = g.X(20.5)
    g.pline((cx + k, cy + k), (e, e))


@icon("settings")
def _(g):
    # A dial with eight ticks (the hi-fi knob of the design language).
    cx, cy, r = g.circle(12, 12, 3)
    r1, r2 = g.U(6.5), g.U(9.5)
    for k in range(8):
        a = math.radians(k * 45)
        if k % 2 == 0:
            dx, dy = round(math.cos(a)), round(math.sin(a))
            p0 = (cx + dx * rnd(r1), cy + dy * rnd(r1))
            p1 = (cx + dx * rnd(r2), cy + dy * rnd(r2))
        else:
            p0 = (cx + math.cos(a) * r1, cy + math.sin(a) * r1)
            p1 = (cx + math.cos(a) * r2, cy + math.sin(a) * r2)
        g.pline(p0, p1)


@icon("share")
def _(g):
    cx = g.X(12)
    tip, end = g.X(3.5), g.X(15)
    h = g.L(4.5)
    g.pline((cx, tip), (cx, end))
    g.pline((cx - h, tip + h), (cx, tip), (cx + h, tip + h))
    hb = g.L(7)
    y0, y1 = g.X(12), g.X(20.5)
    g.pline((cx - hb, y0), (cx - hb, y1), (cx + hb, y1), (cx + hb, y0), r=g.U(1.5))


def shuffle_lines(g):
    yt, yb = g.X(6), g.X(18)
    x0, b0, b1, x1 = g.X(4), g.X(7), g.X(17), g.X(20)
    h = g.L(2)
    g.pline((x0, yt), (b0, yt), (b1, yb), (x1, yb))
    # the other track, broken where it crosses the first
    dx, dy = b1 - b0, yb - yt
    L = math.hypot(dx, dy)
    gap = (g.w + 1.25) / L
    g.pline((x0, yb), (b0, yb), (b0 + dx * (0.5 - gap), yb - dy * (0.5 - gap)))
    g.pline((b0 + dx * (0.5 + gap), yb - dy * (0.5 + gap)), (b1, yt), (x1, yt))
    for y in (yt, yb):
        g.pline((x1 - h, y - h), (x1, y), (x1 - h, y + h))


@icon("shuffle")
def _(g):
    shuffle_lines(g)


@icon("shuffle-smart")
def _(g):
    shuffle_lines(g)
    cx, cy = g.X(11), g.X(4.5)
    k = 1 if g.small else max(1, g.L(3))
    g.pline((cx, max(0.5, cy - k)), (cx, cy + k))
    g.pline((cx - k, cy), (cx + k, cy))


@icon("sidebar")
def _(g):
    x0, x1, y0, y1 = g.X(2.5), g.X(21.5), g.X(4.5), g.X(19.5)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    xd = g.X(9.5)
    g.pline((xd, y0), (xd, y1))
    a = x0 + g.L(3)
    b = max(a, xd - g.L(3))
    for y in g.rows(8.5, 11.5, 2):
        g.pline((a, y), (b, y))


@icon("sort")
def _(g):
    for y, (a, b) in zip(g.rows(7, 17, 3), ((4, 20), (7, 17), (10, 14))):
        g.pline((g.X(a), y), (g.X(b), y))


def star4(cx, cy, R, k):
    """A four-point star with concave sides (quadratic curves through the center's diagonals)."""
    return (f"M{fmt(cx)} {fmt(cy - R)}Q{fmt(cx + k)} {fmt(cy - k)} {fmt(cx + R)} {fmt(cy)}"
            f"Q{fmt(cx + k)} {fmt(cy + k)} {fmt(cx)} {fmt(cy + R)}Q{fmt(cx - k)} {fmt(cy + k)} {fmt(cx - R)} {fmt(cy)}"
            f"Q{fmt(cx - k)} {fmt(cy - k)} {fmt(cx)} {fmt(cy - R)}Z")


@icon("sparkle")
def _(g):
    if g.small:   # one star: the small one would merge with it
        c, R = g.X(12), g.L(8.5)
        g.pd(star4(c, c, R, R * 0.16))
        return
    cx, cy, R = g.X(10), g.X(10), g.L(7)
    g.pd(star4(cx, cy, R, R * 0.16))
    sx, sy, r = g.X(18.5), g.X(18), max(1.5, g.L(3.25))
    g.pd(star4(sx, sy, r, r * 0.16))


@icon("spinner")
def _(g):
    cx, cy, r = g.circle_at(12, 12, 9)
    g.parc(cx, cy, r, -90, 0)


@icon("spotify-link")
def _(g):
    g.circle(12, 12, 9)
    g.path("M7.25 9.75C10.5 8.75 14 9 16.75 10.5M7.75 13C10.5 12.25 13.25 12.5 15.75 13.75M8.5 16C10.5 15.5 12.75 15.6 14.75 16.6")


@icon("stats")
def _(g):
    x0, y1 = g.X(4), g.X(20)
    d = g.L(4)
    xe = x0 + 4 * d
    g.pline((x0, g.X(4)), (x0, y1), (xe, y1))
    base = g.X(16)
    for k, top in enumerate((12, 8, 10, 6)):
        x = x0 + (k + 1) * d
        g.pline((x, base), (x, min(base, g.X(top))))


@icon("swap-match")
def _(g):
    y1, y2 = g.X(8), g.X(16)
    h = g.L(3.5)
    t1, t2 = g.X(18), g.X(6)
    g.pline((g.X(4), y1), (t1, y1))
    g.pline((t1 - h, y1 - h), (t1, y1), (t1 - h, y1 + h))
    g.pline((g.X(20), y2), (t2, y2))
    g.pline((t2 + h, y2 - h), (t2, y2), (t2 + h, y2 + h))


@icon("trash")
def _(g):
    l0, l1 = g.X(4), g.X(20)
    cx = mid(l0, l1)
    lid = g.X(7)
    g.pline((l0, lid), (l1, lid))
    hh = g.half(cx, 2.5)
    g.pline((cx - hh, lid), (cx - hh, g.X(4)), (cx + hh, g.X(4)), (cx + hh, lid), r=g.U(0.75))
    top_w, bot_w = g.half(cx, 6), g.half(cx, 5)
    yb = g.X(20.5)
    g.pline((cx - top_w, lid), (cx - bot_w, yb), (cx + bot_w, yb), (cx + top_w, lid), r=g.U(1.5))
    li = g.half(cx, 2)
    for x in (cx - li, cx + li):
        g.pline((x, g.X(11)), (x, g.X(16.5)))


@icon("user")
def _(g):
    cx, cy, r = g.circle(12, 7.5, 4)
    shoulders(g, cx, cy + r, g.L(7.5), g.X(20.5))


@icon("users")
def _(g):
    cx, cy, r = g.circle(9, 8.5, 3.5)
    W, base = g.L(6.5), g.X(20)
    W = min(W, base - (cy + r) - g.w - 1)   # the bust's top clear of the head
    g.parc(cx, base, W, 180, 360)
    # the second person, behind the first: the outer half of a head and of a bust
    g.parc(cx + g.L(5.5), cy, r, -70, 70)
    g.parc(cx + g.L(6), base, W, -62, 0)


@icon("video")
def _(g):
    x0, x1, y0, y1 = g.X(2.75), g.X(15), g.X(6.5), g.X(17.5)
    g.prect(x0, y0, x1, y1, g.U(1.5))
    yc = mid(y0, y1)
    a, b = g.half(yc, 1.75), g.half(yc, 4.75)
    xr = g.X(21)
    g.pline((x1, yc - a), (xr, yc - b), (xr, yc + b), (x1, yc + a), r=g.U(0.75))


@icon("warning")
def _(g):
    l, r = g.X(3), g.X(21)
    cx = mid(l, r)
    ay, by = g.X(3.5), g.X(19.5)
    g.pline((cx, ay), (r, by), (l, by), close=True, r=g.U(1.5))
    sx = cx if cx % 1 else cx + 0.5
    dy = below(by - g.hw - 1 - g.dot_r)
    bottom = below(dy - g.dot_r - 1 - g.hw)
    top = min(bottom, g.X(9.5))
    g.pline((sx, top), (sx, bottom))
    g.pdot(sx, dy)


@icon("youtube-source")
def _(g):
    x0, x1, y0, y1 = g.X(2.5), g.X(21.5), g.X(6), g.X(18)
    g.prect(x0, y0, x1, y1, g.U(3))
    yc = mid(y0, y1)
    h = g.half(yc, 2.5)
    tx0, tx1 = g.X(10), g.X(15)
    g.pline((tx0, yc - h), (tx1, yc), (tx0, yc + h), close=True)


# ---- output ---------------------------------------------------------------------------------------------------------

def render(name, n):
    g = G(n)
    ICONS[name](g)
    return g.svg()


def build():
    out = {}
    for n in GRIDS:
        for name in sorted(ICONS):
            out[f"icons/{n}/{name}.svg"] = render(name, n)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 when assets/icons differs")
    ap.add_argument("--design", type=pathlib.Path, help="also write <DIR>/icons (the design package)")
    args = ap.parse_args()
    files = build()
    targets = [ROOT / "assets"] + ([args.design] if args.design else [])
    if args.check:
        bad = [p for p, text in files.items()
               if not (ROOT / "assets" / p).exists() or (ROOT / "assets" / p).read_text(encoding="utf-8") != text]
        extra = [str(p.relative_to(ROOT / "assets")).replace("\\", "/") for n in GRIDS
                 for p in (ROOT / "assets" / "icons" / str(n)).glob("*.svg")
                 if str(p.relative_to(ROOT / "assets")).replace("\\", "/") not in files]
        for p in bad:
            print("differs:", p)
        for p in extra:
            print("not drawn here:", p)
        sys.exit(1 if bad or extra else 0)
    for base in targets:
        for n in GRIDS:
            d = base / "icons" / str(n)
            d.mkdir(parents=True, exist_ok=True)
            for old in d.glob("*.svg"):
                if f"icons/{n}/{old.name}" not in files:
                    old.unlink()
        for p, text in files.items():
            f = base / p
            if not f.exists() or f.read_text(encoding="utf-8") != text:
                f.write_text(text, encoding="utf-8", newline="\n")
    print(f"{len(ICONS)} icons x {len(GRIDS)} grids -> {', '.join(str(t) for t in targets)}")


if __name__ == "__main__":
    main()
