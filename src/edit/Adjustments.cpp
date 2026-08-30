#include "edit/Adjustments.h"

#include "edit/HealTool.h"
#include "edit/ShapeTool.h"
#include "edit/TextTool.h"

#include <QPainter>
#include <QPainterPath>
#include <QHash>
#include <QRectF>
#include <QTransform>
#include <QFont>
#include <QFontMetrics>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

inline int clamp16(int v) { return v < 0 ? 0 : (v > 65535 ? 65535 : v); }
inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

QImage orient(const QImage &img, const Adjustments &adj) {
    QImage out = img;
    if (adj.rotationQuadrants % 4 != 0) {
        QTransform t;
        t.rotate(90.0 * (adj.rotationQuadrants % 4));
        out = out.transformed(t);
    }
    if (adj.flipH || adj.flipV)
        out = out.mirrored(adj.flipH, adj.flipV);
    return out;
}

// Levels/Curve control points are stored normalized to [0,1] (or 0-255 for
// the historical Levels black/white points, rescaled below), independent of
// the pipeline's working bit depth.
constexpr int kLutSize = 65536; // one entry per 16-bit level

// Build a 65536-entry LUT from monotonic control points (x,y in [0,1]) via
// linear interpolation. Returns identity if fewer than two points.
void buildCurveLut(const QVector<QPointF> &pts, std::vector<int> &lut) {
    lut.resize(kLutSize);
    for (int i = 0; i < kLutSize; ++i) lut[i] = i;
    if (pts.size() < 2) return;
    QVector<QPointF> p = pts;
    std::sort(p.begin(), p.end(),
              [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    for (int i = 0; i < kLutSize; ++i) {
        double x = i / double(kLutSize - 1);
        double y;
        if (x <= p.first().x()) y = p.first().y();
        else if (x >= p.last().x()) y = p.last().y();
        else {
            int k = 1;
            while (k < p.size() && p[k].x() < x) ++k;
            const QPointF &a = p[k - 1];
            const QPointF &b = p[k];
            double t = (x - a.x()) / std::max(1e-6, b.x() - a.x());
            y = a.y() + t * (b.y() - a.y());
        }
        lut[i] = clamp16(int(std::lround(y * (kLutSize - 1))));
    }
}

// Build a 65536-entry LUT for one Levels channel: clip to [inBlack,inWhite],
// apply the midtone gamma, then map into the [outBlack,outWhite] output
// range. `c`'s black/white points are stored in legacy 0-255 units, so scale
// by 257 (== 65535/255 exactly) into the working 16-bit range.
void buildLevelsLut(const LevelsChannel &c, std::vector<int> &lut) {
    lut.resize(kLutSize);
    constexpr double k8to16 = 257.0;
    const double inB = c.inBlack * k8to16;
    const double inW = c.inWhite * k8to16;
    const double span = std::max(1.0, inW - inB); // guard divide-by-zero
    const double invGamma = 1.0 / std::clamp(c.gamma, 0.01, 9.99);
    const double outB = c.outBlack * k8to16;
    const double outSpan = (c.outWhite - c.outBlack) * k8to16;
    for (int i = 0; i < kLutSize; ++i) {
        double v = (i - inB) / span;
        v = clampd(v, 0.0, 1.0);
        v = std::pow(v, invGamma);
        lut[i] = clamp16(int(std::lround(outB + v * outSpan)));
    }
}

inline double smoothstep01(double t) {
    t = clampd(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Targeted color-range tolerance: pixels farther than this (redmean colour
// distance, 8-bit scale — see colorDist) from the picked target get no
// adjustment; closer pixels fade in with smoothstep falloff.
constexpr double kColorRangeTolerance = 60.0;

// One precomputed targeted color-range entry: target in 8-bit scale (matching
// the redmean distance units), delta in the 16-bit working range.
struct ColorRangeParam {
    double tr, tg, tb;
    int channel;
    double delta;
};

// Full tone/colour transform — shared by the global pass (applyAdjustments)
// and per-layer content (applyLayerContent): WB gains, curve, levels,
// contrast/brightness, highlights/shadows, saturation/vibrance.
struct ToneParams {
    double contrastFactor, brightness;
    double wbR, wbG, wbB;
    double hiAmt, shAmt, satScale, vibAmt;
    std::vector<int> curveLut;
    std::vector<int> lvlRgb, lvlR, lvlG, lvlB;
    std::vector<ColorRangeParam> colorRanges;
};

ToneParams makeToneParams(int contrast, int brightness, int highlights,
                          int shadows, int saturation, int vibrance,
                          int temperature, int tint, double wbR, double wbG,
                          double wbB, const QVector<QPointF> &curve,
                          const Levels &levels,
                          const QVector<ColorRangeAdjust> &colorRanges = {}) {
    ToneParams p;
    const double c = contrast;
    // Classic Photoshop contrast formula: a dimensionless ratio, scale-
    // invariant with respect to the working pixel bit depth.
    p.contrastFactor = (259.0 * (c + 255.0)) / (255.0 * (259.0 - c));
    // `brightness` is an 8-bit-scale slider value (-100..100); rescale to the
    // 16-bit working range by the exact 65535/255 ratio.
    p.brightness = brightness * 257.0;
    const double tempF = temperature / 100.0;
    const double tintF = tint / 100.0;
    p.wbR = wbR * (1.0 + 0.4 * tempF);
    p.wbG = wbG * (1.0 + 0.4 * tintF);
    p.wbB = wbB * (1.0 - 0.4 * tempF);
    p.hiAmt = highlights / 100.0;
    p.shAmt = shadows / 100.0;
    p.satScale = 1.0 + saturation / 100.0;
    p.vibAmt = vibrance / 100.0;
    buildCurveLut(curve, p.curveLut);
    buildLevelsLut(levels.rgb, p.lvlRgb);
    buildLevelsLut(levels.r, p.lvlR);
    buildLevelsLut(levels.g, p.lvlG);
    buildLevelsLut(levels.b, p.lvlB);
    for (const ColorRangeAdjust &cr : colorRanges) {
        if (cr.amount == 0) continue;
        p.colorRanges.push_back({double(cr.r), double(cr.g), double(cr.b),
                                 cr.channel, cr.amount * 257.0});
    }
    return p;
}

QRgba64 applyTone(QRgba64 px, const ToneParams &p) {
    double r = px.red(), g = px.green(), b = px.blue();
    r *= p.wbR; g *= p.wbG; b *= p.wbB;
    r = p.curveLut[clamp16(int(r))];
    g = p.curveLut[clamp16(int(g))];
    b = p.curveLut[clamp16(int(b))];
    r = p.lvlR[p.lvlRgb[clamp16(int(r))]];
    g = p.lvlG[p.lvlRgb[clamp16(int(g))]];
    b = p.lvlB[p.lvlRgb[clamp16(int(b))]];
    for (const ColorRangeParam &cr : p.colorRanges) {
        // Redmean distance in 8-bit scale against the stored pick — same
        // formula as colorDist below, on the post-levels pixel so the match
        // tracks what the user sees.
        const double pr = r / 257.0, pg = g / 257.0, pb = b / 257.0;
        const double dr = pr - cr.tr, dg = pg - cr.tg, db = pb - cr.tb;
        const double rmean = (pr + cr.tr) / 2.0;
        const double d2 = (2.0 + rmean / 256.0) * dr * dr + 4.0 * dg * dg +
                          (2.0 + (255.0 - rmean) / 256.0) * db * db;
        if (d2 >= kColorRangeTolerance * kColorRangeTolerance) continue;
        const double wgt =
            1.0 - smoothstep01(std::sqrt(d2) / kColorRangeTolerance);
        if (wgt <= 0.0) continue;
        (cr.channel == 0 ? r : cr.channel == 1 ? g : b) += wgt * cr.delta;
    }
    r = p.contrastFactor * (r - 32768.0) + 32768.0 + p.brightness;
    g = p.contrastFactor * (g - 32768.0) + 32768.0 + p.brightness;
    b = p.contrastFactor * (b - 32768.0) + 32768.0 + p.brightness;
    double luma = 0.299 * r + 0.587 * g + 0.114 * b;
    double ln = clampd(luma / 65535.0, 0.0, 1.0);
    if (p.hiAmt != 0.0) {
        double d = p.hiAmt * 15420.0 * ln * ln;
        r += d; g += d; b += d;
    }
    if (p.shAmt != 0.0) {
        double d = p.shAmt * 15420.0 * (1.0 - ln) * (1.0 - ln);
        r += d; g += d; b += d;
    }
    luma = 0.299 * r + 0.587 * g + 0.114 * b;
    double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    double sat = mx > 0 ? (mx - mn) / mx : 0.0;
    double scale = p.satScale + p.vibAmt * (1.0 - sat);
    r = luma + (r - luma) * scale;
    g = luma + (g - luma) * scale;
    b = luma + (b - luma) * scale;
    return qRgba64(clamp16(int(std::lround(r))), clamp16(int(std::lround(g))),
                   clamp16(int(std::lround(b))), px.alpha());
}

// Blend one 0..255 channel of the layer's local result ("src") with the
// channel below it ("dst"), per standard Photoshop-style formulas in
// non-linear (sRGB) space.
inline double blendChannel(BlendMode mode, double dst, double src) {
    constexpr double kMax = 65535.0;
    switch (mode) {
    case BlendMode::Multiply:
        return dst * src / kMax;
    case BlendMode::Screen:
        return kMax - (kMax - dst) * (kMax - src) / kMax;
    case BlendMode::Overlay:
        return dst <= kMax / 2.0 ? (2.0 * dst * src) / kMax
                                 : kMax - 2.0 * (kMax - dst) * (kMax - src) / kMax;
    case BlendMode::SoftLight: {
        double a = dst / kMax, b = src / kMax;
        double d = a <= 0.25 ? ((16.0 * a - 12.0) * a + 4.0) * a
                              : std::sqrt(a);
        double r = b <= 0.5 ? a - (1.0 - 2.0 * b) * a * (1.0 - a)
                            : a + (2.0 * b - 1.0) * (d - a);
        return r * kMax;
    }
    case BlendMode::Darken:
        return std::min(dst, src);
    case BlendMode::Lighten:
        return std::max(dst, src);
    case BlendMode::ColorDodge:
        return src >= kMax ? kMax : std::min(kMax, dst * kMax / (kMax - src));
    case BlendMode::ColorBurn:
        return src <= 0.0 ? 0.0 : kMax - std::min(kMax, (kMax - dst) * kMax / src);
    case BlendMode::Difference:
        return std::abs(dst - src);
    case BlendMode::Exclusion:
        return dst + src - 2.0 * dst * src / kMax;
    // Hue/Saturation/Color/Luminosity are handled by blendHSLTriple(), not
    // per-channel — callers must branch before reaching here.
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
    case BlendMode::Normal:
    default:
        return src;
    }
}

// Full-RGB-triple blend for the HSL-based modes (Hue/Saturation/Color/
// Luminosity), per the standard Photoshop/PDF compositing formulas: `dst` is
// the backdrop (image so far), `src` the layer's own content. Works in
// 0..1 float per channel; `dst`/`src` channels are 0..65535 (16-bit).
QRgba64 blendHSLTriple(BlendMode mode, QRgba64 dst, QRgba64 src) {
    auto toVec = [](QRgba64 c) {
        return std::array<double, 3>{c.red() / 65535.0, c.green() / 65535.0, c.blue() / 65535.0};
    };
    auto lum = [](const std::array<double, 3> &c) {
        return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
    };
    auto clipColor = [&](std::array<double, 3> c) {
        double l = lum(c);
        double n = std::min({c[0], c[1], c[2]});
        double x = std::max({c[0], c[1], c[2]});
        if (n < 0.0)
            for (double &v : c) v = l + (v - l) * l / std::max(1e-9, l - n);
        if (x > 1.0)
            for (double &v : c) v = l + (v - l) * (1.0 - l) / std::max(1e-9, x - l);
        return c;
    };
    auto setLum = [&](std::array<double, 3> c, double l) {
        double d = l - lum(c);
        for (double &v : c) v += d;
        return clipColor(c);
    };
    auto sat = [](const std::array<double, 3> &c) {
        return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
    };
    auto setSat = [](std::array<double, 3> c, double s) {
        int lo = 0, mid = 1, hi = 2;
        for (int i = 1; i < 3; ++i) if (c[i] < c[lo]) lo = i;
        for (int i = 0; i < 3; ++i) if (c[i] > c[hi]) hi = i;
        for (int i = 0; i < 3; ++i) if (i != lo && i != hi) mid = i;
        if (lo == hi) { c[0] = c[1] = c[2] = 0.0; return c; } // fully desaturated backdrop/source
        if (c[hi] > c[lo]) {
            c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
            c[hi] = s;
        } else {
            c[mid] = c[hi] = 0.0;
        }
        c[lo] = 0.0;
        return c;
    };

    const std::array<double, 3> cb = toVec(dst), cs = toVec(src);
    std::array<double, 3> out;
    switch (mode) {
    case BlendMode::Hue:
        out = setLum(setSat(cs, sat(cb)), lum(cb));
        break;
    case BlendMode::Saturation:
        out = setLum(setSat(cb, sat(cs)), lum(cb));
        break;
    case BlendMode::Color:
        out = setLum(cs, lum(cb));
        break;
    case BlendMode::Luminosity:
    default:
        out = setLum(cb, lum(cs));
        break;
    }
    auto to16 = [](double v) { return quint16(std::clamp(int(std::lround(v * 65535.0)), 0, 65535)); };
    return qRgba64(to16(out[0]), to16(out[1]), to16(out[2]), dst.alpha());
}

// Mask weight [0,1] at a width-normalized point for radial/linear masks.
double radialWeight(const Mask &m, double nx, double ny) {
    double dx = nx - m.center.x(), dy = ny - m.center.y();
    double ca = std::cos(-m.angle), sa = std::sin(-m.angle);
    double ex = (dx * ca - dy * sa) / std::max(1e-6, m.radiusX);
    double ey = (dx * sa + dy * ca) / std::max(1e-6, m.radiusY);
    double d = std::sqrt(ex * ex + ey * ey);
    double inner = 1.0 - clampd(m.feather, 0.0, 1.0);
    double w;
    if (d <= inner) w = 1.0;
    else if (d >= 1.0) w = 0.0;
    else w = smoothstep01((1.0 - d) / std::max(1e-6, 1.0 - inner));
    return m.inverted ? 1.0 - w : w;
}

double linearWeight(const Mask &m, double nx, double ny) {
    double ax = m.p1.x() - m.p0.x(), ay = m.p1.y() - m.p0.y();
    double len2 = ax * ax + ay * ay;
    double t = len2 > 1e-9
                   ? ((nx - m.p0.x()) * ax + (ny - m.p0.y()) * ay) / len2
                   : 0.0;
    double w = 1.0 - smoothstep01(t); // full at p0, zero at p1
    return m.inverted ? 1.0 - w : w;
}

// Gradient Fill's own layer content: a full-canvas two-color interpolation,
// using the SAME radial/linear weight field a Radial/Linear mask already
// computes for masking (see radialWeight/linearWeight above) — but here as a
// per-pixel color-mix factor (t: 1 at center/start, 0 at/beyond the far
// edge), not as this layer's alpha. Alpha is always opaque; the composite
// loop deliberately skips the usual mask-weight-as-alpha step for gradient
// fills (see applyMasks) so the fill covers the whole layer like a real
// gradient tool, rather than fading to transparent past the feather point.
QImage renderGradientFill(const Mask &m, int w, int h) {
    QImage loc(w, h, QImage::Format_RGBA64);
    const double W = w;
    const QColor &ca = m.gradientColorA, &cb = m.gradientColorB;
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(loc.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double t = clampd(m.type == MaskType::Radial ? radialWeight(m, x / W, y / W)
                                                                : linearWeight(m, x / W, y / W),
                                    0.0, 1.0);
            line[x] = qRgba64(
                quint16(std::lround((ca.redF() * t + cb.redF() * (1.0 - t)) * 65535.0)),
                quint16(std::lround((ca.greenF() * t + cb.greenF() * (1.0 - t)) * 65535.0)),
                quint16(std::lround((ca.blueF() * t + cb.blueF() * (1.0 - t)) * 65535.0)),
                65535);
        }
    }
    return loc;
}

// Cheap perceptual ("redmean") weighted colour distance — no colourspace
// conversion needed, good enough to decide "same object or not" for Auto Mask.
inline double colorDist(QRgb a, QRgb b) {
    const double dr = qRed(a) - qRed(b);
    const double dg = qGreen(a) - qGreen(b);
    const double db = qBlue(a) - qBlue(b);
    const double rmean = (qRed(a) + qRed(b)) / 2.0;
    const double d2 = (2.0 + rmean / 256.0) * dr * dr + 4.0 * dg * dg +
                      (2.0 + (255.0 - rmean) / 256.0) * db * db;
    return std::sqrt(d2);
}

// Auto Mask tolerance: pixels farther than this (redmean colour distance) from
// the seed colour under a dab are excluded, so the stroke stops at edges.
constexpr double kAutoMaskTolerance = 45.0;

// Separable box blur over a single-channel 0..255 buffer, in place. Same
// moving-average approach as boxBlur (below) but without the RGB packing,
// used to soften Mask::selectionClipNorm's hard edge into a feathered one.
void boxBlurAlpha(std::vector<uchar> &buf, int w, int h, int radius) {
    if (radius < 1 || w <= 0 || h <= 0) return;
    std::vector<int> tmp(size_t(w) * h);
    const int win = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        const uchar *row = &buf[size_t(y) * w];
        long sum = 0;
        for (int x = -radius; x <= radius; ++x) sum += row[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            tmp[size_t(y) * w + x] = int(sum / win);
            sum += row[std::clamp(x + radius + 1, 0, w - 1)] - row[std::clamp(x - radius, 0, w - 1)];
        }
    }
    for (int x = 0; x < w; ++x) {
        long sum = 0;
        for (int y = -radius; y <= radius; ++y) sum += tmp[size_t(std::clamp(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            buf[size_t(y) * w + x] = uchar(std::clamp(int(sum / win), 0, 255));
            sum += tmp[size_t(std::clamp(y + radius + 1, 0, h - 1)) * w + x] -
                   tmp[size_t(std::clamp(y - radius, 0, h - 1)) * w + x];
        }
    }
}

// Builds a soft 0..255 alpha buffer for m.selectionClipNorm at (w,h): a hard
// rasterization of the path, blurred by m.selectionFeatherNorm (Photoshop's
// Select > Feather). Returns an empty vector when there's no clip or no
// feather, so callers can cheaply fall back to a plain QPainterPath::contains
// test instead of an alpha lookup.
std::vector<uchar> selectionFeatherAlpha(const Mask &m, int w, int h) {
    if (m.selectionClipNorm.isEmpty() || m.selectionFeatherNorm <= 0.0 || w <= 0 || h <= 0)
        return {};
    QImage hardMask(w, h, QImage::Format_Grayscale8);
    hardMask.fill(0);
    {
        QPainter mp(&hardMask);
        mp.setRenderHint(QPainter::Antialiasing, true);
        QTransform t;
        t.scale(w, w); // width-normalized -> pixel space, same convention as elsewhere
        mp.setPen(Qt::NoPen);
        mp.setBrush(Qt::white);
        mp.drawPath(t.map(m.selectionClipNorm));
    }
    std::vector<uchar> alpha(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const uchar *row = hardMask.constScanLine(y);
        std::copy(row, row + w, alpha.begin() + size_t(y) * w);
    }
    boxBlurAlpha(alpha, w, h, std::max(1, int(std::lround(m.selectionFeatherNorm * w))));
    return alpha;
}

// Rasterize a brush mask into a coverage buffer (0..255) at image resolution.
// Dabs are applied in stroke order: normal dabs union (max) into the coverage,
// Alt-painted (erase) dabs subtract from it, so painting after erasing (or
// vice versa) behaves like Lightroom's brush eraser. `ref`, if non-null and
// matching (w,h), enables Auto Mask edge-aware limiting.
//
// `cache`, if given, lets repeated calls for the same mask/resolution during
// a drag only rasterize the newly-appended stroke points instead of redoing
// the whole stroke; see BrushRasterCache. Falls back to a full rebuild
// whenever the cache doesn't (or can no longer) apply.
// `colOut`, if given, receives the color of whichever dab last "won" the
// coverage contest at each pixel (see per-dab BrushStrokePoint::color) — used
// by Paint-type masks. Ties (equal coverage) go to the later dab, so a new
// stroke painted over an already fully-opaque area still takes over the
// color there instead of being hidden behind the earlier stroke.
//
// When `cache` is given, the rasterization happens in place in the cache's
// own buffers (only newly-appended points are touched) — `cov`/`colOut` are
// only copied from the cache if `populateOut` is true. During an active drag,
// most masks in the stack haven't changed at all; the caller can pass
// `populateOut=false` and read the cache's buffers directly, avoiding a
// full-image copy (twice, in and out) on every mouse-move sample for masks
// that didn't change.
void rasterizeBrush(const Mask &m, std::vector<uchar> &cov, int w, int h,
                    const QImage *ref = nullptr, BrushRasterCache *cache = nullptr,
                    std::vector<QRgb> *colOut = nullptr, bool populateOut = true,
                    std::vector<uchar> *eraseOut = nullptr,
                    // When non-null, set (only when this call reused the
                    // cache and appended >=1 new point, i.e. a true
                    // incremental step) to the union of the newly-stamped
                    // dabs' bounding boxes, in `cov`'s own pixel space. Left
                    // untouched (caller must check *incrementalOut) on a
                    // full/from-scratch rebuild, since then the whole buffer
                    // changed. Lets a caller that separately caches the
                    // final composited output (see BrushRasterCache::
                    // lastComposite) patch just this rect instead of
                    // recompositing everything.
                    QRect *touchedRectOut = nullptr, bool *incrementalOut = nullptr) {
    // Each stroke point carries its own radius/hardness (captured at paint
    // time), so a later brush-size change doesn't invalidate dabs already
    // rasterized — only new points need to be added.
    const bool canReuse = cache && cache->valid && cache->w == w && cache->h == h &&
                          cache->autoMask == m.autoMask &&
                          cache->pointCount <= m.stroke.size() &&
                          (cache->pointCount == 0 ||
                           cache->lastPoint == m.stroke[cache->pointCount - 1]);

    std::vector<uchar> *covBuf = cache ? &cache->cov : &cov;
    std::vector<QRgb> *colBuf = cache ? &cache->col : colOut;
    std::vector<uchar> *eraseBuf = cache ? &cache->erase : eraseOut;
    std::vector<uint32_t> localRunId;
    std::vector<uint32_t> *runIdBuf = cache ? &cache->runId : &localRunId;

    int startIdx = 0;
    uint32_t runCounter = 0;
    if (canReuse) {
        startIdx = cache->pointCount;
        runCounter = cache->lastRunId;
    } else {
        covBuf->assign(size_t(w) * h, 0);
        if (colOut) colBuf->assign(size_t(w) * h, 0);
        if (eraseBuf) eraseBuf->assign(size_t(w) * h, 0);
        runIdBuf->assign(size_t(w) * h, 0);
    }

    const double W = w;
    const bool autoMask = m.autoMask && ref && ref->width() == w && ref->height() == h;
    const bool hasClip = !m.selectionClipNorm.isEmpty();
    // Computed once per call (not per dab): empty when there's no clip or no
    // feather, in which case stampDab falls back to a plain contains() test.
    const std::vector<uchar> featherAlpha = selectionFeatherAlpha(m, w, h);

    // Cheap deterministic per-pixel hash -> [0,1), used by the `grain` term
    // below. No RNG state, so identical inputs always render identically.
    auto hash01 = [](int x, int y) -> double {
        uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= (h >> 16);
        return (h & 0xFFFFFFu) / double(0xFFFFFFu);
    };

    // Stamps one soft-edged circular dab, in pixel space, with its own
    // radius/hardness/color/erase — used both for a stroke's recorded
    // points and for the sub-dabs interpolated between them below. `grain`
    // (0..1, default 0 = no effect) multiplies the computed coverage by a
    // deterministic per-dab noise term derived from a hash of each pixel's
    // integer coordinates, giving Pen-type dabs a graphite-like texture.
    // `cloneOffsetPx` is only meaningful when `isClone` — the (source - dab)
    // translation, in `ref`'s own pixel space (see the isClone branch below
    // for how it's derived), so each covered pixel samples the
    // correspondingly offset pixel from `ref` instead of one flat `color`.
    auto stampDab = [&](double px, double py, double rad, double hardness, bool erase, QRgb color,
                        uint32_t runId, double grain = 0.0, bool isClone = false,
                        QPointF cloneOffsetPx = QPointF()) {
        const double inner = clampd(hardness, 0.0, 1.0) * rad;
        const double band = std::max(1e-6, rad - inner);
        const int x0 = std::max(0, int(px - rad));
        const int x1 = std::min(w - 1, int(px + rad));
        const int y0 = std::max(0, int(py - rad));
        const int y1 = std::min(h - 1, int(py + rad));
        QRgb seed = 0;
        if (autoMask)
            seed = ref->pixel(std::clamp(int(std::lround(px)), 0, w - 1),
                              std::clamp(int(std::lround(py)), 0, h - 1));
        const bool canClone = isClone && ref && ref->width() > 0 && ref->height() > 0;
        const double cloneScale = canClone ? double(ref->width()) / W : 1.0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                double dx = x - px, dy = y - py;
                double dist = std::sqrt(dx * dx + dy * dy);
                double v = dist <= inner ? 1.0
                           : dist >= rad ? 0.0
                                         : smoothstep01((rad - dist) / band);
                if (v <= 0.0) continue;
                // Clip each covered pixel to the selection active when this
                // dab was painted, not just the dab's center point — a large
                // brush near a selection edge must not bleed past it. Soft
                // (feathered) when featherAlpha is populated, hard otherwise.
                if (hasClip) {
                    if (!featherAlpha.empty()) {
                        double fa = featherAlpha[size_t(y) * w + x] / 255.0;
                        if (fa <= 0.0) continue;
                        v *= fa;
                    } else if (!m.selectionClipNorm.contains(QPointF(x / W, y / W))) {
                        continue;
                    }
                }
                if (autoMask) {
                    double cd = colorDist(ref->pixel(x, y), seed);
                    v *= 1.0 - smoothstep01(cd / kAutoMaskTolerance);
                    if (v <= 0.0) continue;
                }
                if (grain > 0.0) {
                    v *= 1.0 - grain * hash01(x, y);
                    if (v <= 0.0) continue;
                }
                uchar iv = uchar(std::lround(v * 255.0));
                size_t idx = size_t(y) * w + x;
                uchar &dst = (*covBuf)[idx];
                if (erase) {
                    dst = uchar(std::max(0, int(dst) - int(iv)));
                    if (eraseBuf) {
                        uchar &e = (*eraseBuf)[idx];
                        e = uchar(std::max(int(e), int(iv)));
                    }
                    continue;
                }
                uint32_t &pixRun = (*runIdBuf)[idx];
                // Same run (self-overlap within one continuous drag): keep
                // the max-coverage envelope so overlapping soft-edged dabs of
                // one stroke blend into a smooth outline instead of a
                // scalloped or double-darkened one (see stampDab's comment).
                // A different run (an earlier, already-finished stroke, or a
                // never-painted pixel): alpha-over the new dab's coverage on
                // top of whatever is there — never *below* it, or a soft,
                // low-coverage edge pixel of the new dab would punch a hole
                // through an already fully-opaque older stroke — while still
                // making the new stroke's color win wherever it has any
                // coverage, so a later stroke visibly paints over an earlier
                // one.
                const bool samePriorRun = (pixRun == runId);
                const uchar newCov = samePriorRun ? iv
                    : uchar(iv + (int(dst) * (255 - iv)) / 255);
                if (samePriorRun ? (iv >= dst) : (iv > 0)) {
                    dst = newCov;
                    pixRun = runId;
                    if (colOut) {
                        if (canClone) {
                            int sx = std::clamp(int(std::lround(x * cloneScale + cloneOffsetPx.x())),
                                                0, ref->width() - 1);
                            int sy = std::clamp(int(std::lround(y * cloneScale + cloneOffsetPx.y())),
                                                0, ref->height() - 1);
                            (*colBuf)[idx] = ref->pixel(sx, sy);
                        } else {
                            (*colBuf)[idx] = color;
                        }
                    }
                }
            }
        }
    };

    // Pressure-sensitive width: matches the tablet driver's own
    // pressure-preview widget — a bare touch tapers to a hairline and
    // width builds up to full radius as pressure increases. Shared by
    // every dab (Pen and Brush alike) so both tools feel the same under
    // the stylus; only Pen additionally varies hardness/opacity/grain by
    // pencil grade (see penParams below).
    auto pressureRadius = [&](const BrushStrokePoint &sp) {
        constexpr double kMinFrac = 0.04;
        constexpr double kEaseExp = 0.6; // ease-in: width ramps up faster at low/mid pressure
        const double pressure = clampd(sp.pressure, 0.0, 1.0);
        const double eased = std::pow(pressure, kEaseExp);
        const double frac = kMinFrac + (1.0 - kMinFrac) * eased;
        const double baseRad = std::max(0.5, sp.radius * W);
        return std::max(0.5, baseRad * frac);
    };

    // Pen: derive effective radius/hardness/opacity-multiplier/grain from
    // (sp.radius, sp.hardness, sp.penGrade, sp.pressure) — all captured
    // per-dab, since Pen and Brush dabs can be interleaved in the same
    // stroke/layer (see BrushStrokePoint::isPen).
    // penGrade in [-6,5] (6B..5H); normalize to t in [0,1] (0=softest,1=hardest).
    auto penParams = [&](const BrushStrokePoint &sp, double &effRad,
                         double &effHardness, double &opacityMul, double &grain) {
        const double t = clampd((sp.penGrade + 6.0) / 11.0, 0.0, 1.0);
        constexpr double kSoftHardnessFloor = 0.15;
        constexpr double kHardHardnessCeil = 0.95;
        effHardness = kSoftHardnessFloor + (kHardHardnessCeil - kSoftHardnessFloor) * t;
        constexpr double kSoftOpacityMul = 1.0;
        constexpr double kHardOpacityMul = 0.6;
        opacityMul = kSoftOpacityMul + (kHardOpacityMul - kSoftOpacityMul) * t;
        constexpr double kSoftGrain = 0.22;
        constexpr double kHardGrain = 0.0;
        grain = kSoftGrain + (kHardGrain - kSoftGrain) * t;
        effRad = pressureRadius(sp);
    };

    double touchedX0 = 1e18, touchedY0 = 1e18, touchedX1 = -1e18, touchedY1 = -1e18;
    auto growTouched = [&](double cx, double cy, double r) {
        touchedX0 = std::min(touchedX0, cx - r);
        touchedY0 = std::min(touchedY0, cy - r);
        touchedX1 = std::max(touchedX1, cx + r);
        touchedY1 = std::max(touchedY1, cy + r);
    };
    for (int i = startIdx; i < m.stroke.size(); ++i) {
        const BrushStrokePoint &sp = m.stroke[i];
        if (sp.newStroke) ++runCounter;
        double rad = pressureRadius(sp);
        double hardness = sp.hardness;
        double opacityMul = 1.0, grain = 0.0;
        if (sp.isPen) penParams(sp, rad, hardness, opacityMul, grain);
        const double px = sp.pt.x() * W, py = sp.pt.y() * W;
        QRgb dabColor = sp.color;
        if (sp.isPen && opacityMul < 1.0) {
            // Fold the opacity multiplier into the dab's alpha so lighter
            // (harder-grade) dabs deposit less coverage.
            dabColor = qRgba(qRed(dabColor), qGreen(dabColor), qBlue(dabColor),
                             int(std::lround(qAlpha(dabColor) * opacityMul)));
        }
        // Clone dabs: offset in `ref`'s own pixel space (see stampDab), so
        // the per-pixel sample point is independent of the raster resolution.
        const QPointF cloneOffsetPx = (sp.isClone && ref)
            ? QPointF((sp.cloneSourcePt.x() - sp.pt.x()) * ref->width(),
                     (sp.cloneSourcePt.y() - sp.pt.y()) * ref->width())
            : QPointF();

        // Dab spacing (mouse-move sampling) is independent of brush radius,
        // so consecutive dabs of a soft brush can land far enough apart that
        // maxing their soft edges leaves a scalloped envelope instead of a
        // smooth stroke outline. Interpolate sub-dabs along the segment from
        // the previous point, spaced relative to the (interpolated) radius,
        // to fill in the gap densely enough to read as a continuous line.
        if (i > 0 && !sp.newStroke) {
            const BrushStrokePoint &pp = m.stroke[i - 1];
            const double ppx = pp.pt.x() * W, ppy = pp.pt.y() * W;
            double pprad = std::max(0.5, pp.radius * W);
            growTouched(ppx, ppy, pprad); // interpolated sub-dabs span pp..sp
            double pphardness = pp.hardness;
            double ppOpacityMul = 1.0, ppGrain = 0.0;
            if (pp.isPen) penParams(pp, pprad, pphardness, ppOpacityMul, ppGrain);
            const QPointF ppCloneOffsetPx = (pp.isClone && ref)
                ? QPointF((pp.cloneSourcePt.x() - pp.pt.x()) * ref->width(),
                         (pp.cloneSourcePt.y() - pp.pt.y()) * ref->width())
                : QPointF();
            const double dx = px - ppx, dy = py - ppy;
            const double dist = std::sqrt(dx * dx + dy * dy);
            const double spacing = std::max(1.0, std::min(rad, pprad) * 0.2);
            const int steps = int(dist / spacing);
            for (int s = 1; s < steps; ++s) {
                const double t = double(s) / steps;
                QPointF interpOffsetPx = ppCloneOffsetPx + (cloneOffsetPx - ppCloneOffsetPx) * t;
                stampDab(ppx + dx * t, ppy + dy * t,
                         pprad + (rad - pprad) * t,
                         pphardness + (hardness - pphardness) * t,
                         sp.erase, dabColor, runCounter,
                         ppGrain + (grain - ppGrain) * t,
                         sp.isClone, interpOffsetPx);
            }
        }

        stampDab(px, py, rad, hardness, sp.erase, dabColor, runCounter, grain, sp.isClone, cloneOffsetPx);
        growTouched(px, py, rad);
    }

    if (cache) cache->lastRunId = runCounter;
    if (incrementalOut) *incrementalOut = canReuse;
    if (touchedRectOut && canReuse && touchedX1 >= touchedX0) {
        const int rx0 = std::max(0, int(std::floor(touchedX0)) - 1);
        const int ry0 = std::max(0, int(std::floor(touchedY0)) - 1);
        const int rx1 = std::min(w - 1, int(std::ceil(touchedX1)) + 1);
        const int ry1 = std::min(h - 1, int(std::ceil(touchedY1)) + 1);
        if (rx1 >= rx0 && ry1 >= ry0)
            *touchedRectOut = QRect(rx0, ry0, rx1 - rx0 + 1, ry1 - ry0 + 1);
    }

    if (cache) {
        cache->w = w;
        cache->h = h;
        cache->pointCount = m.stroke.size();
        cache->brushRadius = m.brushRadius;
        cache->hardness = m.hardness;
        cache->autoMask = m.autoMask;
        cache->lastPoint = m.stroke.isEmpty() ? BrushStrokePoint{} : m.stroke.last();
        cache->valid = true;
        if (populateOut) {
            cov = cache->cov;
            if (colOut) *colOut = cache->col;
            if (eraseOut) *eraseOut = cache->erase;
        }
    }
}

} // namespace

QImage bucketFillPaintMask(const Mask &m, const QPointF &clickNorm,
                          const QColor &color, int w, int h) {
    if (w <= 0 || h <= 0) return m.fillMask;

    std::vector<uchar> cov;
    rasterizeBrush(m, cov, w, h);

    const int px = std::clamp(int(std::lround(clickNorm.x() * w)), 0, w - 1);
    const int py = std::clamp(int(std::lround(clickNorm.y() * w)), 0, h - 1);

    const bool hasPrevFill = !m.fillMask.isNull();
    QImage prevFillScaled;
    if (hasPrevFill)
        prevFillScaled = m.fillMask.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32);

    // Brush strokes are a separate layer of paint the bucket never touches
    // (matches the original "don't fill over hand-painted areas" behaviour):
    // they always act as flood-fill walls, regardless of color.
    auto covBlocked = [&](int x, int y) -> bool {
        return cov[size_t(y) * w + x] > 0;
    };
    // Existing bucket-filled pixels are transparent under the color's own
    // alpha, so treat anything with alpha above a small anti-aliasing
    // threshold as "painted" and everything else as empty canvas.
    auto fillPixel = [&](int x, int y) -> QRgb {
        if (!hasPrevFill) return qRgba(0, 0, 0, 0);
        return reinterpret_cast<const QRgb *>(prevFillScaled.constScanLine(y))[x];
    };
    auto closeColor = [](QRgb a, QRgb b) -> bool {
        return std::abs(qRed(a) - qRed(b)) <= 24 &&
               std::abs(qGreen(a) - qGreen(b)) <= 24 &&
               std::abs(qBlue(a) - qBlue(b)) <= 24 &&
               std::abs(qAlpha(a) - qAlpha(b)) <= 24;
    };

    if (py >= h || px >= w || covBlocked(px, py)) return m.fillMask; // clicked on a brush stroke

    // What the click landed on determines the flood target: either empty
    // canvas (classic fill-the-hole behavior) or an existing flat-colored
    // fill region, which lets a second click recolor it (e.g. white -> green)
    // instead of being a no-op.
    const QRgb clickFill = fillPixel(px, py);
    const bool startFilled = qAlpha(clickFill) > 16;

    const bool hasClip = !m.selectionClipNorm.isEmpty();
    const double W = w;
    const std::vector<uchar> featherAlpha = selectionFeatherAlpha(m, w, h);
    // The flood-fill's membership test itself stays a hard boundary (a fill
    // either grows into a pixel or it doesn't); feathering only softens the
    // final written alpha near the selection edge, applied below where each
    // pixel is stamped with fillRgb.
    auto matchesRegion = [&](int x, int y) -> bool {
        if (covBlocked(x, y)) return false;
        if (hasClip) {
            if (!featherAlpha.empty()) {
                if (featherAlpha[size_t(y) * w + x] == 0) return false;
            } else if (!m.selectionClipNorm.contains(QPointF(x / W, y / W))) {
                return false;
            }
        }
        const QRgb p = fillPixel(x, y);
        const bool filled = qAlpha(p) > 16;
        if (startFilled) return filled && closeColor(p, clickFill);
        return !filled;
    };

    std::vector<uchar> visited(size_t(w) * h, 0);
    QImage regionImg(w, h, QImage::Format_ARGB32);
    regionImg.fill(Qt::transparent);
    const QRgb fillRgb = color.rgba();

    std::vector<QPoint> stack;
    stack.reserve(4096);
    stack.push_back(QPoint(px, py));
    visited[size_t(py) * w + px] = 1;
    static const int dx[4] = {1, -1, 0, 0};
    static const int dy[4] = {0, 0, 1, -1};
    while (!stack.empty()) {
        const QPoint p = stack.back();
        stack.pop_back();
        QRgb stamp = fillRgb;
        if (!featherAlpha.empty()) {
            double fa = featherAlpha[size_t(p.y()) * w + p.x()] / 255.0;
            stamp = qRgba(qRed(fillRgb), qGreen(fillRgb), qBlue(fillRgb),
                         int(std::lround(qAlpha(fillRgb) * fa)));
        }
        reinterpret_cast<QRgb *>(regionImg.scanLine(p.y()))[p.x()] = stamp;
        for (int k = 0; k < 4; ++k) {
            const int nx = p.x() + dx[k], ny = p.y() + dy[k];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            const size_t nidx = size_t(ny) * w + nx;
            if (visited[nidx] || !matchesRegion(nx, ny)) continue;
            visited[nidx] = 1;
            stack.push_back(QPoint(nx, ny));
        }
    }

    QImage merged;
    if (hasPrevFill) {
        merged = prevFillScaled;
    } else {
        merged = QImage(w, h, QImage::Format_ARGB32);
        merged.fill(Qt::transparent);
    }
    QPainter p(&merged);
    p.drawImage(0, 0, regionImg);
    p.end();
    return merged;
}

namespace {

// Rasterizes a Text-type mask's glyph shape into a coverage buffer, same
// uchar-per-pixel layout as rasterizeBrush's `cov` output (0 = no coverage,
// 255 = full). `textPos`/`textPixelSize` are width-normalized like other
// mask geometry (see Mask::center). Cheap enough (a handful of glyph paths)
// to recompute fresh every render — no incremental cache needed, unlike
// brush strokes.
void rasterizeText(const Mask &m, std::vector<uchar> &cov, int w, int h) {
    cov.assign(size_t(w) * h, 0);
    if (m.text.trimmed().isEmpty() || w <= 0 || h <= 0) return;

    QImage mask(w, h, QImage::Format_Alpha8);
    mask.fill(0);
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont font(m.textFamily);
    font.setPixelSize(std::max(1, int(std::lround(m.textPixelSize * w))));
    font.setBold(m.textBold);
    font.setItalic(m.textItalic);
    QFontMetricsF fm(font);

    QPainterPath path;
    const QStringList lines = m.text.split(QLatin1Char('\n'));
    const double x0 = m.textPos.x() * w, y0 = m.textPos.y() * w;
    double y = y0 + fm.ascent();
    for (const QString &line : lines) {
        if (!line.isEmpty()) path.addText(x0, y, font, line);
        y += fm.lineSpacing();
    }
    p.fillPath(path, Qt::white);
    p.end();

    for (int yy = 0; yy < h; ++yy) {
        const uchar *src = mask.constScanLine(yy);
        std::copy(src, src + w, cov.begin() + size_t(yy) * w);
    }
}

ShapeOp maskToShapeOp(const Mask &m); // fwd decl
TextOp maskToTextOp(const Mask &m);   // fwd decl

// Draws a Shape or TextBox mask's own content (fill/stroke, or glyphs/
// outline/shadow/background) into a transparent w×h canvas via the existing
// ShapeTool.cpp/TextTool.cpp drawing code, then splits the result into a
// straight-alpha colour buffer (`loc`, for the generic per-pixel blend loop
// below) and a coverage buffer (`cov`, read the same way rasterizeBrush/
// rasterizeText's output is). `orientedToGeom`/`geomRotationDeg`/`scale` map
// the mask's raw oriented-image pixel-space geometry into `img`'s local
// pixel space, exactly like applyShapes/applyTexts.
QImage rasterizeShapeOrTextBox(const Mask &m, std::vector<uchar> &cov, int w, int h,
                               const QTransform &orientedToGeom, double geomRotationDeg,
                               double scale) {
    cov.assign(size_t(w) * h, 0);
    QImage overlay(w, h, QImage::Format_ARGB32_Premultiplied);
    overlay.fill(Qt::transparent);
    if (w <= 0 || h <= 0) return QImage(w, h, QImage::Format_RGBA64);

    if (m.type == MaskType::Shape) {
        ShapeOp op = maskToShapeOp(m);
        ShapeOp local = op;
        if (op.type == ShapeType::Line) {
            local.p1 = orientedToGeom.map(op.p1) * scale;
            local.p2 = orientedToGeom.map(op.p2) * scale;
        } else {
            QPointF center = orientedToGeom.map(op.rect.center()) * scale;
            local.rect = QRectF(QPointF(0, 0), op.rect.size() * scale);
            local.rect.moveCenter(center);
            local.rotation = op.rotation + geomRotationDeg;
        }
        local.strokeWidth *= scale;
        applyShapeOp(overlay, local);
    } else {
        TextOp op = maskToTextOp(m);
        TextOp local = op;
        local.pos = orientedToGeom.map(op.pos) * scale;
        local.rotation = op.rotation + geomRotationDeg;
        local.pixelSize *= scale;
        local.outlineWidth *= scale;
        local.shadowOffset *= scale;
        local.shadowBlur *= scale;
        local.bgPadding *= scale;
        applyTextOp(overlay, local);
    }

    QImage straight = overlay.convertToFormat(QImage::Format_ARGB32);
    QImage loc(w, h, QImage::Format_RGBA64);
    for (int y = 0; y < h; ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(straight.constScanLine(y));
        QRgba64 *dst = reinterpret_cast<QRgba64 *>(loc.scanLine(y));
        uchar *covLine = cov.data() + size_t(y) * w;
        for (int x = 0; x < w; ++x) {
            const QRgb px = src[x];
            covLine[x] = uchar(qAlpha(px));
            dst[x] = qRgba64(quint16(qRed(px) * 257), quint16(qGreen(px) * 257),
                             quint16(qBlue(px) * 257), 65535);
        }
    }
    return loc;
}

QImage applyLayerContent(const QImage &src, const MaskAdjust &a); // fwd decl

// Scale `src` to cover a w×h target (aspect-fill) and crop to exactly that
// size. `offset` pans the crop window, with (-1,-1) = top-left, (0,0) =
// centered, (+1,+1) = bottom-right.
QImage coverFit(const QImage &src, int w, int h, const QPointF &offset) {
    if (src.isNull() || w <= 0 || h <= 0) return QImage();
    QImage scaled = src.scaled(w, h, Qt::KeepAspectRatioByExpanding,
                               Qt::SmoothTransformation);
    const int maxX = std::max(0, scaled.width() - w);
    const int maxY = std::max(0, scaled.height() - h);
    const double ox = clampd(offset.x(), -1.0, 1.0);
    const double oy = clampd(offset.y(), -1.0, 1.0);
    const int x = std::clamp(int(std::lround(maxX / 2.0 + ox * maxX / 2.0)), 0, maxX);
    const int y = std::clamp(int(std::lround(maxY / 2.0 + oy * maxY / 2.0)), 0, maxY);
    return scaled.copy(x, y, w, h);
}

QRectF imageLayerFrame(int canvasW, int canvasH, const Mask &m) {
    const double w = std::max(1, int(std::lround(canvasW * std::max(0.01, m.sourceImageScale.x()))));
    const double h = std::max(1, int(std::lround(canvasH * std::max(0.01, m.sourceImageScale.y()))));
    const double cx = canvasW * (0.5 + 0.5 * clampd(m.sourceImageOffset.x(), -1.0, 1.0));
    const double cy = canvasH * (0.5 + 0.5 * clampd(m.sourceImageOffset.y(), -1.0, 1.0));
    return QRectF(cx - w / 2.0, cy - h / 2.0, w, h);
}

// Mask::Shape/TextBox fields mirror ShapeOp/TextOp exactly (see the
// TODO(shape-layer migration stage B) comments on Mask in Adjustments.h), so
// rendering reuses ShapeTool.cpp's/TextTool.cpp's existing drawing code
// (applyShapeOp/applyTextOp) rather than duplicating it — these just repack
// the flattened Mask fields into the op structs those functions expect.
ShapeOp maskToShapeOp(const Mask &m) {
    ShapeOp op;
    op.type = m.shapeType;
    op.rect = m.shapeRect;
    op.p1 = m.shapeP1;
    op.p2 = m.shapeP2;
    op.rotation = m.shapeRotation;
    op.sides = m.shapeSides;
    op.innerRadiusRatio = m.shapeInnerRadiusRatio;
    op.fillEnabled = m.shapeFillEnabled;
    op.fillColor = m.shapeFillColor;
    if (m.isShapeImageFilled() && !m.shapeImageCache.isNull())
        op.fillImage = &m.shapeImageCache;
    op.strokeEnabled = m.shapeStrokeEnabled;
    op.strokeColor = m.shapeStrokeColor;
    op.strokeWidth = m.shapeStrokeWidth;
    op.opacity = 1.0; // Mask::opacity is applied by applyMasks' generic weight/opacity path
    op.visible = true; // visibility already gates entry into applyMasks' loop
    return op;
}

TextOp maskToTextOp(const Mask &m) {
    TextOp op;
    op.pos = m.textBoxPos;
    op.rotation = m.textBoxRotation;
    op.text = m.textBoxText;
    op.family = m.textBoxFamily;
    op.pixelSize = m.textBoxPixelSize;
    op.bold = m.textBoxBold;
    op.italic = m.textBoxItalic;
    op.color = m.textBoxColor;
    op.outlineEnabled = m.textBoxOutlineEnabled;
    op.outlineColor = m.textBoxOutlineColor;
    op.outlineWidth = m.textBoxOutlineWidth;
    op.shadowEnabled = m.textBoxShadowEnabled;
    op.shadowOffset = m.textBoxShadowOffset;
    op.shadowBlur = m.textBoxShadowBlur;
    op.shadowOpacity = m.textBoxShadowOpacity;
    op.shadowColor = m.textBoxShadowColor;
    op.bgEnabled = m.textBoxBgEnabled;
    op.bgColor = m.textBoxBgColor;
    op.bgOpacity = m.textBoxBgOpacity;
    op.bgPadding = m.textBoxBgPadding;
    return op;
}

// Apply all layers, blending each layer's full tone/colour/detail content
// into the composite-so-far by its per-pixel mask weight, opacity, and blend
// mode. Image layers substitute a cover-fit of their own source photo for
// "the composite so far" as the input to that tone pass.
//
// Iterated back-to-front: `masks` is stored/displayed top-of-stack-first (the
// LayersPanel list shows index 0 at the top row), but compositing must apply
// the bottom-most layer first so a higher layer paints over a lower one —
// hence the reverse loop.
// StaticOnly/InteractiveOnly let a caller run just one tier as its own pass
// (see applyPaintMasks below, kept as a standalone utility/for tests); the
// main render path (applyAdjustments) always uses MaskPass::All so every
// layer — Paint/Shape/TextBox included — composites in true stack order
// against every other layer, regardless of tier. Reordering a Background (or
// any static-tier layer) above or below a Paint/Shape/TextBox layer changes
// the render exactly as it would in any other layer-based editor.
enum class MaskPass { All, StaticOnly, InteractiveOnly };

void applyMasks(QImage &img, const QVector<Mask> &masks,
               QVector<BrushRasterCache> *brushCache = nullptr,
               int snapshotAfterIndex = -1, QImage *snapshotOut = nullptr,
               MaskPass pass = MaskPass::All,
               const QTransform &orientedToGeom = QTransform(),
               double geomRotationDeg = 0.0, double scale = 1.0,
               // `belowSnapshotIndex`/`belowSnapshotOut`: like snapshotAfterIndex/
               // snapshotOut, but captured *before* masks[belowSnapshotIndex] is
               // composited (i.e. excludes it) rather than after (includes it).
               // Used to cache "everything below the layer currently being
               // interactively edited" so a later call can resume compositing
               // from there via resumeFromIndex/resumeImg instead of redoing
               // the whole stack. Zero extra cost when < 0.
               int belowSnapshotIndex = -1, QImage *belowSnapshotOut = nullptr,
               // When resumeImg is given, compositing starts from *resumeImg
               // (instead of the caller's `img`) at `resumeFromIndex` (instead
               // of the top of the stack) — i.e. resumes a previous call that
               // captured a belowSnapshot at the same index.
               int resumeFromIndex = -1, const QImage *resumeImg = nullptr,
               // Opt-in layer-group compositing: when non-null, a contiguous
               // run of masks sharing a non-empty groupId with a matching
               // MaskGroup entry is composited as one virtual layer — its
               // members blend amongst themselves first (recursing into this
               // same function over just that index range), then the whole
               // result is blended into the backdrop using the *group's own*
               // opacity/blend/visibility, instead of each member blending
               // into the backdrop independently. Left null (the default) by
               // every call site except the top-level settled/full render —
               // interactive drag-preview fast paths intentionally keep the
               // legacy flat behavior (see applyAdjustments) rather than
               // paying for group-span detection on every live redraw.
               const QVector<MaskGroup> *groups = nullptr,
               // Inclusive lower bound for the loop, used internally when
               // recursing into just a group's own member span; leave at the
               // default (0) to composite the whole stack, as before.
               int stopAtIndex = 0,
               // Set only when the Paint-layer dirty-rect fast path (below)
               // was used, to the exact pixel rect that changed from the
               // caller's `img`/`resumeImg` - lets a caller that only cares
               // about the visible delta (ImageCanvas::setImage) skip
               // re-processing the rest of the buffer too. Left untouched
               // (caller must treat a default/invalid QRect as "unknown,
               // assume everything changed") whenever the fast path wasn't
               // used for any mask this call.
               QRect *dirtyRectOut = nullptr) {
    if (resumeImg) img = *resumeImg;
    const int w = img.width(), h = img.height();
    if (w == 0 || h == 0) return;
    const double W = w;
    std::vector<uchar> cov;
    if (brushCache && brushCache->size() != masks.size())
        brushCache->resize(masks.size());
    const int startMi = (resumeFromIndex >= 0)
                            ? std::min(resumeFromIndex, int(masks.size()) - 1)
                            : masks.size() - 1;
    // Precompute contiguous group spans (hiIdx -> loIdx), keyed by the
    // highest index in each span, since the loop below counts mi DOWN from
    // startMi to 0 (index 0 is the topmost layer) — the highest index in a
    // group's contiguous range is the first member this loop reaches.
    QHash<int, int> groupSpanLoByHi;
    QHash<int, const MaskGroup *> groupByHi;
    if (groups) {
        for (int i = 0; i < masks.size();) {
            const QString &gid = masks[i].groupId;
            if (gid.isEmpty()) { ++i; continue; }
            int hi = i;
            while (hi + 1 < masks.size() && masks[hi + 1].groupId == gid) ++hi;
            const MaskGroup *grp = nullptr;
            for (const MaskGroup &g : *groups)
                if (g.id == gid) { grp = &g; break; }
            if (grp) {
                groupSpanLoByHi[hi] = i;
                groupByHi[hi] = grp;
            }
            i = hi + 1;
        }
    }
    for (int mi = startMi; mi >= stopAtIndex; --mi) {
        if (groupByHi.contains(mi)) {
            const MaskGroup &grp = *groupByHi[mi];
            const int loIdx = groupSpanLoByHi[mi];
            if (grp.visible && grp.opacity > 0.0) {
                QImage before = img;
                QImage groupImg = img;
                applyMasks(groupImg, masks, brushCache, -1, nullptr, pass,
                          orientedToGeom, geomRotationDeg, scale, -1, nullptr,
                          /*resumeFromIndex=*/mi, /*resumeImg=*/nullptr,
                          /*groups=*/nullptr, /*stopAtIndex=*/loIdx);
                const double gwgt = clampd(grp.opacity, 0.0, 1.0);
                const bool isHSL = grp.blend == BlendMode::Hue || grp.blend == BlendMode::Saturation ||
                                   grp.blend == BlendMode::Color || grp.blend == BlendMode::Luminosity;
                for (int y = 0; y < h; ++y) {
                    QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
                    const QRgba64 *beforeLine = reinterpret_cast<const QRgba64 *>(before.scanLine(y));
                    const QRgba64 *groupLine = reinterpret_cast<const QRgba64 *>(groupImg.scanLine(y));
                    for (int x = 0; x < w; ++x) {
                        QRgba64 b = beforeLine[x], g = groupLine[x];
                        double br, bg, bb;
                        if (isHSL) {
                            QRgba64 blended = blendHSLTriple(grp.blend, b, g);
                            br = blended.red(); bg = blended.green(); bb = blended.blue();
                        } else {
                            br = blendChannel(grp.blend, b.red(), g.red());
                            bg = blendChannel(grp.blend, b.green(), g.green());
                            bb = blendChannel(grp.blend, b.blue(), g.blue());
                        }
                        const double inv = 1.0 - gwgt;
                        line[x] = qRgba64(
                            clamp16(int(std::lround(b.red() * inv + br * gwgt))),
                            clamp16(int(std::lround(b.green() * inv + bg * gwgt))),
                            clamp16(int(std::lround(b.blue() * inv + bb * gwgt))),
                            clamp16(int(std::lround(b.alpha() * inv + g.alpha() * gwgt))));
                    }
                }
            }
            // else: group hidden/fully-transparent — img already holds the
            // correct result (its state entering the group), so just skip
            // every member without compositing them.
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            mi = loIdx; // loop's own --mi lands on loIdx-1 next iteration
            continue;
        }
        if (mi == belowSnapshotIndex && belowSnapshotOut) *belowSnapshotOut = img;
        const Mask &m = masks[mi];
        // Shared by the full per-pixel blend loop below and the drag-preview
        // dirty-rect fast path (see the Paint-layer short-circuit further
        // down) so both stay byte-for-byte identical instead of the fast
        // path silently drifting from the real blend-mode math over time.
        auto blendPixel = [&](QRgba64 src, QRgba64 locPx, double wgt) -> QRgba64 {
            double br, bg, bb;
            const bool isHSL = m.blend == BlendMode::Hue || m.blend == BlendMode::Saturation ||
                               m.blend == BlendMode::Color || m.blend == BlendMode::Luminosity;
            if (isHSL) {
                QRgba64 blended = blendHSLTriple(m.blend, src, locPx);
                br = blended.red();
                bg = blended.green();
                bb = blended.blue();
            } else {
                br = blendChannel(m.blend, src.red(), locPx.red());
                bg = blendChannel(m.blend, src.green(), locPx.green());
                bb = blendChannel(m.blend, src.blue(), locPx.blue());
            }
            const double inv = 1.0 - wgt;
            return qRgba64(
                clamp16(int(std::lround(src.red() * inv + br * wgt))),
                clamp16(int(std::lround(src.green() * inv + bg * wgt))),
                clamp16(int(std::lround(src.blue() * inv + bb * wgt))),
                clamp16(int(std::lround(src.alpha() * inv + 65535.0 * wgt))));
        };
        const bool interactiveType = m.type == MaskType::Paint ||
                                     m.type == MaskType::Shape ||
                                     m.type == MaskType::TextBox;
        if ((pass == MaskPass::StaticOnly && interactiveType) ||
            (pass == MaskPass::InteractiveOnly && !interactiveType)) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        if (!m.visible || m.opacity <= 0.0) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        const bool imageLayer = m.isImageLayer();
        const bool backgroundLayer = m.isBackgroundLayer();
        if (imageLayer && (m.sourceMissing || m.sourceImageCache.isNull())) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        if (backgroundLayer && m.sourceImageCache.isNull()) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        const bool paintLayer = m.type == MaskType::Paint;
        const bool textLayer = m.type == MaskType::Text;
        const bool shapeLayer = m.type == MaskType::Shape;
        const bool textBoxLayer = m.type == MaskType::TextBox;
        if (!imageLayer && !backgroundLayer && !paintLayer && !textLayer && !shapeLayer &&
            !textBoxLayer && m.adj.isZero() && m.eraseStrokes.isEmpty()) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        if (m.type == MaskType::Brush && m.stroke.isEmpty() && m.eraseStrokes.isEmpty()) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        if (paintLayer && m.stroke.isEmpty() && m.fillMask.isNull() && m.eraseStrokes.isEmpty()) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        if (textLayer && m.text.trimmed().isEmpty() && m.eraseStrokes.isEmpty()) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        if (textBoxLayer && m.textBoxText.trimmed().isEmpty() && m.eraseStrokes.isEmpty()) {
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        std::vector<QRgb> colBuf;
        std::vector<uchar> eraseBufOut;
        BrushRasterCache *bc = brushCache ? &(*brushCache)[mi] : nullptr;
        // Only meaningful for Paint-type masks (see rasterizeBrush's
        // touchedRectOut/incrementalOut doc comment); left at defaults
        // (empty rect, false) for Brush-type adjustment masks below, which
        // don't use the dirty-rect fast path.
        QRect touchedRect;
        bool incremental = false;
        if (m.type == MaskType::Brush || paintLayer)
            rasterizeBrush(m, cov, w, h, &img, bc, paintLayer ? &colBuf : nullptr,
                           /*populateOut=*/false, paintLayer ? &eraseBufOut : nullptr,
                           paintLayer ? &touchedRect : nullptr, paintLayer ? &incremental : nullptr);
        // Drag-preview dirty-rect fast path: applyMasks' paintLayer
        // compositing below is O(w*h) per call regardless of how small the
        // new dab is, which makes every mouse-move frame while actively
        // painting expensive. While dragging (resumeImg set - see
        // applyAdjustments/RenderWorker::renderDragFrame) with no erase
        // strokes added this frame, only the bounding box around the newest
        // dab(s) actually changed (rasterizeBrush's incremental cache only
        // ever grows coverage there - see stampDab's max-combine), so patch
        // just that region into the previous frame's fully-composited result
        // (BrushRasterCache::lastComposite) instead. A bucket-fill/
        // Ctrl+Backspace `fillMask` is fine too, as long as it's the same one
        // (by cacheKey) the cache was last fully primed against — see
        // BrushRasterCache::fillMaskCacheKey. Falls through to the normal
        // full-buffer path below whenever the fast path isn't usable (first
        // frame of a stroke, cache had to rebuild from scratch, erase
        // present, the fill just changed, or no cached composite yet); that
        // path re-primes lastComposite (and the fill cache) for the next
        // frame.
        const bool hasFillNow = !m.fillMask.isNull();
        const bool fillCacheUsable = !hasFillNow ||
            (bc && bc->fillScaledValid && bc->fillMaskCacheKey == m.fillMask.cacheKey() &&
             bc->fillScaledCache.size() == img.size());
        if (paintLayer && bc && resumeImg && m.eraseStrokes.isEmpty() && fillCacheUsable &&
            incremental && bc->lastCompositeValid && bc->lastComposite.size() == img.size()) {
            if (touchedRect.isEmpty()) {
                // No new points landed this frame (e.g. a duplicate/
                // sub-threshold mouse-move) - the previous composite is
                // still exactly correct, nothing to redo.
                img = bc->lastComposite;
            } else {
                img = bc->lastComposite;
                img.detach(); // one deliberate copy now, not one per scanLine() write below
                // Mirrors the hasStroke=true specialization of the general
                // paintFinalCov/blend computation below (fc from the cached
                // scaled fill, sc from this dab's coverage), so the two stay
                // numerically identical; only restricted to touchedRect
                // instead of the whole buffer. `patchLoc` plays the same role
                // as `loc` there (paintColor fallback fill, then fill/dab
                // color written wherever coverage > 0).
                QImage patchLoc(w, h, QImage::Format_RGBA64);
                patchLoc.fill(m.paintColor);
                const int x0 = touchedRect.left(), x1 = touchedRect.right();
                const int y0 = touchedRect.top(), y1 = touchedRect.bottom();
                std::vector<uchar> patchFinalCov(size_t(touchedRect.width()) * touchedRect.height(), 0);
                for (int y = y0; y <= y1; ++y) {
                    QRgba64 *line = reinterpret_cast<QRgba64 *>(patchLoc.scanLine(y));
                    const QRgb *fillLine = hasFillNow
                        ? reinterpret_cast<const QRgb *>(bc->fillScaledCache.constScanLine(y))
                        : nullptr;
                    for (int x = x0; x <= x1; ++x) {
                        const size_t idx = size_t(y) * w + x;
                        const uchar sc = bc->cov[idx];
                        const uchar fc = hasFillNow ? uchar(qAlpha(fillLine[x])) : 0;
                        if (sc == 0 && fc == 0) continue;
                        int r = 0, g = 0, b = 0;
                        if (fc > 0) {
                            const QRgb fcColor = fillLine[x];
                            r = qRed(fcColor);
                            g = qGreen(fcColor);
                            b = qBlue(fcColor);
                        }
                        if (sc > 0) {
                            const QRgb c = bc->col[idx];
                            const double a = sc / 255.0;
                            r = int(qRed(c) * a + r * (1.0 - a));
                            g = int(qGreen(c) * a + g * (1.0 - a));
                            b = int(qBlue(c) * a + b * (1.0 - a));
                        }
                        const uchar finalCov = uchar(std::min(255, fc + sc - (fc * sc) / 255));
                        patchFinalCov[size_t(y - y0) * touchedRect.width() + (x - x0)] = finalCov;
                        line[x] = qRgba64(quint16(r * 257), quint16(g * 257), quint16(b * 257),
                                          line[x].alpha());
                    }
                }
                const double op = clampd(m.opacity, 0.0, 1.0);
                for (int y = y0; y <= y1; ++y) {
                    QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
                    const QRgba64 *locLine = reinterpret_cast<const QRgba64 *>(patchLoc.constScanLine(y));
                    for (int x = x0; x <= x1; ++x) {
                        const uchar finalCov =
                            patchFinalCov[size_t(y - y0) * touchedRect.width() + (x - x0)];
                        if (finalCov == 0) continue;
                        const double wgt = (finalCov / 255.0) * op;
                        if (wgt <= 0.0) continue;
                        line[x] = blendPixel(line[x], locLine[x], wgt);
                    }
                }
            }
            bc->lastComposite = img;
            bc->lastCompositeValid = true;
            if (dirtyRectOut && !touchedRect.isEmpty()) *dirtyRectOut = touchedRect;
            if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
            continue;
        }
        // Erase-tool coverage: applies uniformly to this layer's final
        // compositing weight regardless of layer type (image, background,
        // paint, brush, shape, text box, text, or an adjustment mask), so
        // "erase" works on anything, not just image-layer pixels.
        std::vector<double> eraseCov;
        if (!m.eraseStrokes.isEmpty()) {
            eraseCov.assign(size_t(w) * h, 0.0);
            const bool hasClip = !m.selectionClipNorm.isEmpty();
            const std::vector<uchar> featherAlpha = selectionFeatherAlpha(m, w, h);
            for (const ErasePoint &ep : m.eraseStrokes) {
                const double px = ep.pt.x() * W, py = ep.pt.y() * W;
                const double rad = std::max(0.5, ep.radius * W);
                const int x0 = std::max(0, int(px - rad));
                const int x1 = std::min(w - 1, int(px + rad));
                const int y0 = std::max(0, int(py - rad));
                const int y1 = std::min(h - 1, int(py + rad));
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        double dx = x - px, dy = y - py;
                        double dist = std::sqrt(dx * dx + dy * dy);
                        double v = dist >= rad ? 0.0
                                                : smoothstep01((rad - dist) / rad);
                        if (v > 0.0 && hasClip) {
                            if (!featherAlpha.empty())
                                v *= featherAlpha[size_t(y) * w + x] / 255.0;
                            else if (!m.selectionClipNorm.contains(QPointF(x / W, y / W)))
                                v = 0.0;
                        }
                        // Union-combine overlapping dabs (screen blend)
                        // rather than max: each dab's own smoothstep falloff
                        // only reaches 1.0 exactly at its center, so with
                        // max, pixels between two overlapping-but-offset dab
                        // centers could never reach full coverage, leaving a
                        // faint uniform residue along the whole stroke.
                        double &c = eraseCov[size_t(y) * w + x];
                        c = 1.0 - (1.0 - c) * (1.0 - v);
                    }
                }
            }
        }
        QImage loc;
        if (imageLayer) {
            QRectF frame = imageLayerFrame(w, h, m);
            QImage fitted = coverFit(m.sourceImageCache, std::max(1, int(std::lround(frame.width()))),
                                     std::max(1, int(std::lround(frame.height()))),
                                     QPointF());
            loc = QImage(w, h, QImage::Format_ARGB32);
            loc.fill(Qt::transparent);
            QPainter p(&loc);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(frame.topLeft(), fitted);
            p.end();
            // Heal ops owned by this layer (see Mask::heals) — applied in the
            // same untoned, oriented-image/pre-crop pixel space as the global
            // Adjustments::heals, but only to this layer's own content, so a
            // duplicated/independent layer can be healed without touching the
            // original it was copied from.
            if (!m.heals.isEmpty()) applyHeal(loc, m.heals);
            loc = applyLayerContent(loc, m.adj);
        } else if (backgroundLayer) {
            // Full-frame, non-repositionable: just the tab's base photo
            // (already sized to the canvas), tone-adjusted by this layer's
            // own `adj` like any other layer's content.
            QImage src = m.sourceImageCache;
            if (src.width() != w || src.height() != h)
                src = src.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            loc = applyLayerContent(src, m.adj);
            // Ctrl+Backspace ("fill with color") on the Background layer
            // paints directly into its own content rather than adding a new
            // layer, reusing `stroke`/`paintColor` the same way a Paint mask
            // does. Composited here (straight-alpha over `loc`) so it stays
            // undo-safe (lives in `m_adj.masks`) without a synthetic layer.
            if (!m.stroke.isEmpty()) {
                std::vector<uchar> fillCov;
                std::vector<QRgb> fillCol;
                rasterizeBrush(m, fillCov, w, h, nullptr, nullptr, &fillCol);
                for (int y = 0; y < h; ++y) {
                    QRgba64 *line = reinterpret_cast<QRgba64 *>(loc.scanLine(y));
                    for (int x = 0; x < w; ++x) {
                        const size_t idx = size_t(y) * w + x;
                        if (fillCov[idx] == 0) continue;
                        const double a = fillCov[idx] / 255.0;
                        const QRgb c = fillCol[idx];
                        const QRgba64 dst = line[x];
                        const double dstA = dst.alpha() / 65535.0;
                        const double outA = a + dstA * (1.0 - a);
                        if (outA <= 0.0) continue;
                        const double sr = qRed(c) * 257.0, sg = qGreen(c) * 257.0,
                                    sb = qBlue(c) * 257.0;
                        line[x] = qRgba64(
                            clamp16(int(std::lround((sr * a + dst.red() * dstA * (1.0 - a)) / outA))),
                            clamp16(int(std::lround((sg * a + dst.green() * dstA * (1.0 - a)) / outA))),
                            clamp16(int(std::lround((sb * a + dst.blue() * dstA * (1.0 - a)) / outA))),
                            clamp16(int(std::lround(outA * 65535.0))));
                    }
                }
            }
        } else if (paintLayer) {
            loc = QImage(w, h, QImage::Format_RGBA64);
            loc.fill(m.paintColor); // fallback fill; per-dab colors applied below
        } else if (shapeLayer || textBoxLayer) {
            // Shape/TextBox carry their own fully-rendered colour (fill,
            // stroke, glyphs, outline, shadow, background) — rasterize
            // straight into `loc`/`cov` below instead of going through
            // applyLayerContent (which would tone-adjust the composite-so-
            // far, not this layer's own drawn content).
            loc = rasterizeShapeOrTextBox(m, cov, w, h, orientedToGeom, geomRotationDeg, scale);
        } else if (m.isGradientFill && (m.type == MaskType::Radial || m.type == MaskType::Linear)) {
            loc = renderGradientFill(m, w, h);
        } else {
            loc = applyLayerContent(img, m.adj);
        }
        // bc/colBuf/eraseBufOut/rasterizeBrush already ran earlier (right
        // after the early-exit checks, before the dirty-rect fast-path
        // short-circuit above); only the text case still needs handling here.
        if (textLayer)
            rasterizeText(m, cov, w, h); // no incremental cache — cheap to redo each render
        // shapeLayer/textBoxLayer already populated `cov` above (in
        // rasterizeShapeOrTextBox), read straight from it, same as text.
        // With a cache, rasterizeBrush left the up-to-date buffers in the
        // cache itself (see populateOut above) — read from there directly to
        // avoid a full-image copy of masks that didn't change this frame.
        // Text never uses the cache (it's not brush/paint), so always read
        // straight from `cov`.
        const std::vector<uchar> &covRead =
            (bc && (m.type == MaskType::Brush || paintLayer)) ? bc->cov : cov;
        const std::vector<QRgb> &colRead = bc ? bc->col : colBuf;
        const std::vector<uchar> &eraseRead = bc ? bc->erase : eraseBufOut;
        // Paint bucket: cumulative flood-filled regions, resampled to this
        // render's resolution and alpha-composited under the stroke
        // coverage/color above — brush dabs are always painted on top of a
        // bucket fill (strokes are the "later" edit), instead of only the
        // pixel with higher raw coverage winning.
        std::vector<uchar> paintFinalCov;
        QImage fillScaled;
        const bool hasFill = paintLayer && !m.fillMask.isNull();
        if (hasFill)
            fillScaled = m.fillMask.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32);
        if (paintLayer) {
            const bool hasStroke = bc ? !bc->cov.empty() : !colBuf.empty();
            paintFinalCov.assign(size_t(w) * h, 0);
            for (int y = 0; y < h; ++y) {
                QRgba64 *line = reinterpret_cast<QRgba64 *>(loc.scanLine(y));
                const QRgb *fillLine =
                    hasFill ? reinterpret_cast<const QRgb *>(fillScaled.constScanLine(y)) : nullptr;
                for (int x = 0; x < w; ++x) {
                    const size_t idx = size_t(y) * w + x;
                    const uchar sc = hasStroke ? covRead[idx] : 0;
                    uchar fc = hasFill ? uchar(qAlpha(fillLine[x])) : 0;
                    // An erase dab must cut through bucket-fill coverage too,
                    // not just stroke coverage — otherwise erasing over a
                    // filled region has no visible effect (fc dominates the
                    // union below unchanged). See eraseRead/BrushRasterCache::erase.
                    if (fc > 0 && !eraseRead.empty())
                        fc = uchar(fc * (255 - eraseRead[idx]) / 255);
                    int r = 0, g = 0, b = 0;
                    if (fc > 0) {
                        QRgb c = fillLine[x];
                        r = qRed(c);
                        g = qGreen(c);
                        b = qBlue(c);
                    }
                    if (sc > 0) {
                        // Each dab's own color (captured at paint time) wins
                        // wherever it set the max coverage, so re-picking the
                        // color later only affects new dabs, not ones already
                        // painted.
                        QRgb c = colRead[idx];
                        const double a = sc / 255.0;
                        r = int(qRed(c) * a + r * (1.0 - a));
                        g = int(qGreen(c) * a + g * (1.0 - a));
                        b = int(qBlue(c) * a + b * (1.0 - a));
                    }
                    const uchar finalCov = uchar(std::min(255, fc + sc - (fc * sc) / 255));
                    paintFinalCov[idx] = finalCov;
                    if (finalCov > 0)
                        line[x] = qRgba64(quint16(r * 257), quint16(g * 257), quint16(b * 257),
                                          line[x].alpha());
                }
            }
        }
        const double op = clampd(m.opacity, 0.0, 1.0);
        for (int y = 0; y < h; ++y) {
            QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
            const QRgba64 *locLine = reinterpret_cast<const QRgba64 *>(loc.scanLine(y));
            for (int x = 0; x < w; ++x) {
                double wgt;
                // Gradient Fill uses its Radial/Linear weight field to choose
                // this layer's own per-pixel color (see renderGradientFill),
                // not as this layer's alpha — it covers the whole canvas at
                // full opacity, like a real gradient tool, rather than fading
                // to transparent past the feather point the way an ordinary
                // Radial/Linear adjustment mask does.
                if (m.type == MaskType::None || backgroundLayer ||
                    (m.isGradientFill && (m.type == MaskType::Radial || m.type == MaskType::Linear)))
                    wgt = 1.0;
                else if (m.type == MaskType::Radial)
                    wgt = radialWeight(m, x / W, y / W);
                else if (m.type == MaskType::Linear)
                    wgt = linearWeight(m, x / W, y / W);
                else if (paintLayer)
                    wgt = paintFinalCov[size_t(y) * w + x] / 255.0;
                else
                    wgt = covRead[size_t(y) * w + x] / 255.0;
                if (imageLayer || backgroundLayer)
                    wgt *= locLine[x].alpha() / 65535.0;
                if (!eraseCov.empty())
                    wgt *= (1.0 - eraseCov[size_t(y) * w + x]);
                wgt *= op;
                if (wgt <= 0.0) continue;
                line[x] = blendPixel(line[x], locLine[x], wgt);
            }
        }
        // Prime/refresh the dirty-rect fast path's cache (see above) with
        // this frame's full result, so the *next* drag frame for this mask
        // can patch instead of recomposite. Skipped (and any stale value
        // invalidated) whenever erase strokes are present, since the fast
        // path's append-only assumption doesn't hold then. A bucket-fill/
        // Ctrl+Backspace fill doesn't disqualify the cache — it's static
        // across a drag — but its scaled copy + cacheKey are refreshed here
        // too, so the fast path can tell a *new* fill apart from this one.
        if (paintLayer && bc) {
            if (hasFill) {
                bc->fillScaledCache = fillScaled;
                bc->fillMaskCacheKey = m.fillMask.cacheKey();
                bc->fillScaledValid = true;
            } else {
                bc->fillScaledValid = false;
            }
            if (m.eraseStrokes.isEmpty()) {
                bc->lastComposite = img;
                bc->lastCompositeValid = true;
            } else {
                bc->lastCompositeValid = false;
            }
        }
        if (mi == snapshotAfterIndex && snapshotOut) *snapshotOut = img;
    }
}

