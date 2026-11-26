#include "video/AudioAlign.h"

#include <algorithm>
#include <cmath>

namespace st::video {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kB = AudioFeatures::kBands;
constexpr int kCoarse = 4;          // fine frames per coarse frame (the first correlation pass: 40 ms)
constexpr double kQuietDb = -30;    // a frame this far under the median is quiet (silence, a fade)

size_t pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place radix-2 FFT over separate real / imaginary arrays (plain doubles: fast in Debug builds too), with a cached
// twiddle table (inverse unscaled).
class Fft {
public:
    explicit Fft(size_t n) : n_(n), wr_(n / 2), wi_(n / 2), rev_(n) {
        for (size_t i = 0; i < n / 2; ++i) {
            const double a = -2 * kPi * static_cast<double>(i) / static_cast<double>(n);
            wr_[i] = std::cos(a);
            wi_[i] = std::sin(a);
        }
        int bits = 0;
        while ((size_t(1) << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
            rev_[i] = r;
        }
    }
    void run(double* re, double* im, bool inverse) const {
        const size_t* rev = rev_.data();
        for (size_t i = 0; i < n_; ++i)
            if (i < rev[i]) {
                std::swap(re[i], re[rev[i]]);
                std::swap(im[i], im[rev[i]]);
            }
        const double sign = inverse ? -1.0 : 1.0;
        const double* wr = wr_.data();
        const double* wi = wi_.data();
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t step = n_ / len, half = len / 2;
            for (size_t i = 0; i < n_; i += len) {
                double* ar = re + i;
                double* ai = im + i;
                for (size_t j = 0, k = 0; j < half; ++j, k += step) {
                    const double c = wr[k], s = sign * wi[k];
                    const double xr = ar[j + half], xi = ai[j + half];
                    const double vr = xr * c - xi * s, vi = xr * s + xi * c;
                    ar[j + half] = ar[j] - vr;
                    ai[j + half] = ai[j] - vi;
                    ar[j] += vr;
                    ai[j] += vi;
                }
            }
        }
    }
    size_t size() const { return n_; }

private:
    size_t n_;
    std::vector<double> wr_, wi_;
    std::vector<size_t> rev_;
};

// A complex signal as two arrays.
struct Spectrum {
    std::vector<double> re, im;
    explicit Spectrum(size_t n = 0) : re(n), im(n) {}
    void clear() {
        std::fill(re.begin(), re.end(), 0.0);
        std::fill(im.begin(), im.end(), 0.0);
    }
};

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    const auto mid = v.begin() + static_cast<ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Prefix sums of x and x^2 (n + 1 entries each).
struct Prefix {
    std::vector<double> s1, s2;
    explicit Prefix(const std::vector<float>& x) : s1(x.size() + 1), s2(x.size() + 1) {
        for (size_t i = 0; i < x.size(); ++i) {
            s1[i + 1] = s1[i] + x[i];
            s2[i + 1] = s2[i] + static_cast<double>(x[i]) * x[i];
        }
    }
    // Sum of squared deviations from the mean over [p, p + n).
    double var(size_t p, size_t n) const {
        const double a = s1[p + n] - s1[p], b = s2[p + n] - s2[p];
        return std::max(0.0, b - a * a / static_cast<double>(n));
    }
};

// Band-summed variance at every position (window of n frames) and the floor used against near-silent video parts.
struct VarianceTrack {
    std::vector<double> var;   // per start position
    double median = 0;
};
VarianceTrack varianceTrack(const std::vector<Prefix>& pre, size_t frames, size_t n) {
    VarianceTrack t;
    if (frames < n) return t;
    t.var.resize(frames - n + 1);
    for (size_t p = 0; p + n <= frames; ++p) {
        double v = 0;
        for (const auto& b : pre) v += b.var(p, n);
        t.var[p] = v;
    }
    std::vector<double> copy(t.var.begin(), t.var.end());
    t.median = medianOf(std::move(copy));
    return t;
}

struct Candidate {
    double lag = 0;   // seconds, video - song
    double r = 0;
};

struct Window {
    double start = 0, center = 0, end = 0;   // song seconds
    int T0 = 0;                              // first fine frame
    bool silent = false;
    std::vector<int> coarse;                 // the coarse pass's peaks (lags in fine frames)
    std::vector<Candidate> cands;
};

// A fitted run of windows: lag(t) = a + b * (t - t0).
struct Line {
    double a = 0, b = 0, t0 = 0;
    double lagAt(double t) const { return a + b * (t - t0); }
};

struct Point {
    double t, lag, r;
    double alt = 0;   // the window's best other lag's r (how distinct this one is)
};

Line fit(const std::vector<Point>& pts, size_t from, size_t to) {
    double sw = 0, st = 0, sl = 0;
    for (size_t i = from; i < to; ++i) {
        sw += pts[i].r;
        st += pts[i].r * pts[i].t;
        sl += pts[i].r * pts[i].lag;
    }
    Line L;
    if (sw <= 0) return L;
    L.t0 = st / sw;
    const double lm = sl / sw;
    double num = 0, den = 0;
    for (size_t i = from; i < to; ++i) {
        num += pts[i].r * (pts[i].t - L.t0) * (pts[i].lag - lm);
        den += pts[i].r * (pts[i].t - L.t0) * (pts[i].t - L.t0);
    }
    L.a = lm;
    // A speed difference only from a long run (30 s) and when it adds up to more than 25 ms over it: over a few windows,
    // or as a hair of a slope, the lags' scatter would read as one.
    const double span = pts[to - 1].t - pts[from].t;
    L.b = den > 1e-9 && span >= 30 ? num / den : 0;
    if (std::fabs(L.b) * span < 0.025) L.b = 0;
    return L;
}

double maxResidual(const std::vector<Point>& pts, size_t from, size_t to, const Line& L) {
    double m = 0;
    for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(pts[i].lag - L.lagAt(pts[i].t)));
    return m;
}

