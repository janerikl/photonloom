#include "edit/RetouchWindow.h"
#include "edit/NewDocumentDialog.h"
#include "edit/RetouchTab.h"
#include "edit/ImageCanvas.h"
#include "edit/ExportDialog.h"
#include "edit/TiffExport.h"
#include "edit/CurveEditor.h"
#include "edit/EditSidecar.h"
#include "edit/RecentSessions.h"
#include "edit/RecentFiles.h"
#include "edit/RecentProjects.h"
#include "ui/BrowseTab.h"
#include "ui/FilmstripWidget.h"
#include "ui/LevelsPanel.h"
#include "ui/LayerAdjustmentsPanel.h"
#include "ui/LayersPanel.h"
#include "ui/AssetsPanel.h"
#include "ui/ToolFlyout.h"
#include "ui/TetherView.h"
#include "ui/PreferencesDialog.h"
#include "ui/ShortcutRegistry.h"
#include "camera/CameraModels.h"
#include "ui/ControlsPanel.h"
#include "ui/ScrubSpinBox.h"
#include "ui/BrushPresetMenuButton.h"
#include "svg/SvgEditorTab.h"
#include <QFrame>
#include "capture/NefPreview.h"

#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QCloseEvent>
#include <QKeySequence>
#include <QShortcut>
#include <cmath>

#include <QTabWidget>
#include <QDockWidget>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSlider>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QCheckBox>
#include <QFontComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QColorDialog>
#include <QLabel>
#include <QToolBar>
#include <QStackedWidget>
#include <QListWidget>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QStatusBar>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QFileInfo>
#include <QDir>
#include <QSignalBlocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QIcon>

namespace {
// Small programmatically-drawn icons for the left tool bar (no image assets
// in this project). Each icon is drawn twice: a neutral grey Off state
// and a light On state so the active (checked) tool stands out clearly.
constexpr int kIconPx = 28;
const QColor kIconOff(220, 220, 220);    // idle: same bright grey as the hover background, for visibility
const QColor kIconOn(235, 235, 235);     // active: light
const QColor kIconDisabled(220, 220, 220, 90); // disabled: idle color, faded
const QColor kIconHover(20, 20, 20);      // hover (idle tool): near-black, for contrast against the lighter hover background

QPixmap drawMove(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const double cx = kIconPx / 2.0, cy = kIconPx / 2.0;
    const double arm = 10.0, headL = 6.0, headW = 3.5, shaftHalf = 1.2, gap = 2.0;

    // Four short shafts from just outside the center out to the base of
    // each arrowhead, leaving a gap at the middle so the arms read as
    // distinct spokes instead of merging into a solid center blob.
    auto shaft = [&](double dx, double dy) {
        QPointF perp(-dy, dx);
        QPointF inner(cx + dx * gap, cy + dy * gap);
        QPointF outer(cx + dx * (arm - headL), cy + dy * (arm - headL));
        QPolygonF quad;
        quad << inner + perp * shaftHalf << outer + perp * shaftHalf
             << outer - perp * shaftHalf << inner - perp * shaftHalf;
        p.drawPolygon(quad);
    };
    shaft(0, -1);
    shaft(0, 1);
    shaft(-1, 0);
    shaft(1, 0);

    auto head = [&](const QPointF &tip, const QPointF &back, const QPointF &perp) {
        QPolygonF tri;
        tri << tip << (back + perp * headW) << (back - perp * headW);
        p.drawPolygon(tri);
    };
    head(QPointF(cx, cy - arm), QPointF(cx, cy - arm + headL), QPointF(1, 0));
    head(QPointF(cx, cy + arm), QPointF(cx, cy + arm - headL), QPointF(1, 0));
    head(QPointF(cx - arm, cy), QPointF(cx - arm + headL, cy), QPointF(0, 1));
    head(QPointF(cx + arm, cy), QPointF(cx + arm - headL, cy), QPointF(0, 1));
    return pm;
}

QPixmap drawZoom(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 2);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(4, 4, 14, 14));
    p.drawLine(QPointF(15, 15), QPointF(23, 23));
    return pm;
}

QPixmap drawCrop(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 2);
    pen.setCapStyle(Qt::SquareCap);
    p.setPen(pen);
    // Two overlapping corner brackets, the classic crop-tool mark.
    p.drawLine(8, 4, 8, 20);
    p.drawLine(8, 20, 24, 20);
    p.drawLine(4, 8, 20, 8);
    p.drawLine(20, 8, 20, 24);
    return pm;
}

QPixmap drawHeal(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 2);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(4, 4, 20, 20)); // brush outline
    p.setPen(Qt::NoPen);
    QColor fill = c;
    fill.setAlpha(140);
    p.setBrush(fill);
    p.drawEllipse(QRectF(10, 10, 8, 8)); // spot being healed
    return pm;
}

QPixmap drawMask(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Half-filled circle: the classic "mask" motif.
    p.setPen(QPen(c, 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(4, 4, 20, 20));
    p.setPen(Qt::NoPen);
    QColor fill = c;
    fill.setAlpha(140);
    p.setBrush(fill);
    p.drawPie(QRectF(4, 4, 20, 20), 90 * 16, 180 * 16);
    return pm;
}

// Mask subtool glyphs, used both on the flyout strip and (as the active
// subtool) on the mask tool button itself.
QPixmap drawMaskRadial(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(c, 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(3, 6, 22, 16)); // outer ellipse
    QColor fill = c;
    fill.setAlpha(120);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(QRectF(9, 10, 10, 8)); // inner falloff
    return pm;
}

QPixmap drawMaskLinear(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Graduated: a band shaded on one side, split by the gradient line.
    QColor fill = c;
    fill.setAlpha(120);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRect(QRectF(4, 4, 20, 8));
    p.setPen(QPen(c, 2));
    p.drawLine(QPointF(4, 14), QPointF(24, 14));
    return pm;
}

QPixmap drawMaskBrush(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // A soft brush dab.
    QColor fill = c;
    fill.setAlpha(120);
    p.setPen(QPen(c, 2));
    p.setBrush(fill);
    p.drawEllipse(QRectF(6, 6, 16, 16));
    return pm;
}

QPixmap drawErase(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // A classic slanted rubber-eraser block.
    p.translate(kIconPx / 2.0, kIconPx / 2.0);
    p.rotate(-40);
    p.translate(-kIconPx / 2.0, -kIconPx / 2.0);
    QPainterPath body;
    body.addRoundedRect(QRectF(6, 9, 16, 10), 2, 2);
    p.setPen(QPen(c, 2));
    QColor fill = c;
    fill.setAlpha(90);
    p.setBrush(fill);
    p.drawPath(body);
    // Divider line separating the coloured heel from the clean tip.
    p.drawLine(QPointF(12, 9), QPointF(12, 19));
    return pm;
}

// Dashed lasso outline with a small "x" motif: content-aware object removal.
QPixmap drawRemoveObject(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 2);
    pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(4, 4, 20, 20));
    QPen xPen(c, 2.2);
    xPen.setStyle(Qt::SolidLine);
    xPen.setCapStyle(Qt::RoundCap);
    p.setPen(xPen);
    p.drawLine(QPointF(10, 10), QPointF(18, 18));
    p.drawLine(QPointF(18, 10), QPointF(10, 18));
    return pm;
}

// House-painting brush: wide flat bristle head, metal ferrule band, and a
// wooden handle sticking up from it.
QPixmap drawBrushTool(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Wide bristle head with a ragged bottom edge.
    QPainterPath bristles;
    bristles.moveTo(5, 19);
    bristles.lineTo(23, 19);
    bristles.lineTo(21, 27);
    bristles.lineTo(18, 23);
    bristles.lineTo(15, 27);
    bristles.lineTo(12, 23);
    bristles.lineTo(9, 27);
    bristles.lineTo(7, 23);
    bristles.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(bristles);

    // Metal ferrule band clamping the bristles to the handle.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c, 2));
    p.drawRect(QRectF(5, 14, 18, 5));

    // Wooden handle.
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(10, 3, 8, 12), 2, 2);

    return pm;
}

// Tilted paint bucket with a drip, plus a small paint-pour blob: the classic
// flood-fill glyph, distinct from the brush's bristle-tip shape.
// Lead pencil glyph for the Pen tool: sharpened graphite point, wood taper,
// hexagonal shaft, and a ferrule + eraser at the top, angled for drawing.
QPixmap drawCrayonTool(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.translate(kIconPx / 2.0, kIconPx / 2.0);
    p.rotate(45);
    p.translate(-kIconPx / 2.0, -kIconPx / 2.0);

    // Graphite point.
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    QPainterPath lead;
    lead.moveTo(14, 27);
    lead.lineTo(12, 22);
    lead.lineTo(16, 22);
    lead.closeSubpath();
    p.drawPath(lead);

    // Sharpened wood taper.
    QColor fill = c;
    fill.setAlpha(60);
    p.setPen(QPen(c, 1.5));
    p.setBrush(fill);
    QPainterPath wood;
    wood.moveTo(12, 22);
    wood.lineTo(16, 22);
    wood.lineTo(19, 17);
    wood.lineTo(9, 17);
    wood.closeSubpath();
    p.drawPath(wood);

    // Hexagonal shaft.
    p.drawRect(QRectF(9, 8, 10, 9));

    // Ferrule band and eraser at the top.
    p.setBrush(c);
    p.drawRect(QRectF(9, 6, 10, 2));
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(9, 3, 10, 4), 1.5, 1.5);

    return pm;
}

QPixmap drawPaintBucket(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.translate(kIconPx / 2.0, kIconPx / 2.0);
    p.rotate(-25);
    p.translate(-kIconPx / 2.0, -kIconPx / 2.0);

    // Bucket body: an open-top trapezoid.
    QPainterPath body;
    body.moveTo(7, 12);
    body.lineTo(21, 12);
    body.lineTo(19, 23);
    body.lineTo(9, 23);
    body.closeSubpath();
    p.setPen(QPen(c, 2));
    QColor fill = c;
    fill.setAlpha(90);
    p.setBrush(fill);
    p.drawPath(body);
    // Rim ellipse.
    p.drawEllipse(QRectF(7, 9, 14, 5));

    p.resetTransform();
    // Drip pouring from the spout, plus a small puddle: reads as "fill".
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    QPainterPath drip;
    drip.moveTo(20, 18);
    drip.cubicTo(24, 21, 25, 24, 22, 25);
    drip.cubicTo(19, 24, 20, 21, 20, 18);
    p.drawPath(drip);
    p.drawEllipse(QRectF(21, 25, 4, 3));
    return pm;
}

// Simple "T" glyph for the text tool.
QPixmap drawTextTool(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 4);
    pen.setCapStyle(Qt::FlatCap);
    p.setPen(pen);
    p.drawLine(QPointF(6, 6), QPointF(22, 6));
    p.drawLine(QPointF(14, 6), QPointF(14, 24));
    return pm;
}

// Simple rect + ellipse glyph for the shape tool.
QPixmap drawShapeTool(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(c, 2.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(4, 4, 13, 13));
    p.drawEllipse(QRectF(13, 13, 13, 13));
    return pm;
}

// Dashed rectangle glyph for the marquee-selection tool.
QPixmap drawSelectMarquee(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 2, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(5, 5, 18, 18));
    return pm;
}

// Dashed freeform-blob glyph for the lasso-selection tool.
QPixmap drawSelectLasso(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 2, Qt::DashLine);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(6, 14);
    path.cubicTo(4, 6, 16, 3, 21, 8);
    path.cubicTo(26, 13, 22, 22, 14, 23);
    path.cubicTo(8, 24, 5, 19, 6, 14);
    p.drawPath(path);
    return pm;
}

// Wand-with-sparkle glyph for the magic-wand-selection tool.
QPixmap drawSelectWand(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 2.5);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(QPointF(8, 22), QPointF(20, 10));
    p.setPen(QPen(c, 1.5));
    p.drawLine(QPointF(21, 4), QPointF(21, 8));
    p.drawLine(QPointF(19, 6), QPointF(23, 6));
    p.drawLine(QPointF(25, 8), QPointF(25, 10));
    return pm;
}

// Filled circle with a dashed selection ring around it — glyph for the
// Selection Brush (paint to add/subtract from the active selection).
QPixmap drawSelectBrush(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    QColor fill = c;
    fill.setAlpha(90);
    p.setBrush(fill);
    p.drawEllipse(QRectF(9, 9, 12, 12));
    QPen dashed(c, 1.5, Qt::DashLine);
    p.setPen(dashed);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(3, 3, 24, 24));
    return pm;
}

// Hand-stamper glyph (base pad + tapered body + top handle) for Clone Stamp.
QPixmap drawCloneStamp(const QColor &c) {
    QPixmap pm(kIconPx, kIconPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(c, 1.5));
    QColor fill = c;
    fill.setAlpha(60);
    p.setBrush(fill);

    // Imprint pad at the base.
    p.drawRect(QRectF(6, 23, 16, 4));

    // Tapered body connecting pad to handle.
    QPolygonF body;
    body << QPointF(9, 23) << QPointF(23, 23) << QPointF(19, 13) << QPointF(13, 13);
    p.drawPolygon(body);

    // Handle on top.
    p.drawRoundedRect(QRectF(12, 5, 8, 9), 2, 2);

    return pm;
}

// Overlay a small corner triangle marking a tool that owns a subtool flyout.
void addFlyoutMarker(QPixmap &pm, const QColor &c) {
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const qreal s = kIconPx;
    QPolygonF tri;
    tri << QPointF(s - 6, s) << QPointF(s, s - 6) << QPointF(s, s);
    p.drawPolygon(tri);
}

// Build a two-state icon: dark grey when idle, light when checked/active.
QIcon makeToolIcon(QPixmap (*draw)(const QColor &)) {
    QIcon icon;
    icon.addPixmap(draw(kIconOff), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(kIconOn), QIcon::Normal, QIcon::On);
    icon.addPixmap(draw(kIconHover), QIcon::Active, QIcon::Off);
    icon.addPixmap(draw(kIconOn), QIcon::Active, QIcon::On);
    icon.addPixmap(draw(kIconDisabled), QIcon::Disabled, QIcon::Off);
    return icon;
}

QIcon makeMoveIcon() { return makeToolIcon(drawMove); }
QIcon makeZoomIcon() { return makeToolIcon(drawZoom); }
QIcon makeCropIcon() { return makeToolIcon(drawCrop); }
QIcon makeHealIcon() { return makeToolIcon(drawHeal); }
QIcon makeMaskIcon() { return makeToolIcon(drawMask); }
QIcon makeEraseIcon() { return makeToolIcon(drawErase); }
QIcon makeBrushToolIcon() { return makeToolIcon(drawBrushTool); }
QIcon makePaintBucketIcon() { return makeToolIcon(drawPaintBucket); }
QIcon makeTextIcon() { return makeToolIcon(drawTextTool); }
QIcon makeShapeIcon() { return makeToolIcon(drawShapeTool); }
QIcon makeSelectMarqueeIcon() { return makeToolIcon(drawSelectMarquee); }
QIcon makeSelectLassoIcon() { return makeToolIcon(drawSelectLasso); }
QIcon makeSelectWandIcon() { return makeToolIcon(drawSelectWand); }
QIcon makeSelectBrushIcon() { return makeToolIcon(drawSelectBrush); }
QIcon makeCloneStampIcon() { return makeToolIcon(drawCloneStamp); }
QIcon makeCrayonToolIcon() { return makeToolIcon(drawCrayonTool); }
QIcon makeRemoveObjectIcon() { return makeToolIcon(drawRemoveObject); }

// Two-state icon like makeToolIcon, but with the flyout corner marker baked in.
// Used for the mask tool button, whose glyph reflects its active subtool.
QIcon makeFlyoutToolIcon(QPixmap (*draw)(const QColor &)) {
    QIcon icon;
    QPixmap off = draw(kIconOff);
    addFlyoutMarker(off, kIconOff);
    QPixmap on = draw(kIconOn);
    addFlyoutMarker(on, kIconOn);
    QPixmap disabled = draw(kIconDisabled);
    addFlyoutMarker(disabled, kIconDisabled);
    QPixmap hover = draw(kIconHover);
    addFlyoutMarker(hover, kIconHover);
    icon.addPixmap(off, QIcon::Normal, QIcon::Off);
    icon.addPixmap(on, QIcon::Normal, QIcon::On);
    icon.addPixmap(hover, QIcon::Active, QIcon::Off);
    icon.addPixmap(on, QIcon::Active, QIcon::On);
    icon.addPixmap(disabled, QIcon::Disabled, QIcon::Off);
    return icon;
}

// Glyph for a given mask subtype (shared by the flyout and the tool button).
QPixmap (*maskGlyph(MaskType t))(const QColor &) {
    switch (t) {
    case MaskType::Radial: return drawMaskRadial;
    case MaskType::Linear: return drawMaskLinear;
    case MaskType::Brush:  return drawMaskBrush;
    case MaskType::Paint:  return drawMaskBrush;
    case MaskType::None:   return drawMask;
    }
    return drawMaskRadial;
}

// Theatre-curtain glyph for the before/after reveal button beside the
// filmstrip. Drawn at 2x and rendered with a device pixel ratio so it stays
// crisp at any display scale (no external image/SVG assets in this project).
// `open` widens the parted gap, giving a little "reveal" animation as the
// user presses and holds.
QPixmap drawCurtain(bool open) {
    constexpr int kPx = 32;
    constexpr int kSuperSample = 3;
    QPixmap pm(kPx * kSuperSample, kPx * kSuperSample);
    pm.setDevicePixelRatio(kSuperSample);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor deep(96, 12, 20);
    const QColor mid(163, 26, 34);
    const QColor bright(219, 68, 64);
    const QColor gold(214, 178, 62);
    const qreal top = 6.0;
    const qreal bottom = kPx - 4.0;
    const qreal gap = open ? 6.5 : 1.6; // half-width of the parted middle gap

    // Valance / pelmet across the top rail.
    QLinearGradient valGrad(0, top - 4, 0, top + 1);
    valGrad.setColorAt(0, bright);
    valGrad.setColorAt(1, deep);
    p.setPen(Qt::NoPen);
    p.setBrush(valGrad);
    p.drawRoundedRect(QRectF(2, top - 4, kPx - 4, 5), 1.5, 1.5);

    auto panel = [&](bool left) {
        const qreal outerX = left ? 2.5 : kPx - 2.5;
        const qreal midX = kPx / 2.0;
        const qreal innerTopX = left ? midX - gap : midX + gap;
        const qreal innerBottomX = left ? midX - gap - 3.0 : midX + gap + 3.0;
        const qreal sign = left ? 1.0 : -1.0;

        QPainterPath path;
        path.moveTo(outerX, top);
        path.lineTo(innerTopX, top);
        path.quadTo(midX - sign * 2.0, bottom - 5, innerBottomX, bottom - 9);
        path.quadTo(outerX + sign * 3.0, bottom - 2, outerX, bottom);
        path.closeSubpath();

        QLinearGradient shade(left ? outerX : innerTopX, 0, left ? innerTopX : outerX, 0);
        shade.setColorAt(0.0, left ? deep : bright);
        shade.setColorAt(0.5, mid);
        shade.setColorAt(1.0, left ? bright : deep);
        p.setPen(QPen(deep.darker(130), 0.5));
        p.setBrush(shade);
        p.drawPath(path);

        // Vertical fold lines for a velvet, gathered look.
        p.setPen(QPen(deep.darker(150), 0.7));
        for (int i = 1; i <= 3; ++i) {
            qreal t = i / 4.0;
            qreal fx = outerX + (innerTopX - outerX) * t;
            QPainterPath fold;
            fold.moveTo(fx, top + 0.8);
            fold.quadTo(fx - sign * 1.4, (top + bottom) / 2.0, fx, bottom - 8);
            p.drawPath(fold);
        }

        // Gold tie-back rope with a small tassel where the panel is gathered.
        p.setPen(QPen(gold, 1.1));
        p.drawLine(QPointF(innerTopX, bottom - 11), QPointF(innerBottomX, bottom - 8));
        p.setPen(Qt::NoPen);
        p.setBrush(gold);
        p.drawEllipse(QPointF(innerBottomX, bottom - 7), 1.2, 1.2);
    };

    panel(true);
    panel(false);

    // Warm stage-light glow in the parted gap, hinting at what's revealed.
    QLinearGradient glow(kPx / 2.0 - gap, 0, kPx / 2.0 + gap, 0);
    glow.setColorAt(0.0, QColor(255, 255, 255, 0));
    glow.setColorAt(0.5, QColor(255, 244, 214, open ? 200 : 90));
    glow.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawRect(QRectF(kPx / 2.0 - gap, top + 1, gap * 2.0, bottom - top - 2));

    return pm;
}

QIcon makeCurtainIcon(bool open) {
    QIcon icon;
    icon.addPixmap(drawCurtain(open));
    return icon;
}
} // namespace