// Separable moving-average blur — a fast approximation of a Gaussian, used for
// clarity (large radius) and sharpening (small radius). radius in pixels.
QImage boxBlur(const QImage &src, int radius) {
    if (radius < 1) return src;
    const int w = src.width(), h = src.height();
    QImage tmp(w, h, QImage::Format_RGBA64);
    QImage dst(w, h, QImage::Format_RGBA64);
    const int win = radius * 2 + 1;

    // Horizontal pass.
    for (int y = 0; y < h; ++y) {
        const QRgba64 *s = reinterpret_cast<const QRgba64 *>(src.scanLine(y));
        QRgba64 *t = reinterpret_cast<QRgba64 *>(tmp.scanLine(y));
        long sr = 0, sg = 0, sb = 0;
        for (int x = -radius; x <= radius; ++x) {
            const QRgba64 p = s[std::clamp(x, 0, w - 1)];
            sr += p.red(); sg += p.green(); sb += p.blue();
        }
        for (int x = 0; x < w; ++x) {
            t[x] = qRgba64(quint16(sr / win), quint16(sg / win), quint16(sb / win), 0xFFFF);
            const QRgba64 pout = s[std::clamp(x - radius, 0, w - 1)];
            const QRgba64 pin = s[std::clamp(x + radius + 1, 0, w - 1)];
            sr += pin.red() - pout.red();
            sg += pin.green() - pout.green();
            sb += pin.blue() - pout.blue();
        }
    }
    // Vertical pass.
    for (int x = 0; x < w; ++x) {
        long sr = 0, sg = 0, sb = 0;
        for (int y = -radius; y <= radius; ++y) {
            const QRgba64 p = reinterpret_cast<const QRgba64 *>(
                tmp.scanLine(std::clamp(y, 0, h - 1)))[x];
            sr += p.red(); sg += p.green(); sb += p.blue();
        }
        for (int y = 0; y < h; ++y) {
            reinterpret_cast<QRgba64 *>(dst.scanLine(y))[x] =
                qRgba64(quint16(sr / win), quint16(sg / win), quint16(sb / win), 0xFFFF);
            const QRgba64 pout = reinterpret_cast<const QRgba64 *>(
                tmp.scanLine(std::clamp(y - radius, 0, h - 1)))[x];
            const QRgba64 pin = reinterpret_cast<const QRgba64 *>(
                tmp.scanLine(std::clamp(y + radius + 1, 0, h - 1)))[x];
            sr += pin.red() - pout.red();
            sg += pin.green() - pout.green();
            sb += pin.blue() - pout.blue();
        }
    }
    return dst;
}