struct Run {
    std::vector<Point> pts;
    Line line;
    double windowStart = 0, windowEnd = 0;   // the first window's start, the last one's end (song seconds)
    double score = 0;
    double margin() const {
        double m = 0;
        for (const auto& p : pts) m += p.r - p.alt;
        return pts.empty() ? 0 : m / static_cast<double>(pts.size());
    }
};

constexpr size_t kMinRunWindows = 3;

// Splits pts[from, to) into runs that each fit a line within the residual and slope limits (at most `budget` splits);
// returns false (and nothing) for a part that drifts.
bool fitRuns(const std::vector<Point>& pts, size_t from, size_t to, const AlignOptions& o, int budget, std::vector<std::pair<size_t, size_t>>& out) {
    if (to - from < kMinRunWindows) return false;
    const Line L = fit(pts, from, to);
    if (maxResidual(pts, from, to, L) <= o.maxResidualSec && std::fabs(L.b) <= o.maxSlopeDev) {
        out.emplace_back(from, to);
        return true;
    }
    if (budget <= 0) return false;
    // The split that leaves the best-fitting halves.
    size_t bestAt = 0;
    double bestCost = 1e18;
    for (size_t k = from + kMinRunWindows; k + kMinRunWindows <= to; ++k) {
        const double c = maxResidual(pts, from, k, fit(pts, from, k)) + maxResidual(pts, k, to, fit(pts, k, to));
        if (c < bestCost) {
            bestCost = c;
            bestAt = k;
        }
    }
    if (!bestAt) return false;
    std::vector<std::pair<size_t, size_t>> left, right;
    const bool l = fitRuns(pts, from, bestAt, o, budget - 1, left);
    const bool r = fitRuns(pts, bestAt, to, o, budget - 1, right);
    if (!l && !r) return false;
    out.insert(out.end(), left.begin(), left.end());
    out.insert(out.end(), right.begin(), right.end());
    return true;
}

// Song / video features z-normalized per band (frame scores for change points, the local correlation).
struct Normed {
    std::vector<float> z[kB];
};
Normed normed(const AudioFeatures& f) {
    Normed n;
    for (int b = 0; b < kB; ++b) {
        const auto& x = f.flux[b];
        double m = 0, s = 0;
        for (float v : x) m += v;
        m /= std::max<size_t>(1, x.size());
        for (float v : x) s += (v - m) * (v - m);
        s = std::sqrt(s / std::max<size_t>(1, x.size()));
        n.z[b].resize(x.size());
        for (size_t i = 0; i < x.size(); ++i) n.z[b][i] = s > 1e-9 ? static_cast<float>((x[i] - m) / s) : 0.f;
    }
    return n;
}

// Pearson correlation of the song around each frame of [lo, hi) (a window of `half` frames on either side) with the
// video along `seg`'s line; -1 where the line leaves the video.
std::vector<double> localCorrelation(const AudioFeatures& song, const AudioFeatures& video, const AlignSegment& seg, int lo, int hi,
                                     int half) {
    const int n = hi - lo;
    std::vector<double> out(static_cast<size_t>(std::max(0, n)), -1.0);
    if (n <= 0) return out;
    const int from = std::max(0, lo - half), to = std::min(song.frames, hi + half);
    const int len = to - from;
    if (len <= 0) return out;
    // Per band prefix sums of a, a^2, v, v^2, a*v along the line (v = 0 and invalid where the line leaves the video).
    std::vector<double> sums[kB][5];
    std::vector<int> valid(static_cast<size_t>(len) + 1, 0);
    for (int t = 0; t < len; ++t) {
        const double vs = seg.videoAt((from + t) / static_cast<double>(AudioFeatures::kFps)) * AudioFeatures::kFps;
        const long m = std::lround(vs);
        valid[static_cast<size_t>(t) + 1] = valid[static_cast<size_t>(t)] + (m >= 0 && m < video.frames ? 1 : 0);
    }
    for (int b = 0; b < kB; ++b) {
        for (auto& s : sums[b]) s.assign(static_cast<size_t>(len) + 1, 0.0);
        for (int t = 0; t < len; ++t) {
            const double vs = seg.videoAt((from + t) / static_cast<double>(AudioFeatures::kFps)) * AudioFeatures::kFps;
            const long m = std::lround(vs);
            const double a = song.flux[b][static_cast<size_t>(from + t)];
            const double v = m >= 0 && m < video.frames ? video.flux[b][static_cast<size_t>(m)] : 0.0;
            const size_t i = static_cast<size_t>(t);
            sums[b][0][i + 1] = sums[b][0][i] + a;
            sums[b][1][i + 1] = sums[b][1][i] + a * a;
            sums[b][2][i + 1] = sums[b][2][i] + v;
            sums[b][3][i + 1] = sums[b][3][i] + v * v;
            sums[b][4][i + 1] = sums[b][4][i] + a * v;
        }
    }
    for (int t = lo; t < hi; ++t) {
        const int s = std::max(from, t - half) - from, e = std::min(to, t + half) - from;
        const double cnt = e - s;
        if (cnt < half || valid[static_cast<size_t>(e)] - valid[static_cast<size_t>(s)] < e - s) continue;
        double num = 0, va = 0, vv = 0;
        for (int b = 0; b < kB; ++b) {
            auto d = [&](int k) { return sums[b][k][static_cast<size_t>(e)] - sums[b][k][static_cast<size_t>(s)]; };
            num += d(4) - d(0) * d(2) / cnt;
            va += d(1) - d(0) * d(0) / cnt;
            vv += d(3) - d(2) * d(2) / cnt;
        }
        // A near-silent stretch of the video can't vouch for anything: its variance counts as at least a tenth of the
        // song's here.
        vv = std::max(vv, 0.1 * va);
        out[static_cast<size_t>(t - lo)] = va > 1e-9 ? num / std::sqrt(va * vv) : 0.0;
    }
    return out;
}