RetouchWindow::RetouchWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Photonloom");
    resize(1200, 820);

    auto *toolbar = addToolBar("Main");
    toolbar->setObjectName("mainToolBar");
    toolbar->setMovable(false);

    // Mode switch: mutually-exclusive Browse / Retouch / Tether / SVG at the
    // far left.
    m_browseModeAction = toolbar->addAction("Browse");
    m_retouchModeAction = toolbar->addAction("Retouch");
    m_tetherModeAction = toolbar->addAction("Tether");
    m_svgModeAction = toolbar->addAction("SVG");
    m_browseModeAction->setCheckable(true);
    m_retouchModeAction->setCheckable(true);
    m_tetherModeAction->setCheckable(true);
    m_svgModeAction->setCheckable(true);
    auto *modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addAction(m_browseModeAction);
    modeGroup->addAction(m_retouchModeAction);
    modeGroup->addAction(m_tetherModeAction);
    modeGroup->addAction(m_svgModeAction);
    connect(m_browseModeAction, &QAction::triggered, this,
            [this] { setMode(Mode::Browse); });
    connect(m_retouchModeAction, &QAction::triggered, this,
            [this] { setMode(Mode::Retouch); });
    connect(m_tetherModeAction, &QAction::triggered, this,
            [this] { setMode(Mode::Tether); });
    connect(m_svgModeAction, &QAction::triggered, this,
            [this] { setMode(Mode::Svg); });
    // Save / Save All / Export live in the File menu only (not the toolbar).
    m_saveAction = new QAction("Save", this);
    m_saveAction->setShortcut(QKeySequence::Save); // Ctrl+S
    ShortcutRegistry::instance().registerShortcut("file.save", "Menu", "Save", m_saveAction, QKeySequence::Save);
    m_saveAllAction = new QAction("Save All", this);
    m_saveAsProjectAction = new QAction("Save As Photonloom Project…", this);
    m_exportAction = new QAction("Export…", this);
    // Open Session/Photos moved out of the toolbar (still in the File menu)
    // to make room for the contextual tool-options row below.
    auto *openSessionAction = new QAction("Open Session…", this);
    auto *openPhotosAction = new QAction("Open Photos…", this);
    auto *openProjectAction = new QAction("Open Project…", this);
    auto *newDocAction = new QAction("New…", this);
    newDocAction->setShortcut(QKeySequence::New); // Ctrl+N
    ShortcutRegistry::instance().registerShortcut("file.new", "Menu", "New…", newDocAction, QKeySequence::New);
    connect(newDocAction, &QAction::triggered, this, &RetouchWindow::onNewDocument);
    connect(openSessionAction, &QAction::triggered, this, &RetouchWindow::onOpenSession);
    connect(openPhotosAction, &QAction::triggered, this, &RetouchWindow::onOpenPhotos);
    connect(openProjectAction, &QAction::triggered, this, &RetouchWindow::onOpenProject);
    connect(m_saveAction, &QAction::triggered, this, &RetouchWindow::onSave);
    connect(m_saveAllAction, &QAction::triggered, this, &RetouchWindow::onSaveAll);
    connect(m_saveAsProjectAction, &QAction::triggered, this, &RetouchWindow::onSaveAsProject);
    connect(m_exportAction, &QAction::triggered, this, &RetouchWindow::onExport);

    m_fileMenu = menuBar()->addMenu("File");
    m_fileMenu->addAction(newDocAction);
    m_fileMenu->addSeparator();
    m_fileMenu->addAction(openSessionAction);
    m_fileMenu->addAction(openPhotosAction);
    m_fileMenu->addAction(openProjectAction);
    // Recent sessions live inline here, between these two separators. Recent
    // item actions are inserted before m_recentEndSeparator by
    // rebuildRecentSessionsMenu(); both separators hide when the list is empty.
    m_recentBeginSeparator = m_fileMenu->addSeparator();
    m_recentEndSeparator = m_fileMenu->addSeparator();
    // Recently opened/saved Photonloom project (.ploom) files live directly
    // below the recent-sessions section, using the same
    // insert-before-end-separator pattern via rebuildRecentProjectsMenu().
    m_recentProjectsBeginSeparator = m_fileMenu->addSeparator();
    m_recentProjectsEndSeparator = m_fileMenu->addSeparator();
    // Recently opened individual photos live in their own section, below the
    // recent-projects section, using the same insert-before-end-separator
    // pattern via rebuildRecentFilesMenu().
    m_recentFilesBeginSeparator = m_fileMenu->addSeparator();
    m_recentFilesEndSeparator = m_fileMenu->addSeparator();
    m_fileMenu->addAction(m_saveAction);
    m_fileMenu->addAction(m_saveAllAction);
    m_fileMenu->addAction(m_saveAsProjectAction);
    m_fileMenu->addSeparator();
    m_fileMenu->addAction(m_exportAction);
    rebuildRecentSessionsMenu();
    rebuildRecentFilesMenu();
    rebuildRecentProjectsMenu();

    auto *editMenu = menuBar()->addMenu("Edit");
    m_undoAction = editMenu->addAction("Undo");
    m_undoAction->setShortcut(QKeySequence::Undo); // Ctrl+Z
    ShortcutRegistry::instance().registerShortcut("edit.undo", "Menu", "Undo", m_undoAction, QKeySequence::Undo);
    m_redoAction = editMenu->addAction("Redo");
    m_redoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y)); // Ctrl+Y
    ShortcutRegistry::instance().registerShortcut("edit.redo", "Menu", "Redo", m_redoAction, QKeySequence(Qt::CTRL | Qt::Key_Y));
    m_undoAction->setEnabled(false);
    m_redoAction->setEnabled(false);
    connect(m_undoAction, &QAction::triggered, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->undo();
    });
    connect(m_redoAction, &QAction::triggered, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->redo();
    });

    editMenu->addSeparator();
    m_copyEditsAction = editMenu->addAction("Copy Edits");
    m_copyEditsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    ShortcutRegistry::instance().registerShortcut("edit.copyEdits", "Menu", "Copy Edits", m_copyEditsAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    m_pasteEditsAction = editMenu->addAction("Paste Edits");
    m_pasteEditsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    ShortcutRegistry::instance().registerShortcut("edit.pasteEdits", "Menu", "Paste Edits", m_pasteEditsAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    m_syncEditsAction = editMenu->addAction("Sync Edits to Selected");
    m_syncEditsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    ShortcutRegistry::instance().registerShortcut("edit.syncEdits", "Menu", "Sync Edits to Selected", m_syncEditsAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    m_copyEditsAction->setEnabled(false);
    m_pasteEditsAction->setEnabled(false);
    m_syncEditsAction->setEnabled(false);
    connect(m_copyEditsAction, &QAction::triggered, this, &RetouchWindow::onCopyEdits);
    connect(m_pasteEditsAction, &QAction::triggered, this, &RetouchWindow::onPasteEdits);
    connect(m_syncEditsAction, &QAction::triggered, this, &RetouchWindow::onSyncEdits);

    m_presetsMenu = menuBar()->addMenu("Presets");
    rebuildPresetsMenu();

    editMenu->addSeparator();
    m_groupShapesAction = editMenu->addAction("Group Shapes");
    m_groupShapesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    ShortcutRegistry::instance().registerShortcut("edit.groupShapes", "Menu", "Group Shapes/Layers", m_groupShapesAction, QKeySequence(Qt::CTRL | Qt::Key_G));
    m_ungroupShapesAction = editMenu->addAction("Ungroup Shapes");
    m_ungroupShapesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    ShortcutRegistry::instance().registerShortcut("edit.ungroupShapes", "Menu", "Ungroup Shapes/Layers", m_ungroupShapesAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    connect(m_groupShapesAction, &QAction::triggered, this, [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        // Ctrl+G is overloaded: with 2+ layers selected in the Layers panel,
        // it groups those layers (mirroring the panel's own Group button);
        // otherwise it falls back to grouping the canvas's selected shapes.
        QVector<int> selected = m_layersPanel->selectedMaskIndices();
        if (selected.size() >= 2) tab->groupMasks(selected);
        else tab->groupSelectedShapes();
        refreshMaskPanel();
    });
    connect(m_ungroupShapesAction, &QAction::triggered, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) { tab->ungroupSelectedShapes(); refreshMaskPanel(); }
    });

    editMenu->addSeparator();
    m_deselectAction = editMenu->addAction("Deselect");
    m_deselectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    ShortcutRegistry::instance().registerShortcut("edit.deselect", "Menu", "Deselect", m_deselectAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    m_invertSelectionAction = editMenu->addAction("Invert Selection");
    m_invertSelectionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    ShortcutRegistry::instance().registerShortcut("edit.invertSelection", "Menu", "Invert Selection", m_invertSelectionAction, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    m_featherSelectionAction = editMenu->addAction("Feather Selection...");
    m_featherSelectionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_D));
    ShortcutRegistry::instance().registerShortcut("edit.featherSelection", "Menu", "Feather Selection…", m_featherSelectionAction, QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_D));
    m_copySelectionAction = editMenu->addAction("Copy");
    m_copySelectionAction->setShortcut(QKeySequence::Copy);
    ShortcutRegistry::instance().registerShortcut("edit.copySelection", "Menu", "Copy", m_copySelectionAction, QKeySequence::Copy);
    m_pasteSelectionAction = editMenu->addAction("Paste");
    m_pasteSelectionAction->setShortcut(QKeySequence::Paste);
    ShortcutRegistry::instance().registerShortcut("edit.pasteSelection", "Menu", "Paste", m_pasteSelectionAction, QKeySequence::Paste);
    m_pasteSelectionAction->setEnabled(false);
    m_saveSelectionAsAssetAction = editMenu->addAction("Save Selection as Asset...");
    connect(m_deselectAction, &QAction::triggered, this, &RetouchWindow::onDeselect);
    connect(m_invertSelectionAction, &QAction::triggered, this, &RetouchWindow::onInvertSelection);
    connect(m_featherSelectionAction, &QAction::triggered, this, &RetouchWindow::onFeatherSelection);
    connect(m_copySelectionAction, &QAction::triggered, this, &RetouchWindow::onCopySelection);
    connect(m_pasteSelectionAction, &QAction::triggered, this, &RetouchWindow::onPasteSelection);
    connect(m_saveSelectionAsAssetAction, &QAction::triggered, this, &RetouchWindow::onSaveSelectionAsAsset);

    // Center: a stack (editing tabs / tether) with the shared filmstrip below,
    // so the filmstrip is visible in both modes.
    auto *central = new QWidget;
    auto *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);

    m_tabs = new QTabWidget;
    m_tabs->setTabsClosable(true);
    m_tabs->setDocumentMode(true);
    connect(m_tabs, &QTabWidget::currentChanged, this, &RetouchWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this,
            &RetouchWindow::onTabCloseRequested);

    m_tetherView = new TetherView;

    m_svgEditorTab = new SvgEditorTab;
    connect(m_svgEditorTab, &SvgEditorTab::sendToRetouchRequested, this,
            [this](const QImage &image, const QString &name) {
                RetouchTab *tab = currentTab();
                if (tab && tab->isReady()) {
                    tab->addImageLayerFromImage(image, name);
                    refreshMaskPanel();
                } else {
                    QMessageBox::information(this, "Send to Retouch",
                        "Open or select a Retouch document tab first.");
                }
            });

    m_browseTab = new BrowseTab;
    connect(m_browseTab, &BrowseTab::openRequested, this,
            &RetouchWindow::onBrowseOpenRequested);

    m_modeStack = new QStackedWidget;
    m_modeStack->addWidget(m_tabs);         // index 0 = Retouch
    m_modeStack->addWidget(m_tetherView);   // index 1 = Tether
    m_modeStack->addWidget(m_svgEditorTab); // index 2 = SVG
    m_modeStack->addWidget(m_browseTab);    // index 3 = Browse

    m_filmstrip = new FilmstripWidget;
    connect(m_filmstrip, &FilmstripWidget::frameSelected, this,
            &RetouchWindow::onFilmstripSelected);
    connect(m_filmstrip, &FilmstripWidget::syncEditsRequested, this,
            &RetouchWindow::onSyncEdits);
    connect(m_filmstrip, &FilmstripWidget::deleteRequested, this,
            &RetouchWindow::onDeleteRequested);
    connect(m_filmstrip, &FilmstripWidget::renameRequested, this,
            &RetouchWindow::onRenameRequested);
    connect(m_filmstrip, &FilmstripWidget::ratingChanged, this,
            &RetouchWindow::onRatingChanged);
    connect(m_filmstrip, &FilmstripWidget::itemSelectionChanged, this,
            &RetouchWindow::updateEditClipboardActions);

    // Curtain button beside the filmstrip: press-and-hold to compare against
    // the unedited image.
    m_beforeAfter = new QToolButton;
    m_beforeAfter->setFixedSize(40, 40);
    m_beforeAfter->setIconSize(QSize(32, 32));
    m_beforeAfter->setAutoRaise(true);
    m_beforeAfter->setIcon(makeCurtainIcon(false));
    m_beforeAfter->setToolTip("Hold to reveal the original, unedited photo");
    connect(m_beforeAfter, &QToolButton::pressed, this, [this] {
        m_beforeAfter->setIcon(makeCurtainIcon(true));
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) tab->showOriginal(true);
    });
    connect(m_beforeAfter, &QToolButton::released, this, [this] {
        m_beforeAfter->setIcon(makeCurtainIcon(false));
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) tab->showOriginal(false);
    });

    auto *filmstripRow = new QHBoxLayout;
    filmstripRow->setContentsMargins(0, 0, 0, 0);
    filmstripRow->setSpacing(6);
    filmstripRow->addWidget(m_filmstrip, 1);
    filmstripRow->addWidget(m_beforeAfter, 0, Qt::AlignVCenter);

    vbox->addWidget(m_modeStack, 1);
    vbox->addLayout(filmstripRow, 0);
    setCentralWidget(central);

    buildToolPanel();
    buildToolOptionsBar();
    buildDock();
    buildOrientationDock();
    buildHistoryDock();
    buildLevelsDock();
    buildLayersDock();
    buildAssetsDock();
    buildViewMenu();

    // Tether chrome: camera controls dock + tether action toolbar. Visibility is
    // driven by mode in Task 3; created hidden here.
    m_controlsDock = new QDockWidget("Controls", this);
    m_controlsDock->setObjectName("controlsDock");
    m_controlsDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    m_controlsDock->setWidget(m_tetherView->controlsPanel());
    addDockWidget(Qt::RightDockWidgetArea, m_controlsDock);
    m_controlsDock->hide();

    m_tetherToolBar = addToolBar("Tether");
    m_tetherToolBar->setObjectName("tetherToolBar");
    m_tetherToolBar->setMovable(false);
    m_tetherToolBar->addActions(m_tetherView->tetherActions());
    m_tetherToolBar->hide();

    // Captures flow into the shared filmstrip; tether status into the status bar.
    connect(m_tetherView, &TetherView::captureComplete, this,
            [this](const QString &path) { addToFilmstrip(path); });
    connect(m_tetherView, &TetherView::statusMessage, this,
            [this](const QString &msg) { m_statusLabel->setText(msg); });

    // Preferences dialog: per-model AF frame calibration for click-to-focus.
    m_prefsDialog = new PreferencesDialog(this);
    connect(m_prefsDialog, &PreferencesDialog::afFrameSizeChanged,
            m_tetherView, &TetherView::setAfFrameSize);
    connect(m_tetherView, &TetherView::cameraConnected, this,
            [this](const QString &name) {
                m_prefsDialog->selectModelById(
                    QString::fromStdString(cammodel::matchModel(name.toStdString())));
            });
    connect(m_prefsDialog, &PreferencesDialog::calibrationRequested, this,
            [this] {
                m_prefsDialog->hide();
                setMode(Mode::Tether);
                m_tetherView->startCalibration();
            });
    connect(m_tetherView, &TetherView::calibrationFinished, this,
            [this](int w, int h) {
                m_prefsDialog->setAfFrame(w, h);
                m_statusLabel->setText(
                    QString("Calibration saved: AF frame %1 × %2").arg(w).arg(h));
            });

    // Apply the current model's AF frame to the live view at startup.
    {
        QSettings s;
        const QString model = s.value("af/currentModel", "custom").toString();
        int w = 0, h = 0;
        afFrameForModel(model, w, h);
        m_tetherView->setAfFrameSize(w, h);
    }

    // File → Preferences…
    m_fileMenu->addSeparator();
    QAction *prefsAction = new QAction("Preferences…", this);
    prefsAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Comma), QKeySequence(Qt::Key_F12)});
    // Registered with its primary sequence only — rebinding via the registry
    // reduces this action to a single shortcut instead of the two defaults.
    ShortcutRegistry::instance().registerShortcut("file.preferences", "Menu", "Preferences…", prefsAction, QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(prefsAction, &QAction::triggered, this, [this] {
        m_prefsDialog->show();
        m_prefsDialog->raise();
        m_prefsDialog->activateWindow();
    });
    m_fileMenu->addAction(prefsAction);

    m_statusLabel = new QLabel("Open a photo to begin");
    statusBar()->addWidget(m_statusLabel);

    setDockEnabled(false);

    auto *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escShortcut->setContext(Qt::WindowShortcut);
    connect(escShortcut, &QShortcut::activated, this, &RetouchWindow::deselectAllTools);
    ShortcutRegistry::instance().registerShortcut("canvas.deselectTools", "Canvas", "Deselect all tools", escShortcut, QKeySequence(Qt::Key_Escape));

    // Ctrl+0 fits the image to the window, same as the Fit button.
    auto *fitShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
    fitShortcut->setContext(Qt::WindowShortcut);
    connect(fitShortcut, &QShortcut::activated, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) tab->zoomFit();
    });
    ShortcutRegistry::instance().registerShortcut("canvas.fitToWindow", "Canvas", "Fit to window", fitShortcut, QKeySequence(Qt::CTRL | Qt::Key_0));

    // Restore saved window geometry + dock layout, or fall back to defaults.
    // Restore runs *after* setMode so persisted show/hide of the editing docks
    // (including Layers) wins; then Controls visibility is re-asserted by app
    // logic.
    //
    // The actual restoreGeometry()/restoreState() calls are deferred to the
    // next event-loop iteration (see restoreWindowState() below): QMainWindow
    // only applies saved *dock sizes* correctly once the window has a real,
    // laid-out geometry, which isn't the case yet here (show() hasn't run).
    // Calling restoreState() this early silently drops splitter sizes (e.g.
    // the Levels dock height), even though dock position/visibility restore
    // fine.
    setMode(Mode::Retouch);
    QTimer::singleShot(0, this, &RetouchWindow::restoreWindowState);
}

void RetouchWindow::restoreWindowState() {
    QSettings settings;
    if (settings.contains("window/state")) {
        if (settings.contains("window/geometry"))
            restoreGeometry(settings.value("window/geometry").toByteArray());
        restoreState(settings.value("window/state").toByteArray());
        // Controls visibility is app-controlled, never persisted-visible.
        // The tether toolbar is likewise mode-driven: restoreState() would
        // resurrect it if the last session ended in Tether mode, but startup
        // always forces Retouch, so re-assert its hidden state here.
        if (m_controlsDock)   m_controlsDock->hide();
        if (m_tetherToolBar)  m_tetherToolBar->hide();
    } else {
        applyDefaultDockLayout();
    }
}

// View menu: toggle visibility of the Tools bar, Adjustments dock, and the
// filmstrip strip below the tabs. Built after those widgets exist.
void RetouchWindow::buildViewMenu() {
    auto *viewMenu = menuBar()->addMenu("View");
    if (m_toolsBar) viewMenu->addAction(m_toolsBar->toggleViewAction());
    if (m_adjustmentsDock) viewMenu->addAction(m_adjustmentsDock->toggleViewAction());
    if (m_orientationDock) viewMenu->addAction(m_orientationDock->toggleViewAction());
    if (m_historyDock) viewMenu->addAction(m_historyDock->toggleViewAction());
    if (m_levelsDock) viewMenu->addAction(m_levelsDock->toggleViewAction());
    if (m_layersDock) viewMenu->addAction(m_layersDock->toggleViewAction());
    if (m_assetsDock) viewMenu->addAction(m_assetsDock->toggleViewAction());
    // m_layerAdjustmentsDock is created lazily (on first section request),
    // so it has no toggleViewAction() to add here yet; reopening it is done
    // via the Layers list's right-click menu, not View.

    m_filmstripAction = new QAction("Filmstrip", this);
    m_filmstripAction->setCheckable(true);
    m_filmstripAction->setChecked(true);
    connect(m_filmstripAction, &QAction::toggled, this, [this](bool on) {
        if (m_filmstrip) m_filmstrip->setVisible(on);
        if (m_beforeAfter) m_beforeAfter->setVisible(on);
    });
    viewMenu->addAction(m_filmstripAction);

    m_rulersAction = new QAction("Rulers", this);
    m_rulersAction->setCheckable(true);
    m_rulersAction->setChecked(QSettings().value("canvas/showRulers", false).toBool());
    connect(m_rulersAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("canvas/showRulers", on);
        for (RetouchTab *tab : m_openTabs)
            if (tab->canvas()) tab->canvas()->setShowRulers(on);
    });
    viewMenu->addAction(m_rulersAction);

    {
        auto *gridMenu = viewMenu->addMenu("Composition Grid");
        auto *group = new QActionGroup(this);
        group->setExclusive(true);
        int current = QSettings().value("canvas/compositionGrid", int(GridMode::Off)).toInt();
        struct Entry { const char *label; GridMode grid; };
        const Entry entries[] = {
            {"None", GridMode::Off},
            {"Rule of Thirds", GridMode::Thirds},
            {"Golden Ratio", GridMode::GoldenRatio},
            {"Golden Spiral", GridMode::GoldenSpiral},
            {"Center Crosshair", GridMode::Crosshair},
            {"Diagonals", GridMode::Diagonals},
        };
        for (const Entry &e : entries) {
            auto *act = gridMenu->addAction(e.label);
            act->setCheckable(true);
            act->setChecked(current == static_cast<int>(e.grid));
            group->addAction(act);
            GridMode grid = e.grid;
            connect(act, &QAction::toggled, this, [this, grid](bool on) {
                if (!on) return;
                QSettings().setValue("canvas/compositionGrid", static_cast<int>(grid));
                for (RetouchTab *tab : m_openTabs)
                    if (tab->canvas()) tab->canvas()->setCompositionGrid(grid);
            });
        }
    }

    viewMenu->addSeparator();
    auto *resetPanelsAction = new QAction("Reset Panels", this);
    connect(resetPanelsAction, &QAction::triggered, this, [this] {
        QSettings settings;
        settings.remove("window/state"); // panels only; leave window/geometry
        applyDefaultDockLayout();
    });
    viewMenu->addAction(resetPanelsAction);

    viewMenu->addSeparator();
    m_layoutsMenu = viewMenu->addMenu("Layouts");
    rebuildLayoutsMenu();
}

// Rebuild the Layouts submenu: built-in presets (Painting, Photo Editing),
// then any user-saved templates, then Save/Delete actions. Called at startup
// and whenever a template is saved or deleted.
void RetouchWindow::rebuildLayoutsMenu() {
    if (!m_layoutsMenu) return;
    m_layoutsMenu->clear();

    auto *paintingAct = m_layoutsMenu->addAction("Painting");
    connect(paintingAct, &QAction::triggered, this, &RetouchWindow::applyBuiltInPaintingLayout);
    auto *photoAct = m_layoutsMenu->addAction("Photo Editing");
    connect(photoAct, &QAction::triggered, this, &RetouchWindow::applyBuiltInPhotoEditingLayout);

    const QList<ViewTemplate> &custom = m_viewTemplateStore.custom();
    if (!custom.isEmpty()) {
        m_layoutsMenu->addSeparator();
        for (const ViewTemplate &t : custom) {
            auto *act = m_layoutsMenu->addAction(t.name);
            connect(act, &QAction::triggered, this, [this, t] { applyViewTemplate(t); });
        }
    }

    m_layoutsMenu->addSeparator();
    auto *saveAct = m_layoutsMenu->addAction("Save Current as Template…");
    connect(saveAct, &QAction::triggered, this, &RetouchWindow::onSaveViewTemplate);
    if (!custom.isEmpty()) {
        auto *deleteAct = m_layoutsMenu->addAction("Delete Template…");
        connect(deleteAct, &QAction::triggered, this, &RetouchWindow::onDeleteViewTemplate);
    }
}