// Chroma-only noise reduction, weighted toward shadows: blurs a copy of the
// image, then for each pixel blends its chroma (colour, independent of luma)
// toward the blurred chroma, leaving luma untouched so edges/detail survive.
// The blend strength falls off as (1 - luma)^2, the same shape used for the
// `shadows` slider in applyTone, so near-black speckle gets smoothed heavily
// while midtones/highlights are barely touched.
void applyDenoise(QImage &img, int denoise) {
    if (denoise <= 0) return;
    const int w = img.width(), h = img.height();
    int radius = std::max(2, std::min(w, h) / 150);
    QImage blur = boxBlur(img, radius);
    double amt = denoise / 100.0;
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
        const QRgba64 *bl = reinterpret_cast<const QRgba64 *>(blur.scanLine(y));
        for (int x = 0; x < w; ++x) {
            QRgba64 p = line[x], q = bl[x];
            double r = p.red(), g = p.green(), b = p.blue();
            double br = q.red(), bg = q.green(), bb = q.blue();
            double luma = 0.299 * r + 0.587 * g + 0.114 * b;
            double blLuma = 0.299 * br + 0.587 * bg + 0.114 * bb;
            double shadowWeight = 1.0 - clampd(luma / 65535.0, 0.0, 1.0);
            shadowWeight *= shadowWeight;
            double k = amt * shadowWeight;
            double cr = (r - luma) + k * ((br - blLuma) - (r - luma));
            double cg = (g - luma) + k * ((bg - blLuma) - (g - luma));
            double cb = (b - luma) + k * ((bb - blLuma) - (b - luma));
            line[x] = qRgba64(clamp16(int(std::lround(luma + cr))),
                              clamp16(int(std::lround(luma + cg))),
                              clamp16(int(std::lround(luma + cb))), p.alpha());
        }
    }
}