// Per-frame agreement of the song with the video along a line (z-normalized product, summed over the bands).
double frameScore(const Normed& s, const Normed& v, int videoFrames, const AlignSegment& seg, int t) {
    const long m = std::lround(seg.videoAt(t / static_cast<double>(AudioFeatures::kFps)) * AudioFeatures::kFps);
    if (m < 0 || m >= videoFrames) return -1;
    double sum = 0;
    for (int b = 0; b < kB; ++b) sum += s.z[b][static_cast<size_t>(t)] * v.z[b][static_cast<size_t>(m)];
    return sum;
}

} // namespace

// ---- features --------------------------------------------------------------------------------------------------------

AudioFeatures analyzeAudio(const float* x, size_t count, int sampleRate) {
    AudioFeatures f;
    if (sampleRate < 1000 || count == 0) return f;
    const size_t hop = static_cast<size_t>(sampleRate / AudioFeatures::kFps);
    const size_t n = pow2(static_cast<size_t>(sampleRate * 0.04));   // 512 at 12 kHz: 43 ms
    const int frames = static_cast<int>(count / hop);
    f.frames = frames;
    static const double kEdges[kB + 1] = {0, 150, 400, 1000, 2500, 6000};
    int binLo[kB], binHi[kB];
    for (int b = 0; b < kB; ++b) {
        binLo[b] = std::max(1, static_cast<int>(std::ceil(kEdges[b] * static_cast<double>(n) / sampleRate)));
        binHi[b] = std::min(static_cast<int>(n / 2), static_cast<int>(std::ceil(kEdges[b + 1] * static_cast<double>(n) / sampleRate)));
    }
    // Real FFT of n points through a complex one of n / 2 (even samples real, odd imaginary).
    const size_t h = n / 2;
    const Fft fft(h);
    std::vector<double> window(n);
    for (size_t i = 0; i < n; ++i) window[i] = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n));
    Spectrum z(h);
    std::vector<double> twr(h), twi(h);
    for (size_t k = 0; k < h; ++k) {
        twr[k] = std::cos(-2 * kPi * static_cast<double>(k) / static_cast<double>(n));
        twi[k] = std::sin(-2 * kPi * static_cast<double>(k) / static_cast<double>(n));
    }
    std::vector<double> energy(static_cast<size_t>(frames) * kB, 0.0);
    std::vector<double> total(static_cast<size_t>(frames), 0.0);
    for (int t = 0; t < frames; ++t) {
        const size_t s0 = static_cast<size_t>(t) * hop;
        for (size_t k = 0; k < h; ++k) {
            const size_t i0 = s0 + 2 * k, i1 = i0 + 1;
            z.re[k] = i0 < count ? x[i0] * window[2 * k] : 0.0;
            z.im[k] = i1 < count ? x[i1] * window[2 * k + 1] : 0.0;
        }
        fft.run(z.re.data(), z.im.data(), false);
        for (int b = 0; b < kB; ++b) {
            double e = 0;
            for (int k = binLo[b]; k < binHi[b]; ++k) {
                // X[k] = (Z[k] + conj Z[h-k]) / 2 - i/2 * W^k * (Z[k] - conj Z[h-k])
                const size_t kk = static_cast<size_t>(k), kc = (h - kk) % h;
                const double ar = z.re[kk], ai = z.im[kk], br = z.re[kc], bi = -z.im[kc];
                const double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
                const double dr = ar - br, di = ai - bi;
                const double wr = twr[kk], wi = twi[kk];
                const double pr = wr * dr - wi * di, pi = wr * di + wi * dr;   // W^k * (Z - conj Z)
                const double Xr = er + 0.5 * pi, Xi = ei - 0.5 * pr;           // - i/2 * p
                e += Xr * Xr + Xi * Xi;
            }
            energy[static_cast<size_t>(t) * kB + b] = e;
            total[static_cast<size_t>(t)] += e;
        }
    }
    std::vector<double> sounding;
    for (double e : total)
        if (e > 1e-12) sounding.push_back(e);
    const double med = std::max(1e-12, medianOf(std::move(sounding)));
    for (int b = 0; b < kB; ++b) f.flux[b].assign(static_cast<size_t>(frames), 0.f);
    f.levelDb.resize(static_cast<size_t>(frames));
    double prev[kB] = {};
    for (int t = 0; t < frames; ++t) {
        for (int b = 0; b < kB; ++b) {
            const double L = std::log1p(1000.0 * energy[static_cast<size_t>(t) * kB + b] / med);
            if (t > 0) f.flux[b][static_cast<size_t>(t)] = static_cast<float>(std::max(0.0, L - prev[b]));
            prev[b] = L;
        }
        f.levelDb[static_cast<size_t>(t)] = static_cast<float>(10 * std::log10((total[static_cast<size_t>(t)] + 1e-20) / med));
    }
    // Every band at the same weight (unit deviation): the kick's band would otherwise outweigh the rest, and its
    // regular beat says little about where in the song a window is; the upper bands (voice, hats) say much more.
    for (int b = 0; b < kB; ++b) {
        auto& x = f.flux[b];
        double m = 0, s = 0;
        for (float v : x) m += v;
        m /= std::max<size_t>(1, x.size());
        for (float v : x) s += (v - m) * (v - m);
        s = std::sqrt(s / std::max<size_t>(1, x.size()));
        if (s > 1e-9)
            for (auto& v : x) v = static_cast<float>(v / s);
    }
    return f;
}