// Apply a saved (custom) template: the dock/toolbar state blob, plus the
// filmstrip and rulers toggles that live outside QMainWindow::saveState()
// (filmstrip is a plain central-layout widget, not a dock; rulers are a
// per-tab canvas setting). Re-asserts the same app-controlled exceptions
// restoreWindowState() enforces: Controls dock and Tether toolbar are
// mode-driven, never left visible by a stored layout.
void RetouchWindow::applyViewTemplate(const ViewTemplate &t) {
    restoreState(t.state);
    if (m_controlsDock)  m_controlsDock->hide();
    if (m_tetherToolBar) m_tetherToolBar->hide();
    if (m_filmstripAction) m_filmstripAction->setChecked(t.filmstripVisible);
    if (m_rulersAction) m_rulersAction->setChecked(t.rulersVisible);
    m_statusLabel->setText(QString("Layout applied: \"%1\"").arg(t.name));
}

// Built-in "Painting" preset: tool + tool-options bars and the Adjustments
// dock front-and-centre; History/Levels/Layers docks hidden to keep focus on
// brush/tool controls. Filmstrip hidden, rulers off.
void RetouchWindow::applyBuiltInPaintingLayout() {
    applyDefaultDockLayout();
    if (m_toolsBar) m_toolsBar->show();
    if (m_toolOptionsBar) m_toolOptionsBar->show();
    if (m_adjustmentsDock) m_adjustmentsDock->show();
    if (m_historyDock) m_historyDock->hide();
    if (m_levelsDock) m_levelsDock->hide();
    if (m_layersDock) m_layersDock->show();
    if (m_adjustmentsDock) m_adjustmentsDock->raise();
    if (m_filmstripAction) m_filmstripAction->setChecked(false);
    if (m_rulersAction) m_rulersAction->setChecked(false);
    m_statusLabel->setText("Layout: Painting");
}

// Built-in "Photo Editing" preset: full dock stack (Adjustments/History/
// Levels/Layers) visible, filmstrip visible, rulers on, tool options hidden
// since photo-editing workflows lean on the docks rather than paint tools.
void RetouchWindow::applyBuiltInPhotoEditingLayout() {
    applyDefaultDockLayout();
    if (m_toolsBar) m_toolsBar->show();
    if (m_toolOptionsBar) m_toolOptionsBar->hide();
    if (m_adjustmentsDock) m_adjustmentsDock->show();
    if (m_historyDock) m_historyDock->show();
    if (m_levelsDock) m_levelsDock->show();
    if (m_layersDock) m_layersDock->show();
    if (m_filmstripAction) m_filmstripAction->setChecked(true);
    if (m_rulersAction) m_rulersAction->setChecked(true);
    m_statusLabel->setText("Layout: Photo Editing");
}

void RetouchWindow::onSaveViewTemplate() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save Layout Template", "Template name:",
                                                QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    ViewTemplate t;
    t.name = name.trimmed();
    t.state = saveState();
    t.filmstripVisible = m_filmstripAction ? m_filmstripAction->isChecked() : true;
    t.rulersVisible = m_rulersAction ? m_rulersAction->isChecked() : false;
    m_viewTemplateStore.addOrUpdate(t);
    rebuildLayoutsMenu();
    m_statusLabel->setText(QString("Saved layout template \"%1\"").arg(t.name));
}

void RetouchWindow::onDeleteViewTemplate() {
    const QList<ViewTemplate> &custom = m_viewTemplateStore.custom();
    if (custom.isEmpty()) return;
    QStringList names;
    for (const ViewTemplate &t : custom) names << t.name;
    bool ok = false;
    const QString name = QInputDialog::getItem(this, "Delete Layout Template", "Template:",
                                                names, 0, false, &ok);
    if (!ok || name.isEmpty()) return;
    m_viewTemplateStore.remove(name);
    rebuildLayoutsMenu();
    m_statusLabel->setText(QString("Deleted layout template \"%1\"").arg(name));
}

// Re-apply the default dock arrangement to the (already-created) docks: all
// editing docks on the right, Levels split above the Adjustments/History/Layers
// tab group, Tools toolbar on the left. applyModeChrome then asserts
// mode-driven visibility (editing docks hidden in Tether mode; Controls shown
// there instead). Used on first launch (no saved state) and by Reset Panels.
void RetouchWindow::applyDefaultDockLayout() {
    for (QDockWidget *d : {m_levelsDock, m_adjustmentsDock, m_orientationDock,
                           m_historyDock, m_layersDock, m_controlsDock}) {
        if (d) {
            d->setFloating(false);
            addDockWidget(Qt::RightDockWidgetArea, d);
        }
    }
    if (m_adjustmentsDock && m_orientationDock)
        tabifyDockWidget(m_adjustmentsDock, m_orientationDock);
    if (m_adjustmentsDock && m_historyDock)
        tabifyDockWidget(m_adjustmentsDock, m_historyDock);
    if (m_adjustmentsDock && m_layersDock)
        tabifyDockWidget(m_adjustmentsDock, m_layersDock);
    if (m_levelsDock && m_adjustmentsDock)
        splitDockWidget(m_levelsDock, m_adjustmentsDock, Qt::Vertical);
    if (m_toolsBar)
        addToolBar(Qt::LeftToolBarArea, m_toolsBar);

    // Default visibility for the persistent editing docks.
    if (m_adjustmentsDock)  m_adjustmentsDock->show();
    if (m_orientationDock)  m_orientationDock->show();
    if (m_historyDock)      m_historyDock->show();
    if (m_levelsDock)       m_levelsDock->show();
    if (m_layersDock)       m_layersDock->show();

    // Let mode/tool chrome have the final say on editing-dock/Controls/Tools
    // visibility.
    const bool tether = m_tetherModeAction && m_tetherModeAction->isChecked();
    applyModeChrome(tether ? Mode::Tether : Mode::Retouch);
}

// Narrow left icon toolbar: mutually-exclusive Zoom / Crop / Spot Heal tools.
// Only one can be active; selecting one restricts the mouse gestures the
// canvas responds to (e.g. marquee-drag zoom and Ctrl+wheel only work while
// the Zoom tool is selected).
void RetouchWindow::buildToolPanel() {
    m_toolsBar = new QToolBar("Tools", this);
    m_toolsBar->setObjectName("toolsBar");
    m_toolsBar->setOrientation(Qt::Vertical);
    m_toolsBar->setIconSize(QSize(22, 22));
    // A dark, sunken background on the active tool so its light icon stands out.
    m_toolsBar->setStyleSheet(
        "QToolButton { border: none; padding: 4px; border-radius: 4px; }"
        "QToolButton:hover { background: rgba(220,220,220,0.85); }"
        "QToolButton:checked { background: #3a3f47; }"
        "QToolButton:disabled { background: transparent; }");
    addToolBar(Qt::LeftToolBarArea, m_toolsBar);

    m_moveToggle = new QToolButton;
    m_moveToggle->setIcon(makeMoveIcon());
    m_moveToggle->setCheckable(true);
    m_moveToggle->setShortcut(QKeySequence(Qt::Key_V));
    ShortcutRegistry::instance().registerShortcut("tool.move", "Tools", "Move", m_moveToggle, QKeySequence(Qt::Key_V));
    m_moveToggle->setToolTip(
        "Move (V) — drag a shape/text/image layer to move it, or drag on a "
        "Paint/Brush layer's strokes; clipped to the active selection if one exists");
    m_toolsBar->addWidget(m_moveToggle);

    m_toolZoom = new QToolButton;
    m_toolZoom->setIcon(makeZoomIcon());
    m_toolZoom->setCheckable(true);
    m_toolZoom->setShortcut(QKeySequence(Qt::Key_Z));
    ShortcutRegistry::instance().registerShortcut("tool.zoom", "Tools", "Zoom", m_toolZoom, QKeySequence(Qt::Key_Z));
    m_toolZoom->setToolTip("Zoom (Z) — drag to marquee-zoom, Ctrl+wheel to zoom");
    m_toolsBar->addWidget(m_toolZoom);

    m_cropToggle = new QToolButton;
    m_cropToggle->setIcon(makeCropIcon());
    m_cropToggle->setCheckable(true);
    m_cropToggle->setShortcut(QKeySequence(Qt::Key_C));
    ShortcutRegistry::instance().registerShortcut("tool.crop", "Tools", "Crop", m_cropToggle, QKeySequence(Qt::Key_C));
    m_cropToggle->setToolTip("Crop (C)");
    m_toolsBar->addWidget(m_cropToggle);

    m_healToggle = new QToolButton;
    m_healToggle->setIcon(makeHealIcon());
    m_healToggle->setCheckable(true);
    m_healToggle->setShortcut(QKeySequence(Qt::Key_H));
    ShortcutRegistry::instance().registerShortcut("tool.heal", "Tools", "Spot Heal", m_healToggle, QKeySequence(Qt::Key_H));
    m_healToggle->setToolTip("Spot Heal (H) — click blemishes; Ctrl+wheel resizes brush");
    m_toolsBar->addWidget(m_healToggle);

    m_brushToggle = new QToolButton;
    m_brushToggle->setIcon(makeBrushToolIcon());
    m_brushToggle->setCheckable(true);
    m_brushToggle->setShortcut(QKeySequence(Qt::Key_B));
    ShortcutRegistry::instance().registerShortcut("tool.brush", "Tools", "Brush", m_brushToggle, QKeySequence(Qt::Key_B));
    m_brushToggle->setToolTip("Brush (B) — paint with the foreground color; Ctrl+wheel resizes brush");
    m_toolsBar->addWidget(m_brushToggle);

    m_bucketToggle = new QToolButton;
    m_bucketToggle->setIcon(makePaintBucketIcon());
    m_bucketToggle->setCheckable(true);
    m_bucketToggle->setToolTip("Paint Bucket — click to flood-fill an enclosed area on a Paint layer with the foreground color");
    m_toolsBar->addWidget(m_bucketToggle);

    m_eraseToggle = new QToolButton;
    m_eraseToggle->setIcon(makeEraseIcon());
    m_eraseToggle->setCheckable(true);
    m_eraseToggle->setShortcut(QKeySequence(Qt::Key_E));
    ShortcutRegistry::instance().registerShortcut("tool.erase", "Tools", "Erase", m_eraseToggle, QKeySequence(Qt::Key_E));
    m_eraseToggle->setToolTip("Erase (E) — erase brush strokes on a paint layer, or paint transparency onto an image layer; Ctrl+wheel resizes brush");
    m_toolsBar->addWidget(m_eraseToggle);

    m_removeObjectToggle = new QToolButton;
    m_removeObjectToggle->setIcon(makeRemoveObjectIcon());
    m_removeObjectToggle->setCheckable(true);
    m_removeObjectToggle->setShortcut(QKeySequence(Qt::Key_J));
    ShortcutRegistry::instance().registerShortcut("tool.removeObject", "Tools", "Remove Object", m_removeObjectToggle, QKeySequence(Qt::Key_J));
    m_removeObjectToggle->setToolTip("Remove Object (J) — paint over an object to remove it; Ctrl+wheel resizes brush");
    m_toolsBar->addWidget(m_removeObjectToggle);

    m_penToggle = new QToolButton;
    m_penToggle->setIcon(makeCrayonToolIcon());
    m_penToggle->setCheckable(true);
    m_penToggle->setShortcut(QKeySequence(Qt::Key_N));
    ShortcutRegistry::instance().registerShortcut("tool.pen", "Tools", "Pen", m_penToggle, QKeySequence(Qt::Key_N));
    m_penToggle->setToolTip("Pen (N) — pencil strokes with grade-driven hardness/texture; Ctrl+wheel resizes");
    m_toolsBar->addWidget(m_penToggle);

    m_textToggle = new QToolButton;
    m_textToggle->setIcon(makeTextIcon());
    m_textToggle->setCheckable(true);
    m_textToggle->setShortcut(QKeySequence(Qt::Key_T));
    ShortcutRegistry::instance().registerShortcut("tool.text", "Tools", "Text", m_textToggle, QKeySequence(Qt::Key_T));
    m_textToggle->setToolTip("Text (T) — click to place, drag to move, drag the handle to rotate");
    m_toolsBar->addWidget(m_textToggle);

    m_shapeToggle = new QToolButton;
    m_shapeToggle->setIcon(makeShapeIcon());
    m_shapeToggle->setCheckable(true);
    m_shapeToggle->setShortcut(QKeySequence(Qt::Key_U));
    ShortcutRegistry::instance().registerShortcut("tool.shape", "Tools", "Shape", m_shapeToggle, QKeySequence(Qt::Key_U));
    m_shapeToggle->setToolTip("Shape (U) — drag to draw, drag handles to move/resize/rotate");
    m_toolsBar->addWidget(m_shapeToggle);

    m_selectMarqueeToggle = new QToolButton;
    m_selectMarqueeToggle->setIcon(makeSelectMarqueeIcon());
    m_selectMarqueeToggle->setCheckable(true);
    m_selectMarqueeToggle->setShortcut(QKeySequence(Qt::Key_M));
    ShortcutRegistry::instance().registerShortcut("tool.selectMarquee", "Tools", "Rectangular Selection", m_selectMarqueeToggle, QKeySequence(Qt::Key_M));
    m_selectMarqueeToggle->setToolTip(
        "Rectangular Selection (M) — drag to select; Shift adds, Alt subtracts");
    m_toolsBar->addWidget(m_selectMarqueeToggle);

    m_selectLassoToggle = new QToolButton;
    m_selectLassoToggle->setIcon(makeSelectLassoIcon());
    m_selectLassoToggle->setCheckable(true);
    m_selectLassoToggle->setShortcut(QKeySequence(Qt::Key_L));
    ShortcutRegistry::instance().registerShortcut("tool.selectLasso", "Tools", "Lasso Selection", m_selectLassoToggle, QKeySequence(Qt::Key_L));
    m_selectLassoToggle->setToolTip(
        "Lasso Selection (L) — drag a freehand outline; Shift adds, Alt subtracts");
    m_toolsBar->addWidget(m_selectLassoToggle);

    m_selectWandToggle = new QToolButton;
    m_selectWandToggle->setIcon(makeSelectWandIcon());
    m_selectWandToggle->setCheckable(true);
    m_selectWandToggle->setShortcut(QKeySequence(Qt::Key_W));
    ShortcutRegistry::instance().registerShortcut("tool.selectWand", "Tools", "Magic Wand", m_selectWandToggle, QKeySequence(Qt::Key_W));
    m_selectWandToggle->setToolTip(
        "Magic Wand (W) — click to select similar colors; Shift adds, Alt subtracts");
    m_toolsBar->addWidget(m_selectWandToggle);

    m_selectBrushToggle = new QToolButton;
    m_selectBrushToggle->setIcon(makeSelectBrushIcon());
    m_selectBrushToggle->setCheckable(true);
    m_selectBrushToggle->setShortcut(QKeySequence(Qt::Key_Q));
    ShortcutRegistry::instance().registerShortcut("tool.selectBrush", "Tools", "Selection Brush", m_selectBrushToggle, QKeySequence(Qt::Key_Q));
    m_selectBrushToggle->setToolTip(
        "Selection Brush (Q) — drag to add to the selection; Alt subtracts");
    m_toolsBar->addWidget(m_selectBrushToggle);

    m_cloneToggle = new QToolButton;
    m_cloneToggle->setIcon(makeCloneStampIcon());
    m_cloneToggle->setCheckable(true);
    m_cloneToggle->setShortcut(QKeySequence(Qt::Key_S));
    ShortcutRegistry::instance().registerShortcut("tool.clone", "Tools", "Clone Stamp", m_cloneToggle, QKeySequence(Qt::Key_S));
    m_cloneToggle->setToolTip(
        "Clone Stamp (S) — Alt+click to set the source, drag to paint from it");
    m_toolsBar->addWidget(m_cloneToggle);

    m_maskToggle = new FlyoutToolButton;
    m_maskToggle->setIcon(makeFlyoutToolIcon(maskGlyph(m_activeMaskSubtool)));
    m_maskToggle->setCheckable(true);
    m_maskToggle->setShortcut(QKeySequence(Qt::Key_K));
    ShortcutRegistry::instance().registerShortcut("tool.mask", "Tools", "Local Masks", m_maskToggle, QKeySequence(Qt::Key_K));
    m_maskToggle->setToolTip("Local Masks (K) — click to add; hold for radial / graduated / brush");
    m_toolsBar->addWidget(m_maskToggle);
    connect(m_maskToggle, &FlyoutToolButton::flyoutRequested, this,
            [this] { openMaskFlyout(); });

    m_colorSwatch = new ColorSwatchWidget;
    m_toolsBar->addWidget(m_colorSwatch);

    auto *swapShortcut = new QShortcut(QKeySequence(Qt::Key_X), this);
    connect(swapShortcut, &QShortcut::activated, m_colorSwatch, &ColorSwatchWidget::swapColors);
    ShortcutRegistry::instance().registerShortcut("canvas.swapColors", "Canvas", "Swap foreground/background color", swapShortcut, QKeySequence(Qt::Key_X));
    auto *resetShortcut = new QShortcut(QKeySequence(Qt::Key_D), this);
    connect(resetShortcut, &QShortcut::activated, m_colorSwatch, &ColorSwatchWidget::resetColors);
    ShortcutRegistry::instance().registerShortcut("canvas.resetColors", "Canvas", "Reset colors", resetShortcut, QKeySequence(Qt::Key_D));

    // Photoshop-style fill shortcuts: Ctrl+Backspace = background color,
    // Alt+Backspace = foreground color.
    auto *fillBgShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backspace), this);
    connect(fillBgShortcut, &QShortcut::activated, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) tab->fillActiveMask(m_colorSwatch->backgroundColor());
    });
    ShortcutRegistry::instance().registerShortcut("canvas.fillBackground", "Canvas", "Fill with background color", fillBgShortcut, QKeySequence(Qt::CTRL | Qt::Key_Backspace));
    auto *fillFgShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Backspace), this);
    connect(fillFgShortcut, &QShortcut::activated, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) tab->fillActiveMask(m_colorSwatch->foregroundColor());
    });
    ShortcutRegistry::instance().registerShortcut("canvas.fillForeground", "Canvas", "Fill with foreground color", fillFgShortcut, QKeySequence(Qt::ALT | Qt::Key_Backspace));

    connect(m_colorSwatch, &ColorSwatchWidget::foregroundColorChanged, this,
            [this](const QColor &c) {
                RetouchTab *tab = currentTab();
                if (tab && tab->isReady()) tab->setPaintColor(c);
            });

    // Each tool turns off every other tool (and the WB eyedropper) when
    // selected, and swaps in that tool's options row under the main toolbar.
    // The mutual-exclusion bookkeeping itself lives in
    // deactivateOtherToolButtons()/deactivateAllToolModes() (see RetouchWindow.h)
    // so adding a new tool only means adding it to those two helpers' lists,
    // not editing every existing tool's handler below.
    connect(m_toolZoom, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_toolZoom);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(0);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) tab->setZoomMode(on);
    });
    connect(m_cropToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_cropToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(1);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) {
            tab->setCropMode(on);
            if (on) tab->setCropAspect(m_cropAspect->currentData().toDouble());
        }
    });
    connect(m_healToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_healToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(2);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) {
            tab->setHealBrush(m_healBrush->value());
            tab->setHealMode(on);
        }
    });
    connect(m_maskToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        // Radial and Linear subtools operate against the currently selected
        // mask (must already be that type); "Brush" (mask-brush) and "Layer"
        // (None) subtools keep the prior always-create-a-new-layer behaviour,
        // since they aren't part of this gating (see canActivateTool docs).
        const bool isGatedSubtool = m_activeMaskSubtool == MaskType::Radial ||
                                    m_activeMaskSubtool == MaskType::Linear;
        if (on && isGatedSubtool && (!tab || !tab->isReady() ||
                                     !tab->canActivateTool(m_activeMaskSubtool))) {
            // Defensive guard: the toggle should already be disabled unless
            // the selection matches the active subtool (see refreshMaskPanel);
            // this only fires from a stale keyboard shortcut or similar.
            QSignalBlocker b(m_maskToggle);
            m_maskToggle->setChecked(false);
            return;
        }
        if (on) {
            deactivateOtherToolButtons(m_maskToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsBar->setVisible(false); // layers/masks use their own docks
            if (m_layersDock) { m_layersDock->show(); m_layersDock->raise(); }
        }
        // Layers/Masks panels stay visible when the K tool is toggled off —
        // they're persistent docks, not transient tool-options popups.
        if (tab && tab->isReady()) tab->setMaskMode(on);
        // A plain click on the tool creates a mask of the active subtool,
        // except for the gated Radial/Linear subtools: those activate against
        // the existing (already-verified-matching) selection instead.
        if (on && !isGatedSubtool) addActiveMask();
        refreshMaskPanel();
    });
    connect(m_brushToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            // Defensive guard: the toggle should already be disabled unless
            // the current selection is a Paint layer (see refreshMaskPanel);
            // this only fires if something enabled it anyway (e.g. a stale
            // keyboard shortcut) or triggered it before selection existed.
            if (!tab || !tab->isReady() || !tab->canActivateTool(MaskType::Paint)) {
                QSignalBlocker b(m_brushToggle);
                m_brushToggle->setChecked(false);
                return;
            }
            deactivateOtherToolButtons(m_brushToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(3);
            m_toolOptionsBar->setVisible(true);
            tab->setPaintColor(m_colorSwatch->foregroundColor());
            if (!m_paintSizeCustomized) {
                m_syncingPaintSize = true;
                m_paintSize->setValue(20);
                m_syncingPaintSize = false;
            }
            updatePaintSizePxLabel();
            {
                const int w = tab->imageWidth();
                const double norm = w > 0 ? m_paintSize->value() / double(w) : 0.006;
                tab->setActiveMaskShape(false, 0.0, m_paintHardness->value() / 100.0, norm, false);
            }
            tab->setActiveMaskOpacity(m_paintOpacity->value() / 100.0);
            tab->setPenToolActive(false);
            tab->setMaskForceErase(false);
            tab->setMaskMode(true);
            if (m_layersDock) { m_layersDock->show(); m_layersDock->raise(); }
            refreshMaskPanel();
        } else {
            m_toolOptionsBar->setVisible(false);
            if (tab && tab->isReady()) tab->setMaskMode(false);
        }
    });
    connect(m_penToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            if (!tab || !tab->isReady()) {
                QSignalBlocker b(m_penToggle);
                m_penToggle->setChecked(false);
                return;
            }
            // Pen shares the same Paint-type layer as Brush (see
            // BrushStrokePoint::isPen) so the two tools can be freely
            // switched between without leaving the layer. If nothing Paint-
            // type is selected yet, auto-create one, mirroring LayersPanel's
            // addMaskRequested(MaskType::Paint), so the toolbar button works
            // with a single click like Brush does.
            if (!tab->canActivateTool(MaskType::Paint)) {
                tab->addMask(MaskType::Paint);
                refreshMaskPanel();
            }
            if (!tab->canActivateTool(MaskType::Paint)) {
                QSignalBlocker b(m_penToggle);
                m_penToggle->setChecked(false);
                return;
            }
            deactivateOtherToolButtons(m_penToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(8);
            m_toolOptionsBar->setVisible(true);
            if (!m_penSizeCustomized && tab->imageWidth() > 0) {
                // A pencil should read as a thin, precise line, not a brush —
                // default to ~2px wide (in the finer v/1000.0 scale) rather
                // than reusing Brush's ~20px-equivalent default, which made
                // Pen strokes look identical to Brush.
                m_syncingPaintSize = true;
                m_penSize->setValue(std::clamp(
                    int(std::lround(2.0 / tab->imageWidth() * 1000.0)), 1, 100));
                m_syncingPaintSize = false;
            }
            updatePenGradeLabel();
            tab->setActiveMaskShape(false, 0.0, 0.5, m_penSize->value() / 1000.0, false);
            tab->setActivePenGrade(double(m_penGrade->value()));
            tab->setPenToolActive(true);
            tab->setMaskForceErase(false);
            tab->setMaskMode(true);
            if (m_layersDock) { m_layersDock->show(); m_layersDock->raise(); }
            refreshMaskPanel();
        } else {
            m_toolOptionsBar->setVisible(false);
            if (tab && tab->isReady()) { tab->setPenToolActive(false); tab->setMaskMode(false); }
        }
    });
    connect(m_bucketToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            // Only makes sense against an existing Paint layer's own painted
            // content (e.g. a brush outline to fill the inside of) — same
            // gating as the Brush tool, see canActivateTool docs.
            if (!tab || !tab->isReady() || !tab->canActivateTool(MaskType::Paint)) {
                QSignalBlocker b(m_bucketToggle);
                m_bucketToggle->setChecked(false);
                return;
            }
            deactivateOtherToolButtons(m_bucketToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsBar->setVisible(false); // no per-tool options; fills with the current foreground color
            tab->setBucketMode(true);
        } else {
            if (tab && tab->isReady()) tab->setBucketMode(false);
        }
    });
    connect(m_eraseToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        // Erase is a single unified tool: it works the same way (punching a
        // feathered reduction into the active layer's compositing weight,
        // via eraseStrokes) regardless of layer type — image, paint, brush,
        // shape, text, text box, background, or an adjustment mask.
        if (on) {
            deactivateOtherToolButtons(m_eraseToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(4);
            m_toolOptionsBar->setVisible(true);
            if (m_layersDock) { m_layersDock->show(); m_layersDock->raise(); }
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) {
            tab->setEraseBrush(m_eraseBrush->value());
            tab->setEraseMode(on);
        }
    });
    connect(m_removeObjectToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_removeObjectToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(7);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) {
            tab->setRemoveObjectBrush(m_removeObjectBrush->value());
            tab->setRemoveObjectMode(on);
        }
    });
    connect(m_textToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_textToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(5);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) tab->setTextMode(on);
        updateTextOptionsFromTab();
    });
    connect(m_shapeToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_shapeToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(6);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) tab->setShapeMode(on);
        updateShapeOptionsFromTab();
    });
    connect(m_moveToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_moveToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsBar->setVisible(false); // no per-tool options
        }
        if (tab && tab->isReady()) tab->setMoveMode(on);
    });
    connect(m_selectMarqueeToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_selectMarqueeToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsBar->setVisible(false); // no per-tool options
        }
        if (tab && tab->isReady()) tab->setSelectMarqueeMode(on);
    });
    connect(m_selectLassoToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_selectLassoToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsBar->setVisible(false); // no per-tool options
        }
        if (tab && tab->isReady()) tab->setSelectLassoMode(on);
    });
    connect(m_selectWandToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_selectWandToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(9);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) tab->setSelectMagicWandMode(on);
    });
    connect(m_selectBrushToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            deactivateOtherToolButtons(m_selectBrushToggle);
            deactivateAllToolModes(tab);
            if (tab) tab->setSelectBrushRadius(m_selectBrushSize->value() / 100.0);
            m_toolOptionsStack->setCurrentIndex(11);
            m_toolOptionsBar->setVisible(true);
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) tab->setSelectBrushMode(on);
    });
    connect(m_cloneToggle, &QToolButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            // Clone paints into a Paint-type layer, same gating/auto-create
            // pattern as Pen (see m_penToggle's handler).
            if (!tab || !tab->isReady()) {
                QSignalBlocker b(m_cloneToggle);
                m_cloneToggle->setChecked(false);
                return;
            }
            if (!tab->canActivateTool(MaskType::Paint)) {
                tab->addMask(MaskType::Paint);
                // addMask() turns on drag-mask mode for the new Paint layer,
                // which would swallow Clone's own alt-click/drag handling
                // below (see the comment further down). Turn it back off.
                tab->setMaskMode(false);
                refreshMaskPanel();
            }
            if (!tab->canActivateTool(MaskType::Paint)) {
                QSignalBlocker b(m_cloneToggle);
                m_cloneToggle->setChecked(false);
                return;
            }
            deactivateOtherToolButtons(m_cloneToggle);
            deactivateAllToolModes(tab);
            m_toolOptionsStack->setCurrentIndex(10);
            m_toolOptionsBar->setVisible(true);
            // Deliberately does NOT call tab->setMaskMode(true): that would
            // put ImageCanvas into its generic interactive-mask-editing input
            // path (mousePressEvent's m_maskMode block, which emits
            // maskBrushPoint directly) and that block is checked before the
            // clone block, so it would swallow every press before Clone's
            // own offset-sampling handling ever ran. Clone uses its own
            // m_cloneMode input path (see ImageCanvas::setCloneMode);
            // addMask()/canActivateTool above already make the Paint layer
            // "active" so its gizmo/overlay still shows via the normal
            // selection mechanism.
            {
                const int w = tab->imageWidth();
                const double norm = w > 0 ? m_cloneSize->value() / double(w) : 0.006;
                tab->setActiveMaskShape(false, 0.0, 1.0, norm, false);
            }
            if (m_layersDock) { m_layersDock->show(); m_layersDock->raise(); }
            refreshMaskPanel();
        } else {
            m_toolOptionsBar->setVisible(false);
        }
        if (tab && tab->isReady()) tab->setCloneMode(on);
    });
}