// Midtone local-contrast boost (large-radius blur, mixed in by midtone weight).
void applyClarity(QImage &img, int clarity) {
    if (clarity == 0) return;
    const int w = img.width(), h = img.height();
    int radius = std::max(2, std::min(w, h) / 60);
    QImage blur = boxBlur(img, radius);
    double amt = clarity / 100.0;
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
        const QRgba64 *bl = reinterpret_cast<const QRgba64 *>(blur.scanLine(y));
        for (int x = 0; x < w; ++x) {
            QRgba64 p = line[x], q = bl[x];
            double lz = (0.299 * p.red() + 0.587 * p.green() + 0.114 * p.blue()) / 65535.0;
            double mid = 1.0 - std::abs(lz - 0.5) * 2.0; // midtone weight
            double k = amt * mid;
            int r = clamp16(int(p.red() + k * (int(p.red()) - int(q.red()))));
            int g = clamp16(int(p.green() + k * (int(p.green()) - int(q.green()))));
            int b = clamp16(int(p.blue() + k * (int(p.blue()) - int(q.blue()))));
            line[x] = qRgba64(r, g, b, p.alpha());
        }
    }
}

// Small-radius unsharp mask.
void applySharpen(QImage &img, int sharpen) {
    if (sharpen <= 0) return;
    const int w = img.width(), h = img.height();
    QImage blur = boxBlur(img, 1);
    double amt = sharpen / 100.0 * 1.5;
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
        const QRgba64 *bl = reinterpret_cast<const QRgba64 *>(blur.scanLine(y));
        for (int x = 0; x < w; ++x) {
            QRgba64 p = line[x], q = bl[x];
            int r = clamp16(int(p.red() + amt * (int(p.red()) - int(q.red()))));
            int g = clamp16(int(p.green() + amt * (int(p.green()) - int(q.green()))));
            int b = clamp16(int(p.blue() + amt * (int(p.blue()) - int(q.blue()))));
            line[x] = qRgba64(r, g, b, p.alpha());
        }
    }
}