// ---- alignment -------------------------------------------------------------------------------------------------------

AlignResult alignAudio(const AudioFeatures& song, const AudioFeatures& video, const AlignOptions& o) {
    AlignResult res;
    const int fps = AudioFeatures::kFps;
    const int W = static_cast<int>(o.windowSec * fps), H = static_cast<int>(o.hopSec * fps);
    if (song.frames < W + 2 * H || video.frames < W) {
        res.verdict = "too short";
        return res;
    }
    // Coarse features (40 ms): the first pass over every lag.
    const int WC = W / kCoarse, HC = H / kCoarse;
    auto coarse = [](const AudioFeatures& f) {
        std::vector<std::vector<float>> c(kB);
        const size_t n = static_cast<size_t>(f.frames / kCoarse);
        for (int b = 0; b < kB; ++b) {
            c[static_cast<size_t>(b)].resize(n);
            for (size_t i = 0; i < n; ++i) {
                float s = 0;
                for (int k = 0; k < kCoarse; ++k) s += f.flux[b][i * kCoarse + static_cast<size_t>(k)];
                c[static_cast<size_t>(b)][i] = s / kCoarse;
            }
        }
        return c;
    };
    const auto sc = coarse(song), vc = coarse(video);
    const size_t nsc = sc[0].size(), nvc = vc[0].size();
    std::vector<Prefix> vcPre, vfPre;
    for (int b = 0; b < kB; ++b) {
        vcPre.emplace_back(vc[static_cast<size_t>(b)]);
        vfPre.emplace_back(video.flux[b]);
    }
    const VarianceTrack vcVar = varianceTrack(vcPre, nvc, static_cast<size_t>(WC));
    const VarianceTrack vfVar = varianceTrack(vfPre, static_cast<size_t>(video.frames), static_cast<size_t>(W));
    const Fft fft(pow2(nvc + static_cast<size_t>(WC)));
    const size_t M = fft.size();
    // The video's bands go through the FFT two at a time (one as the real, one as the imaginary part) and are split
    // after: X = (Z[k] + conj Z[-k]) / 2, Y = (Z[k] - conj Z[-k]) / 2i.
    std::vector<Spectrum> V(kB, Spectrum(M));
    for (int b = 0; b < kB; b += 2) {
        Spectrum z(M);
        for (size_t i = 0; i < nvc; ++i) {
            z.re[i] = vc[static_cast<size_t>(b)][i];
            if (b + 1 < kB) z.im[i] = vc[static_cast<size_t>(b + 1)][i];
        }
        fft.run(z.re.data(), z.im.data(), false);
        for (size_t k = 0; k < M; ++k) {
            const size_t kc = (M - k) % M;
            const double ar = z.re[k], ai = z.im[k], br = z.re[kc], bi = -z.im[kc];
            V[static_cast<size_t>(b)].re[k] = 0.5 * (ar + br);
            V[static_cast<size_t>(b)].im[k] = 0.5 * (ai + bi);
            if (b + 1 < kB) {   // (a - b) / 2i = (ai - bi) / 2 - i (ar - br) / 2
                V[static_cast<size_t>(b + 1)].re[k] = 0.5 * (ai - bi);
                V[static_cast<size_t>(b + 1)].im[k] = -0.5 * (ar - br);
            }
        }
    }
    const int maxLagC = static_cast<int>(o.maxLagSec * fps / kCoarse);

    // 1. Each window's candidate lags.
    std::vector<Window> windows;
    Spectrum acc(M), z(M);
    for (int t0 = 0; static_cast<size_t>(t0 + WC) <= nsc; t0 += HC) {
        Window w;
        w.start = static_cast<double>(t0 * kCoarse) / fps;
        w.center = w.start + o.windowSec / 2;
        w.end = w.start + o.windowSec;
        w.T0 = t0 * kCoarse;
        // The mean-removed window (bands two at a time through the FFT), its energy; acc = sum of V * conj(A).
        double na2 = 0;
        acc.clear();
        for (int b = 0; b < kB; b += 2) {
            z.clear();
            for (int j = 0; j < 2 && b + j < kB; ++j) {
                const auto& s = sc[static_cast<size_t>(b + j)];
                double m = 0;
                for (int i = 0; i < WC; ++i) m += s[static_cast<size_t>(t0 + i)];
                m /= WC;
                auto& dst = j == 0 ? z.re : z.im;
                for (int i = 0; i < WC; ++i) {
                    const double d = s[static_cast<size_t>(t0 + i)] - m;
                    dst[static_cast<size_t>(i)] = d;
                    na2 += d * d;
                }
            }
            fft.run(z.re.data(), z.im.data(), false);
            const Spectrum& v0 = V[static_cast<size_t>(b)];
            const Spectrum* v1 = b + 1 < kB ? &V[static_cast<size_t>(b + 1)] : nullptr;
            for (size_t k = 0; k < M; ++k) {
                const size_t kc = (M - k) % M;
                const double ar = z.re[k], ai = z.im[k], br = z.re[kc], bi = -z.im[kc];
                // This band pair's spectra A0, A1, then V * conj(A).
                const double a0r = 0.5 * (ar + br), a0i = 0.5 * (ai + bi);
                acc.re[k] += v0.re[k] * a0r + v0.im[k] * a0i;
                acc.im[k] += v0.im[k] * a0r - v0.re[k] * a0i;
                if (v1) {
                    const double a1r = 0.5 * (ai - bi), a1i = -0.5 * (ar - br);
                    acc.re[k] += v1->re[k] * a1r + v1->im[k] * a1i;
                    acc.im[k] += v1->im[k] * a1r - v1->re[k] * a1i;
                }
            }
        }
        if (na2 < 1e-6) {
            w.silent = true;
            windows.push_back(std::move(w));
            continue;
        }
        fft.run(acc.re.data(), acc.im.data(), true);
        const double floor = std::max(0.1 * vcVar.median, 0.1 * na2);
        const int pLo = std::max(0, t0 - maxLagC), pHi = std::min(static_cast<int>(nvc) - WC, t0 + maxLagC);
        std::vector<std::pair<double, int>> peaks;
        std::vector<double> r(static_cast<size_t>(std::max(0, pHi - pLo + 1)));
        for (int p = pLo; p <= pHi; ++p)
            r[static_cast<size_t>(p - pLo)] = acc.re[static_cast<size_t>(p)] / static_cast<double>(M) /
                                              std::sqrt(na2 * std::max(vcVar.var[static_cast<size_t>(p)], floor));
        for (int i = 0; i < static_cast<int>(r.size()); ++i) {
            const double v = r[static_cast<size_t>(i)];
            if (v < 0.15) continue;
            if (i > 0 && r[static_cast<size_t>(i - 1)] > v) continue;
            if (i + 1 < static_cast<int>(r.size()) && r[static_cast<size_t>(i + 1)] >= v) continue;
            peaks.emplace_back(v, pLo + i);
        }
        std::sort(peaks.begin(), peaks.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        std::vector<int> kept;
        for (const auto& [v, p] : peaks) {
            if (kept.size() >= 5) break;
            if (std::any_of(kept.begin(), kept.end(), [&](int q) { return std::abs(q - p) <= 12; })) continue;
            kept.push_back(p);
        }
        for (int p : kept) w.coarse.push_back(p * kCoarse - t0 * kCoarse);   // fine frames of lag
        windows.push_back(std::move(w));
    }
    const size_t nw = windows.size();

    // Fine pass (10 ms): Pearson over a window's fine frames at video position q, then a parabolic peak around the
    // best of +-8 frames.
    std::vector<float> fwin[kB];
    for (auto& f : fwin) f.resize(static_cast<size_t>(W));
    auto refine = [&](size_t wi, const std::vector<int>& lags) {
        Window& w = windows[wi];
        double fa2 = 0;
        for (int b = 0; b < kB; ++b) {
            double m = 0;
            for (int i = 0; i < W; ++i) m += song.flux[b][static_cast<size_t>(w.T0 + i)];
            m /= W;
            for (int i = 0; i < W; ++i) {
                fwin[b][static_cast<size_t>(i)] = static_cast<float>(song.flux[b][static_cast<size_t>(w.T0 + i)] - m);
                fa2 += static_cast<double>(fwin[b][static_cast<size_t>(i)]) * fwin[b][static_cast<size_t>(i)];
            }
        }
        const double ffloor = std::max(0.1 * vfVar.median, 0.1 * fa2);
        auto fineR = [&](int q) {
            if (q < 0 || q + W > video.frames || fa2 <= 0) return -1.0;
            double num = 0;
            for (int b = 0; b < kB; ++b) {
                const float* v = video.flux[b].data() + q;
                const float* s = fwin[b].data();
                double acc1 = 0;
                for (int i = 0; i < W; ++i) acc1 += static_cast<double>(s[i]) * v[i];
                num += acc1;
            }
            return num / std::sqrt(fa2 * std::max(vfVar.var[static_cast<size_t>(q)], ffloor));
        };
        for (int lagFrames : lags) {
            const int center = w.T0 + lagFrames;
            int best = center;
            double bestR = -2;
            double rs[17];
            for (int d = -8; d <= 8; ++d) {
                rs[d + 8] = fineR(center + d);
                if (rs[d + 8] > bestR) {
                    bestR = rs[d + 8];
                    best = center + d;
                }
            }
            if (bestR < 0.15) continue;
            double frac = 0;
            const int bi = best - center + 8;
            if (bi > 0 && bi < 16) {
                const double y0 = rs[bi - 1], y1 = rs[bi], y2 = rs[bi + 1];
                const double den = y0 - 2 * y1 + y2;
                if (den < 0) frac = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
            }
            const double lag = (best + frac - w.T0) / static_cast<double>(fps);
            if (std::any_of(w.cands.begin(), w.cands.end(), [&](const Candidate& c) { return std::fabs(c.lag - lag) < 0.03; })) continue;
            w.cands.push_back({lag, bestR});
        }
    };
    for (size_t wi = 0; wi < nw; ++wi)
        if (!windows[wi].silent) refine(wi, windows[wi].coarse);
    // A lag a neighbour has is also tried here (forwards, then backwards): in a song of repeated identical parts the
    // true lag is not always among a window's own best peaks, and the dynamic programme can only follow a lag that is
    // a candidate in every window of the run.
    auto propagate = [&](size_t from, size_t to) {
        if (windows[to].silent || windows[from].silent) return;
        std::vector<int> lags;
        for (const auto& c : windows[from].cands) {
            if (c.r < 0.3) continue;
            if (std::any_of(windows[to].cands.begin(), windows[to].cands.end(), [&](const Candidate& d) { return std::fabs(d.lag - c.lag) < 0.03; }))
                continue;
            lags.push_back(static_cast<int>(std::lround(c.lag * fps)));
        }
        refine(to, lags);
        auto& cs = windows[to].cands;   // the strongest dozen
        std::sort(cs.begin(), cs.end(), [](const Candidate& x, const Candidate& y) { return x.r > y.r; });
        if (cs.size() > 12) cs.resize(12);
    };
    for (size_t wi = 1; wi < nw; ++wi) propagate(wi - 1, wi);
    for (size_t wi = nw - 1; wi-- > 0;) propagate(wi + 1, wi);

    // 2. One lag (or none) per window: dynamic programme.
    const double tol = 0.06;   // 60 ms between neighbouring windows: same lag (the coarse grid, a speed difference)
    std::vector<std::vector<double>> score(nw);
    std::vector<std::vector<int>> back(nw);
    for (size_t w = 0; w < nw; ++w) {
        const auto& cw = windows[w].cands;
        const size_t ns = cw.size() + 1;   // last = none
        score[w].assign(ns, -1e18);
        back[w].assign(ns, -1);
        for (size_t s = 0; s < ns; ++s) {
            const bool none = s == cw.size();
            if (windows[w].silent && !none) continue;
            const double emit = none ? o.matchR : cw[s].r;
            if (w == 0) {
                score[w][s] = emit;
                continue;
            }
            const auto& cp = windows[w - 1].cands;
            for (size_t q = 0; q < score[w - 1].size(); ++q) {
                if (score[w - 1][q] <= -1e17) continue;
                const bool pnone = q == cp.size();
                double tr = 0;
                if (none != pnone) tr = -o.changePenalty / 2;
                else if (!none && std::fabs(cw[s].lag - cp[q].lag) > tol) tr = -o.changePenalty;
                const double v = score[w - 1][q] + tr + emit;
                if (v > score[w][s]) {
                    score[w][s] = v;
                    back[w][s] = static_cast<int>(q);
                }
            }
        }
    }
    std::vector<int> path(nw, -1);
    {
        int s = static_cast<int>(std::max_element(score[nw - 1].begin(), score[nw - 1].end()) - score[nw - 1].begin());
        for (size_t w = nw; w-- > 0;) {
            path[w] = s;
            s = back[w][static_cast<size_t>(s)];
        }
    }
    for (size_t w = 0; w < nw; ++w) {
        AlignResult::WindowPick p;
        p.center = windows[w].center;
        p.silent = windows[w].silent;
        const auto& cw = windows[w].cands;
        p.matched = path[w] >= 0 && static_cast<size_t>(path[w]) < cw.size();
        if (p.matched) {
            p.lag = cw[static_cast<size_t>(path[w])].lag;
            p.r = cw[static_cast<size_t>(path[w])].r;
        } else if (!cw.empty()) {
            const auto best = std::max_element(cw.begin(), cw.end(), [](const auto& x, const auto& y) { return x.r < y.r; });
            p.lag = best->lag;
            p.r = best->r;
        }
        res.windows.push_back(p);
    }

    // 3. Runs of windows on one lag, fitted to lines.
    std::vector<Run> runs;
    bool drift = false;
    {
        std::vector<Point> pts;
        std::vector<size_t> idx;
        auto flush = [&] {
            std::vector<std::pair<size_t, size_t>> parts;
            if (pts.size() >= kMinRunWindows && !fitRuns(pts, 0, pts.size(), o, 3, parts)) drift = true;
            for (const auto& [from, to] : parts) {
                Run r;
                r.pts.assign(pts.begin() + static_cast<ptrdiff_t>(from), pts.begin() + static_cast<ptrdiff_t>(to));
                r.line = fit(pts, from, to);
                r.windowStart = windows[idx[from]].start;
                r.windowEnd = windows[idx[to - 1]].end;
                for (const auto& p : r.pts) r.score += p.r;
                r.score /= static_cast<double>(r.pts.size());
                runs.push_back(std::move(r));
            }
            pts.clear();
            idx.clear();
        };
        for (size_t w = 0; w < nw; ++w) {
            const auto& cw = windows[w].cands;
            const int s = path[w];
            if (s < 0 || static_cast<size_t>(s) >= cw.size()) {
                flush();
                continue;
            }
            const Candidate& c = cw[static_cast<size_t>(s)];
            if (!pts.empty() && std::fabs(c.lag - pts.back().lag) > tol) flush();
            double alt = 0;
            for (const auto& other : cw)
                if (std::fabs(other.lag - c.lag) > 0.25) alt = std::max(alt, other.r);
            pts.push_back({windows[w].center, c.lag, std::max(0.01, c.r), alt});
            idx.push_back(w);
        }
        flush();
    }
    // Runs on one line (a quiet or masked part between them) are one stretch of the video's timeline.
    for (size_t i = 1; i < runs.size();) {
        Run& A = runs[i - 1];
        const Run& B = runs[i];
        const double t = B.pts.front().t;
        if (std::fabs(A.line.lagAt(t) - B.line.lagAt(t)) <= 0.04 && std::fabs(A.line.b - B.line.b) <= 0.002) {
            std::vector<Point> all = A.pts;
            all.insert(all.end(), B.pts.begin(), B.pts.end());
            const Line L = fit(all, 0, all.size());
            if (maxResidual(all, 0, all.size(), L) <= o.maxResidualSec) {
                A.pts = std::move(all);
                A.line = L;
                A.windowEnd = B.windowEnd;
                A.score = 0;
                for (const auto& p : A.pts) A.score += p.r;
                A.score /= static_cast<double>(A.pts.size());
                runs.erase(runs.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }
    // A run is worth something when it fits well, and a short one only when its lag stands out from the window's
    // other candidates: a regular beat lines up at many lags for a few windows (a long run of the true lag through
    // repeated identical parts is fine: it is long).
    std::erase_if(runs, [&](const Run& r) {
        if (r.score < o.minSegmentScore) return true;
        return r.pts.back().t - r.pts.front().t < 30 && r.margin() < 0.08;
    });
    if (runs.empty()) {
        res.verdict = drift ? "drift" : "no match";
        return res;
    }
    {
        double m = 0, n = 0;
        for (const auto& r : runs) {
            m += r.margin() * static_cast<double>(r.pts.size());
            n += static_cast<double>(r.pts.size());
        }
        res.margin = n > 0 ? m / n : 0;
    }

    // 4. Segments with their ends found to the frame.
    std::vector<AlignSegment> segs;
    for (const auto& r : runs) {
        AlignSegment s;
        s.slope = 1 + r.line.b;
        s.audioStart = r.pts.front().t;   // the matched core: window centers; widened below
        s.audioEnd = r.pts.back().t;
        s.videoStart = s.audioStart + r.line.lagAt(s.audioStart);
        s.score = r.score;
        segs.push_back(s);
    }
    const Normed zs = normed(song), zv = normed(video);
    const int half = 75;          // 1.5 s local correlation
    const double localMin = 0.3;
    auto toFrame = [&](double sec) { return std::clamp(static_cast<int>(std::lround(sec * fps)), 0, song.frames); };
    // How far a segment's line keeps matching beyond its core, frame by frame, down to `limit` (outwards).
    auto reach = [&](const AlignSegment& s, bool forward, int limit) {
        const int from = toFrame(forward ? s.audioEnd : s.audioStart);
        const int lo = forward ? from : limit, hi = forward ? limit : from;
        const auto r = localCorrelation(song, video, s, lo, hi, half);
        int edge = from;
        if (forward) {
            for (int t = from; t < hi && r[static_cast<size_t>(t - lo)] >= localMin; ++t) edge = t + 1;
        } else {
            for (int t = from - 1; t >= lo && r[static_cast<size_t>(t - lo)] >= localMin; --t) edge = t;
        }
        return edge;
    };
    {
        std::vector<std::pair<int, int>> edges(segs.size());
        for (size_t i = 0; i < segs.size(); ++i) {   // each grows up to its neighbours' cores (they may overlap then)
            const int prevCore = i > 0 ? toFrame(segs[i - 1].audioEnd) : 0;
            const int nextCore = i + 1 < segs.size() ? toFrame(segs[i + 1].audioStart) : song.frames;
            edges[i] = {reach(segs[i], false, prevCore), reach(segs[i], true, nextCore)};
        }
        for (size_t i = 0; i < segs.size(); ++i) {
            segs[i].audioStart = edges[i].first / static_cast<double>(fps);
            segs[i].audioEnd = edges[i].second / static_cast<double>(fps);
            segs[i].videoStart = segs[i].audioStart + runs[i].line.lagAt(segs[i].audioStart);   // the line, on the new start
        }
    }
    // Neighbouring segments that grew into each other: the change point is where the next line fits better.
    for (size_t i = 1; i < segs.size(); ++i) {
        AlignSegment& A = segs[i - 1];
        AlignSegment& B = segs[i];
        if (A.audioEnd <= B.audioStart) continue;
        const int lo = toFrame(B.audioStart), hi = toFrame(A.audioEnd);
        double run = 0, best = 0;
        int at = lo;
        // Cumulative (score along A) - (score along B): the boundary sits after the frame where it peaks.
        for (int t = lo; t < hi; ++t) {
            run += frameScore(zs, zv, video.frames, A, t) - frameScore(zs, zv, video.frames, B, t);
            if (run > best) {
                best = run;
                at = t + 1;
            }
        }
        const double cut = at / static_cast<double>(fps);
        A.audioEnd = cut;
        const Line& LB = runs[i].line;
        B.audioStart = cut;
        B.videoStart = cut + LB.lagAt(cut);
    }
    // Neighbours a few frames apart in offset (a micro-edit) with a short unsure stretch between them meet in its
    // middle: the video's timeline is as good as continuous there.
    for (size_t i = 1; i < segs.size(); ++i) {
        AlignSegment& A = segs[i - 1];
        AlignSegment& B = segs[i];
        const double gap = B.audioStart - A.audioEnd;
        if (gap <= 0 || gap > 4) continue;
        const double mid = A.audioEnd + gap / 2;
        if (std::fabs(B.offsetAt(mid) - A.offsetAt(mid)) >= o.smallJumpSec) continue;
        A.audioEnd = mid;
        B.videoStart = B.videoAt(mid);
        B.audioStart = mid;
    }
    // The song's quiet start / end: the first / last segment reaches over it while the video has frames there.
    {
        AlignSegment& F = segs.front();
        int t = toFrame(F.audioStart);
        while (t > 0 && song.levelDb[static_cast<size_t>(t - 1)] < kQuietDb && F.videoAt((t - 1) / static_cast<double>(fps)) >= 0) --t;
        const double ns = t / static_cast<double>(fps);
        F.videoStart = F.videoAt(ns);
        F.audioStart = ns;
        AlignSegment& Lst = segs.back();
        t = toFrame(Lst.audioEnd);
        while (t < song.frames && song.levelDb[static_cast<size_t>(t)] < kQuietDb &&
               Lst.videoAt((t + 1) / static_cast<double>(fps)) < video.seconds())
            ++t;
        Lst.audioEnd = t / static_cast<double>(fps);
    }
    std::erase_if(segs, [](const AlignSegment& s) { return s.audioEnd - s.audioStart < 4.0; });
    if (segs.empty()) {
        res.verdict = "no match";
        return res;
    }

    // 5. Verdict.
    int first = 0, last = song.frames;
    while (first < song.frames && song.levelDb[static_cast<size_t>(first)] < kQuietDb) ++first;
    while (last > first && song.levelDb[static_cast<size_t>(last - 1)] < kQuietDb) --last;
    const double soundA = first / static_cast<double>(fps), soundB = last / static_cast<double>(fps);
    double covered = 0, scoreSum = 0, lenSum = 0;
    for (const auto& s : segs) {
        covered += std::max(0.0, std::min(s.audioEnd, soundB) - std::max(s.audioStart, soundA));
        scoreSum += s.score * (s.audioEnd - s.audioStart);
        lenSum += s.audioEnd - s.audioStart;
    }
    res.coverage = soundB > soundA ? covered / (soundB - soundA) : 0;
    res.score = lenSum > 0 ? scoreSum / lenSum : 0;
    res.segments = segs;
    bool ordered = true;
    int jumps = 0;   // holds inside the song (a later start or an earlier end is no interruption)
    for (size_t i = 1; i < segs.size(); ++i) {
        const double prevEnd = segs[i - 1].videoAt(segs[i - 1].audioEnd), next = segs[i].videoAt(segs[i].audioStart);
        if (next < prevEnd - 0.5) ordered = false;
        if (segs[i].audioStart - segs[i - 1].audioEnd > 0.05 || std::fabs(next - prevEnd) >= o.smallJumpSec) ++jumps;
    }
    if (!ordered) res.verdict = "reordered";
    else if (res.margin < o.minMargin) res.verdict = "ambiguous";   // a beat grid lining up, not the recording
    else if (jumps > o.maxJumps) res.verdict = "too many parts";
    else if (res.coverage < o.minCoverage) res.verdict = drift ? "drift" : "too little";
    else {
        res.accepted = true;
        res.verdict = segs.size() == 1 ? "same edit" : "edited";
    }
    return res;
}

// ---- time map --------------------------------------------------------------------------------------------------------

TimeMap::TimeMap(std::vector<AlignSegment> segments, double holdSec, double smallJumpSec) {
    std::sort(segments.begin(), segments.end(), [](const auto& a, const auto& b) { return a.audioStart < b.audioStart; });
    for (auto& s : segments) {
        if (!segments_.empty()) {
            const auto& p = segments_.back();
            const double gap = s.audioStart - p.audioEnd;
            // A micro-edit (the next part sits a few frames off, right after): the picture just steps, no hold.
            if (gap <= 0.05 && std::fabs(s.videoAt(s.audioStart) - p.videoAt(p.audioEnd)) < smallJumpSec) {
                s.videoStart = s.videoAt(p.audioEnd);
                s.audioStart = p.audioEnd;
                segments_.push_back(s);
                continue;
            }
            // After a jump in the video's timeline the decoder needs a moment to get to the new place: a hold first.
            if (gap < holdSec) {
                const double cut = segments_.back().audioEnd + holdSec;
                if (cut >= s.audioEnd - 1.0) continue;
                s.videoStart = s.videoAt(cut);
                s.audioStart = cut;
            }
        }
        if (s.audioEnd > s.audioStart) segments_.push_back(s);
    }
}

TimeMap::Point TimeMap::at(double t) const {
    if (segments_.empty()) return {t, 1, true};
    for (const auto& s : segments_) {
        if (t < s.audioStart) return {s.videoStart, 0, false};   // holding: on the next segment's first frame
        if (t < s.audioEnd) return {s.videoAt(t), s.slope, true};
    }
    const auto& l = segments_.back();
    return {l.videoAt(l.audioEnd), 0, false};
}

} // namespace st::video