// Contextual options row shown under the main toolbar only while a left-bar
// tool (Zoom/Crop/Heal) is selected; each tool gets its own page in the stack.
void RetouchWindow::buildToolOptionsBar() {
    addToolBarBreak(Qt::TopToolBarArea);
    m_toolOptionsBar = new QToolBar("Tool Options", this);
    m_toolOptionsBar->setMovable(false);
    addToolBar(Qt::TopToolBarArea, m_toolOptionsBar);
    m_toolOptionsBar->setVisible(false); // hidden until a tool is selected

    m_toolOptionsStack = new QStackedWidget;

    // --- Zoom page (index 0) ---
    auto *zoomPage = new QWidget;
    auto *zoomRow = new QHBoxLayout(zoomPage);
    zoomRow->setContentsMargins(4, 2, 4, 2);
    m_zoomFit = new QPushButton("Fit");
    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setRange(10, 800);
    m_zoomSlider->setValue(100);
    m_zoomSlider->setMinimumWidth(160);
    m_zoomLabel = new QLabel("100%");
    m_zoomLabel->setMinimumWidth(44);
    zoomRow->addWidget(new QLabel("Zoom:"));
    zoomRow->addWidget(m_zoomFit);
    zoomRow->addWidget(m_zoomSlider);
    zoomRow->addWidget(m_zoomLabel);
    zoomRow->addStretch(1);
    m_toolOptionsStack->addWidget(zoomPage);

    connect(m_zoomFit, &QPushButton::clicked, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) tab->zoomFit();
    });
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_syncing) return;
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) tab->setZoomPercent(v);
    });

    // --- Crop page (index 1) ---
    auto *cropPage = new QWidget;
    auto *cropRow = new QHBoxLayout(cropPage);
    cropRow->setContentsMargins(4, 2, 4, 2);
    m_cropAspect = new QComboBox;
    m_cropAspect->addItem("Freeform", 0.0);
    m_cropAspect->addItem("1:1 (square)", 1.0);
    m_cropAspect->addItem("3:2", 3.0 / 2.0);
    m_cropAspect->addItem("4:3", 4.0 / 3.0);
    m_cropAspect->addItem("5:4", 5.0 / 4.0);
    m_cropAspect->addItem("16:9", 16.0 / 9.0);
    m_cropAspect->addItem("2:3 (portrait)", 2.0 / 3.0);
    m_cropAspect->addItem("3:4 (portrait)", 3.0 / 4.0);
    m_cropAspect->addItem("9:16 (portrait)", 9.0 / 16.0);
    m_cropApply = new QPushButton("Apply Crop");
    m_cropReset = new QPushButton("Reset Crop");
    m_cropApply->setEnabled(false);
    cropRow->addWidget(new QLabel("Ratio:"));
    cropRow->addWidget(m_cropAspect);
    cropRow->addWidget(m_cropApply);
    cropRow->addWidget(m_cropReset);
    cropRow->addStretch(1);
    m_toolOptionsStack->addWidget(cropPage);

    connect(m_cropAspect, &QComboBox::currentIndexChanged, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->setCropAspect(m_cropAspect->currentData().toDouble());
    });
    connect(m_cropApply, &QPushButton::clicked, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->applyCrop();
        m_cropToggle->setChecked(false); // also hides this options row
    });
    connect(m_cropReset, &QPushButton::clicked, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->resetCrop();
    });

    // --- Spot Heal page (index 2) ---
    auto *healPage = new QWidget;
    auto *healRow = new QHBoxLayout(healPage);
    healRow->setContentsMargins(4, 2, 4, 2);
    m_healBrush = new QSlider(Qt::Horizontal);
    m_healBrush->setRange(4, 80);
    m_healBrush->setValue(20);
    m_healBrush->setMinimumWidth(160);
    m_healClear = new QPushButton("Clear Spots");
    healRow->addWidget(new QLabel("Brush size:"));
    healRow->addWidget(m_healBrush);
    healRow->addWidget(m_healClear);
    healRow->addStretch(1);
    m_toolOptionsStack->addWidget(healPage);

    connect(m_healBrush, &QSlider::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setHealBrush(v);
    });
    connect(m_healClear, &QPushButton::clicked, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->clearHeals();
    });

    // --- Brush page (index 3) ---
    auto *brushPage = new QWidget;
    auto *brushRow = new QHBoxLayout(brushPage);
    brushRow->setContentsMargins(4, 2, 4, 2);
    m_paintSize = new QSlider(Qt::Horizontal);
    m_paintSize->setRange(1, 2000); // image px (raw pixel diameter at full source resolution)
    m_paintSize->setValue(20); // placeholder until the Brush-toggle handler picks a real default
    m_paintSize->setMinimumWidth(120);
    m_paintSizePx = new QLabel;
    m_paintSizePx->setMinimumWidth(40);
    m_paintSizePx->setStyleSheet("color: #999;");
    m_paintHardness = new QSlider(Qt::Horizontal);
    m_paintHardness->setRange(0, 100);
    m_paintHardness->setValue(100);
    m_paintHardness->setMinimumWidth(100);
    m_paintOpacity = new QSlider(Qt::Horizontal);
    m_paintOpacity->setRange(1, 100);
    m_paintOpacity->setValue(100);
    m_paintOpacity->setMinimumWidth(100);
    brushRow->addWidget(new QLabel("Size:"));
    brushRow->addWidget(m_paintSize);
    brushRow->addWidget(m_paintSizePx);
    brushRow->addWidget(new QLabel("Hardness:"));
    brushRow->addWidget(m_paintHardness);
    brushRow->addWidget(new QLabel("Opacity:"));
    brushRow->addWidget(m_paintOpacity);
    m_brushToolPresets = new BrushPresetMenuButton;
    brushRow->addWidget(m_brushToolPresets);
    brushRow->addStretch(1);
    m_toolOptionsStack->addWidget(brushPage);

    connect(m_paintSize, &QSlider::valueChanged, this, [this](int v) {
        if (!m_syncingPaintSize) m_paintSizeCustomized = true;
        updatePaintSizePxLabel();
        RetouchTab *tab = currentTab();
        if (tab) {
            const int w = tab->imageWidth();
            const double norm = w > 0 ? v / double(w) : 0.006;
            tab->setActiveMaskShape(false, 0.0, m_paintHardness->value() / 100.0, norm, false);
        }
    });
    connect(m_paintHardness, &QSlider::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) {
            const int w = tab->imageWidth();
            const double norm = w > 0 ? m_paintSize->value() / double(w) : 0.006;
            tab->setActiveMaskShape(false, 0.0, v / 100.0, norm, false);
        }
    });
    connect(m_paintOpacity, &QSlider::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setActiveMaskOpacity(v / 100.0);
    });
    connect(m_brushToolPresets, &BrushPresetMenuButton::presetApplied, this,
            [this](double brushRadius, double hardness) {
                RetouchTab *tab = currentTab();
                const int w = tab ? tab->imageWidth() : 0;
                if (w > 0) m_paintSize->setValue(std::clamp(int(std::lround(brushRadius * w)), 1, 2000));
                m_paintHardness->setValue(int(std::lround(hardness * 100)));
            });
    connect(m_brushToolPresets, &QToolButton::pressed, this, [this] {
        RetouchTab *tab = currentTab();
        const int w = tab ? tab->imageWidth() : 0;
        const double norm = w > 0 ? m_paintSize->value() / double(w) : 0.006;
        m_brushToolPresets->setCurrentValues(norm,
                                             m_paintHardness->value() / 100.0);
    });

    // --- Erase page (index 4) ---
    auto *erasePage = new QWidget;
    auto *eraseRow = new QHBoxLayout(erasePage);
    eraseRow->setContentsMargins(4, 2, 4, 2);
    m_eraseBrush = new QSlider(Qt::Horizontal);
    m_eraseBrush->setRange(1, 80);
    m_eraseBrush->setValue(20);
    m_eraseBrush->setMinimumWidth(160);
    eraseRow->addWidget(new QLabel("Brush size:"));
    eraseRow->addWidget(m_eraseBrush);
    eraseRow->addStretch(1);
    m_toolOptionsStack->addWidget(erasePage);

    connect(m_eraseBrush, &QSlider::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setEraseBrush(v);
    });

    // --- Text page (index 5) ---
    auto *textPage = new QWidget;
    auto *textRow = new QHBoxLayout(textPage);
    textRow->setContentsMargins(4, 2, 4, 2);

    m_textFont = new QFontComboBox;
    m_textFont->setMinimumWidth(140);
    auto *textSizeScrub = new ScrubSpinBox;
    textSizeScrub->setScrubPixelsPerStep(4);
    m_textSize = textSizeScrub;
    m_textSize->setRange(4, 500);
    m_textSize->setValue(48);
    m_textSize->setSuffix(" px");
    m_textBold = new QToolButton;
    m_textBold->setText("B");
    m_textBold->setCheckable(true);
    m_textItalic = new QToolButton;
    m_textItalic->setText("I");
    m_textItalic->setCheckable(true);
    m_textColorBtn = new QPushButton("Color");

    m_textOutlineColorBtn = new QPushButton("Color");
    auto *outlineWidthScrub = new ScrubDoubleSpinBox;
    outlineWidthScrub->setScrubPixelsPerStep(4.0);
    m_textOutlineWidth = outlineWidthScrub;
    m_textOutlineWidth->setRange(0.0, 60.0); // 0 = effectively off, no separate enable control
    m_textOutlineWidth->setValue(3.0);
    m_textOutlineWidth->setSuffix(" px");

    m_textShadowColorBtn = new QPushButton("Color");
    auto *shadowBlurScrub = new ScrubDoubleSpinBox;
    shadowBlurScrub->setScrubPixelsPerStep(4.0);
    m_textShadowBlur = shadowBlurScrub;
    m_textShadowBlur->setRange(0.0, 100.0);
    m_textShadowBlur->setValue(14.0);
    m_textShadowBlur->setSuffix(" px");
    auto *shadowOpacityScrub = new ScrubDoubleSpinBox;
    shadowOpacityScrub->setScrubPixelsPerStep(150.0); // 0..1 range: slow, precise drag
    m_textShadowOpacity = shadowOpacityScrub;
    m_textShadowOpacity->setRange(0.0, 1.0);
    m_textShadowOpacity->setSingleStep(0.05);
    m_textShadowOpacity->setValue(0.75);

    m_textBgColorBtn = new QPushButton("Color");
    auto *bgOpacityScrub = new ScrubDoubleSpinBox;
    bgOpacityScrub->setScrubPixelsPerStep(150.0);
    m_textBgOpacity = bgOpacityScrub;
    m_textBgOpacity->setRange(0.0, 1.0);
    m_textBgOpacity->setSingleStep(0.05);
    m_textBgOpacity->setValue(0.6);
    auto *bgPaddingScrub = new ScrubDoubleSpinBox;
    bgPaddingScrub->setScrubPixelsPerStep(3.0);
    m_textBgPadding = bgPaddingScrub;
    m_textBgPadding->setRange(0.0, 200.0);
    m_textBgPadding->setValue(10.0);
    m_textBgPadding->setSuffix(" px");

    m_textDelete = new QPushButton("Delete");

    textRow->addWidget(m_textFont);
    textRow->addWidget(m_textSize);
    textRow->addWidget(m_textBold);
    textRow->addWidget(m_textItalic);
    textRow->addWidget(new QLabel("Fill:"));
    textRow->addWidget(m_textColorBtn);
    textRow->addWidget(new QLabel("Outline:"));
    textRow->addWidget(m_textOutlineColorBtn);
    textRow->addWidget(new QLabel("Width:"));
    textRow->addWidget(m_textOutlineWidth);
    auto *sep1 = new QFrame; sep1->setFrameShape(QFrame::VLine); sep1->setFrameShadow(QFrame::Sunken);
    textRow->addWidget(sep1);
    textRow->addWidget(new QLabel("Shadow:"));
    textRow->addWidget(m_textShadowColorBtn);
    textRow->addWidget(new QLabel("Blur:"));
    textRow->addWidget(m_textShadowBlur);
    textRow->addWidget(new QLabel("Opacity:"));
    textRow->addWidget(m_textShadowOpacity);
    auto *sep2 = new QFrame; sep2->setFrameShape(QFrame::VLine); sep2->setFrameShadow(QFrame::Sunken);
    textRow->addWidget(sep2);
    textRow->addWidget(new QLabel("Background:"));
    textRow->addWidget(m_textBgColorBtn);
    textRow->addWidget(new QLabel("Opacity:"));
    textRow->addWidget(m_textBgOpacity);
    textRow->addWidget(new QLabel("Padding:"));
    textRow->addWidget(m_textBgPadding);
    textRow->addWidget(m_textDelete);
    textRow->addStretch(1);
    m_toolOptionsStack->addWidget(textPage);

    // Each group is pushed independently, and always as enabled=true — there's
    // no separate on/off toggle; touching that group's color or any of its
    // sliders is itself what turns it on. (To turn one off, drag its
    // width/opacity down to 0 — the renderer already treats that as invisible.)
    auto pushFontStyle = [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        tab->setTextFont(m_textFont->currentFont().family(), m_textSize->value(),
                         m_textBold->isChecked(), m_textItalic->isChecked());
    };
    auto pushOutlineStyle = [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        tab->setTextOutline(true, m_textOutlineColorBtn->property("color").value<QColor>(),
                            m_textOutlineWidth->value());
    };
    auto pushShadowStyle = [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        tab->setTextShadow(true, QPointF(8, 8), m_textShadowBlur->value(),
                           m_textShadowOpacity->value(),
                           m_textShadowColorBtn->property("color").value<QColor>());
    };
    auto pushBgStyle = [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        tab->setTextBackground(true, m_textBgColorBtn->property("color").value<QColor>(),
                               m_textBgOpacity->value(), m_textBgPadding->value());
    };
    connect(m_textFont, &QFontComboBox::currentFontChanged, this, [pushFontStyle] { pushFontStyle(); });
    connect(m_textSize, &QSpinBox::valueChanged, this, [pushFontStyle] { pushFontStyle(); });
    connect(m_textBold, &QToolButton::toggled, this, [pushFontStyle] { pushFontStyle(); });
    connect(m_textItalic, &QToolButton::toggled, this, [pushFontStyle] { pushFontStyle(); });
    connect(m_textOutlineWidth, &QDoubleSpinBox::valueChanged, this, [pushOutlineStyle] { pushOutlineStyle(); });
    connect(m_textShadowBlur, &QDoubleSpinBox::valueChanged, this, [pushShadowStyle] { pushShadowStyle(); });
    connect(m_textShadowOpacity, &QDoubleSpinBox::valueChanged, this, [pushShadowStyle] { pushShadowStyle(); });
    connect(m_textBgOpacity, &QDoubleSpinBox::valueChanged, this, [pushBgStyle] { pushBgStyle(); });
    connect(m_textBgPadding, &QDoubleSpinBox::valueChanged, this, [pushBgStyle] { pushBgStyle(); });

    connect(m_textColorBtn, &QPushButton::clicked, this, [this] {
        QColor c = QColorDialog::getColor(m_textColorBtn->property("color").value<QColor>(),
                                          this, "Text Color", QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        setColorSwatchButton(m_textColorBtn, c);
        RetouchTab *tab = currentTab();
        if (tab) tab->setTextColor(c);
    });
    connect(m_textOutlineColorBtn, &QPushButton::clicked, this, [this, pushOutlineStyle] {
        QColor c = QColorDialog::getColor(m_textOutlineColorBtn->property("color").value<QColor>(),
                                          this, "Outline Color", QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        setColorSwatchButton(m_textOutlineColorBtn, c);
        pushOutlineStyle();
    });
    connect(m_textShadowColorBtn, &QPushButton::clicked, this, [this, pushShadowStyle] {
        QColor c = QColorDialog::getColor(m_textShadowColorBtn->property("color").value<QColor>(),
                                          this, "Shadow Color", QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        setColorSwatchButton(m_textShadowColorBtn, c);
        pushShadowStyle();
    });
    connect(m_textBgColorBtn, &QPushButton::clicked, this, [this, pushBgStyle] {
        QColor c = QColorDialog::getColor(m_textBgColorBtn->property("color").value<QColor>(),
                                          this, "Background Color", QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        setColorSwatchButton(m_textBgColorBtn, c);
        pushBgStyle();
    });
    connect(m_textDelete, &QPushButton::clicked, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->deleteActiveText();
        updateTextOptionsFromTab();
    });

    setColorSwatchButton(m_textColorBtn, Qt::black);
    setColorSwatchButton(m_textOutlineColorBtn, Qt::black);
    setColorSwatchButton(m_textShadowColorBtn, Qt::black);
    setColorSwatchButton(m_textBgColorBtn, Qt::black);

    // --- Shape page (index 6) ---
    auto *shapePage = new QWidget;
    auto *shapeRow = new QHBoxLayout(shapePage);
    shapeRow->setContentsMargins(4, 2, 4, 2);

    m_shapeType = new QComboBox;
    m_shapeType->addItem("Rectangle", int(ShapeType::Rectangle));
    m_shapeType->addItem("Ellipse", int(ShapeType::Ellipse));
    m_shapeType->addItem("Line", int(ShapeType::Line));
    m_shapeType->addItem("Polygon", int(ShapeType::Polygon));
    m_shapeType->addItem("Star", int(ShapeType::Star));
    m_shapeType->addItem("Heart", int(ShapeType::Heart));

    auto *sidesScrub = new ScrubSpinBox;
    sidesScrub->setScrubPixelsPerStep(6);
    m_shapeSides = sidesScrub;
    m_shapeSides->setRange(3, 20);
    m_shapeSides->setValue(5);

    auto *innerRatioScrub = new ScrubDoubleSpinBox;
    innerRatioScrub->setScrubPixelsPerStep(150.0);
    m_shapeInnerRatio = innerRatioScrub;
    m_shapeInnerRatio->setRange(0.1, 0.9);
    m_shapeInnerRatio->setSingleStep(0.05);
    m_shapeInnerRatio->setValue(0.5);

    m_shapeFillEnabled = new QCheckBox("Fill:");
    m_shapeFillEnabled->setChecked(true);
    m_shapeFillColorBtn = new QPushButton("Color");

    m_shapeStrokeEnabled = new QCheckBox("Stroke:");
    m_shapeStrokeEnabled->setChecked(true);
    m_shapeStrokeColorBtn = new QPushButton("Color");
    auto *strokeWidthScrub = new ScrubDoubleSpinBox;
    strokeWidthScrub->setScrubPixelsPerStep(4.0);
    m_shapeStrokeWidth = strokeWidthScrub;
    m_shapeStrokeWidth->setRange(0.0, 200.0);
    m_shapeStrokeWidth->setValue(4.0);
    m_shapeStrokeWidth->setSuffix(" px");

    m_shapeDelete = new QPushButton("Delete");

    shapeRow->addWidget(new QLabel("Shape:"));
    shapeRow->addWidget(m_shapeType);
    shapeRow->addWidget(new QLabel("Sides:"));
    shapeRow->addWidget(m_shapeSides);
    shapeRow->addWidget(new QLabel("Inner:"));
    shapeRow->addWidget(m_shapeInnerRatio);
    auto *shapeSep1 = new QFrame; shapeSep1->setFrameShape(QFrame::VLine); shapeSep1->setFrameShadow(QFrame::Sunken);
    shapeRow->addWidget(shapeSep1);
    shapeRow->addWidget(m_shapeFillEnabled);
    shapeRow->addWidget(m_shapeFillColorBtn);
    auto *shapeSep2 = new QFrame; shapeSep2->setFrameShape(QFrame::VLine); shapeSep2->setFrameShadow(QFrame::Sunken);
    shapeRow->addWidget(shapeSep2);
    shapeRow->addWidget(m_shapeStrokeEnabled);
    shapeRow->addWidget(m_shapeStrokeColorBtn);
    shapeRow->addWidget(new QLabel("Width:"));
    shapeRow->addWidget(m_shapeStrokeWidth);
    shapeRow->addWidget(m_shapeDelete);
    shapeRow->addStretch(1);
    m_toolOptionsStack->addWidget(shapePage);

    // --- Remove Object page (index 7) ---
    auto *removeObjectPage = new QWidget;
    auto *removeObjectRow = new QHBoxLayout(removeObjectPage);
    removeObjectRow->setContentsMargins(4, 2, 4, 2);
    m_removeObjectBrush = new QSlider(Qt::Horizontal);
    m_removeObjectBrush->setRange(8, 150);
    m_removeObjectBrush->setValue(30);
    m_removeObjectBrush->setMinimumWidth(160);
    removeObjectRow->addWidget(new QLabel("Brush size:"));
    removeObjectRow->addWidget(m_removeObjectBrush);
    removeObjectRow->addStretch(1);
    m_toolOptionsStack->addWidget(removeObjectPage);

    connect(m_removeObjectBrush, &QSlider::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setRemoveObjectBrush(v);
    });

    // --- Pen page (index 8) ---
    // Grade drives both hardness and opacity (see rasterizeBrush's Pen path
    // in Adjustments.cpp), so unlike the Brush page there's no separate
    // hardness/opacity slider — just Size and Grade.
    auto *penPage = new QWidget;
    auto *penRow = new QHBoxLayout(penPage);
    penRow->setContentsMargins(4, 2, 4, 2);
    m_penSize = new QSlider(Qt::Horizontal);
    // Tenths of a percent of image width (v/1000.0, vs Brush's v/100.0) —
    // a pencil needs to reach genuinely thin widths that Brush's coarser
    // 1%-of-width minimum can't represent.
    m_penSize->setRange(1, 100);
    m_penSize->setValue(5);
    m_penSize->setMinimumWidth(120);
    penRow->addWidget(new QLabel("Size:"));
    penRow->addWidget(m_penSize);
    m_penGrade = new QSlider(Qt::Horizontal);
    m_penGrade->setRange(-6, 5); // 6B..5H, in whole-grade steps
    m_penGrade->setValue(0);     // HB
    m_penGrade->setMinimumWidth(140);
    penRow->addWidget(new QLabel("6B"));
    penRow->addWidget(m_penGrade);
    penRow->addWidget(new QLabel("5H"));
    m_penGradeLabel = new QLabel("HB");
    m_penGradeLabel->setMinimumWidth(28);
    m_penGradeLabel->setStyleSheet("color: #999;");
    penRow->addWidget(m_penGradeLabel);
    penRow->addStretch(1);
    m_toolOptionsStack->addWidget(penPage);

    connect(m_penSize, &QSlider::valueChanged, this, [this](int v) {
        if (!m_syncingPaintSize) m_penSizeCustomized = true;
        RetouchTab *tab = currentTab();
        if (tab) tab->setActiveMaskShape(false, 0.0, 0.5, v / 1000.0, false);
    });
    connect(m_penGrade, &QSlider::valueChanged, this, [this](int v) {
        updatePenGradeLabel();
        RetouchTab *tab = currentTab();
        if (tab) tab->setActivePenGrade(double(v));
    });

    // --- Magic Wand page (index 9) ---
    auto *wandPage = new QWidget;
    auto *wandRow = new QHBoxLayout(wandPage);
    wandRow->setContentsMargins(4, 2, 4, 2);
    m_wandTolerance = new QSlider(Qt::Horizontal);
    m_wandTolerance->setRange(0, 255);
    m_wandTolerance->setValue(32);
    m_wandTolerance->setMinimumWidth(160);
    wandRow->addWidget(new QLabel("Tolerance:"));
    wandRow->addWidget(m_wandTolerance);
    wandRow->addStretch(1);
    m_toolOptionsStack->addWidget(wandPage);

    connect(m_wandTolerance, &QSlider::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setMagicWandTolerance(v);
    });

    // --- Clone Stamp page (index 10) ---
    auto *clonePage = new QWidget;
    auto *cloneRow = new QHBoxLayout(clonePage);
    cloneRow->setContentsMargins(4, 2, 4, 2);
    m_cloneSize = new QSlider(Qt::Horizontal);
    m_cloneSize->setRange(1, 400);
    m_cloneSize->setValue(20);
    m_cloneSize->setMinimumWidth(140);
    cloneRow->addWidget(new QLabel("Size:"));
    cloneRow->addWidget(m_cloneSize);
    cloneRow->addStretch(1);
    m_toolOptionsStack->addWidget(clonePage);

    // Clone always stamps at full hardness (hard edge): a soft edge blends a
    // dab sampled from elsewhere in the photo against destination pixels of
    // a slightly different tone, which shows up as a visible halo/ring right
    // at the dab boundary. A hard edge avoids that seam.
    auto applyCloneShape = [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        const int w = tab->imageWidth();
        const double norm = w > 0 ? m_cloneSize->value() / double(w) : 0.006;
        tab->setActiveMaskShape(false, 0.0, 1.0, norm, false);
    };
    connect(m_cloneSize, &QSlider::valueChanged, this, [applyCloneShape](int) { applyCloneShape(); });

    // --- Selection Brush page (index 11) ---
    auto *selectBrushPage = new QWidget;
    auto *selectBrushRow = new QHBoxLayout(selectBrushPage);
    selectBrushRow->setContentsMargins(4, 2, 4, 2);
    m_selectBrushSize = new QSlider(Qt::Horizontal);
    m_selectBrushSize->setRange(1, 40); // percent of image width, same scale as Brush/Clone Size
    m_selectBrushSize->setValue(4);
    m_selectBrushSize->setMinimumWidth(140);
    selectBrushRow->addWidget(new QLabel("Size:"));
    selectBrushRow->addWidget(m_selectBrushSize);
    selectBrushRow->addStretch(1);
    m_toolOptionsStack->addWidget(selectBrushPage);

    connect(m_selectBrushSize, &QSlider::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setSelectBrushRadius(v / 100.0);
    });

    connect(m_shapeType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        ShapeType t = ShapeType(m_shapeType->currentData().toInt());
        tab->setActiveShapeType(t);
        bool isPoly = (t == ShapeType::Polygon || t == ShapeType::Star);
        m_shapeSides->setVisible(isPoly);
        m_shapeInnerRatio->setVisible(t == ShapeType::Star);
        bool isLine = (t == ShapeType::Line);
        m_shapeFillEnabled->setEnabled(!isLine);
        m_shapeFillColorBtn->setEnabled(!isLine);
    });
    connect(m_shapeSides, &QSpinBox::valueChanged, this, [this](int v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setShapeSides(v);
    });
    connect(m_shapeInnerRatio, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setShapeInnerRadiusRatio(v);
    });
    auto pushShapeFill = [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        tab->setShapeFill(m_shapeFillEnabled->isChecked(),
                          m_shapeFillColorBtn->property("color").value<QColor>());
    };
    auto pushShapeStroke = [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        tab->setShapeStroke(m_shapeStrokeEnabled->isChecked(),
                            m_shapeStrokeColorBtn->property("color").value<QColor>(),
                            m_shapeStrokeWidth->value());
    };
    connect(m_shapeFillEnabled, &QCheckBox::toggled, this, [pushShapeFill] { pushShapeFill(); });
    connect(m_shapeStrokeEnabled, &QCheckBox::toggled, this, [pushShapeStroke] { pushShapeStroke(); });
    connect(m_shapeStrokeWidth, &QDoubleSpinBox::valueChanged, this, [pushShapeStroke] { pushShapeStroke(); });
    connect(m_shapeFillColorBtn, &QPushButton::clicked, this, [this, pushShapeFill] {
        QColor c = QColorDialog::getColor(m_shapeFillColorBtn->property("color").value<QColor>(),
                                          this, "Fill Color", QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        setColorSwatchButton(m_shapeFillColorBtn, c);
        pushShapeFill();
    });
    connect(m_shapeStrokeColorBtn, &QPushButton::clicked, this, [this, pushShapeStroke] {
        QColor c = QColorDialog::getColor(m_shapeStrokeColorBtn->property("color").value<QColor>(),
                                          this, "Stroke Color", QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        setColorSwatchButton(m_shapeStrokeColorBtn, c);
        pushShapeStroke();
    });
    connect(m_shapeDelete, &QPushButton::clicked, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) tab->deleteActiveShape();
        updateShapeOptionsFromTab();
    });

    setColorSwatchButton(m_shapeFillColorBtn, Qt::white);
    setColorSwatchButton(m_shapeStrokeColorBtn, Qt::black);

    m_toolOptionsBar->addWidget(m_toolOptionsStack);
}