void applyVignette(QImage &img, int vignette) {
    if (vignette == 0) return;
    const int w = img.width(), h = img.height();
    double amt = vignette / 100.0;
    double cx = w / 2.0, cy = h / 2.0;
    double maxd = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            double dx = x - cx, dy = y - cy;
            double d = std::sqrt(dx * dx + dy * dy) / maxd; // 0..1
            double f = 1.0 + amt * d * d; // amt<0 darkens corners
            QRgba64 p = line[x];
            line[x] = qRgba64(clamp16(int(p.red() * f)),
                              clamp16(int(p.green() * f)),
                              clamp16(int(p.blue() * f)), p.alpha());
        }
    }
}

// Artificial directional lighting: derives a fake "height map" from the
// image's own (blurred) luminance, differentiates it to get fake surface
// normals, and shades those normals against a light direction set by `angle`.
// Runs at a downsampled working resolution (<=512px longest edge) so cost is
// independent of source resolution; only the final per-pixel multiply runs
// at full res, matching applyVignette's cost profile.
void applyLighting(QImage &img, int angle, int intensity) {
    if (intensity == 0) return;
    const int w = img.width(), h = img.height();
    if (w < 2 || h < 2) return;

    const int longEdge = std::max(w, h);
    const int workLong = std::min(longEdge, 512);
    const double scale = double(workLong) / double(longEdge);
    const int ww = std::max(2, int(std::lround(w * scale)));
    const int wh = std::max(2, int(std::lround(h * scale)));

    // Downsampled luminance buffer (box-average of the source, cheap enough
    // for a 512px-capped target regardless of source size).
    std::vector<float> luma(size_t(ww) * wh, 0.0f);
    {
        std::vector<double> sum(size_t(ww) * wh, 0.0);
        std::vector<int> count(size_t(ww) * wh, 0);
        for (int y = 0; y < h; ++y) {
            const QRgba64 *line = reinterpret_cast<const QRgba64 *>(img.constScanLine(y));
            int wy = std::min(wh - 1, int(y * scale));
            for (int x = 0; x < w; ++x) {
                const QRgba64 p = line[x];
                int wx = std::min(ww - 1, int(x * scale));
                double l = 0.2126 * p.red() + 0.7152 * p.green() + 0.0722 * p.blue();
                size_t idx = size_t(wy) * ww + wx;
                sum[idx] += l;
                count[idx] += 1;
            }
        }
        for (size_t i = 0; i < sum.size(); ++i)
            luma[i] = count[i] > 0 ? float(sum[i] / count[i]) : 0.0f;
    }

    // Height proxy: small box blur of the luminance buffer (avoids amplifying
    // sensor/JPEG noise into speckle when we differentiate it next).
    auto at = [&](const std::vector<float> &buf, int x, int y) -> float {
        x = std::clamp(x, 0, ww - 1);
        y = std::clamp(y, 0, wh - 1);
        return buf[size_t(y) * ww + x];
    };
    std::vector<float> height(luma.size(), 0.0f);
    {
        const int r = 2;
        for (int y = 0; y < wh; ++y) {
            for (int x = 0; x < ww; ++x) {
                float s = 0.0f;
                int n = 0;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        s += at(luma, x + dx, y + dy);
                        ++n;
                    }
                height[size_t(y) * ww + x] = s / n;
            }
        }
    }

    // Sobel gradient -> fake normal -> Lambertian shading multiplier, kept at
    // working resolution; upsampled (bilinear) into the full-res multiply.
    const double kGradientScale = 1.0 / (8.0 * 65535.0 / 4.0);
    const double kZ = 0.8;   // implied "steepness" of the fake relief
    const double lightZ = 0.5; // light elevation; keeps it off edge-on/overhead
    const double angleRad = angle * M_PI / 180.0;
    double lx = std::cos(angleRad), ly = std::sin(angleRad), lz = lightZ;
    double llen = std::sqrt(lx * lx + ly * ly + lz * lz);
    lx /= llen; ly /= llen; lz /= llen;
    // Baseline dot product for a perfectly flat surface (N = (0,0,1)); a flat
    // image must produce zero shading change, so this is subtracted below.
    const double flatDot = lz;
    const double strength = intensity / 100.0;
    const double kGain = 0.6;

    std::vector<float> factor(luma.size(), 1.0f);
    for (int y = 0; y < wh; ++y) {
        for (int x = 0; x < ww; ++x) {
            double gx = (at(height, x + 1, y - 1) + 2 * at(height, x + 1, y) + at(height, x + 1, y + 1)) -
                        (at(height, x - 1, y - 1) + 2 * at(height, x - 1, y) + at(height, x - 1, y + 1));
            double gy = (at(height, x - 1, y + 1) + 2 * at(height, x, y + 1) + at(height, x + 1, y + 1)) -
                        (at(height, x - 1, y - 1) + 2 * at(height, x, y - 1) + at(height, x + 1, y - 1));
            double dx = gx * kGradientScale, dy = gy * kGradientScale;
            double nx = -dx, ny = -dy, nz = kZ;
            double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= nlen; ny /= nlen; nz /= nlen;
            double ndotl = (nx * lx + ny * ly + nz * lz) - flatDot;
            double f = 1.0 + strength * ndotl * kGain;
            factor[size_t(y) * ww + x] = float(std::clamp(f, 0.3, 2.0));
        }
    }

    // Bilinear-sample the working-res factor map at full resolution and
    // multiply into RGB, same shape as applyVignette's final loop.
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
        double fy = (y + 0.5) * scale - 0.5;
        int y0 = std::clamp(int(std::floor(fy)), 0, wh - 1);
        int y1 = std::min(y0 + 1, wh - 1);
        double ty = std::clamp(fy - y0, 0.0, 1.0);
        for (int x = 0; x < w; ++x) {
            double fx = (x + 0.5) * scale - 0.5;
            int x0 = std::clamp(int(std::floor(fx)), 0, ww - 1);
            int x1 = std::min(x0 + 1, ww - 1);
            double tx = std::clamp(fx - x0, 0.0, 1.0);
            double f00 = factor[size_t(y0) * ww + x0];
            double f10 = factor[size_t(y0) * ww + x1];
            double f01 = factor[size_t(y1) * ww + x0];
            double f11 = factor[size_t(y1) * ww + x1];
            double f = f00 * (1 - tx) * (1 - ty) + f10 * tx * (1 - ty) +
                       f01 * (1 - tx) * ty + f11 * tx * ty;
            QRgba64 p = line[x];
            line[x] = qRgba64(clamp16(int(p.red() * f)),
                              clamp16(int(p.green() * f)),
                              clamp16(int(p.blue() * f)), p.alpha());
        }
    }
}

// Flat-color painterly/posterize stylization. Blurs away fine detail/noise
// (so regions merge into flat shapes rather than posterizing pixel noise),
// then quantizes the image to a small palette via k-means, mapping every
// pixel to its nearest palette color.
void applyFlatStyle(QImage &img, int amount) {
    if (amount <= 0) return;
    amount = std::clamp(amount, 0, 100);
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0) return;

    int blurRadius = 2 + amount / 6; // up to ~18px at amount=100
    QImage blurred = boxBlur(img, blurRadius);

    // Fewer palette colors as amount increases -> more graphic/flat look.
    int paletteSize = std::clamp(28 - amount / 4, 4, 24);

    // k-means on a stride-sampled subset of pixels for speed.
    struct Rgb { double r, g, b; };
    std::vector<Rgb> samples;
    int stride = std::max(1, int(std::sqrt(double(w) * h) / 100.0));
    for (int y = 0; y < h; y += stride) {
        const QRgba64 *line = reinterpret_cast<const QRgba64 *>(blurred.scanLine(y));
        for (int x = 0; x < w; x += stride)
            samples.push_back({double(line[x].red()), double(line[x].green()),
                               double(line[x].blue())});
    }
    if (samples.empty()) return;

    auto dist2 = [](const Rgb &a, const Rgb &b) {
        double dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
        return dr * dr + dg * dg + db * db;
    };

    // Farthest-point (k-means++-style) seeding: spreads initial centroids
    // across the color space instead of clumping on one dominant color.
    std::vector<Rgb> centroids;
    centroids.push_back(samples[0]);
    while (int(centroids.size()) < paletteSize && centroids.size() < samples.size()) {
        double bestDist = -1.0;
        int bestIdx = 0;
        for (int i = 0; i < int(samples.size()); ++i) {
            double d = std::numeric_limits<double>::max();
            for (const Rgb &c : centroids) d = std::min(d, dist2(samples[i], c));
            if (d > bestDist) { bestDist = d; bestIdx = i; }
        }
        centroids.push_back(samples[bestIdx]);
    }

    for (int iter = 0; iter < 6; ++iter) {
        std::vector<double> sumR(centroids.size(), 0), sumG(centroids.size(), 0),
            sumB(centroids.size(), 0);
        std::vector<int> count(centroids.size(), 0);
        for (const Rgb &s : samples) {
            int bestIdx = 0;
            double bestDist = std::numeric_limits<double>::max();
            for (int i = 0; i < int(centroids.size()); ++i) {
                double d = dist2(s, centroids[i]);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            sumR[bestIdx] += s.r; sumG[bestIdx] += s.g; sumB[bestIdx] += s.b;
            count[bestIdx]++;
        }
        for (int i = 0; i < int(centroids.size()); ++i) {
            if (count[i] > 0)
                centroids[i] = {sumR[i] / count[i], sumG[i] / count[i], sumB[i] / count[i]};
        }
    }

    // Map every pixel of the blurred image to its nearest palette color.
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(blurred.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgba64 p = line[x];
            Rgb s{double(p.red()), double(p.green()), double(p.blue())};
            int bestIdx = 0;
            double bestDist = std::numeric_limits<double>::max();
            for (int i = 0; i < int(centroids.size()); ++i) {
                double d = dist2(s, centroids[i]);
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            const Rgb &c = centroids[bestIdx];
            line[x] = qRgba64(clamp16(int(std::lround(c.r))), clamp16(int(std::lround(c.g))),
                              clamp16(int(std::lround(c.b))), p.alpha());
        }
    }

    img = blurred;
}