void RetouchWindow::buildDock() {
    auto *dock = new QDockWidget("Adjustments", this);
    m_adjustmentsDock = dock;
    dock->setObjectName("adjustmentsDock");
    dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    auto *panel = new QWidget;
    auto *outer = new QVBoxLayout(panel);

    auto makeSlider = [this](QFormLayout *form, const QString &label,
                             int lo = -100, int hi = 100) {
        auto *s = new QSlider(Qt::Horizontal);
        s->setRange(lo, hi);
        s->setValue(0);
        form->addRow(label + ":", s);
        connect(s, &QSlider::valueChanged, this, &RetouchWindow::onToneChanged);
        return s;
    };

    outer->addWidget(new QLabel("<b>Tone</b>"));
    auto *toneForm = new QFormLayout;
    m_brightness = makeSlider(toneForm, "Brightness");
    m_contrast = makeSlider(toneForm, "Contrast");
    m_highlights = makeSlider(toneForm, "Highlights");
    m_shadows = makeSlider(toneForm, "Shadows");
    outer->addLayout(toneForm);

    outer->addSpacing(6);
    outer->addWidget(new QLabel("<b>Colour</b>"));
    auto *colForm = new QFormLayout;
    m_saturation = makeSlider(colForm, "Saturation");
    m_vibrance = makeSlider(colForm, "Vibrance");
    m_temperature = makeSlider(colForm, "Temperature");
    m_tint = makeSlider(colForm, "Tint (green/magenta)");
    outer->addLayout(colForm);
    m_wbPick = new QPushButton("White-balance eyedropper");
    m_wbPick->setCheckable(true);
    outer->addWidget(m_wbPick);
    connect(m_wbPick, &QPushButton::toggled, this, [this](bool on) {
        RetouchTab *tab = currentTab();
        if (on) {
            // Mutually exclusive with the left-bar tools (Zoom/Crop/Heal).
            if (m_toolZoom) { QSignalBlocker b(m_toolZoom); m_toolZoom->setChecked(false); }
            if (m_cropToggle) { QSignalBlocker b(m_cropToggle); m_cropToggle->setChecked(false); }
            if (m_healToggle) { QSignalBlocker b(m_healToggle); m_healToggle->setChecked(false); }
            if (tab) { tab->setZoomMode(false); tab->setCropMode(false); tab->setHealMode(false); tab->setColorRangePickMode(false); }
            if (m_levelsPanel) m_levelsPanel->setTargetPickChecked(false);
        }
        if (tab) tab->setWbPickMode(on);
    });

    outer->addSpacing(6);
    outer->addWidget(new QLabel("<b>Tone Curve</b>"));
    m_curve = new CurveEditor;
    outer->addWidget(m_curve);
    connect(m_curve, &CurveEditor::curveChanged, this,
            [this](const QVector<QPointF> &pts) {
                if (m_syncing) return;
                RetouchTab *tab = currentTab();
                if (!tab || !tab->isReady()) return;
                Adjustments a = tab->adjustments();
                a.curve = pts;
                tab->setAdjustments(a);
            });

    outer->addSpacing(6);
    outer->addWidget(new QLabel("<b>Detail &amp; Effects</b>"));
    auto *fxForm = new QFormLayout;
    m_denoise = makeSlider(fxForm, "Denoise", 0, 100);
    m_clarity = makeSlider(fxForm, "Clarity");
    m_sharpen = makeSlider(fxForm, "Sharpen", 0, 100);
    m_vignette = makeSlider(fxForm, "Vignette");
    m_flatStyle = makeSlider(fxForm, "Style (flat/posterize)", 0, 100);
    outer->addLayout(fxForm);

    outer->addSpacing(6);
    outer->addWidget(new QLabel("<b>Lighting</b>"));
    auto *lightForm = new QFormLayout;
    m_lightAngle = makeSlider(lightForm, "Light Angle", 0, 360);
    m_lightIntensity = makeSlider(lightForm, "Light Intensity");
    outer->addLayout(lightForm);

    outer->addStretch(1);

    // Many controls now — make the dock scrollable.
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(panel);
    dock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, dock);
}

void RetouchWindow::buildOrientationDock() {
    auto *dock = new QDockWidget("Orientation", this);
    m_orientationDock = dock;
    dock->setObjectName("orientationDock");
    dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);

    auto *panel = new QWidget;
    auto *outer = new QVBoxLayout(panel);
    auto *rotRow = new QHBoxLayout;
    m_rotLeft = new QPushButton("⟲ 90°");
    m_rotRight = new QPushButton("⟳ 90°");
    m_flipH = new QPushButton("Flip H");
    m_flipV = new QPushButton("Flip V");
    rotRow->addWidget(m_rotLeft);
    rotRow->addWidget(m_rotRight);
    outer->addLayout(rotRow);
    auto *flipRow = new QHBoxLayout;
    flipRow->addWidget(m_flipH);
    flipRow->addWidget(m_flipV);
    outer->addLayout(flipRow);
    outer->addStretch(1);

    // Orientation handlers mutate the current tab's adjustments.
    auto mutateCurrent = [this](std::function<void(Adjustments &)> fn) {
        RetouchTab *tab = currentTab();
        if (!tab || !tab->isReady()) return;
        Adjustments a = tab->adjustments();
        fn(a);
        tab->setAdjustments(a);
    };
    connect(m_rotLeft, &QPushButton::clicked, this, [mutateCurrent] {
        mutateCurrent([](Adjustments &a) { a.rotationQuadrants = (a.rotationQuadrants + 3) % 4; });
    });
    connect(m_rotRight, &QPushButton::clicked, this, [mutateCurrent] {
        mutateCurrent([](Adjustments &a) { a.rotationQuadrants = (a.rotationQuadrants + 1) % 4; });
    });
    connect(m_flipH, &QPushButton::clicked, this, [mutateCurrent] {
        mutateCurrent([](Adjustments &a) { a.flipH = !a.flipH; });
    });
    connect(m_flipV, &QPushButton::clicked, this, [mutateCurrent] {
        mutateCurrent([](Adjustments &a) { a.flipV = !a.flipV; });
    });

    dock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    // Stack under the Adjustments dock as a tab if both are on the right.
    if (m_adjustmentsDock) tabifyDockWidget(m_adjustmentsDock, dock);
}

void RetouchWindow::buildHistoryDock() {
    auto *dock = new QDockWidget("History", this);
    m_historyDock = dock;
    dock->setObjectName("historyDock");
    dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    m_historyList = new QListWidget;
    m_historyList->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_historyList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                RetouchTab *tab = currentTab();
                if (tab) tab->jumpToHistory(m_historyList->row(item));
            });
    dock->setWidget(m_historyList);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    // Stack under the Adjustments dock as a tab if both are on the right.
    if (m_adjustmentsDock) tabifyDockWidget(m_adjustmentsDock, dock);
}

void RetouchWindow::refreshHistoryPanel() {
    if (!m_historyList) return;
    RetouchTab *tab = currentTab();
    QSignalBlocker block(m_historyList);
    m_historyList->clear();
    if (!tab || !tab->isReady()) return;
    const QVector<Adjustments> &hist = tab->history();
    for (int i = 0; i < hist.size(); ++i) {
        QString label = (i == 0) ? QStringLiteral("Original")
                                 : historyStepLabel(hist[i - 1], hist[i]);
        m_historyList->addItem(label);
    }
    int cur = tab->historyIndex();
    if (cur >= 0 && cur < m_historyList->count())
        m_historyList->setCurrentRow(cur);
}

void RetouchWindow::buildLevelsDock() {
    auto *dock = new QDockWidget("Levels", this);
    m_levelsDock = dock;
    dock->setObjectName("levelsDock");
    dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    m_levelsPanel = new LevelsPanel;
    dock->setWidget(m_levelsPanel);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    // Pin above the Adjustments dock so it reads at the top-right.
    if (m_adjustmentsDock)
        splitDockWidget(dock, m_adjustmentsDock, Qt::Vertical);

    connect(m_levelsPanel, &LevelsPanel::levelsChanged, this,
            [this](const Levels &lv) {
                if (m_syncing) return;
                RetouchTab *tab = currentTab();
                if (!tab || !tab->isReady()) return;
                Adjustments a = tab->adjustments();
                a.levels = lv;
                tab->setAdjustments(a);
            });
    // Targeted color adjustment: mutually exclusive with every other canvas
    // tool, same pattern as the tool-bar toggles.
    connect(m_levelsPanel, &LevelsPanel::targetPickToggled, this,
            [this](bool on) {
                RetouchTab *tab = currentTab();
                if (on) {
                    if (m_toolZoom) { QSignalBlocker b(m_toolZoom); m_toolZoom->setChecked(false); }
                    if (m_cropToggle) { QSignalBlocker b(m_cropToggle); m_cropToggle->setChecked(false); }
                    if (m_healToggle) { QSignalBlocker b(m_healToggle); m_healToggle->setChecked(false); }
                    if (m_wbPick) { QSignalBlocker b(m_wbPick); m_wbPick->setChecked(false); }
                    if (m_maskToggle) { QSignalBlocker b(m_maskToggle); m_maskToggle->setChecked(false); }
                    if (m_brushToggle) { QSignalBlocker b(m_brushToggle); m_brushToggle->setChecked(false); }
                    if (m_bucketToggle) { QSignalBlocker b(m_bucketToggle); m_bucketToggle->setChecked(false); }
                    if (tab) { tab->setZoomMode(false); tab->setCropMode(false); tab->setHealMode(false); tab->setWbPickMode(false); tab->setMaskMode(false); }
                    if (m_toolOptionsBar) m_toolOptionsBar->setVisible(false);
                }
                if (tab && tab->isReady()) tab->setColorRangePickMode(on);
            });
}

void RetouchWindow::refreshLevels() {
    if (!m_levelsPanel) return;
    RetouchTab *tab = currentTab();
    if (tab && tab->isReady()) {
        m_syncing = true;
        m_levelsPanel->setLevels(tab->adjustments().levels);
        m_syncing = false;
        if (!tab->previewImage().isNull())
            m_levelsPanel->setImage(tab->previewImage());
    } else {
        m_levelsPanel->clear();
    }
}

void RetouchWindow::buildAssetsDock() {
    auto *dock = new QDockWidget("Assets", this);
    m_assetsDock = dock;
    dock->setObjectName("assetsDock");
    dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    m_assetsPanel = new AssetsPanel;
    dock->setWidget(m_assetsPanel);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    if (m_layersDock) tabifyDockWidget(m_layersDock, dock);

    connect(m_assetsPanel, &AssetsPanel::insertAssetRequested, this, [this](const AssetStamp &asset) {
        RetouchTab *tab = currentTab();
        if (!tab || !tab->isReady()) return;
        int idx = tab->insertAssetStamp(asset.imagePath, asset.nativeSize, asset.name);
        if (idx >= 0) {
            refreshMaskPanel();
            m_statusLabel->setText(QString("Inserted asset \"%1\"").arg(asset.name));
        }
    });
    connect(m_assetsPanel, &AssetsPanel::deleteAssetRequested, this, [this](const QString &name) {
        m_assetStampStore.remove(name);
        refreshAssetsPanel();
    });

    refreshAssetsPanel();
}

void RetouchWindow::refreshAssetsPanel() {
    if (m_assetsPanel) m_assetsPanel->setAssets(m_assetStampStore.all());
}

void RetouchWindow::buildLayersDock() {
    auto *dock = new QDockWidget("Layers", this);
    m_layersDock = dock;
    dock->setObjectName("layersDock");
    dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
    m_layersPanel = new LayersPanel;
    dock->setWidget(m_layersPanel);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    if (m_adjustmentsDock) tabifyDockWidget(m_adjustmentsDock, dock);
    dock->hide(); // shown only while the Mask tool is active

    // The per-layer editing sections live in their own LayerAdjustmentsPanel,
    // created (and docked next to m_layersDock) the first time one is
    // requested. Built eagerly here (just not docked/shown yet) so
    // LayersPanel::setAdjustmentsPanel() can wire it up immediately.
    m_layerAdjustmentsPanel = new LayerAdjustmentsPanel;
    m_layersPanel->setAdjustmentsPanel(m_layerAdjustmentsPanel);

    connect(dock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        RetouchTab *tab = currentTab();
        if (tab) tab->setMaskPreviewEnabled(visible);
    });

    connect(m_layersPanel, &LayersPanel::sectionRequested, this,
            &RetouchWindow::showLayerAdjustmentsSection);

    connect(m_layersPanel, &LayersPanel::selectMaskRequested, this, [this](int i) {
        RetouchTab *tab = currentTab();
        if (tab) tab->selectMask(i);
    });
    connect(m_layersPanel, &LayersPanel::deleteMaskRequested, this, [this] {
        RetouchTab *tab = currentTab();
        if (tab) {
            tab->deleteActiveMask(); // Background is a normal mask now, same delete path
            refreshMaskPanel();
        }
    });
    connect(m_layersPanel, &LayersPanel::addMaskRequested, this, [this](MaskType type) {
        RetouchTab *tab = currentTab();
        if (tab && tab->isReady()) { tab->addMask(type); refreshMaskPanel(); }
    });
    connect(m_layersPanel, &LayersPanel::addLayerRequested, this,
            [this](MaskType type, ShapeType shapeType) {
                RetouchTab *tab = currentTab();
                if (tab && tab->isReady()) { tab->addMask(type, shapeType); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::addImageLayerRequested, this,
            [this](const QString &path) {
                RetouchTab *tab = currentTab();
                if (tab && tab->isReady()) { tab->addImageLayer(path); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::addSvgLayerRequested, this,
            [this](const QString &path) {
                RetouchTab *tab = currentTab();
                if (tab && tab->isReady()) { tab->addSvgLayer(path); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::duplicateMaskRequested, this, [this] {
        RetouchTab *tab = currentTab();
        if (!tab) return;
        // duplicateActiveMask() itself demotes a Background duplicate to a
        // regular layer (only one Background-type mask is ever allowed),
        // while preserving the crop and per-layer adjustments baked into it.
        tab->duplicateActiveMask();
        refreshMaskPanel();
    });
    connect(m_layersPanel, &LayersPanel::maskAdjustChanged, this,
            [this](const MaskAdjust &a) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskAdjust(a);
            });
    connect(m_layersPanel, &LayersPanel::maskImageTransformChanged, this,
            [this](double offsetX, double offsetY, double scaleX, double scaleY,
                   bool lockRatio) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskImageTransform(offsetX, offsetY, scaleX,
                                                         scaleY, lockRatio);
            });
    connect(m_layersPanel, &LayersPanel::maskTypeChanged, this, [this](MaskType t) {
        RetouchTab *tab = currentTab();
        if (tab) { tab->setActiveMaskType(t); refreshMaskPanel(); }
    });
    connect(m_layersPanel, &LayersPanel::maskShapeChanged, this,
            [this](bool inv, double f, double h, double br, bool am) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskShape(inv, f, h, br, am);
            });
    connect(m_layersPanel, &LayersPanel::maskTextChanged, this,
            [this](const QString &text, const QString &family, double pixelSize,
                   bool bold, bool italic) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskText(text, family, pixelSize, bold, italic);
            });
    connect(m_layersPanel, &LayersPanel::gradientFillChanged, this,
            [this](bool enabled, const QColor &colorA, const QColor &colorB) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskGradientFill(enabled, colorA, colorB);
            });
    connect(m_layersPanel, &LayersPanel::maskOpacityChanged, this,
            [this](double opacity) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskOpacity(opacity);
            });
    connect(m_layersPanel, &LayersPanel::maskBlendChanged, this,
            [this](BlendMode mode) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskBlend(mode);
            });
    connect(m_layersPanel, &LayersPanel::groupPropertiesChanged, this,
            [this](const QString &groupId, double opacity, bool visible, BlendMode blend) {
                RetouchTab *tab = currentTab();
                if (tab) { tab->setGroupProperties(groupId, opacity, visible, blend); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::maskVisibleChanged, this,
            [this](int index, bool visible) {
                RetouchTab *tab = currentTab();
                if (tab) {
                    tab->setMaskVisible(index, visible); // Background is a normal mask now
                    refreshMaskPanel();
                }
            });
    connect(m_layersPanel, &LayersPanel::maskNameChanged, this,
            [this](const QString &name) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setActiveMaskName(name);
            });
    connect(m_layersPanel, &LayersPanel::maskRenamed, this,
            [this](int index, const QString &name) {
                RetouchTab *tab = currentTab();
                if (tab) tab->setMaskName(index, name);
            });
    connect(m_layersPanel, &LayersPanel::maskReorderRequested, this,
            [this](const QVector<int> &newOrder, const QVector<int> &leftGroupIndices,
                   const QVector<QPair<int, QString>> &joinGroups) {
                RetouchTab *tab = currentTab();
                if (tab) { tab->reorderMasks(newOrder, leftGroupIndices, joinGroups); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::groupMasksRequested, this,
            [this](const QVector<int> &indices) {
                RetouchTab *tab = currentTab();
                if (tab) { tab->groupMasks(indices); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::ungroupMasksRequested, this,
            [this](const QVector<int> &indices) {
                RetouchTab *tab = currentTab();
                if (tab) { tab->ungroupMasks(indices); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::groupRenamed, this,
            [this](const QString &groupId, const QString &name) {
                RetouchTab *tab = currentTab();
                if (tab) tab->renameGroup(groupId, name);
            });
    connect(m_layersPanel, &LayersPanel::selectRemovalRequested, this, [this](int index) {
        RetouchTab *tab = currentTab();
        if (tab) { tab->selectRemoval(index); refreshMaskPanel(); }
    });
    connect(m_layersPanel, &LayersPanel::removalVisibleChanged, this,
            [this](int index, bool visible) {
                RetouchTab *tab = currentTab();
                if (tab) { tab->setRemovalVisible(index, visible); refreshMaskPanel(); }
            });
    connect(m_layersPanel, &LayersPanel::deleteRemovalRequested, this, [this](int index) {
        RetouchTab *tab = currentTab();
        if (tab) { tab->deleteRemoval(index); refreshMaskPanel(); }
    });
}

// Opens (creating on first use) the LayerAdjustmentsPanel dock as a floating
// window over the canvas, and switches it to the requested section. `section`
// is a LayerAdjustmentsPanel::Section value, forwarded as a plain int since
// LayersPanel only forward-declares that type.
void RetouchWindow::showLayerAdjustmentsSection(int section) {
    if (!m_layerAdjustmentsPanel) return;
    if (!m_layerAdjustmentsDock) {
        auto *dock = new QDockWidget("Layer Adjustments", this);
        m_layerAdjustmentsDock = dock;
        dock->setObjectName("layerAdjustmentsDock");
        dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
        dock->setWidget(m_layerAdjustmentsPanel);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->setFloating(true);
        dock->resize(330, 300);
        if (m_layersDock) {
            QPoint pos = m_layersDock->mapToGlobal(QPoint(0, 0));
            dock->move(pos.x() - dock->width() - 8, pos.y());
        } else {
            QPoint pos = mapToGlobal(QPoint(0, 0));
            dock->move(pos.x() + 200, pos.y() + 60);
        }
    }
    m_layerAdjustmentsPanel->showSection(LayerAdjustmentsPanel::Section(section));
    m_layerAdjustmentsDock->show();
    m_layerAdjustmentsDock->raise();
}

void RetouchWindow::updatePaintSizePxLabel() {
    if (!m_paintSizePx) return;
    m_paintSizePx->setText(QString("%1px").arg(m_paintSize->value()));
}

// Real pencil grade naming for the Pen tool's Grade slider: -6..-1 = 6B..1B,
// 0 = HB, 1..5 = 1H..5H (see Mask::penGrade in Adjustments.h).
void RetouchWindow::updatePenGradeLabel() {
    if (!m_penGradeLabel || !m_penGrade) return;
    const int v = m_penGrade->value();
    if (v == 0) m_penGradeLabel->setText("HB");
    else if (v < 0) m_penGradeLabel->setText(QString("%1B").arg(-v));
    else m_penGradeLabel->setText(QString("%1H").arg(v));
}

// Unchecks every left-bar tool toggle button except `keep`, via QSignalBlocker
// so toggling them off doesn't re-enter these very toggled() handlers. Shared
// by every tool's toggled(true) handler in buildToolbar() so adding a new
// tool means adding it to this one list, not to every other tool's handler.
void RetouchWindow::deactivateOtherToolButtons(QAbstractButton *keep) {
    const QVector<QAbstractButton *> buttons = {
        m_moveToggle, m_toolZoom, m_cropToggle, m_healToggle, m_wbPick, m_maskToggle,
        m_brushToggle, m_bucketToggle, m_eraseToggle, m_textToggle, m_shapeToggle,
        m_removeObjectToggle, m_penToggle, m_selectMarqueeToggle, m_selectLassoToggle,
        m_selectWandToggle, m_cloneToggle,
    };
    for (QAbstractButton *btn : buttons) {
        if (btn && btn != keep) {
            QSignalBlocker b(btn);
            btn->setChecked(false);
        }
    }
}

// Turns off every tool mode on `tab`. Called by a tool's toggled(true)
// handler right before it turns its own mode on, so it doesn't need to
// enumerate every *other* mode to clear itself.
void RetouchWindow::deactivateAllToolModes(RetouchTab *tab) {
    if (m_levelsPanel) m_levelsPanel->setTargetPickChecked(false);
    if (!tab) return;
    tab->setMoveMode(false);
    tab->setZoomMode(false);
    tab->setCropMode(false);
    tab->setHealMode(false);
    tab->setWbPickMode(false);
    tab->setMaskMode(false);
    tab->setColorRangePickMode(false);
    tab->setEraseMode(false);
    tab->setTextMode(false);
    tab->setShapeMode(false);
    tab->setRemoveObjectMode(false);
    tab->setBucketMode(false);
    tab->setSelectMarqueeMode(false);
    tab->setSelectLassoMode(false);
    tab->setSelectMagicWandMode(false);
    tab->setSelectBrushMode(false);
    tab->setCloneMode(false);
}

void RetouchWindow::refreshMaskPanel() {
    RetouchTab *tab = currentTab();
    const bool ready = tab && tab->isReady();
    if (m_layersPanel) {
        if (ready) {
            m_layersPanel->setMasks(tab->masks(), tab->activeMaskIndex());
            m_layersPanel->setGroups(tab->groups());
            m_layersPanel->setRemovals(tab->removals(), tab->activeRemovalIndex());
            m_layersPanel->setImageWidth(tab->imageWidth());
        } else {
            m_layersPanel->clear();
        }
    }
    updatePaintSizePxLabel();
    const int idx = ready ? tab->activeMaskIndex() : -1;
    const bool isImageLayer = ready && idx >= 0 && idx < tab->masks().size() &&
                              tab->masks()[idx].isImageLayer();
    const bool canErasePaint = ready && tab->canActivateTool(MaskType::Paint);
    if (m_eraseToggle) {
        m_eraseToggle->setEnabled(isImageLayer || canErasePaint);
        if (!isImageLayer && !canErasePaint && m_eraseToggle->isChecked())
            m_eraseToggle->setChecked(false);
    }
    if (m_healToggle) {
        m_healToggle->setEnabled(isImageLayer);
        if (!isImageLayer && m_healToggle->isChecked())
            m_healToggle->setChecked(false);
    }
    if (m_removeObjectToggle) {
        // Acts on the composited base image (like Heal), not a specific layer.
        m_removeObjectToggle->setEnabled(ready);
        if (!ready && m_removeObjectToggle->isChecked())
            m_removeObjectToggle->setChecked(false);
    }
    // Brush/Paint, Radial, and Linear only operate against a matching
    // existing selection (they no longer auto-create a layer on toggle-on),
    // so gray them out whenever the selection doesn't match. With nothing
    // selected (idx == -1, e.g. no image loaded yet) all three stay disabled.
    if (m_brushToggle) {
        const bool canPaint = ready && tab->canActivateTool(MaskType::Paint);
        m_brushToggle->setEnabled(canPaint);
        if (!canPaint && m_brushToggle->isChecked())
            m_brushToggle->setChecked(false);
    }
    if (m_bucketToggle) {
        const bool canPaint = ready && tab->canActivateTool(MaskType::Paint);
        m_bucketToggle->setEnabled(canPaint);
        if (!canPaint && m_bucketToggle->isChecked())
            m_bucketToggle->setChecked(false);
    }
    if (m_penToggle) {
        // Pen shares Brush's Paint-type layer (see BrushStrokePoint::isPen)
        // rather than gating on its own selection, so — unlike Brush/Paint,
        // which only enable against an existing matching selection — this
        // stays enabled whenever the document is ready; the toggle handler
        // auto-creates a Paint layer on click if none is selected yet.
        m_penToggle->setEnabled(ready);
        if (!ready && m_penToggle->isChecked())
            m_penToggle->setChecked(false);
    }
    if (m_maskToggle) {
        // The K tool's "Layer"/"Brush" (mask-brush) subtools aren't gated —
        // they keep the always-create-a-new-layer behaviour — so the shared
        // toggle button stays enabled for those regardless of selection.
        // Only the Radial/Linear subtools require a matching selection.
        const bool isGatedSubtool = m_activeMaskSubtool == MaskType::Radial ||
                                    m_activeMaskSubtool == MaskType::Linear;
        const bool canActivate = !isGatedSubtool ||
                                 (ready && tab->canActivateTool(m_activeMaskSubtool));
        m_maskToggle->setEnabled(canActivate);
        if (!canActivate && m_maskToggle->isChecked())
            m_maskToggle->setChecked(false);
    }
}

void RetouchWindow::openMaskFlyout() {
    const QVector<SubTool> tools{
        {int(MaskType::Radial), drawMaskRadial, "Radial", "Radial mask layer"},
        {int(MaskType::Linear), drawMaskLinear, "Graduated", "Graduated mask layer"},
        {int(MaskType::Brush), drawMaskBrush, "Brush", "Brush mask layer"},
        {int(MaskType::None), drawMask, "Layer", "Unmasked adjustment layer"},
    };
    auto *flyout = new ToolFlyout(tools, int(m_activeMaskSubtool), this);
    connect(flyout, &ToolFlyout::chosen, this, [this](int id) {
        setMaskSubtool(MaskType(id));
        const bool isGatedSubtool = m_activeMaskSubtool == MaskType::Radial ||
                                    m_activeMaskSubtool == MaskType::Linear;
        // Picking a subtool activates the mask tool (creating a mask via the
        // toggle handler) or, if already active, creates one directly — except
        // for the gated Radial/Linear subtools, which only activate against a
        // matching existing selection rather than creating a new layer.
        if (m_maskToggle->isChecked()) {
            if (isGatedSubtool) {
                RetouchTab *tab = currentTab();
                if (!tab || !tab->isReady() || !tab->canActivateTool(m_activeMaskSubtool)) {
                    QSignalBlocker b(m_maskToggle);
                    m_maskToggle->setChecked(false);
                    if (tab && tab->isReady()) tab->setMaskMode(false);
                }
            } else {
                addActiveMask();
            }
        } else {
            m_maskToggle->setChecked(true);
        }
        refreshMaskPanel();
    });
    // Just to the right of the mask button, vertically aligned with it.
    const QPoint tl = m_maskToggle->mapToGlobal(QPoint(m_maskToggle->width() + 4, 0));
    flyout->showAt(tl);
}

void RetouchWindow::setMaskSubtool(MaskType t) {
    m_activeMaskSubtool = t;
    if (m_maskToggle)
        m_maskToggle->setIcon(makeFlyoutToolIcon(maskGlyph(t)));
}

void RetouchWindow::addActiveMask() {
    RetouchTab *tab = currentTab();
    if (tab && tab->isReady()) {
        tab->addMask(m_activeMaskSubtool);
        refreshMaskPanel();
    }
}

void RetouchWindow::mergePortable(const Adjustments &src, Adjustments &dst) {
    // Portable (image-independent) fields only; geometry & heals stay as dst had.
    dst.brightness = src.brightness;
    dst.contrast = src.contrast;
    dst.highlights = src.highlights;
    dst.shadows = src.shadows;
    dst.saturation = src.saturation;
    dst.vibrance = src.vibrance;
    dst.temperature = src.temperature;
    dst.tint = src.tint;
    dst.wbR = src.wbR;
    dst.wbG = src.wbG;
    dst.wbB = src.wbB;
    dst.denoise = src.denoise;
    dst.clarity = src.clarity;
    dst.sharpen = src.sharpen;
    dst.vignette = src.vignette;
    dst.flatStyle = src.flatStyle;
    dst.curve = src.curve;
    dst.levels = src.levels;
}

void RetouchWindow::updateEditClipboardActions() {
    RetouchTab *tab = currentTab();
    const bool tabReady = tab && tab->isReady();
    if (m_copyEditsAction) m_copyEditsAction->setEnabled(tabReady);
    if (m_pasteEditsAction)
        m_pasteEditsAction->setEnabled(m_hasEditClipboard && tabReady);
    if (m_syncEditsAction)
        m_syncEditsAction->setEnabled(m_hasEditClipboard &&
                                      !m_filmstrip->selectedPaths().isEmpty());
}

void RetouchWindow::onCopyEdits() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    m_editClipboard = tab->adjustments();
    m_hasEditClipboard = true;
    updateEditClipboardActions();
    m_statusLabel->setText("Edits copied");
}

bool RetouchWindow::applyClipboardTo(const QString &path) {
    if (RetouchTab *tab = m_openTabs.value(path, nullptr)) {
        // Route through the open tab so history + dirty state stay consistent.
        Adjustments a = tab->adjustments();
        mergePortable(m_editClipboard, a);
        if (a == tab->adjustments()) return false;
        tab->setAdjustments(a);
        return true;
    }
    // Closed photo: merge into its sidecar (or defaults) and persist.
    Adjustments a;
    EditSidecar::load(path, a); // leaves a at defaults if none exists
    Adjustments before = a;
    mergePortable(m_editClipboard, a);
    if (a == before) return false;
    if (EditSidecar::save(path, a))
        m_filmstrip->setBadge(path, FilmstripWidget::Saved);
    return true;
}

void RetouchWindow::onPasteEdits() {
    RetouchTab *tab = currentTab();
    if (!m_hasEditClipboard || !tab || !tab->isReady()) return;
    applyClipboardTo(tab->path());
    m_statusLabel->setText("Edits pasted");
}

void RetouchWindow::onSyncEdits() {
    if (!m_hasEditClipboard) {
        m_statusLabel->setText("Copy edits first (Edit ▸ Copy Edits)");
        return;
    }
    const QStringList targets = m_filmstrip->selectedPaths();
    if (targets.isEmpty()) {
        m_statusLabel->setText("Select photos in the filmstrip to sync");
        return;
    }
    int changed = 0;
    for (const QString &path : targets)
        if (applyClipboardTo(path)) ++changed;
    m_statusLabel->setText(
        QString("Synced edits to %1 of %2 selected")
            .arg(changed)
            .arg(targets.size()));
}

void RetouchWindow::onDeselect() {
    RetouchTab *tab = currentTab();
    if (tab && tab->isReady()) tab->clearActiveSelection();
}

void RetouchWindow::onInvertSelection() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    tab->canvas()->invertSelection();
}

// Photoshop's Select > Feather: softens the selection's edge over a radius
// in pixels, converted to the width-normalized fraction ImageCanvas/Mask
// selection fields use internally.
void RetouchWindow::onFeatherSelection() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    const int w = tab->imageWidth();
    if (w <= 0) return;
    bool ok = false;
    int px = QInputDialog::getInt(this, "Feather Selection", "Feather Radius (px):",
                                  m_lastFeatherPx, 0, w / 4, 1, &ok);
    if (!ok) return;
    m_lastFeatherPx = px;
    tab->setSelectionFeather(px / double(w));
    m_statusLabel->setText(px > 0 ? QString("Feather set to %1px").arg(px) : "Feather cleared");
}

// Extracts the pixels currently under the active selection from the tab's
// composited render into an in-app clipboard (not the system clipboard),
// keeping the copied region's exact position within the full image so Paste
// can drop it back at the same place via a full-frame image layer.
void RetouchWindow::onCopySelection() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady() || !tab->hasActiveSelection()) {
        m_statusLabel->setText("Make a selection first");
        return;
    }
    QImage src = tab->previewImage();
    if (src.isNull()) return;
    const double W = src.width();
    QTransform normToPx;
    normToPx.scale(W, W); // width-normalized -> pixel space, same convention as onMaskBrushPoint
    QPainterPath pathPx = normToPx.map(tab->canvas()->selectionPathNorm());
    QRect bounds = pathPx.boundingRect().toAlignedRect().intersected(src.rect());
    if (bounds.isEmpty()) return;

    QImage clip(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    clip.fill(Qt::transparent);
    QPainter p(&clip);
    p.setClipPath(pathPx.translated(-bounds.topLeft()));
    p.drawImage(-bounds.topLeft(), src);
    p.end();

    m_selectionClipboard = clip;
    m_selectionClipboardOffsetPx = bounds.topLeft();
    m_pasteSelectionAction->setEnabled(true);
    m_statusLabel->setText("Selection copied");
}

// Extracts the selection the same way onCopySelection does (feather-preserving
// per-pixel alpha clip, cropped to the selection's bounding box), but saves
// the result into the AssetStamp library instead of the in-app clipboard, so
// it can be reused as a resize/rotate-able stamp in any document.
void RetouchWindow::onSaveSelectionAsAsset() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady() || !tab->hasActiveSelection()) {
        m_statusLabel->setText("Make a selection first");
        return;
    }
    QImage src = tab->previewImage();
    if (src.isNull()) return;
    const double W = src.width();
    QTransform normToPx;
    normToPx.scale(W, W);
    QPainterPath pathPx = normToPx.map(tab->canvas()->selectionPathNorm());
    QRect bounds = pathPx.boundingRect().toAlignedRect().intersected(src.rect());
    if (bounds.isEmpty()) return;

    QImage clip(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    clip.fill(Qt::transparent);
    QPainter p(&clip);
    p.setClipPath(pathPx.translated(-bounds.topLeft()));
    p.drawImage(-bounds.topLeft(), src);
    p.end();

    bool ok = false;
    QString name = QInputDialog::getText(this, "Save Selection as Asset", "Asset name:",
                                         QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    AssetStamp saved = m_assetStampStore.addOrUpdate(name.trimmed(), clip);
    if (saved.imagePath.isEmpty()) {
        m_statusLabel->setText("Failed to save asset");
        return;
    }
    refreshAssetsPanel();
    m_statusLabel->setText(QString("Saved asset \"%1\"").arg(saved.name));
}

// Pastes the in-app selection clipboard as a new full-frame image layer
// (transparent outside the copied region), reusing addImageLayerFromImage's
// existing cover-fit placement — since the pasted image is exactly the same
// size as the tab's composite, cover-fit maps it back 1:1 with no distortion,
// landing the copied pixels at their original position.
void RetouchWindow::onPasteSelection() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady() || m_selectionClipboard.isNull()) return;
    QImage full = tab->previewImage();
    if (full.isNull()) return;
    QImage layer(full.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    QPainter p(&layer);
    p.drawImage(m_selectionClipboardOffsetPx, m_selectionClipboard);
    p.end();
    int idx = tab->addImageLayerFromImage(layer, "Pasted Selection");
    if (idx >= 0) {
        refreshMaskPanel();
        m_statusLabel->setText("Selection pasted as new layer");
    }
}

// Repopulate the Presets menu: built-in templates, then custom presets, then
// the Save/Delete management actions. Called at startup and whenever a
// custom preset is added or removed.
void RetouchWindow::rebuildPresetsMenu() {
    if (!m_presetsMenu) return;
    m_presetsMenu->clear();
    m_presetActions.clear();

    const QList<AdjustmentPreset> builtins = AdjustmentPresetStore::builtins();
    for (const AdjustmentPreset &preset : builtins) {
        auto *act = m_presetsMenu->addAction(preset.name);
        connect(act, &QAction::triggered, this,
                [this, preset] { applyAdjustmentPreset(preset); });
        m_presetActions.append(act);
    }

    const QList<AdjustmentPreset> &custom = m_adjustmentPresetStore.custom();
    if (!custom.isEmpty()) {
        m_presetsMenu->addSeparator();
        for (const AdjustmentPreset &preset : custom) {
            auto *act = m_presetsMenu->addAction(preset.name);
            connect(act, &QAction::triggered, this,
                    [this, preset] { applyAdjustmentPreset(preset); });
            m_presetActions.append(act);
        }
    }

    m_presetsMenu->addSeparator();
    auto *saveAct = m_presetsMenu->addAction("Save Current as Preset…");
    connect(saveAct, &QAction::triggered, this, &RetouchWindow::onSaveAdjustmentPreset);
    if (!custom.isEmpty()) {
        auto *deleteAct = m_presetsMenu->addAction("Delete Preset…");
        connect(deleteAct, &QAction::triggered, this, &RetouchWindow::onDeleteAdjustmentPreset);
    }
}

// Apply a preset's portable fields to the current tab, the same way
// Paste Edits applies the clipboard (routes through setAdjustments so
// history/dirty state stay consistent).
void RetouchWindow::applyAdjustmentPreset(const AdjustmentPreset &preset) {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    Adjustments a = tab->adjustments();
    mergePortable(preset.adj, a);
    if (a == tab->adjustments()) return;
    tab->setAdjustments(a);
    m_statusLabel->setText(QString("Applied preset \"%1\"").arg(preset.name));
}

void RetouchWindow::onSaveAdjustmentPreset() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save Preset", "Preset name:",
                                                QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    AdjustmentPreset preset;
    preset.name = name.trimmed();
    preset.adj = tab->adjustments();
    m_adjustmentPresetStore.addOrUpdate(preset);
    rebuildPresetsMenu();
    m_statusLabel->setText(QString("Saved preset \"%1\"").arg(preset.name));
}