// Full per-layer content: the same tone/colour/detail pipeline as the global
// pass, applied to `src` in isolation. Used both for the base's global fields
// (via applyAdjustments) and for each additional layer's MaskAdjust — layers
// are just as capable as the base, minus geometry (which stays global).
QImage applyLayerContent(const QImage &src, const MaskAdjust &a) {
    QImage img = src.convertToFormat(QImage::Format_RGBA64);
    const int w = img.width(), h = img.height();
    ToneParams tp = makeToneParams(a.contrast, a.brightness, a.highlights,
                                   a.shadows, a.saturation, a.vibrance,
                                   a.temperature, a.tint, a.wbR, a.wbG, a.wbB,
                                   a.curve, a.levels);
    for (int y = 0; y < h; ++y) {
        QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) line[x] = applyTone(line[x], tp);
    }
    applyDenoise(img, a.denoise);
    applyClarity(img, a.clarity);
    applySharpen(img, a.sharpen);
    applyLighting(img, a.lightAngle, a.lightIntensity);
    applyVignette(img, a.vignette);
    return img;
}

} // namespace

// True if any mask carries a non-zero local adjustment, or is an image layer
// (which has visible content even with identity adjustments).
static bool hasMaskEdits(const Adjustments &adj) {
    for (const Mask &m : adj.masks) {
        if (!m.visible || m.opacity <= 0.0) continue;
        if (m.isImageLayer()) {
            if (m.sourceMissing || m.sourceImageCache.isNull()) continue;
            return true;
        }
        // Background layer: unlike an external image layer, its content is
        // just the tab's own base photo, which is already accounted for
        // outside of the mask stack — so an untouched (default) Background
        // entry contributes no edit of its own; only a non-identity `adj`
        // does (falls through to the generic check below).
        if (m.type == MaskType::Paint) {
            if (m.stroke.isEmpty() && m.fillMask.isNull()) continue;
            return true;
        }
        if (m.type == MaskType::Text) {
            if (m.text.trimmed().isEmpty()) continue;
            return true;
        }
        if (m.type == MaskType::Shape) return true;
        if (m.type == MaskType::TextBox) {
            if (m.textBoxText.trimmed().isEmpty()) continue;
            return true;
        }
        if (m.adj.isZero()) continue;
        if (m.type == MaskType::Brush && m.stroke.isEmpty()) continue;
        return true;
    }
    return false;
}

QImage maskCoverageOverlay(const Mask &m, int w, int h, const QColor &tint,
                           int maxAlpha, const QImage &source,
                           BrushRasterCache *cache) {
    if (w <= 0 || h <= 0) return QImage();
    // Compute at a reduced resolution so live painting stays responsive; the
    // caller scales it up to the display rect.
    const int longEdge = std::max(w, h);
    const double s = longEdge > 512 ? 512.0 / longEdge : 1.0;
    const int ow = std::max(1, int(w * s)), oh = std::max(1, int(h * s));
    QImage img(ow, oh, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    const double W = ow;
    std::vector<uchar> cov;
    if (m.type == MaskType::Brush) {
        if (m.stroke.isEmpty()) return img;
        QImage ref;
        const QImage *refPtr = nullptr;
        if (m.autoMask && !source.isNull()) {
            ref = (source.width() == ow && source.height() == oh)
                      ? source
                      : source.scaled(ow, oh, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
            refPtr = &ref;
        }
        rasterizeBrush(m, cov, ow, oh, refPtr, cache);
    } else if (m.type == MaskType::Text) {
        rasterizeText(m, cov, ow, oh);
    }
    const int tr = tint.red(), tg = tint.green(), tb = tint.blue();
    for (int y = 0; y < oh; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < ow; ++x) {
            double wgt;
            if (m.type == MaskType::None)
                wgt = 1.0;
            else if (m.type == MaskType::Radial)
                wgt = radialWeight(m, x / W, y / W);
            else if (m.type == MaskType::Linear)
                wgt = linearWeight(m, x / W, y / W);
            else
                wgt = cov[size_t(y) * ow + x] / 255.0;
            if (wgt <= 0.0) continue;
            line[x] = qRgba(tr, tg, tb, int(std::lround(wgt * maxAlpha)));
        }
    }
    return img;
}

namespace {
// Classic 4x4 ordered (Bayer) dither matrix, thresholds spread evenly across
// one 8-bit quantization step (256 sub-levels of the 65536 range).
constexpr int kBayer4x4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};
inline quint8 dither16to8(quint16 v, int x, int y) {
    // Bias by a fraction of one output step (256 levels wide in 16-bit terms)
    // before truncating, so quantization error is spread as noise, not bands.
    const int bias = (kBayer4x4[y & 3][x & 3] * 256) / 16;
    return quint8(std::min(255, (v + bias) >> 8));
}
} // namespace

QImage ditherTo8Bit(const QImage &img) {
    if (img.format() != QImage::Format_RGBA64 &&
        img.format() != QImage::Format_RGBX64 &&
        img.format() != QImage::Format_RGBA64_Premultiplied)
        return img.convertToFormat(QImage::Format_ARGB32);

    const int w = img.width(), h = img.height();
    QImage out(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const QRgba64 *src = reinterpret_cast<const QRgba64 *>(img.scanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgba64 p = src[x];
            dst[x] = qRgba(dither16to8(p.red(), x, y), dither16to8(p.green(), x, y),
                           dither16to8(p.blue(), x, y), p.alpha() >> 8);
        }
    }
    return out;
}

void ditherRegionInto(QImage &out, const QImage &img, const QRect &r) {
    if (img.format() != QImage::Format_RGBA64 &&
        img.format() != QImage::Format_RGBX64 &&
        img.format() != QImage::Format_RGBA64_Premultiplied) {
        // Not the 16-bit format the fast dirty-rect path expects to see
        // here (shouldn't normally happen for the live render pipeline's
        // output) - stay correct by falling back to a full convert-and-copy
        // of just the region.
        const QImage conv = img.convertToFormat(QImage::Format_ARGB32);
        for (int y = r.top(); y <= r.bottom(); ++y)
            memcpy(out.scanLine(y) + r.left() * 4, conv.constScanLine(y) + r.left() * 4,
                  size_t(r.width()) * 4);
        return;
    }
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const QRgba64 *src = reinterpret_cast<const QRgba64 *>(img.scanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgba64 p = src[x];
            dst[x] = qRgba(dither16to8(p.red(), x, y), dither16to8(p.green(), x, y),
                           dither16to8(p.blue(), x, y), p.alpha() >> 8);
        }
    }
}

bool Adjustments::hasCurve() const {
    if (curve.size() < 2) return false;
    for (const QPointF &p : curve)
        if (std::abs(p.x() - p.y()) > 1e-4) return true;
    return false;
}

void applyPaintMasks(QImage &img, const QVector<Mask> &masks,
                     QVector<BrushRasterCache> *brushCache,
                     const QTransform &orientedToGeom, double geomRotationDeg,
                     double scale) {
    if (masks.isEmpty()) return;
    applyMasks(img, masks, brushCache, -1, nullptr, MaskPass::InteractiveOnly,
              orientedToGeom, geomRotationDeg, scale);
}

bool hasToneEdits(const Adjustments &adj) {
    return adj.brightness || adj.contrast || adj.highlights || adj.shadows ||
           adj.saturation || adj.vibrance || adj.temperature || adj.tint ||
           adj.denoise || adj.clarity || adj.sharpen || adj.vignette ||
           adj.lightIntensity || adj.flatStyle ||
           std::abs(adj.wbR - 1.0) > 1e-4 || std::abs(adj.wbG - 1.0) > 1e-4 ||
           std::abs(adj.wbB - 1.0) > 1e-4 || adj.hasCurve() ||
           !adj.levels.isIdentity() || !adj.colorRanges.isEmpty() ||
           hasMaskEdits(adj);
}

QImage applyAdjustments(const QImage &base, const Adjustments &adj,
                        QVector<BrushRasterCache> *brushCache,
                        int maskSnapshotIndex, QImage *maskSnapshotOut,
                        const QTransform &orientedToGeom, double geomRotationDeg,
                        double scale,
                        int belowSnapshotIndex, QImage *belowSnapshotOut,
                        int resumeFromIndex, const QImage *resumeImg,
                        QRect *dirtyRectOut) {
    if (resumeImg) {
        // Fast per-move drag path: orientation, crop, global tone, and every
        // mask below resumeFromIndex are already baked into *resumeImg (it
        // was captured via belowSnapshotIndex/belowSnapshotOut on a prior
        // full render at the same resumeFromIndex); only the active mask and
        // whatever sits above it need recompositing.
        // The caller is responsible for only requesting the fast path when no
        // Background-type mask sits at or above resumeFromIndex — such a
        // layer's content must be rebuilt from the pre-mask toned image,
        // which isn't retained here.
        QImage img;
        if (!adj.masks.isEmpty()) {
            applyMasks(img, adj.masks, brushCache, maskSnapshotIndex, maskSnapshotOut,
                      MaskPass::All, orientedToGeom, geomRotationDeg, scale,
                      -1, nullptr, resumeFromIndex, resumeImg, nullptr, 0, dirtyRectOut);
        } else {
            img = *resumeImg;
        }
        return img;
    }

    if (base.isNull()) return base;

    QImage img = orient(base, adj);

    if (!adj.cropRect.isNull()) {
        QRect r = adj.cropRect.intersected(img.rect());
        if (r.isValid() && !r.isEmpty())
            img = img.copy(r);
    }

    if (!hasToneEdits(adj) && adj.masks.isEmpty()) {
        if (maskSnapshotOut && maskSnapshotIndex >= 0) *maskSnapshotOut = img;
        return img;
    }

    img = img.convertToFormat(QImage::Format_RGBA64);
    const int w = img.width(), h = img.height();

    // --- Per-pixel: white balance, curve, levels, contrast, brightness,
    //     highlights/shadows, saturation/vibrance ---
    // (This is the tab's "global" grade — it has always applied only to the
    // base photo's own pixels, same as before the Background layer became a
    // normal Mask entry: it's baked into `img` here and handed to the
    // Background mask below as its content, exactly like any other layer's
    // content is computed before compositing.)
    // Skip the full-image per-pixel pass entirely when none of the global
    // tone/colour sliders are touched (e.g. while only brushing/masking) —
    // hasToneEdits() also considers mask edits, which don't need this pass.
    const bool hasGlobalTone = adj.brightness || adj.contrast || adj.highlights ||
                               adj.shadows || adj.saturation || adj.vibrance ||
                               adj.temperature || adj.tint ||
                               std::abs(adj.wbR - 1.0) > 1e-4 ||
                               std::abs(adj.wbG - 1.0) > 1e-4 ||
                               std::abs(adj.wbB - 1.0) > 1e-4 || adj.hasCurve() ||
                               !adj.levels.isIdentity() || !adj.colorRanges.isEmpty();
    if (hasGlobalTone) {
        ToneParams tp = makeToneParams(adj.contrast, adj.brightness, adj.highlights,
                                       adj.shadows, adj.saturation, adj.vibrance,
                                       adj.temperature, adj.tint, adj.wbR, adj.wbG,
                                       adj.wbB, adj.curve, adj.levels,
                                       adj.colorRanges);
        for (int y = 0; y < h; ++y) {
            QRgba64 *line = reinterpret_cast<QRgba64 *>(img.scanLine(y));
            for (int x = 0; x < w; ++x) line[x] = applyTone(line[x], tp);
        }
    }

    // --- Additional layers (blend per-layer full content by weight) ---
    // All tiers (static and interactive/Paint-Shape-TextBox) are composited
    // together here in true stack order — a layer's position relative to
    // *every* other layer (not just its own tier) determines what it paints
    // over and what paints over it.
    if (!adj.masks.isEmpty()) {
        // The Background mask (if present) has no content of its own beyond
        // "this tab's base photo" — feed it the globally-toned base computed
        // above so it composites like any other layer, at whatever position
        // it currently occupies in the stack (genuinely reorderable/
        // hideable/deletable, no pinned bottom slot).
        bool hasBackgroundMask = false;
        for (const Mask &m : adj.masks)
            if (m.type == MaskType::Background) { hasBackgroundMask = true; break; }
        // Group-aware compositing (a group's own opacity/blend wrapping its
        // members, see applyMasks) only kicks in when no snapshot/resume is
        // being requested at this call — those are the interactive-drag
        // caching fast paths, and reconciling a snapshot index that falls
        // *inside* a group's span with the group short-circuit isn't worth
        // the complexity/risk here. A live drag on a layer inside a group
        // briefly renders that group flat (members blending straight into
        // the backdrop, ignoring the group's own opacity/blend) until the
        // drag settles and a plain full render — which always requests no
        // snapshot — corrects it.
        const QVector<MaskGroup> *groupsForThisRender =
            (maskSnapshotIndex < 0 && belowSnapshotIndex < 0) ? &adj.groups : nullptr;
        if (hasBackgroundMask) {
            QVector<Mask> stack = adj.masks;
            for (Mask &m : stack)
                if (m.type == MaskType::Background) m.sourceImageCache = img;
            QImage composite(w, h, QImage::Format_RGBA64);
            composite.fill(Qt::transparent);
            applyMasks(composite, stack, brushCache, maskSnapshotIndex, maskSnapshotOut,
                      MaskPass::All, orientedToGeom, geomRotationDeg, scale,
                      belowSnapshotIndex, belowSnapshotOut, -1, nullptr,
                      groupsForThisRender);
            img = composite;
        } else {
            applyMasks(img, adj.masks, brushCache, maskSnapshotIndex, maskSnapshotOut,
                      MaskPass::All, orientedToGeom, geomRotationDeg, scale,
                      belowSnapshotIndex, belowSnapshotOut, -1, nullptr,
                      groupsForThisRender);
        }
    }

    // --- Denoise / clarity / sharpen / vignette (base layer only; layers have their own) ---
    applyDenoise(img, adj.denoise);
    applyClarity(img, adj.clarity);
    applySharpen(img, adj.sharpen);
    applyLighting(img, adj.lightAngle, adj.lightIntensity);
    applyVignette(img, adj.vignette);
    applyFlatStyle(img, adj.flatStyle);

    return img;
}

QString historyStepLabel(const Adjustments &prev, const Adjustments &curr) {
    if (prev == curr) return QStringLiteral("No change");
    if (curr.brightness != prev.brightness)   return QStringLiteral("Brightness");
    if (curr.contrast != prev.contrast)       return QStringLiteral("Contrast");
    if (curr.highlights != prev.highlights)   return QStringLiteral("Highlights");
    if (curr.shadows != prev.shadows)         return QStringLiteral("Shadows");
    if (curr.saturation != prev.saturation)   return QStringLiteral("Saturation");
    if (curr.vibrance != prev.vibrance)       return QStringLiteral("Vibrance");
    if (curr.temperature != prev.temperature) return QStringLiteral("Temperature");
    if (curr.tint != prev.tint)               return QStringLiteral("Tint");
    if (curr.wbR != prev.wbR || curr.wbG != prev.wbG || curr.wbB != prev.wbB)
        return QStringLiteral("White Balance");
    if (curr.denoise != prev.denoise)         return QStringLiteral("Denoise");
    if (curr.clarity != prev.clarity)         return QStringLiteral("Clarity");
    if (curr.sharpen != prev.sharpen)         return QStringLiteral("Sharpen");
    if (curr.vignette != prev.vignette)       return QStringLiteral("Vignette");
    if (curr.lightAngle != prev.lightAngle || curr.lightIntensity != prev.lightIntensity)
        return QStringLiteral("Lighting");
    if (curr.flatStyle != prev.flatStyle)     return QStringLiteral("Style");
    if (curr.curve != prev.curve)             return QStringLiteral("Curve");
    if (curr.levels != prev.levels)           return QStringLiteral("Levels");
    if (curr.colorRanges != prev.colorRanges) return QStringLiteral("Color Range");
    if (curr.masks != prev.masks) {
        if (curr.masks.size() != prev.masks.size())
            return QStringLiteral("Layer");
        return QStringLiteral("Paint");
    }
    if (curr.heals != prev.heals)             return QStringLiteral("Spot Heal");
    if (curr.rotationQuadrants != prev.rotationQuadrants)
        return QStringLiteral("Rotate");
    if (curr.flipH != prev.flipH || curr.flipV != prev.flipV)
        return QStringLiteral("Flip");
    if (curr.cropRect != prev.cropRect)       return QStringLiteral("Crop");
    return QStringLiteral("Adjust");
}