void RetouchWindow::onDeleteAdjustmentPreset() {
    const QList<AdjustmentPreset> &custom = m_adjustmentPresetStore.custom();
    if (custom.isEmpty()) return;
    QStringList names;
    for (const AdjustmentPreset &p : custom) names << p.name;
    bool ok = false;
    const QString name = QInputDialog::getItem(this, "Delete Preset", "Preset:", names, 0,
                                                false, &ok);
    if (!ok || name.isEmpty()) return;
    m_adjustmentPresetStore.remove(name);
    rebuildPresetsMenu();
    m_statusLabel->setText(QString("Deleted preset \"%1\"").arg(name));
}

RetouchTab *RetouchWindow::currentTab() const {
    return qobject_cast<RetouchTab *>(m_tabs->currentWidget());
}

void RetouchWindow::addToFilmstrip(const QString &path) {
    if (m_filmstripPaths.contains(path)) return;
    // Prefer the cached edited thumbnail so the strip shows the latest edits;
    // fall back to the NEF's embedded preview for un-edited photos.
    QImage thumb = EditSidecar::loadThumbnail(path);
    if (thumb.isNull())
        thumb = NefPreview::extract(path);
    m_filmstrip->addCapture(path, thumb, EditSidecar::loadRating(path));
    m_filmstripPaths.insert(path);
    // Show a "saved edits exist" badge if a sidecar is already on disk.
    if (EditSidecar::exists(path))
        m_filmstrip->setBadge(path, FilmstripWidget::Saved);
}

void RetouchWindow::createUntitledTab(const QSize &size) {
    QString key = QString("untitled:%1").arg(++m_untitledCounter);
    auto *tab = new RetouchTab(size);
    m_openTabs.insert(key, tab);
    int idx = m_tabs->addTab(tab, QString("Untitled-%1").arg(m_untitledCounter));
    m_tabs->setCurrentIndex(idx);
    setDockEnabled(true);
    syncDockFromTab();
    refreshHistoryPanel();
    refreshLevels();
    refreshMaskPanel();
    updateEditClipboardActions();
    m_statusLabel->setText(QString("Untitled-%1 ready").arg(m_untitledCounter));
    wireTabSignals(tab);
}

void RetouchWindow::openPhoto(const QString &path) {
    addToFilmstrip(path);

    const QString absPath = QFileInfo(path).absoluteFilePath();
    if (path.endsWith(".ploom", Qt::CaseInsensitive)) {
        RecentProjects::add(absPath);
        rebuildRecentProjectsMenu();
    } else {
        RecentFiles::add(absPath);
        rebuildRecentFilesMenu();
    }

    if (m_openTabs.contains(path)) {
        m_tabs->setCurrentWidget(m_openTabs.value(path));
        return;
    }

    auto *tab = new RetouchTab(path);
    m_openTabs.insert(path, tab);
    int idx = m_tabs->addTab(tab, QFileInfo(path).fileName());
    m_tabs->setCurrentIndex(idx);
    m_statusLabel->setText("Decoding " + QFileInfo(path).fileName() + "…");

    connect(tab, &RetouchTab::decoded, this, [this, tab](bool ok) {
        if (tab == currentTab()) {
            setDockEnabled(ok);
            syncDockFromTab();
            refreshHistoryPanel();
            refreshLevels();
            refreshMaskPanel();
            tab->setMaskPreviewEnabled(m_layersDock && m_layersDock->isVisible());
            updateEditClipboardActions();
            m_statusLabel->setText(ok ? "Ready: " + QFileInfo(tab->path()).fileName()
                                      : "Failed to decode " + QFileInfo(tab->path()).fileName());
        }
    });
    wireTabSignals(tab);
}

void RetouchWindow::wireTabSignals(RetouchTab *tab) {
    connect(tab, &RetouchTab::textsChanged, this, [this, tab] {
        if (tab == currentTab()) updateTextOptionsFromTab();
    });
    connect(tab, &RetouchTab::shapesChanged, this, [this, tab] {
        if (tab == currentTab()) { updateShapeOptionsFromTab(); refreshMaskPanel(); }
    });
    connect(tab, &RetouchTab::cropPending, this, [this, tab](bool has) {
        if (tab == currentTab()) m_cropApply->setEnabled(has);
    });
    connect(tab, &RetouchTab::cropModeExited, this, [this, tab] {
        if (tab == currentTab()) {
            QSignalBlocker b(m_cropToggle);
            m_cropToggle->setChecked(false);
            m_toolOptionsBar->setVisible(false);
        }
    });
    connect(tab, &RetouchTab::objectToolRequested, this, [this, tab](MaskType type) {
        if (tab != currentTab()) return;
        // The tab already selected the clicked layer (selectMask, ahead of
        // this signal) — just flip the matching toolbar button on, exactly
        // as a manual click would; its own toggled() handler resets every
        // other tool and re-syncs m_toolOptionsBar/m_toolOptionsStack.
        switch (type) {
        case MaskType::Shape:   m_shapeToggle->setChecked(true); break;
        case MaskType::TextBox: m_textToggle->setChecked(true); break;
        case MaskType::Paint:   m_brushToggle->setChecked(true); break;
        default:
            // Image layer: no dedicated toolbar tool — just uncheck whatever
            // tool is currently active so plain click/drag on the canvas
            // moves the now-selected layer instead of doing that tool's thing.
            for (QAbstractButton *b : std::initializer_list<QAbstractButton *>{
                     m_toolZoom, m_cropToggle, m_healToggle, m_wbPick,
                     m_maskToggle, m_brushToggle, m_bucketToggle,
                     m_eraseToggle, m_textToggle, m_shapeToggle,
                     m_removeObjectToggle}) {
                if (b->isChecked()) b->setChecked(false);
            }
            break;
        }
    });
    connect(tab, &RetouchTab::wbPicked, this, [this, tab] {
        if (tab == currentTab()) {
            QSignalBlocker b(m_wbPick);
            m_wbPick->setChecked(false);
            tab->setWbPickMode(false);
            m_statusLabel->setText("White balance set");
        }
    });
    connect(tab, &RetouchTab::quickColorPicked, this, [this, tab](const QColor &c) {
        if (tab == currentTab()) m_colorSwatch->setForegroundColor(c);
    });
    connect(tab, &RetouchTab::historyChanged, this,
            [this, tab](bool canUndo, bool canRedo) {
                if (tab != currentTab()) return;
                m_undoAction->setEnabled(canUndo);
                m_redoAction->setEnabled(canRedo);
            });
    connect(tab, &RetouchTab::historyListChanged, this, [this, tab] {
        if (tab != currentTab()) return;
        refreshHistoryPanel();
    });
    connect(tab, &RetouchTab::adjustmentsReplaced, this, [this, tab] {
        if (tab != currentTab()) return;
        syncDockFromTab(); // reflect undone/redone values in the dock
    });
    connect(tab, &RetouchTab::zoomChanged, this, [this, tab](double pct) {
        if (tab != currentTab()) return;
        QSignalBlocker b(m_zoomSlider);
        m_zoomSlider->setValue(int(std::lround(pct)));
        m_zoomLabel->setText(QString::number(int(std::lround(pct))) + "%");
    });
    connect(tab, &RetouchTab::healBrushChanged, this, [this, tab](int radius) {
        if (tab != currentTab()) return;
        QSignalBlocker b(m_healBrush);
        m_healBrush->setValue(radius); // reflect ctrl+wheel resize in the dock
    });
    connect(tab, &RetouchTab::eraseBrushChanged, this, [this, tab](int radius) {
        if (tab != currentTab()) return;
        QSignalBlocker b(m_eraseBrush);
        m_eraseBrush->setValue(radius); // reflect ctrl+wheel resize in the dock
    });
    connect(tab, &RetouchTab::removeObjectBrushChanged, this, [this, tab](int radius) {
        if (tab != currentTab() || !m_removeObjectBrush) return;
        QSignalBlocker b(m_removeObjectBrush);
        m_removeObjectBrush->setValue(radius); // reflect ctrl+wheel resize in the dock
    });
    connect(tab, &RetouchTab::removalsChanged, this, [this, tab] {
        if (tab == currentTab()) refreshMaskPanel();
    });
    connect(tab, &RetouchTab::maskBrushChanged, this, [this, tab](double radiusNorm) {
        if (tab != currentTab()) return;
        if (m_layersPanel) m_layersPanel->setMaskBrushRadius(radiusNorm); // reflect ctrl+wheel resize in the dock
        if (m_paintSize) {
            m_syncingPaintSize = true;
            QSignalBlocker b(m_paintSize);
            const int w = tab->imageWidth();
            const int px = w > 0 ? int(std::lround(radiusNorm * w)) : m_paintSize->value();
            m_paintSize->setValue(std::clamp(px, m_paintSize->minimum(), m_paintSize->maximum()));
            m_syncingPaintSize = false;
            updatePaintSizePxLabel();
        }
        // Clone paints into the same Paint-type mask and shares this signal
        // for its ctrl+wheel resize; keep the Clone Size slider in sync too.
        if (m_cloneSize) {
            QSignalBlocker b(m_cloneSize);
            const int w = tab->imageWidth();
            const int px = w > 0 ? int(std::lround(radiusNorm * w)) : m_cloneSize->value();
            m_cloneSize->setValue(std::clamp(px, m_cloneSize->minimum(), m_cloneSize->maximum()));
        }
    });
    connect(tab, &RetouchTab::selectBrushChanged, this, [this, tab](double radiusNorm) {
        if (tab != currentTab() || !m_selectBrushSize) return;
        QSignalBlocker b(m_selectBrushSize);
        m_selectBrushSize->setValue(int(std::lround(radiusNorm * 100))); // reflect ctrl+wheel resize in the dock
    });
    connect(tab, &RetouchTab::previewUpdated, this, [this, tab] {
        if (tab == currentTab()) m_levelsPanel->setImage(tab->previewImage());
        // Reflect the edit live in the filmstrip thumbnail (in-memory; the
        // on-disk cache is written on save via EditSidecar::saveThumbnail).
        m_filmstrip->updateThumbnail(tab->path(), tab->previewImage());
    });
    connect(tab, &RetouchTab::masksChanged, this, [this, tab] {
        if (tab == currentTab()) refreshMaskPanel();
    });
    connect(tab, &RetouchTab::maskPreviewUpdated, this, [this, tab] {
        if (tab == currentTab() && m_layersPanel)
            m_layersPanel->setLevelsPreviewImage(tab->maskPreviewImage());
    });
    connect(tab, &RetouchTab::editStateChanged, this,
            [this, tab](bool dirty, bool hasEdits) {
                FilmstripWidget::Badge b = dirty ? FilmstripWidget::Unsaved
                                                 : (hasEdits ? FilmstripWidget::Saved
                                                             : FilmstripWidget::NoBadge);
                m_filmstrip->setBadge(tab->path(), b);
            });
}

void RetouchWindow::onFilmstripSelected(const QString &path) {
    setMode(Mode::Retouch);
    openPhoto(path);
}

void RetouchWindow::setMode(Mode mode) {
    QWidget *page = m_tabs;
    if (mode == Mode::Tether) page = m_tetherView;
    else if (mode == Mode::Svg) page = m_svgEditorTab;
    else if (mode == Mode::Browse) page = m_browseTab;
    m_modeStack->setCurrentWidget(page);
    applyModeChrome(mode);
    if (mode == Mode::Browse && m_browseTab) m_browseTab->refresh();
    // Keep the toolbar buttons in sync when called programmatically.
    QSignalBlocker b1(m_tetherModeAction);
    QSignalBlocker b2(m_retouchModeAction);
    QSignalBlocker b3(m_svgModeAction);
    QSignalBlocker b4(m_browseModeAction);
    m_tetherModeAction->setChecked(mode == Mode::Tether);
    m_retouchModeAction->setChecked(mode == Mode::Retouch);
    m_svgModeAction->setChecked(mode == Mode::Svg);
    m_browseModeAction->setChecked(mode == Mode::Browse);
}

void RetouchWindow::onBrowseOpenRequested(const QStringList &paths) {
    for (const QString &path : paths)
        openPhoto(path);
    setMode(Mode::Retouch);
}

void RetouchWindow::closeEvent(QCloseEvent *event) {
    for (RetouchTab *tab : m_openTabs) {
        if (tab && tab->isReady() && tab->isDirty() && !tab->path().isEmpty())
            tab->saveEdits();
    }
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    QMainWindow::closeEvent(event);
}

void RetouchWindow::applyModeChrome(Mode mode) {
    const bool tether = (mode == Mode::Tether);
    const bool retouch = (mode == Mode::Retouch);
    const bool browse = (mode == Mode::Browse);

    // Tether chrome.
    if (m_tetherToolBar) m_tetherToolBar->setVisible(tether);
    if (m_controlsDock)  m_controlsDock->setVisible(tether);
    if (m_tetherView)    m_tetherView->setActive(tether);

    // Shared filmstrip: irrelevant in Browse mode (it has its own thumbnail
    // grid), so hide it there regardless of the View > Filmstrip toggle.
    const bool filmstripWanted = !m_filmstripAction || m_filmstripAction->isChecked();
    if (m_filmstrip)   m_filmstrip->setVisible(!browse && filmstripWanted);
    if (m_beforeAfter) m_beforeAfter->setVisible(!browse && filmstripWanted);

    // Editing chrome (Retouch mode only — hidden for both Tether and Svg).
    if (!retouch) deselectAllTools(); // exit any active tool + hide the options row
    if (m_toolsBar)        m_toolsBar->setVisible(retouch);
    if (m_adjustmentsDock) m_adjustmentsDock->setVisible(retouch);
    if (m_historyDock)     m_historyDock->setVisible(retouch);
    if (m_layersDock)      m_layersDock->setVisible(retouch);
    if (m_levelsDock)      m_levelsDock->setVisible(retouch);

    // Editing-only actions are meaningless outside Retouch mode.
    m_saveAction->setEnabled(retouch);
    m_saveAllAction->setEnabled(retouch);
    m_exportAction->setEnabled(retouch);
    if (!retouch) {
        m_undoAction->setEnabled(false);
        m_redoAction->setEnabled(false);
    } else {
        // Restore undo/redo + dock state for the current tab.
        onTabChanged(m_tabs->currentIndex());
    }
}

void RetouchWindow::setColorSwatchButton(QPushButton *btn, const QColor &color) {
    if (!btn) return;
    btn->setProperty("color", color);
    btn->setStyleSheet(QString("background-color: %1;").arg(color.name(QColor::HexArgb)));
}

// Refreshes the text options row from the active text (or the "next new
// text" defaults if none is selected), without re-emitting change signals.
void RetouchWindow::updateTextOptionsFromTab() {
    if (!m_textFont) return;
    RetouchTab *tab = currentTab();
    TextOp style = (tab && tab->isReady()) ? tab->activeTextStyle() : TextOp();
    bool hasSelection = tab && tab->activeTextIndex() >= 0;

    const QSignalBlocker b1(m_textFont);
    const QSignalBlocker b2(m_textSize);
    const QSignalBlocker b3(m_textBold);
    const QSignalBlocker b4(m_textItalic);
    const QSignalBlocker b6(m_textOutlineWidth);
    const QSignalBlocker b8(m_textShadowBlur);
    const QSignalBlocker b9(m_textShadowOpacity);
    const QSignalBlocker b11(m_textBgOpacity);
    const QSignalBlocker b12(m_textBgPadding);

    m_textFont->setCurrentFont(QFont(style.family));
    m_textSize->setValue(int(std::lround(style.pixelSize)));
    m_textBold->setChecked(style.bold);
    m_textItalic->setChecked(style.italic);
    setColorSwatchButton(m_textColorBtn, style.color);
    setColorSwatchButton(m_textOutlineColorBtn, style.outlineColor);
    m_textOutlineWidth->setValue(style.outlineWidth);
    setColorSwatchButton(m_textShadowColorBtn, style.shadowColor);
    m_textShadowBlur->setValue(style.shadowBlur);
    m_textShadowOpacity->setValue(style.shadowOpacity);
    setColorSwatchButton(m_textBgColorBtn, style.bgColor);
    m_textBgOpacity->setValue(style.bgOpacity);
    m_textBgPadding->setValue(style.bgPadding);
    if (m_textDelete) m_textDelete->setEnabled(hasSelection);
}

// Refreshes the shape options row from the active shape (or the "next new
// shape" defaults if none is selected), without re-emitting change signals.
void RetouchWindow::updateShapeOptionsFromTab() {
    if (!m_shapeType) return;
    RetouchTab *tab = currentTab();
    ShapeOp style = (tab && tab->isReady()) ? tab->activeShapeStyle() : ShapeOp();
    bool hasSelection = tab && tab->activeShapeIndex() >= 0;

    const QSignalBlocker b1(m_shapeType);
    const QSignalBlocker b2(m_shapeSides);
    const QSignalBlocker b3(m_shapeInnerRatio);
    const QSignalBlocker b4(m_shapeFillEnabled);
    const QSignalBlocker b5(m_shapeStrokeEnabled);
    const QSignalBlocker b6(m_shapeStrokeWidth);

    m_shapeType->setCurrentIndex(m_shapeType->findData(int(style.type)));
    m_shapeSides->setValue(style.sides);
    m_shapeInnerRatio->setValue(style.innerRadiusRatio);
    bool isPoly = (style.type == ShapeType::Polygon || style.type == ShapeType::Star);
    m_shapeSides->setVisible(isPoly);
    m_shapeInnerRatio->setVisible(style.type == ShapeType::Star);
    bool isLine = (style.type == ShapeType::Line);
    m_shapeFillEnabled->setEnabled(!isLine);
    m_shapeFillColorBtn->setEnabled(!isLine);
    m_shapeFillEnabled->setChecked(style.fillEnabled);
    setColorSwatchButton(m_shapeFillColorBtn, style.fillColor);
    m_shapeStrokeEnabled->setChecked(style.strokeEnabled);
    setColorSwatchButton(m_shapeStrokeColorBtn, style.strokeColor);
    m_shapeStrokeWidth->setValue(style.strokeWidth);
    if (m_shapeDelete) m_shapeDelete->setEnabled(hasSelection);
}

void RetouchWindow::deselectAllTools() {
    RetouchTab *tab = currentTab();
    // Delegates to the same shared helpers every tool's toggled(true) handler
    // uses, so this list can't drift out of sync as tools are added (see the
    // erase tool previously being missing here).
    deactivateOtherToolButtons(nullptr);
    deactivateAllToolModes(tab);
    if (m_cropApply) m_cropApply->setEnabled(false);
    if (m_toolOptionsBar) m_toolOptionsBar->setVisible(false);
}

void RetouchWindow::onTabChanged(int) {
    RetouchTab *tab = currentTab();
    bool ready = tab && tab->isReady();
    setDockEnabled(ready);
    deselectAllTools();
    m_undoAction->setEnabled(tab && tab->canUndo());
    m_redoAction->setEnabled(tab && tab->canRedo());
    syncDockFromTab();
    refreshHistoryPanel();
    refreshLevels();
    refreshMaskPanel();
    if (tab) tab->setMaskPreviewEnabled(m_layersDock && m_layersDock->isVisible());
    updateEditClipboardActions();
    if (ready) {
        QSignalBlocker b(m_zoomSlider);
        int pct = int(std::lround(tab->zoomPercent()));
        m_zoomSlider->setValue(std::clamp(pct, m_zoomSlider->minimum(), m_zoomSlider->maximum()));
        m_zoomLabel->setText(QString::number(pct) + "%");
    }
    if (tab)
        m_statusLabel->setText(ready ? "Ready: " + QFileInfo(tab->path()).fileName()
                                     : "Decoding " + QFileInfo(tab->path()).fileName() + "…");
}

void RetouchWindow::onTabCloseRequested(int index) {
    auto *tab = qobject_cast<RetouchTab *>(m_tabs->widget(index));
    if (!tab) return;
    if (tab->isReady() && tab->isDirty() && !tab->path().isEmpty()) tab->saveEdits();
    QString key;
    for (auto it = m_openTabs.begin(); it != m_openTabs.end(); ++it) {
        if (it.value() == tab) { key = it.key(); break; }
    }
    if (!key.isEmpty()) m_openTabs.remove(key);
    m_tabs->removeTab(index);
    tab->deleteLater();
}

void RetouchWindow::onDeleteRequested(const QStringList &paths) {
    int deleted = 0, failed = 0;
    for (const QString &path : paths) {
        // Trash the RAW; skip UI/state removal if the file can't be trashed so
        // we never drop a thumbnail while its file remains on disk.
        if (!QFile::moveToTrash(path)) {
            ++failed;
            continue;
        }
        // Best-effort trash of the edit sidecar and cached thumbnail (may not exist).
        if (EditSidecar::exists(path))
            QFile::moveToTrash(EditSidecar::pathFor(path));
        QFile::moveToTrash(EditSidecar::thumbnailPathFor(path));

        // Close an open editor tab for this photo, if any.
        if (RetouchTab *tab = m_openTabs.value(path, nullptr)) {
            int idx = m_tabs->indexOf(tab);
            if (idx >= 0) m_tabs->removeTab(idx);
            m_openTabs.remove(path);
            tab->deleteLater();
        }

        // Remove the filmstrip thumbnail (match by UserRole path).
        for (int i = 0; i < m_filmstrip->count(); ++i) {
            QListWidgetItem *it = m_filmstrip->item(i);
            if (it->data(Qt::UserRole).toString() == path) {
                delete m_filmstrip->takeItem(i);
                break;
            }
        }
        m_filmstripPaths.remove(path);
        ++deleted;
    }

    if (failed > 0)
        m_statusLabel->setText(
            QString("Deleted %1 photo(s); %2 could not be moved to Trash")
                .arg(deleted).arg(failed));
    else
        m_statusLabel->setText(QString("Deleted %1 photo(s)").arg(deleted));
}

void RetouchWindow::onRenameRequested(const QString &path) {
    // RetouchTab doesn't support having its path changed out from under it,
    // so require the tab to be closed first rather than leaving it pointing
    // at a stale path.
    if (m_openTabs.contains(path)) {
        m_statusLabel->setText("Close this photo's tab before renaming it");
        return;
    }

    QFileInfo info(path);
    bool ok = false;
    const QString newBase = QInputDialog::getText(
        this, "Rename Photo", "New name:", QLineEdit::Normal, info.fileName(), &ok);
    if (!ok || newBase.isEmpty() || newBase == info.fileName()) return;

    const QString newPath = info.dir().filePath(newBase);
    if (QFile::exists(newPath)) {
        m_statusLabel->setText("A file with that name already exists");
        return;
    }
    if (!QFile::rename(path, newPath)) {
        m_statusLabel->setText("Rename failed");
        return;
    }
    if (EditSidecar::exists(path))
        QFile::rename(EditSidecar::pathFor(path), EditSidecar::pathFor(newPath));
    if (QFile::exists(EditSidecar::thumbnailPathFor(path)))
        QFile::rename(EditSidecar::thumbnailPathFor(path), EditSidecar::thumbnailPathFor(newPath));

    m_filmstrip->renamePath(path, newPath);
    m_filmstripPaths.remove(path);
    m_filmstripPaths.insert(newPath);
    m_statusLabel->setText(QString("Renamed to %1").arg(newBase));
}

void RetouchWindow::onRatingChanged(const QString &path, int rating) {
    EditSidecar::saveRating(path, rating);
    m_filmstrip->setRating(path, rating);
    m_statusLabel->setText(rating > 0 ? QString("Rated %1 star(s)").arg(rating)
                                       : QString("Rating cleared"));
}

void RetouchWindow::syncDockFromTab() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    m_syncing = true;
    Adjustments a = tab->adjustments();
    auto set = [](QSlider *s, int v) { QSignalBlocker b(s); s->setValue(v); };
    set(m_brightness, a.brightness);
    set(m_contrast, a.contrast);
    set(m_highlights, a.highlights);
    set(m_shadows, a.shadows);
    set(m_saturation, a.saturation);
    set(m_vibrance, a.vibrance);
    set(m_temperature, a.temperature);
    set(m_tint, a.tint);
    set(m_denoise, a.denoise);
    set(m_clarity, a.clarity);
    set(m_sharpen, a.sharpen);
    set(m_vignette, a.vignette);
    set(m_lightAngle, a.lightAngle);
    set(m_lightIntensity, a.lightIntensity);
    set(m_flatStyle, a.flatStyle);
    m_curve->setCurve(a.curve);
    m_syncing = false;
}

void RetouchWindow::onToneChanged() {
    if (m_syncing) return;
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    Adjustments a = tab->adjustments();
    a.brightness = m_brightness->value();
    a.contrast = m_contrast->value();
    a.highlights = m_highlights->value();
    a.shadows = m_shadows->value();
    a.saturation = m_saturation->value();
    a.vibrance = m_vibrance->value();
    a.temperature = m_temperature->value();
    a.tint = m_tint->value();
    a.denoise = m_denoise->value();
    a.clarity = m_clarity->value();
    a.sharpen = m_sharpen->value();
    a.vignette = m_vignette->value();
    a.lightAngle = m_lightAngle->value();
    a.lightIntensity = m_lightIntensity->value();
    a.flatStyle = m_flatStyle->value();
    tab->setAdjustments(a);
}

void RetouchWindow::setDockEnabled(bool enabled) {
    const QList<QWidget *> widgets = {
        m_brightness, m_contrast, m_highlights, m_shadows, m_saturation,
        m_vibrance, m_temperature, m_tint, m_denoise, m_clarity, m_sharpen, m_vignette,
        m_lightAngle, m_lightIntensity,
        m_flatStyle, m_curve, m_wbPick, m_beforeAfter,
        m_zoomSlider, m_zoomFit, m_toolZoom,
        m_rotLeft, m_rotRight, m_flipH, m_flipV,
        m_cropToggle, m_cropReset, m_cropAspect,
        m_healToggle, m_healBrush, m_healClear};
    for (QWidget *w : widgets)
        if (w) w->setEnabled(enabled);
    if (!enabled && m_cropApply) m_cropApply->setEnabled(false);
    if (!enabled && m_levelsPanel) m_levelsPanel->clear();
    if (!enabled && m_layersPanel) m_layersPanel->clear();
}

void RetouchWindow::onOpenSession() {
    QString dir = QFileDialog::getExistingDirectory(
        this, "Open session folder",
        QDir(QDir::homePath()).filePath("Pictures/Tether"));
    if (dir.isEmpty()) return;
    loadSession(dir);
}

// Scan a session folder for NEFs into the filmstrip, then record it as a
// recent session and refresh the File-menu section. Shared by onOpenSession()
// and the recent-entry click handler.
void RetouchWindow::loadSession(const QString &dir) {
    // A session replaces the filmstrip contents, so it shows only this
    // session's photos rather than appending to whatever was there before.
    m_filmstrip->clear();
    m_filmstripPaths.clear();

    int count = 0;
    const QFileInfoList files =
        QDir(dir).entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files) {
        if (fi.suffix().compare("nef", Qt::CaseInsensitive) == 0) {
            addToFilmstrip(fi.absoluteFilePath());
            ++count;
        }
    }
    m_statusLabel->setText(
        QString("Loaded %1 photo(s) from %2").arg(count).arg(dir));

    RecentSessions::add(QDir(dir).absolutePath());
    rebuildRecentSessionsMenu();
}

// Repopulate the recent-session entries between m_recentBeginSeparator and
// m_recentEndSeparator, reflecting the current RecentSessions::load(). Both
// separators are hidden when the list is empty.
void RetouchWindow::rebuildRecentSessionsMenu() {
    if (!m_fileMenu) return;
    for (QAction *a : m_recentActions) {
        m_fileMenu->removeAction(a);
        a->deleteLater();
    }
    m_recentActions.clear();

    const QStringList recent = RecentSessions::load();
    for (const QString &dir : recent) {
        auto *act = new QAction(QDir(dir).dirName(), this);
        act->setToolTip(dir);
        connect(act, &QAction::triggered, this, [this, dir] {
            if (!QDir(dir).exists()) {
                QMessageBox::warning(
                    this, "Open Session",
                    "This session folder no longer exists:\n" + dir);
                RecentSessions::remove(dir);
                rebuildRecentSessionsMenu();
                return;
            }
            loadSession(dir);
        });
        // Insert before the closing separator so entries sit in the section.
        m_fileMenu->insertAction(m_recentEndSeparator, act);
        m_recentActions.append(act);
    }
    // Menu tooltips are not shown by default; enable them for path hints.
    m_fileMenu->setToolTipsVisible(true);

    const bool hasRecent = !m_recentActions.isEmpty();
    if (m_recentBeginSeparator) m_recentBeginSeparator->setVisible(hasRecent);
    if (m_recentEndSeparator) m_recentEndSeparator->setVisible(hasRecent);
}

// Repopulate the recent-file entries between m_recentFilesBeginSeparator and
// m_recentFilesEndSeparator, reflecting the current RecentFiles::load(). Both
// separators are hidden when the list is empty.
void RetouchWindow::rebuildRecentFilesMenu() {
    if (!m_fileMenu) return;
    for (QAction *a : m_recentFileActions) {
        m_fileMenu->removeAction(a);
        a->deleteLater();
    }
    m_recentFileActions.clear();

    const QStringList recent = RecentFiles::load();
    for (const QString &path : recent) {
        auto *act = new QAction(QFileInfo(path).fileName(), this);
        act->setToolTip(path);
        connect(act, &QAction::triggered, this, [this, path] {
            if (!QFileInfo::exists(path)) {
                QMessageBox::warning(
                    this, "Open Photo",
                    "This file no longer exists:\n" + path);
                RecentFiles::remove(path);
                rebuildRecentFilesMenu();
                return;
            }
            openPhoto(path);
        });
        // Insert before the closing separator so entries sit in the section.
        m_fileMenu->insertAction(m_recentFilesEndSeparator, act);
        m_recentFileActions.append(act);
    }
    m_fileMenu->setToolTipsVisible(true);

    const bool hasRecent = !m_recentFileActions.isEmpty();
    if (m_recentFilesBeginSeparator) m_recentFilesBeginSeparator->setVisible(hasRecent);
    if (m_recentFilesEndSeparator) m_recentFilesEndSeparator->setVisible(hasRecent);
}

// Repopulate the recent-project entries between m_recentProjectsBeginSeparator
// and m_recentProjectsEndSeparator, reflecting the current
// RecentProjects::load(). Both separators are hidden when the list is empty.
void RetouchWindow::rebuildRecentProjectsMenu() {
    if (!m_fileMenu) return;
    for (QAction *a : m_recentProjectActions) {
        m_fileMenu->removeAction(a);
        a->deleteLater();
    }
    m_recentProjectActions.clear();

    const QStringList recent = RecentProjects::load();
    for (const QString &path : recent) {
        auto *act = new QAction(QFileInfo(path).fileName(), this);
        act->setToolTip(path);
        connect(act, &QAction::triggered, this, [this, path] {
            if (!QFileInfo::exists(path)) {
                QMessageBox::warning(
                    this, "Open Project",
                    "This project file no longer exists:\n" + path);
                RecentProjects::remove(path);
                rebuildRecentProjectsMenu();
                return;
            }
            openPhoto(path);
        });
        // Insert before the closing separator so entries sit in the section.
        m_fileMenu->insertAction(m_recentProjectsEndSeparator, act);
        m_recentProjectActions.append(act);
    }
    m_fileMenu->setToolTipsVisible(true);

    const bool hasRecent = !m_recentProjectActions.isEmpty();
    if (m_recentProjectsBeginSeparator) m_recentProjectsBeginSeparator->setVisible(hasRecent);
    if (m_recentProjectsEndSeparator) m_recentProjectsEndSeparator->setVisible(hasRecent);
}

void RetouchWindow::onOpenPhotos() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Open photos for editing",
        QDir(QDir::homePath()).filePath("Pictures/Tether"),
        "RAW images (*.nef *.NEF *.cr2 *.cr3 *.arw *.dng *.raf *.rw2 *.orf);;"
        "Photonloom Project (*.ploom);;All files (*)");
    for (const QString &f : files)
        openPhoto(f);
}

void RetouchWindow::onOpenProject() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Open Photonloom Project",
        QDir(QDir::homePath()).filePath("Pictures/Tether"),
        "Photonloom Project (*.ploom);;All files (*)");
    for (const QString &f : files)
        openPhoto(f);
}

void RetouchWindow::onNewDocument() {
    NewDocumentDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    createUntitledTab(dlg.resultPixelSize());
}

void RetouchWindow::onSave() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    if (tab->path().isEmpty()) {
        // A File > New document has no original photo to sidecar against, so
        // saving it means writing a self-contained Photonloom project file
        // (base pixels + all layers/adjustments in one file) rather than
        // flattening to a plain PNG.
        const QString path = QFileDialog::getSaveFileName(
            this, "Save As",
            QDir(QDir::homePath()).filePath("Pictures/Tether/untitled.ploom"),
            "Photonloom Project (*.ploom)");
        if (path.isEmpty()) return; // user cancelled
        if (!tab->saveProjectFile(path)) {
            QMessageBox::warning(this, "Save Failed", "Could not write project to " + path);
            return;
        }
        reKeyTab(tab, path);
        RecentProjects::add(QFileInfo(path).absoluteFilePath());
        rebuildRecentProjectsMenu();
        refreshMaskPanel(); // the tab's path (and Background layer's name) changed
        m_statusLabel->setText("Saved project: " + QFileInfo(path).fileName());
        return;
    }
    tab->saveEdits();
    refreshMaskPanel();
    m_statusLabel->setText("Saved edits: " + QFileInfo(tab->path()).fileName());
}

void RetouchWindow::onSaveAll() {
    int n = 0;
    for (RetouchTab *tab : m_openTabs) {
        if (tab && tab->isReady() && tab->isDirty() && !tab->path().isEmpty()) {
            tab->saveEdits();
            ++n;
        }
    }
    m_statusLabel->setText(QString("Saved edits for %1 photo(s)").arg(n));
}

// Save As Photonloom Project (*.ploom): available for any tab, not just
// path-less ones — bundles the tab's base pixels and full adjustments into
// one self-contained file, independent of whatever original photo it may
// already be sidecar-linked to.
void RetouchWindow::onSaveAsProject() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) return;
    const QString suggestedName =
        (tab->path().isEmpty() ? QStringLiteral("untitled")
                               : QFileInfo(tab->path()).completeBaseName()) +
        ".ploom";
    const QString path = QFileDialog::getSaveFileName(
        this, "Save As Photonloom Project",
        QDir(QDir::homePath()).filePath("Pictures/Tether/" + suggestedName),
        "Photonloom Project (*.ploom)");
    if (path.isEmpty()) return;
    if (!tab->saveProjectFile(path)) {
        QMessageBox::warning(this, "Save Failed", "Could not write project to " + path);
        return;
    }
    reKeyTab(tab, path);
    RecentProjects::add(QFileInfo(path).absoluteFilePath());
    rebuildRecentProjectsMenu();
    refreshMaskPanel();
    m_statusLabel->setText("Saved project: " + QFileInfo(path).fileName());
}

// Re-keys `tab`'s entry in m_openTabs and its tab-bar label to `path` — used
// whenever a tab's on-disk identity changes after a Save As (blank-canvas
// PNG save, or Save As Project).
void RetouchWindow::reKeyTab(RetouchTab *tab, const QString &path) {
    QString oldKey;
    for (auto it = m_openTabs.begin(); it != m_openTabs.end(); ++it) {
        if (it.value() == tab) { oldKey = it.key(); break; }
    }
    if (!oldKey.isEmpty()) m_openTabs.remove(oldKey);
    m_openTabs.insert(path, tab);
    int idx = m_tabs->indexOf(tab);
    if (idx >= 0) m_tabs->setTabText(idx, QFileInfo(path).fileName());
    // Deliberately NOT calling addToFilmstrip(path) here — per spec,
    // auto-adding a saved-from-blank-canvas tab to the filmstrip is out of
    // scope. The user can add it via File > Open Photos later if they want
    // it in the strip.
}

void RetouchWindow::onExport() {
    RetouchTab *tab = currentTab();
    if (!tab || !tab->isReady()) {
        QMessageBox::information(this, "Export", "No decoded photo to export.");
        return;
    }

    ExportDialog dlg(&m_presetStore, this);
    if (dlg.exec() != QDialog::Accepted) return;
    ExportPreset preset = dlg.selectedPreset();

    QImage rendered = tab->renderFullRes();
    if (rendered.isNull()) {
        QMessageBox::warning(this, "Export", "Nothing to export.");
        return;
    }
    QImage resized = applyExportResize(rendered, preset);
    QImage out = preset.format == ExportPreset::TIFF16 ? resized : ditherTo8Bit(resized);

    QFileInfo src(tab->path());
    QDir editedDir(src.absolutePath() + "/edited");
    editedDir.mkpath(".");
    QString suggested =
        editedDir.filePath(src.completeBaseName() + "." + preset.extension());
    QString filter = preset.format == ExportPreset::PNG   ? "PNG (*.png)"
                      : preset.format == ExportPreset::TIFF16 ? "TIFF (*.tif *.tiff)"
                                                              : "JPEG (*.jpg *.jpeg)";

    QString file = QFileDialog::getSaveFileName(this, "Export image", suggested, filter);
    if (file.isEmpty()) return;

    bool ok = preset.format == ExportPreset::TIFF16 ? writeTiff16(out, file)
              : preset.format == ExportPreset::PNG  ? out.save(file, "PNG")
                                                     : out.save(file, "JPEG", preset.quality);
    if (ok)
        m_statusLabel->setText(QString("Exported %1×%2 → %3")
                                   .arg(out.width()).arg(out.height()).arg(file));
    else
        QMessageBox::warning(this, "Export", "Failed to write " + file);
}
