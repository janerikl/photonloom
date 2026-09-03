#include "edit/RetouchTab.h"
#include "edit/ImageCanvas.h"
#include "edit/RawLoader.h"
#include "edit/EditSidecar.h"
#include "edit/HealTool.h"
#include "edit/TextTool.h"
#include "edit/ShapeTool.h"
#include "edit/InpaintTool.h"

#include <QPainter>
#include <QPolygonF>
#include <QSvgRenderer>

#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QDebug>
#include <QTimer>
#include <QThread>
#include <QtConcurrent>
#include <QFont>
#include <QProgressDialog>
#include <QPointer>
#include <algorithm>
#include <cmath>

void RenderWorker::render(const QImage &src, const Adjustments &adj, int maskSnapshotIndex,
                          const QTransform &orientedToGeom, double geomRotationDeg,
                          double scale, int belowSnapshotIndex) {
    QImage maskSnapshot;
    QImage belowSnapshot;
    QImage result = applyAdjustments(src, adj, &m_brushCache, maskSnapshotIndex,
                                     maskSnapshotIndex >= 0 ? &maskSnapshot : nullptr,
                                     orientedToGeom, geomRotationDeg, scale,
                                     belowSnapshotIndex,
                                     belowSnapshotIndex >= 0 ? &belowSnapshot : nullptr);
    emit done(result, maskSnapshot, belowSnapshot, belowSnapshotIndex);
}

void RenderWorker::renderDragFrame(const QImage &belowSnapshot, int dragMaskIndex,
                                   const Adjustments &adj, const QTransform &orientedToGeom,
                                   double geomRotationDeg, double scale) {
    QRect dirtyRect;
    QImage result = applyAdjustments(QImage(), adj, &m_brushCache, -1, nullptr,
                                     orientedToGeom, geomRotationDeg, scale,
                                     -1, nullptr, dragMaskIndex, &belowSnapshot, &dirtyRect);
    emit done(result, QImage(), QImage(), -1, dirtyRect);
}

namespace {
constexpr int kDisplayMaxDim = 1600; // interactive preview resolution cap
// A prior attempt at cutting drag-time CPU downscaled the drag-preview
// buffer (m_dragBelowSnapshot) below kDisplayMaxDim, since applyMasks' paint
// compositing used to be O(w*h) per frame regardless of dab size. That
// worked but visibly softened the in-progress stroke. applyMasks now patches
// only the bounding box around the newest dab into a cached previous-frame
// composite (see BrushRasterCache::lastComposite in Adjustments.h/.cpp),
// so per-frame cost tracks dab size, not buffer resolution - the downscale
// is no longer needed and m_dragBelowSnapshot is used at full resolution.

Adjustments toneOnly(const Adjustments &a) {
    Adjustments t = a; // copy all tone/colour/detail fields
    t.rotationQuadrants = 0;
    t.flipH = t.flipV = false;
    t.cropRect = QRect();
    return t; // geometry neutralised
}

bool geometryDiffers(const Adjustments &a, const Adjustments &b) {
    return a.rotationQuadrants != b.rotationQuadrants || a.flipH != b.flipH ||
           a.flipV != b.flipV || a.cropRect != b.cropRect;
}

// Brush/Paint radius is stored normalized (fraction of image width) so
// strokes stay resolution-independent; this converts the desired 20px
// default into that normalization for the image at hand.
constexpr double kDefaultBrushRadiusPx = 20.0;
double defaultBrushRadiusNorm(int imageWidth) {
    return imageWidth > 0 ? kDefaultBrushRadiusPx / imageWidth : Mask().brushRadius;
}
} // namespace

RetouchTab::RetouchTab(const QString &path, QWidget *parent, WorkingColorSpace defaultSpace)
    : QWidget(parent), m_path(path),
      m_workingColorSpace(EditSidecar::exists(path) ? EditSidecar::loadWorkingColorSpace(path)
                                                     : defaultSpace) {
    // A self-contained project file carries its own base pixels (no
    // external photo/RAW to decode), so it takes a completely different,
    // synchronous load path instead of the RawLoader/QFutureWatcher one below.
    if (path.endsWith(QStringLiteral(".ploom"), Qt::CaseInsensitive)) {
        // Project base pixels are stored as 8-bit PNG, so working-space
        // gamut selection doesn't apply here — always sRGB.
        m_workingColorSpace = WorkingColorSpace::sRGB;
        QImage base;
        const bool ok = EditSidecar::loadProject(path, base, m_adj);
        for (const Mask &m : m_adj.masks) {
            if (m.isImageLayer()) kickoffImageLayerDecode(m.sourceImagePath);
            if (m.isShapeImageFilled()) loadShapeImageCache(m.shapeImagePath);
        }
        ensureBackgroundMask();
        setupCanvasAndWiring();
        if (!ok || base.isNull()) {
            m_canvas->setPlaceholder("Failed to open project file");
            emit decoded(false);
            return;
        }
        m_base = base;
        rebuildGeom();
        retone();
        emit decoded(true);
        emit editStateChanged(m_dirty, hasEdits());
        m_history = {m_adj};
        m_histIndex = 0;
        emit historyChanged(false, false);
        emit historyListChanged();
        return;
    }

    // Restore previously-saved edits, if any (does not mark dirty).
    EditSidecar::load(m_path, m_adj);
    // Any image layers restored from the sidecar need their source photo
    // decoded again — the cache is never persisted.
    for (const Mask &m : m_adj.masks) {
        if (m.isImageLayer()) kickoffImageLayerDecode(m.sourceImagePath);
        if (m.isShapeImageFilled()) loadShapeImageCache(m.shapeImagePath);
    }
    ensureBackgroundMask();

    setupCanvasAndWiring();

    // Decode off the GUI thread.
    m_watcher = new QFutureWatcher<QImage>(this);
    connect(m_watcher, &QFutureWatcher<QImage>::finished, this,
            &RetouchTab::onDecodeFinished);
    m_watcher->setFuture(QtConcurrent::run(RawLoader::loadAny, m_path, m_workingColorSpace));
}

RetouchTab::RetouchTab(const QSize &blankSize, QWidget *parent, WorkingColorSpace defaultSpace)
    : QWidget(parent), m_path(QString()), m_workingColorSpace(defaultSpace) {
    m_base = QImage(blankSize, QImage::Format_ARGB32);
    m_base.fill(Qt::transparent);
    // A blank new document has no photo to hold, so its first layer is a
    // normal, immediately-drawable Paint layer rather than a Background mask
    // (applyAdjustments already composites straight onto the toned base when
    // no Background-type mask is present, so this needs no other layer).
    Mask m;
    m.type = MaskType::Paint;
    m.name = QStringLiteral("Layer 1");
    m.brushRadius = defaultBrushRadiusNorm(blankSize.width());
    m_adj.masks.append(m);
    m_activeMask = 0;
    m_maskMode = true;

    setupCanvasAndWiring();

    m_canvas->setMaskMode(MaskType::Paint, true);
    rebuildGeom();
    retone();
    emit decoded(true);
}

void RetouchTab::setupCanvasAndWiring() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_canvas = new ImageCanvas;
    layout->addWidget(m_canvas);
    connect(m_canvas, &ImageCanvas::cropSelected, this, &RetouchTab::onCanvasCrop);
    connect(m_canvas, &ImageCanvas::commitCropRequested, this, &RetouchTab::applyCrop);
    connect(m_canvas, &ImageCanvas::colorPicked, this, &RetouchTab::onColorPicked);
    connect(m_canvas, &ImageCanvas::quickColorPicked, this, &RetouchTab::onQuickColorPicked);
    connect(m_canvas, &ImageCanvas::colorRangePickStarted, this,
            &RetouchTab::onColorRangePickStarted);
    connect(m_canvas, &ImageCanvas::colorRangeDragged, this,
            &RetouchTab::onColorRangeDragged);
    connect(m_canvas, &ImageCanvas::colorRangeReleased, this,
            &RetouchTab::onColorRangeReleased);
    connect(m_canvas, &ImageCanvas::healAt, this, &RetouchTab::onHealAt);
    connect(m_canvas, &ImageCanvas::textPlaceRequested, this, &RetouchTab::onTextPlaceRequested);
    connect(m_canvas, &ImageCanvas::textSelected, this, &RetouchTab::onTextSelected);
    connect(m_canvas, &ImageCanvas::textDeselected, this, &RetouchTab::onTextDeselected);
    connect(m_canvas, &ImageCanvas::textMoved, this, &RetouchTab::onTextMoved);
    connect(m_canvas, &ImageCanvas::textRotated, this, &RetouchTab::onTextRotated);
    connect(m_canvas, &ImageCanvas::textEditRequested, this, &RetouchTab::onTextEditRequested);
    connect(m_canvas, &ImageCanvas::textEditCommitted, this, &RetouchTab::onTextEditCommitted);
    connect(m_canvas, &ImageCanvas::textEditCancelled, this, &RetouchTab::onTextEditCancelled);
    connect(m_canvas, &ImageCanvas::textLiveContentChanged, this,
            &RetouchTab::onTextLiveContentChanged);
    connect(m_canvas, &ImageCanvas::textDeleteRequested, this, &RetouchTab::onTextDeleteRequested);
    connect(m_canvas, &ImageCanvas::textResizeStarted, this, &RetouchTab::onTextResizeStarted);
    connect(m_canvas, &ImageCanvas::textResized, this, &RetouchTab::onTextResized);
    connect(m_canvas, &ImageCanvas::objectClicked, this, &RetouchTab::onObjectClicked);
    connect(m_canvas, &ImageCanvas::paintLayerMoveStarted, this,
            &RetouchTab::onPaintLayerMoveStarted);
    connect(m_canvas, &ImageCanvas::paintLayerMoveDelta, this,
            &RetouchTab::onPaintLayerMoveDelta);
    connect(m_canvas, &ImageCanvas::paintLayerMoveFinished, this,
            &RetouchTab::onPaintLayerMoveFinished);
    connect(m_canvas, &ImageCanvas::shapeCreateRequested, this, &RetouchTab::onShapeCreateRequested);
    connect(m_canvas, &ImageCanvas::shapeSelected, this, &RetouchTab::onShapeSelected);
    connect(m_canvas, &ImageCanvas::shapeDeselected, this, &RetouchTab::onShapeDeselected);
    connect(m_canvas, &ImageCanvas::shapeMoved, this, &RetouchTab::onShapeMoved);
    connect(m_canvas, &ImageCanvas::shapeResized, this, &RetouchTab::onShapeResized);
    connect(m_canvas, &ImageCanvas::shapeLineEndpointsChanged, this,
            &RetouchTab::onShapeLineEndpointsChanged);
    connect(m_canvas, &ImageCanvas::shapeRotated, this, &RetouchTab::onShapeRotated);
    connect(m_canvas, &ImageCanvas::shapeDeleteRequested, this, &RetouchTab::onShapeDeleteRequested);
    connect(m_canvas, &ImageCanvas::shapeDuplicateRequested, this,
            &RetouchTab::onShapeDuplicateRequested);
    connect(m_canvas, &ImageCanvas::shapeGroupDuplicateRequested, this,
            &RetouchTab::onShapeGroupDuplicateRequested);
    connect(m_canvas, &ImageCanvas::shapeGroupDeleteRequested, this,
            &RetouchTab::onShapeGroupDeleteRequested);
    connect(m_canvas, &ImageCanvas::shapeToggleSelectRequested, this,
            &RetouchTab::onShapeToggleSelectRequested);
    connect(m_canvas, &ImageCanvas::shapeGroupMoveStarted, this,
            &RetouchTab::onShapeGroupMoveStarted);
    connect(m_canvas, &ImageCanvas::shapeGroupMoveRequested, this,
            &RetouchTab::onShapeGroupMoveRequested);
    connect(m_canvas, &ImageCanvas::shapeGroupResizeStarted, this,
            &RetouchTab::onShapeGroupMoveStarted);
    connect(m_canvas, &ImageCanvas::shapeGroupResizeRequested, this,
            &RetouchTab::onShapeGroupResizeRequested);
    connect(m_canvas, &ImageCanvas::eraseAt, this, &RetouchTab::onEraseAt);
    connect(m_canvas, &ImageCanvas::eraseFinished, this, &RetouchTab::onEraseFinished);
    connect(m_canvas, &ImageCanvas::removeObjectAt, this, &RetouchTab::onRemoveObjectAt);
    connect(m_canvas, &ImageCanvas::removeObjectFinished, this, &RetouchTab::onRemoveObjectFinished);
    connect(m_canvas, &ImageCanvas::zoomChanged, this, &RetouchTab::zoomChanged);
    connect(m_canvas, &ImageCanvas::healBrushRadiusChanged, this, [this](int r) {
        m_healRadiusDisplay = r; // keep in sync so heal ops use the new size
        emit healBrushChanged(r);
    });
    connect(m_canvas, &ImageCanvas::eraseBrushRadiusChanged, this, [this](int r) {
        m_eraseRadiusDisplay = r; // keep in sync so erase dabs use the new size
        emit eraseBrushChanged(r);
    });
    connect(m_canvas, &ImageCanvas::removeObjectBrushRadiusChanged, this, [this](int r) {
        m_removeObjectRadiusDisplay = r; // keep in sync so new dabs use the new size
        emit removeObjectBrushChanged(r);
    });
    connect(m_canvas, &ImageCanvas::maskRadialDragged, this, &RetouchTab::onMaskRadial);
    connect(m_canvas, &ImageCanvas::maskLinearDragged, this, &RetouchTab::onMaskLinear);
    connect(m_canvas, &ImageCanvas::maskBrushPoint, this, &RetouchTab::onMaskBrushPoint);
    connect(m_canvas, &ImageCanvas::bucketFillRequested, this, &RetouchTab::onBucketFillRequested);
    connect(m_canvas, &ImageCanvas::maskEditFinished, this, &RetouchTab::onMaskEditFinished);
    connect(m_canvas, &ImageCanvas::selectionPathChanged, this, &RetouchTab::onSelectionPathChanged);
    connect(m_canvas, &ImageCanvas::selectionFeatherChanged, this, &RetouchTab::onSelectionFeatherChanged);
    connect(m_canvas, &ImageCanvas::cloneStrokePoint, this, &RetouchTab::onCloneStrokePoint);
    connect(m_canvas, &ImageCanvas::cloneFinished, this, &RetouchTab::onCloneFinished);
    connect(m_canvas, &ImageCanvas::imageLayerDropped, this, &RetouchTab::addImageLayer);
    connect(m_canvas, &ImageCanvas::maskBrushRadiusChanged, this, [this](double r) {
        if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
        m_adj.masks[m_activeMask].brushRadius = r;
        pushMaskGizmo();
        retone();
        markEdited();
        emit maskBrushChanged(r);
    });
    connect(m_canvas, &ImageCanvas::selectBrushRadiusChanged, this, [this](double r) {
        emit selectBrushChanged(r);
    });
    connect(m_canvas, &ImageCanvas::imageLayerTransformChanged, this,
            [this](const QPointF &offset, const QPointF &scale, bool lockRatio) {
                if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
                Mask &m = m_adj.masks[m_activeMask];
                if (!m.isImageLayer()) return;
                m.sourceImageOffset = offset;
                m.sourceImageScale = scale;
                m.sourceImageLockRatio = lockRatio;
                retone();
                markEdited();
            });
    connect(m_canvas, &ImageCanvas::backgroundColorChanged, this,
            [this](const QColor &color) {
                if (m_adj.backgroundColor == color) return;
                m_adj.backgroundColor = color;
                markEdited();
            });
    connect(m_canvas, &ImageCanvas::guidesChanged, this,
            [this](const QVector<double> &h, const QVector<double> &v) {
                m_adj.guidesH = h;
                m_adj.guidesV = v;
                markEdited();
            });

    m_canvas->setBackgroundColor(m_adj.backgroundColor); // restored from sidecar, if any
    m_canvas->setGuides(m_adj.guidesH, m_adj.guidesV);    // restored from sidecar, if any
    m_canvas->setPlaceholder("Decoding RAW…");

    // After dragging stops, upgrade the preview with the expensive convolutions.
    m_fullRenderTimer = new QTimer(this);
    m_fullRenderTimer->setSingleShot(true);
    m_fullRenderTimer->setInterval(140);
    connect(m_fullRenderTimer, &QTimer::timeout, this, &RetouchTab::retoneFull);

    // Coalesce rapid edits (a slider drag) into one undo step.
    m_commitTimer = new QTimer(this);
    m_commitTimer->setSingleShot(true);
    m_commitTimer->setInterval(350);
    connect(m_commitTimer, &QTimer::timeout, this, &RetouchTab::commitHistory);

    // Render worker on its own thread so applyAdjustments never blocks the GUI.
    qRegisterMetaType<Adjustments>("Adjustments");
    m_renderThread = new QThread(this);
    m_renderWorker = new RenderWorker;
    m_renderWorker->moveToThread(m_renderThread);
    connect(m_renderThread, &QThread::finished, m_renderWorker, &QObject::deleteLater);
    connect(m_renderWorker, &RenderWorker::done, this, &RetouchTab::onRenderDone);
    m_renderThread->start();
}

RetouchTab::~RetouchTab() {
    if (m_watcher) {
        m_watcher->waitForFinished(); // ensure the decode worker isn't writing after free
    }
    if (m_renderThread) {
        m_renderThread->quit();
        m_renderThread->wait();
    }
}

void RetouchTab::onDecodeFinished() {
    m_base = m_watcher->result();
    if (m_base.isNull()) {
        m_canvas->setPlaceholder("Failed to decode RAW");
        emit decoded(false);
        return;
    }
    rebuildGeom();
    emit decoded(true);
    // Reflect any restored edits in the badge (clean, but possibly has edits).
    emit editStateChanged(m_dirty, hasEdits());
    // Seed the undo history with the initial (loaded) state.
    m_history = {m_adj};
    m_histIndex = 0;
    emit historyChanged(false, false);
    emit historyListChanged();
}

bool RetouchTab::hasEdits() const {
    return hasToneEdits(m_adj) || m_adj.rotationQuadrants != 0 || m_adj.flipH ||
           m_adj.flipV || !m_adj.cropRect.isNull() || !m_adj.heals.isEmpty();
}

void RetouchTab::assignPath(const QString &path) {
    m_path = path;
}

void RetouchTab::markEdited() {
    m_dirty = true;
    emit editStateChanged(true, hasEdits());
    if (m_commitTimer) m_commitTimer->start(); // schedule an undo snapshot
}

void RetouchTab::commitHistory() {
    if (m_histIndex < 0) { // not seeded yet
        m_history = {m_adj};
        m_histIndex = 0;
        emit historyChanged(canUndo(), canRedo());
        emit historyListChanged();
        return;
    }
    if (m_adj == m_history[m_histIndex]) return; // nothing new to record
    m_history.resize(m_histIndex + 1);           // drop any redo branch
    m_history.append(m_adj);
    m_histIndex = m_history.size() - 1;
    const int kMaxHistory = 60;
    if (m_history.size() > kMaxHistory) {
        m_history.removeFirst();
        --m_histIndex;
    }
    emit historyChanged(canUndo(), canRedo());
    emit historyListChanged();
}

void RetouchTab::applyHistoryState() {
    m_adj = m_history[m_histIndex];
    rebuildGeom();
    m_canvas->setBackgroundColor(m_adj.backgroundColor);
    m_canvas->setGuides(m_adj.guidesH, m_adj.guidesV);
    // Keep the active-mask index valid after undo/redo changes the mask list.
    if (m_activeMask >= m_adj.masks.size())
        m_activeMask = m_adj.masks.isEmpty() ? -1 : m_adj.masks.size() - 1;
    pushMaskGizmo();
    m_dirty = true;
    emit adjustmentsReplaced();
    emit masksChanged();
    emit editStateChanged(m_dirty, hasEdits());
    emit historyChanged(canUndo(), canRedo());
    emit historyListChanged();
}

void RetouchTab::undo() {
    if (m_commitTimer) m_commitTimer->stop();
    commitHistory(); // capture any in-progress change first
    if (!canUndo()) return;
    --m_histIndex;
    applyHistoryState();
}

void RetouchTab::redo() {
    if (m_commitTimer) m_commitTimer->stop();
    commitHistory();
    if (!canRedo()) return;
    ++m_histIndex;
    applyHistoryState();
}

void RetouchTab::jumpToHistory(int index) {
    if (m_commitTimer) m_commitTimer->stop();
    commitHistory(); // capture any in-progress change first
    if (m_history.isEmpty()) return;
    index = qBound(0, index, m_history.size() - 1);
    if (index == m_histIndex) return;
    m_histIndex = index;
    applyHistoryState();
}

void RetouchTab::saveEdits() {
    // A self-contained project file must be rewritten as a whole (base
    // pixels + adjustments); the per-photo sidecar path would silently
    // write a stray .nte.json next to it instead of updating the project.
    if (m_path.endsWith(QStringLiteral(".ploom"), Qt::CaseInsensitive)) {
        EditSidecar::saveProject(m_path, m_base, m_adj);
        m_dirty = false;
        emit editStateChanged(false, hasEdits());
        return;
    }
    EditSidecar::save(m_path, m_adj);
    EditSidecar::saveWorkingColorSpace(m_path, m_workingColorSpace);
    // Cache the edited look so the filmstrip reflects it across sessions.
    if (!m_lastEdited.isNull())
        EditSidecar::saveThumbnail(m_path, m_lastEdited);
    m_dirty = false;
    emit editStateChanged(false, hasEdits());
}

// Writes a self-contained .ploom project file (base pixels + full
// Adjustments) at `path` and re-keys this tab to it, same as saveEdits()
// does for the sidecar-per-photo format.
bool RetouchTab::saveProjectFile(const QString &path) {
    if (!EditSidecar::saveProject(path, m_base, m_adj)) return false;
    m_path = path;
    m_dirty = false;
    emit editStateChanged(false, hasEdits());
    return true;
}

// Orient (no crop) → heal → paint cached removal fills, all in oriented,
// pre-crop space. Doing this before crop keeps heal/removal coordinates
// independent of the crop rectangle (same convention as HealOp).
QImage RetouchTab::orientedPreCropSource() const {
    Adjustments orientAdj;
    orientAdj.rotationQuadrants = m_adj.rotationQuadrants;
    orientAdj.flipH = m_adj.flipH;
    orientAdj.flipV = m_adj.flipV;
    // The Background layer's own visibility/presence now lives in m_adj.masks
    // (MaskType::Background) like any other layer, so `base` here is always
    // just the tab's raw loaded photo — applyAdjustments composites the
    // masks stack (including Background, wherever it sits) on top of a blank
    // canvas, so a hidden/deleted Background renders as transparent through
    // the normal generic mask-visibility code path.
    QImage oriented = applyAdjustments(m_base, orientAdj);
    if (!m_adj.heals.isEmpty()) applyHeal(oriented, m_adj.heals);

    if (!m_adj.removals.isEmpty()) {
        oriented = oriented.convertToFormat(QImage::Format_ARGB32);
        QPainter p(&oriented);
        for (const RemoveObjectOp &op : m_adj.removals) {
            if (!op.visible || op.fill.isNull() || op.rect.isEmpty()) continue;
            p.drawImage(op.rect.topLeft(), op.fill);
        }
    }
    return oriented;
}

QPointF RetouchTab::orientedDelta(const QPointF &geomDelta) const {
    if (m_geomRotationDeg == 0.0) return geomDelta;
    QTransform r;
    r.rotate(-m_geomRotationDeg);
    return r.map(geomDelta);
}

void RetouchTab::rebuildGeom() {
    if (m_base.isNull()) return;
    QImage oriented = orientedPreCropSource();

    m_geomImg = oriented;
    m_orientedToGeom = QTransform();
    m_geomToOriented = QTransform();
    m_geomRotationDeg = 0.0;
    if (!m_cropMode && !m_adj.cropRect.isNull()) {
        QRect r = m_adj.cropRect.intersected(oriented.rect());
        if (r.isValid() && !r.isEmpty()) {
            if (m_adj.cropAngle == 0.0) {
                m_geomImg = oriented.copy(r);
                m_orientedToGeom.translate(-r.x(), -r.y());
            } else {
                QPointF center = r.center();
                QTransform rot;
                rot.translate(center.x(), center.y());
                rot.rotate(m_adj.cropAngle);
                rot.translate(-center.x(), -center.y());
                QRectF rotatedBounds = rot.mapRect(QRectF(oriented.rect()));
                QImage canvas(rotatedBounds.size().toSize(), QImage::Format_ARGB32_Premultiplied);
                canvas.fill(Qt::transparent);
                QPainter cp(&canvas);
                cp.setRenderHint(QPainter::SmoothPixmapTransform);
                cp.translate(-rotatedBounds.topLeft());
                cp.setTransform(rot, true);
                cp.drawImage(0, 0, oriented);
                cp.end();
                QPointF cropTopLeftInCanvas = QPointF(r.topLeft()) - rotatedBounds.topLeft();
                m_geomImg = canvas.copy(QRect(cropTopLeftInCanvas.toPoint(), r.size()));
                m_geomRotationDeg = m_adj.cropAngle;
                // oriented -> geom: rotate about the crop rect's own center,
                // then place that (invariant) center at the cropped image's
                // own center — see RetouchTab.h's m_orientedToGeom comment.
                QPointF half(r.width() / 2.0, r.height() / 2.0);
                m_orientedToGeom.translate(half.x(), half.y());
                m_orientedToGeom.rotate(m_adj.cropAngle);
                m_orientedToGeom.translate(-center.x(), -center.y());
            }
            m_geomToOriented = m_orientedToGeom.inverted();
        }
    }

    m_canvas->setShowCheckerboard(true);

    if (qMax(m_geomImg.width(), m_geomImg.height()) > kDisplayMaxDim) {
        m_scaled = m_geomImg.scaled(kDisplayMaxDim, kDisplayMaxDim,
                                    Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        m_scaled = m_geomImg;
    }
    m_scaleFromGeom = m_geomImg.width() > 0
                          ? double(m_scaled.width()) / m_geomImg.width()
                          : 1.0;
    // Keep the Background mask's thumbnail source in sync with the current
    // geometry (crop/rotate/flip) — only used by LayersPanel's generic
    // maskThumbnail(); actual rendering always sources fresh content from
    // `m_base` inside applyAdjustments (see applyAdjustments in
    // Adjustments.cpp), so this never causes spurious history/dirty churn
    // (Mask::operator== ignores sourceImageCache).
    for (Mask &m : m_adj.masks)
        if (m.type == MaskType::Background) m.sourceImageCache = m_scaled;
    updateHealSpots();
    updateTextMarkers();
    updateShapeMarkers();
    updateRemovalMarkers();
    updateObjectMarkers();
    retone();
}

// Convert stored text ops (oriented-image, pre-crop coords) into the display
// (m_scaled) pixel space so the canvas can draw/hit-test selection boxes.
void RetouchTab::updateTextMarkers() {
    QVector<ImageCanvas::TextMarker> markers;
    m_textMaskIndices.clear();
    for (int i = 0; i < m_adj.masks.size(); ++i) {
        const Mask &mk = m_adj.masks[i];
        if (mk.type != MaskType::TextBox) continue;
        TextOp local;
        local.pos = m_orientedToGeom.map(mk.textBoxPos) * m_scaleFromGeom;
        local.text = mk.textBoxText;
        local.family = mk.textBoxFamily;
        local.pixelSize = mk.textBoxPixelSize * m_scaleFromGeom;
        local.bold = mk.textBoxBold;
        local.italic = mk.textBoxItalic;
        ImageCanvas::TextMarker m;
        m.rect = textOpBounds(local);
        m.rotation = mk.textBoxRotation + m_geomRotationDeg;
        markers.append(m);
        m_textMaskIndices.append(i);
    }
    m_canvas->setTextMarkers(markers);
    m_canvas->setActiveTextIndex(m_activeText);
}

// Marker index -> real m_adj.masks index (see m_textMaskIndices), or -1 if
// markerIndex is out of range.
int RetouchTab::textMaskIndex(int markerIndex) const {
    if (markerIndex < 0 || markerIndex >= m_textMaskIndices.size()) return -1;
    return m_textMaskIndices[markerIndex];
}

// Convert stored shape ops (oriented-image, pre-crop coords) into the
// display (m_scaled) pixel space so the canvas can draw/hit-test selection
// boxes/handles.
void RetouchTab::updateShapeMarkers() {
    QVector<ImageCanvas::ShapeMarker> markers;
    m_shapeMaskIndices.clear();
    for (int i = 0; i < m_adj.masks.size(); ++i) {
        const Mask &mk = m_adj.masks[i];
        if (mk.type != MaskType::Shape) continue;
        ImageCanvas::ShapeMarker m;
        m.type = mk.shapeType;
        QPointF center = m_orientedToGeom.map(mk.shapeRect.center()) * m_scaleFromGeom;
        m.rect = QRectF(QPointF(0, 0), mk.shapeRect.size() * m_scaleFromGeom);
        m.rect.moveCenter(center);
        m.p1 = m_orientedToGeom.map(mk.shapeP1) * m_scaleFromGeom;
        m.p2 = m_orientedToGeom.map(mk.shapeP2) * m_scaleFromGeom;
        m.rotation = (mk.shapeType == ShapeType::Line) ? mk.shapeRotation
                                                         : mk.shapeRotation + m_geomRotationDeg;
        markers.append(m);
        m_shapeMaskIndices.append(i);
    }
    m_canvas->setShapeMarkers(markers);
    m_canvas->setActiveShapeIndex(m_activeShape);
    m_canvas->setSelectedShapeIndices(m_selectedShapes);
}

// Marker index -> real m_adj.masks index (see m_shapeMaskIndices), or -1 if
// markerIndex is out of range.
int RetouchTab::shapeMaskIndex(int markerIndex) const {
    if (markerIndex < 0 || markerIndex >= m_shapeMaskIndices.size()) return -1;
    return m_shapeMaskIndices[markerIndex];
}

// Bounding-box markers for the click-to-select fallback (see
// ImageCanvas::objectClicked). Unlike shape/text ops, Paint stroke points
// and image-layer offset/scale are stored directly in the current display
// (m_scaled) normalized space (no oriented/pre-crop transform — they're
// painted/positioned live against whatever's currently on screen), so no
// m_orientedToGeom mapping is needed here.
void RetouchTab::updateObjectMarkers() {
    QVector<QRectF> paintRects, imageRects;
    m_paintMaskIndices.clear();
    m_imageLayerMaskIndices.clear();
    const double W = m_scaled.width();
    const double H = m_scaled.height();
    for (int i = 0; i < m_adj.masks.size(); ++i) {
        const Mask &mk = m_adj.masks[i];
        if (mk.type == MaskType::Paint) {
            QRectF bounds;
            for (const BrushStrokePoint &p : mk.stroke) {
                double r = p.radius * W;
                QRectF dabRect(p.pt.x() * W - r, p.pt.y() * W - r, 2 * r, 2 * r);
                bounds = bounds.isNull() ? dabRect : bounds.united(dabRect);
            }
            // Bucket-filled coverage can extend anywhere; approximating its
            // hit area with the full frame is simpler than scanning
            // fillMask's alpha at its own (possibly different) resolution.
            if (!mk.fillMask.isNull()) bounds = bounds.united(QRectF(0, 0, W, H));
            if (!bounds.isNull()) {
                paintRects.append(bounds);
                m_paintMaskIndices.append(i);
            }
        } else if (mk.isImageLayer()) {
            const double w = W * std::max(0.01, mk.sourceImageScale.x());
            const double h = H * std::max(0.01, mk.sourceImageScale.y());
            const double cx = W * (0.5 + 0.5 * std::clamp(mk.sourceImageOffset.x(), -1.0, 1.0));
            const double cy = H * (0.5 + 0.5 * std::clamp(mk.sourceImageOffset.y(), -1.0, 1.0));
            imageRects.append(QRectF(cx - w / 2.0, cy - h / 2.0, w, h));
            m_imageLayerMaskIndices.append(i);
        }
    }
    m_canvas->setPaintMarkers(paintRects);
    m_canvas->setImageLayerMarkers(imageRects);
}

// Canvas click-to-select fallback (ImageCanvas::objectClicked): select the
// clicked layer and switch to its editing tool, same as clicking its row in
// the Layers panel would for selection, plus a tool switch. RetouchWindow
// listens for objectToolRequested to flip the matching toolbar button on,
// which resets every other tool the same way a manual click on that button
// would (see the toggled() lambdas in RetouchWindow's tool setup).
void RetouchTab::onObjectClicked(MaskType type, int markerIndex) {
    int maskIndex = -1;
    switch (type) {
    case MaskType::Shape:    maskIndex = shapeMaskIndex(markerIndex); break;
    case MaskType::TextBox:  maskIndex = textMaskIndex(markerIndex); break;
    case MaskType::Paint:
        maskIndex = (markerIndex >= 0 && markerIndex < m_paintMaskIndices.size())
                        ? m_paintMaskIndices[markerIndex] : -1;
        break;
    case MaskType::Background:
        maskIndex = (markerIndex >= 0 && markerIndex < m_imageLayerMaskIndices.size())
                        ? m_imageLayerMaskIndices[markerIndex] : -1;
        break;
    default: break;
    }
    if (maskIndex < 0) return;
    selectMask(maskIndex);
    emit objectToolRequested(type);
}

// Convert stored heal ops (oriented-image, pre-crop coords) into the display
// (m_scaled) pixel space so the canvas can draw hover-highlight markers.
void RetouchTab::updateHealSpots() {
    QVector<ImageCanvas::HealMarker> spots;
    for (const HealOp &op : m_adj.heals) {
        ImageCanvas::HealMarker m;
        m.pos = m_orientedToGeom.map(QPointF(op.x, op.y)) * m_scaleFromGeom;
        m.radius = op.radius * m_scaleFromGeom;
        spots.append(m);
    }
    m_canvas->setHealSpots(spots);
}

// Convert stored removal ops (oriented-image, pre-crop coords) into the
// display (m_scaled) pixel space so the canvas can draw/hit-test their
// bounding-box outline, mirroring updateShapeMarkers.
void RetouchTab::updateRemovalMarkers() {
    QVector<ImageCanvas::RemovalMarker> markers;
    for (const RemoveObjectOp &op : m_adj.removals) {
        ImageCanvas::RemovalMarker m;
        // Bounding-box highlight only (no rotation field on RemovalMarker),
        // so under a straightened crop this is the axis-aligned bounds of
        // the rotated rect — a close-enough approximation for a cosmetic
        // hover outline.
        QRectF b = m_orientedToGeom.map(QPolygonF(QRectF(op.rect))).boundingRect();
        m.rect = QRectF(b.topLeft() * m_scaleFromGeom, b.size() * m_scaleFromGeom);
        markers.append(m);
    }
    m_canvas->setRemovalMarkers(markers);
    m_canvas->setActiveRemovalIndex(m_activeRemoval);
}

void RetouchTab::retone() {
    if (m_scaled.isNull()) return;
    // Any edit that isn't a brush/erase-stroke continuation invalidates the
    // retoneDrag() below-active-mask cache — the layers it was captured from
    // may have just changed.
    m_dragSnapshotValid = false;
    // Fast interactive path: skip the clarity/sharpen blur convolutions (the
    // expensive part) while the user is dragging, then schedule a full render.
    Adjustments t = toneOnly(m_adj);
    bool heavy = (t.denoise != 0 || t.clarity != 0 || t.sharpen != 0);
    if (heavy) { t.denoise = 0; t.clarity = 0; t.sharpen = 0; }
    // Opportunistically capture the below-active-mask snapshot so a
    // following retoneDrag() (brush/erase dab) can use the fast path.
    int belowIdx = -1;
    if (m_activeMask >= 0 && m_activeMask < m_adj.masks.size()) {
        belowIdx = m_activeMask;
        for (int i = 0; i <= m_activeMask; ++i)
            if (m_adj.masks[i].type == MaskType::Background) { belowIdx = -1; break; }
    }
    requestRender(m_scaled, t, maskPreviewIndex(), belowIdx);
    if (heavy) m_fullRenderTimer->start();
    else m_fullRenderTimer->stop();
}

// Fast per-mouse-move brush/erase-stroke preview; see declaration comment.
void RetouchTab::retoneDrag(bool newStroke) {
    if (m_scaled.isNull()) return;
    const int activeIdx = m_activeMask;
    bool eligible = !newStroke && activeIdx >= 0 && activeIdx < m_adj.masks.size() &&
                    m_dragSnapshotValid && m_dragSnapshotIndex == activeIdx;
    if (!eligible) {
        retone();
        return;
    }
    Adjustments t = toneOnly(m_adj);
    // clarity/sharpen/denoise are always stripped during drag (see retone());
    // the deferred full render triggered there covers them once idle.
    t.denoise = 0; t.clarity = 0; t.sharpen = 0;
    requestDragRender(m_dragBelowSnapshot, activeIdx, t);
    m_fullRenderTimer->start();
}

void RetouchTab::retoneFull() {
    if (m_scaled.isNull()) return;
    m_dragSnapshotValid = false;
    requestRender(m_scaled, toneOnly(m_adj), maskPreviewIndex());
}

// Coalesced async render: at most one job in flight; newer requests overwrite
// the pending one so intermediate drag frames are dropped, not queued.
void RetouchTab::requestRender(const QImage &src, const Adjustments &adj, int maskSnapshotIndex,
                               int belowSnapshotIndex) {
    if (m_rendering) {
        m_pendingSrc = src;
        m_pendingAdj = adj;
        m_pendingMaskIdx = maskSnapshotIndex;
        m_pendingBelowIdx = belowSnapshotIndex;
        m_pendingIsDrag = false;
        m_hasPending = true;
        return;
    }
    m_rendering = true;
    QMetaObject::invokeMethod(m_renderWorker, "render", Qt::QueuedConnection,
                              Q_ARG(QImage, src), Q_ARG(Adjustments, adj),
                              Q_ARG(int, maskSnapshotIndex),
                              Q_ARG(QTransform, m_orientedToGeom),
                              Q_ARG(double, m_geomRotationDeg),
                              Q_ARG(double, m_scaleFromGeom),
                              Q_ARG(int, belowSnapshotIndex));
}

void RetouchTab::requestDragRender(const QImage &belowSnapshot, int dragMaskIndex,
                                   const Adjustments &adj) {
    if (m_rendering) {
        m_pendingDragBelow = belowSnapshot;
        m_pendingDragIndex = dragMaskIndex;
        m_pendingAdj = adj;
        m_pendingIsDrag = true;
        m_hasPending = true;
        return;
    }
    // Each drag frame recomposites the active layer's full buffer (see
    // applyMasks), so without a cap a fast mouse re-queues a new render the
    // instant the previous one finishes, pinning a CPU core for the whole
    // stroke. Coalesce bursts down to ~60fps instead; final (non-drag)
    // renders go through requestRender() and are unaffected.
    if (m_lastDragRenderTime.isValid() &&
        m_lastDragRenderTime.elapsed() < kDragRenderMinIntervalMs) {
        m_pendingDragBelow = belowSnapshot;
        m_pendingDragIndex = dragMaskIndex;
        m_pendingAdj = adj;
        m_pendingIsDrag = true;
        m_hasPending = true;
        if (!m_dragThrottlePending) {
            m_dragThrottlePending = true;
            const int delay = kDragRenderMinIntervalMs - int(m_lastDragRenderTime.elapsed());
            QTimer::singleShot(std::max(0, delay), this, [this]() {
                m_dragThrottlePending = false;
                if (m_hasPending && m_pendingIsDrag && !m_rendering) {
                    m_hasPending = false;
                    requestDragRender(m_pendingDragBelow, m_pendingDragIndex, m_pendingAdj);
                }
            });
        }
        return;
    }
    m_lastDragRenderTime.start();
    m_rendering = true;
    QMetaObject::invokeMethod(m_renderWorker, "renderDragFrame", Qt::QueuedConnection,
                              Q_ARG(QImage, belowSnapshot), Q_ARG(int, dragMaskIndex),
                              Q_ARG(Adjustments, adj),
                              Q_ARG(QTransform, m_orientedToGeom),
                              Q_ARG(double, m_geomRotationDeg),
                              Q_ARG(double, m_scaleFromGeom));
}

void RetouchTab::onRenderDone(const QImage &result, const QImage &maskSnapshot,
                              const QImage &belowSnapshot, int belowSnapshotIndex,
                              const QRect &dirtyRect) {
    m_lastEdited = result;
    if (!maskSnapshot.isNull()) m_maskPreviewImage = maskSnapshot;
    // Only trust the below-snapshot if it's still for the currently active
    // mask — the selection may have changed while this render was in flight.
    if (belowSnapshotIndex >= 0 && belowSnapshotIndex == m_activeMask && !belowSnapshot.isNull()) {
        m_dragBelowSnapshot = belowSnapshot;
        m_dragSnapshotIndex = belowSnapshotIndex;
        m_dragSnapshotValid = true;
    }
    if (!m_showingOriginal) m_canvas->setImage(m_lastEdited, dirtyRect);
    // Skip the expensive per-frame side effects these signals trigger
    // (RetouchWindow recomputes the Levels histogram over the *whole* image
    // and regenerates the filmstrip thumbnail - both O(image area) - see
    // RetouchWindow's previewUpdated/maskPreviewUpdated handlers) while
    // actively dragging a brush. `dirtyRect` valid means this was a
    // dirty-rect drag frame where only a tiny region actually changed, so a
    // stale histogram/thumbnail for the ~100ms until the stroke settles is
    // imperceptible - they catch up via the full render m_fullRenderTimer
    // triggers once dragging stops (see retoneDrag/retoneFull), which always
    // reports an invalid dirtyRect.
    if (!dirtyRect.isValid()) {
        emit previewUpdated();
        if (m_maskPreviewEnabled) emit maskPreviewUpdated();
    }
    m_rendering = false;
    if (m_hasPending) {
        m_hasPending = false;
        if (m_pendingIsDrag)
            requestDragRender(m_pendingDragBelow, m_pendingDragIndex, m_pendingAdj);
        else
            requestRender(m_pendingSrc, m_pendingAdj, m_pendingMaskIdx, m_pendingBelowIdx);
    }
}

void RetouchTab::setMaskPreviewEnabled(bool on) {
    if (m_maskPreviewEnabled == on) return;
    m_maskPreviewEnabled = on;
    if (on) retone(); // populate the snapshot for the currently active layer
}

// Press-and-hold before/after. "Original" is the framed base without tone/
// colour/detail edits (m_scaled is oriented+cropped but untoned).
void RetouchTab::showOriginal(bool on) {
    if (m_scaled.isNull()) return;
    m_showingOriginal = on;
    if (on) {
        m_canvas->setImage(m_scaled);
    } else if (!m_lastEdited.isNull()) {
        m_canvas->setImage(m_lastEdited);
    } else {
        retone();
    }
}

void RetouchTab::setAdjustments(const Adjustments &a) {
    bool geom = geometryDiffers(a, m_adj);
    Adjustments merged = a;
    // Preserve the runtime-only image-layer decode cache: callers snapshot
    // adjustments(), mutate one field, and write the whole struct back, which
    // would otherwise silently revert a cache populated by an in-flight
    // kickoffImageLayerDecode() and permanently disable that layer.
    for (int i = 0; i < merged.masks.size() && i < m_adj.masks.size(); ++i) {
        if (merged.masks[i].sourceImagePath == m_adj.masks[i].sourceImagePath) {
            merged.masks[i].sourceImageCache = m_adj.masks[i].sourceImageCache;
            merged.masks[i].sourceMissing = m_adj.masks[i].sourceMissing;
        }
    }
    m_adj = merged;
    pushMaskGizmo();
    if (geom) rebuildGeom();
    else retone();
    markEdited();
}

void RetouchTab::setCropMode(bool on) {
    m_cropMode = on;
    m_canvas->setCropMode(on);
    if (on) m_canvas->setFocus(); // so Enter commits the crop
    else m_canvas->clearSelection();
    m_pendingCrop = QRect();
    emit cropPending(false);
    rebuildGeom(); // show uncropped while selecting
}

void RetouchTab::setCropAspect(double widthOverHeight) {
    m_canvas->setCropAspect(widthOverHeight);
}

void RetouchTab::setWbPickMode(bool on) {
    m_canvas->setPickMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setColorRangePickMode(bool on) {
    m_canvas->setColorRangePickMode(on);
    if (on) m_canvas->setFocus();
}

// A targeted color-range pick started: reuse a close-enough existing entry
// (same dominant channel) so re-picking the same color continues adjusting it,
// otherwise append a fresh entry for this gesture.
void RetouchTab::onColorRangePickStarted(const QColor &c) {
    const int channel = (c.green() >= c.red() && c.green() >= c.blue())
                            ? 1
                            : (c.red() >= c.blue() ? 0 : 2);
    m_crIndex = -1;
    for (int i = 0; i < m_adj.colorRanges.size(); ++i) {
        const ColorRangeAdjust &cr = m_adj.colorRanges[i];
        if (cr.channel != channel) continue;
        const int dr = cr.r - c.red(), dg = cr.g - c.green(), db = cr.b - c.blue();
        if (dr * dr + dg * dg + db * db <= 30 * 30) {
            m_crIndex = i;
            break;
        }
    }
    if (m_crIndex < 0) {
        ColorRangeAdjust cr;
        cr.r = c.red();
        cr.g = c.green();
        cr.b = c.blue();
        cr.channel = channel;
        m_adj.colorRanges.append(cr);
        m_crIndex = m_adj.colorRanges.size() - 1;
    }
    m_crBaseAmount = m_adj.colorRanges[m_crIndex].amount;
}

void RetouchTab::onColorRangeDragged(int dxPixels) {
    if (m_crIndex < 0 || m_crIndex >= m_adj.colorRanges.size()) return;
    const int amount = qBound(-100, m_crBaseAmount + dxPixels / 3, 100);
    if (m_adj.colorRanges[m_crIndex].amount == amount) return;
    m_adj.colorRanges[m_crIndex].amount = amount;
    m_canvas->setColorRangeAmount(amount);
    retone();
}

void RetouchTab::onColorRangeReleased() {
    if (m_crIndex < 0 || m_crIndex >= m_adj.colorRanges.size()) return;
    const bool removed = m_adj.colorRanges[m_crIndex].amount == 0;
    if (removed) m_adj.colorRanges.removeAt(m_crIndex);
    m_crIndex = -1;
    if (removed) retone();
    markEdited();
}

void RetouchTab::setHealMode(bool on) {
    m_canvas->setHealMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setZoomMode(bool on) {
    m_canvas->setZoomMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setHealBrush(int radiusDisplayPx) {
    m_healRadiusDisplay = radiusDisplayPx;
    m_canvas->setBrushRadius(radiusDisplayPx);
}

void RetouchTab::setEraseMode(bool on) {
    m_canvas->setEraseMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setMaskForceErase(bool on) {
    m_canvas->setMaskForceErase(on);
}

void RetouchTab::setBucketMode(bool on) {
    m_canvas->setBucketMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setSelectMarqueeMode(bool on) {
    m_canvas->setSelectMarqueeMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setSelectLassoMode(bool on) {
    m_canvas->setSelectLassoMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setSelectMagicWandMode(bool on) {
    m_canvas->setSelectMagicWandMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setMagicWandTolerance(int tolerance) {
    m_canvas->setMagicWandTolerance(tolerance);
}

void RetouchTab::setSelectBrushMode(bool on) {
    m_canvas->setSelectBrushMode(on);
    if (on) m_canvas->setFocus();
}

void RetouchTab::setSelectBrushRadius(double normRadius) {
    m_canvas->setSelectBrushRadius(normRadius);
}

void RetouchTab::clearActiveSelection() {
    m_canvas->clearActiveSelection();
}

void RetouchTab::setCloneMode(bool on) {
    m_canvas->setCloneMode(on);
    if (on) m_canvas->setFocus();
}

// Mirrors ImageCanvas's selection so tool handlers below (paint/erase/bucket)
// can clip their writes to it without round-tripping through the canvas.
void RetouchTab::onSelectionPathChanged(const QPainterPath &pathNorm, bool hasSelection) {
    m_selectionPath = pathNorm;
    m_hasSelection = hasSelection;
}

void RetouchTab::onSelectionFeatherChanged(double normRadius) {
    m_selectionFeatherNorm = normRadius;
}

void RetouchTab::setSelectionFeather(double normRadius) {
    m_canvas->setSelectionFeather(normRadius);
}

void RetouchTab::setEraseBrush(int radiusDisplayPx) {
    m_eraseRadiusDisplay = radiusDisplayPx;
    m_canvas->setBrushRadius(radiusDisplayPx);
}

void RetouchTab::setRemoveObjectMode(bool on) {
    m_canvas->setRemoveObjectMode(on);
    if (!on) {
        m_removeObjectDragging = false;
        m_removeObjectStroke.clear();
        m_removeObjectMaskDraft = QImage();
    }
    if (on) m_canvas->setFocus();
}

void RetouchTab::setRemoveObjectBrush(int radiusDisplayPx) {
    m_removeObjectRadiusDisplay = radiusDisplayPx;
    m_canvas->setBrushRadius(radiusDisplayPx);
}

void RetouchTab::selectRemoval(int index) {
    m_activeRemoval = (index >= 0 && index < m_adj.removals.size()) ? index : -1;
    updateRemovalMarkers();
    emit removalsChanged();
}

void RetouchTab::setRemovalVisible(int index, bool visible) {
    if (index < 0 || index >= m_adj.removals.size()) return;
    if (m_adj.removals[index].visible == visible) return;
    m_adj.removals[index].visible = visible;
    rebuildGeom();
    markEdited();
    emit removalsChanged();
}

void RetouchTab::deleteRemoval(int index) {
    if (index < 0 || index >= m_adj.removals.size()) return;
    m_adj.removals.removeAt(index);
    if (m_activeRemoval == index) m_activeRemoval = -1;
    else if (m_activeRemoval > index) --m_activeRemoval;
    rebuildGeom();
    markEdited();
    emit removalsChanged();
}


void RetouchTab::setTextMode(bool on) {
    m_textMode = on;
    m_canvas->setTextMode(on);
    if (on) m_canvas->setFocus();
    else m_activeText = -1;
}

// A click placed a new text op (point in display-image coords, pre-crop
// stored per TextOp convention). Opens the inline editor immediately;
// committing empty text (or cancelling) discards the draft.
// Places a new text box: creates a brand-new Mask (MaskType::TextBox, always
// via addMask() — see class comment) seeded from m_textDefaults, then opens
// the inline editor on it immediately; committing empty text (or cancelling)
// discards the draft. Mirrors onShapeCreateRequested's addMask() usage.
void RetouchTab::onTextPlaceRequested(const QPoint &imgPoint) {
    if (m_scaleFromGeom <= 0) return;
    double inv = 1.0 / m_scaleFromGeom;
    int mi = addMask(MaskType::TextBox);
    Mask &mk = m_adj.masks[mi];
    mk.textBoxPos = m_geomToOriented.map(QPointF(imgPoint.x() * inv, imgPoint.y() * inv));
    mk.textBoxRotation = m_textDefaults.rotation;
    mk.textBoxText.clear();
    mk.textBoxFamily = m_textDefaults.family;
    mk.textBoxPixelSize = m_textDefaults.pixelSize;
    mk.textBoxBold = m_textDefaults.bold;
    mk.textBoxItalic = m_textDefaults.italic;
    mk.textBoxColor = m_textDefaults.color;
    mk.textBoxOutlineEnabled = m_textDefaults.outlineEnabled;
    mk.textBoxOutlineColor = m_textDefaults.outlineColor;
    mk.textBoxOutlineWidth = m_textDefaults.outlineWidth;
    mk.textBoxShadowEnabled = m_textDefaults.shadowEnabled;
    mk.textBoxShadowOffset = m_textDefaults.shadowOffset;
    mk.textBoxShadowBlur = m_textDefaults.shadowBlur;
    mk.textBoxShadowOpacity = m_textDefaults.shadowOpacity;
    mk.textBoxShadowColor = m_textDefaults.shadowColor;
    mk.textBoxBgEnabled = m_textDefaults.bgEnabled;
    mk.textBoxBgColor = m_textDefaults.bgColor;
    mk.textBoxBgOpacity = m_textDefaults.bgOpacity;
    mk.textBoxBgPadding = m_textDefaults.bgPadding;
    updateTextMarkers();
    // addMask() always inserts at masks index 0 (front of stack), i.e. the
    // last entry in the TextBox-filtered marker list built by
    // updateTextMarkers() (which walks masks front-to-back).
    m_activeText = m_textMaskIndices.indexOf(mi);
    m_newTextIndex = m_activeText;
    m_textEditIndex = m_activeText;
    updateTextMarkers();
    retone(); // suppress the (empty) baked-in text while the editor owns it

    QFont font(mk.textBoxFamily);
    font.setPixelSize(std::max(1, int(std::lround(mk.textBoxPixelSize * m_scaleFromGeom))));
    font.setBold(mk.textBoxBold);
    font.setItalic(mk.textBoxItalic);
    m_canvas->beginTextEdit(m_activeText, QPointF(imgPoint), font, mk.textBoxColor, QString());
    emit textsChanged();
}

void RetouchTab::onTextSelected(int index) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    m_activeText = index;
    m_newTextIndex = -1;
    m_canvas->setActiveTextIndex(index);
    // See selectShape()'s comment: keep m_activeMask in sync with
    // canvas-driven selection so the Layers panel highlight (and generic
    // per-mask edits) follow whichever text box was actually just selected.
    m_activeMask = mi;
    pushMaskGizmo();
    emit textsChanged();
}

void RetouchTab::onTextDeselected() {
    m_activeText = -1;
    emit textsChanged();
}

void RetouchTab::onTextMoved(int index, const QPointF &newImgPos) {
    int mi = textMaskIndex(index);
    if (mi < 0 || m_scaleFromGeom <= 0) return;
    double inv = 1.0 / m_scaleFromGeom;
    m_adj.masks[mi].textBoxPos =
        m_geomToOriented.map(QPointF(newImgPos.x() * inv, newImgPos.y() * inv));
    updateTextMarkers();
    retone();
    markEdited();
}

void RetouchTab::onTextRotated(int index, double newRotationDegrees) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    m_adj.masks[mi].textBoxRotation = newRotationDegrees - m_geomRotationDeg;
    updateTextMarkers();
    retone();
    markEdited();
}

void RetouchTab::onTextEditRequested(int index) {
    int mi = textMaskIndex(index);
    if (mi < 0 || m_scaleFromGeom <= 0) return;
    m_activeText = index;
    m_newTextIndex = -1;
    m_textEditIndex = index;
    m_activeMask = mi; // see selectShape()'s comment: keep selection in sync
    const Mask &mk = m_adj.masks[mi];
    QPointF displayPos = m_orientedToGeom.map(mk.textBoxPos) * m_scaleFromGeom;
    QFont font(mk.textBoxFamily);
    font.setPixelSize(std::max(1, int(std::lround(mk.textBoxPixelSize * m_scaleFromGeom))));
    font.setBold(mk.textBoxBold);
    font.setItalic(mk.textBoxItalic);
    m_canvas->beginTextEdit(index, displayPos, font, mk.textBoxColor, mk.textBoxText);
    retone(); // suppress the stale baked-in text while the editor owns it
}

void RetouchTab::onTextEditCommitted(int index, const QString &text) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    bool wasNewEmptyDraft = (index == m_newTextIndex) && m_adj.masks[mi].textBoxText.isEmpty();
    m_newTextIndex = -1;
    if (m_textEditIndex == index) m_textEditIndex = -1;
    if (text.trimmed().isEmpty()) {
        m_adj.masks.removeAt(mi);
        if (m_paintMoveMasterIndex == mi) m_paintMoveMasterIndex = -1;
        else if (m_paintMoveMasterIndex > mi) --m_paintMoveMasterIndex;
        if (m_activeMask == mi) m_activeMask = -1;
        else if (m_activeMask > mi) --m_activeMask;
        if (m_activeText == index) m_activeText = -1;
        else if (m_activeText > index) --m_activeText;
        updateTextMarkers();
        retone();
        if (!wasNewEmptyDraft) markEdited(); // clearing existing text is itself an edit
        return;
    }
    m_adj.masks[mi].textBoxText = text;
    updateTextMarkers();
    retone();
    markEdited();
}

void RetouchTab::onTextEditCancelled(int index) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    if (m_textEditIndex == index) m_textEditIndex = -1;
    if (index == m_newTextIndex && m_adj.masks[mi].textBoxText.isEmpty()) {
        m_adj.masks.removeAt(mi);
        if (m_paintMoveMasterIndex == mi) m_paintMoveMasterIndex = -1;
        else if (m_paintMoveMasterIndex > mi) --m_paintMoveMasterIndex;
        if (m_activeMask == mi) m_activeMask = -1;
        else if (m_activeMask > mi) --m_activeMask;
        if (m_activeText == index) m_activeText = -1;
        else if (m_activeText > index) --m_activeText;
        m_newTextIndex = -1;
        updateTextMarkers();
        retone();
    } else {
        retone(); // re-composite the (unchanged) baked-in text now that the editor is gone
    }
}

// Live-updates the op's text as the user types (before committing), so
// shadow/outline/background style controls preview correctly while the
// editor is still open — see onRenderDone's fill-only suppression.
void RetouchTab::onTextLiveContentChanged(int index, const QString &text) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    m_adj.masks[mi].textBoxText = text;
    updateTextMarkers();
    retone();
}

void RetouchTab::onTextDeleteRequested(int index) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    m_adj.masks.removeAt(mi);
    if (m_paintMoveMasterIndex == mi) m_paintMoveMasterIndex = -1;
    else if (m_paintMoveMasterIndex > mi) --m_paintMoveMasterIndex;
    if (m_activeMask == mi) m_activeMask = -1;
    else if (m_activeMask > mi) --m_activeMask;
    if (m_activeText == index) m_activeText = -1;
    else if (m_activeText > index) --m_activeText;
    if (m_newTextIndex == index) m_newTextIndex = -1;
    updateTextMarkers();
    retone();
    markEdited();
}

void RetouchTab::onTextResizeStarted(int index) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    m_textResizeStartPixelSize = m_adj.masks[mi].textBoxPixelSize;
}

// Corner-drag resize: uniformly scales font size relative to its value when
// the drag began (`ratio` is cumulative from drag start, not incremental).
void RetouchTab::onTextResized(int index, double ratio) {
    int mi = textMaskIndex(index);
    if (mi < 0) return;
    m_adj.masks[mi].textBoxPixelSize = std::clamp(m_textResizeStartPixelSize * ratio, 1.0, 2000.0);
    updateTextMarkers();
    retone();
    markEdited();
    emit textsChanged();
}

void RetouchTab::deleteActiveText() {
    if (m_activeText >= 0) onTextDeleteRequested(m_activeText);
}

TextOp RetouchTab::activeTextStyle() const {
    int mi = textMaskIndex(m_activeText);
    if (mi < 0) return m_textDefaults;
    const Mask &mk = m_adj.masks[mi];
    TextOp op;
    op.pos = mk.textBoxPos;
    op.rotation = mk.textBoxRotation;
    op.text = mk.textBoxText;
    op.family = mk.textBoxFamily;
    op.pixelSize = mk.textBoxPixelSize;
    op.bold = mk.textBoxBold;
    op.italic = mk.textBoxItalic;
    op.color = mk.textBoxColor;
    op.outlineEnabled = mk.textBoxOutlineEnabled;
    op.outlineColor = mk.textBoxOutlineColor;
    op.outlineWidth = mk.textBoxOutlineWidth;
    op.shadowEnabled = mk.textBoxShadowEnabled;
    op.shadowOffset = mk.textBoxShadowOffset;
    op.shadowBlur = mk.textBoxShadowBlur;
    op.shadowOpacity = mk.textBoxShadowOpacity;
    op.shadowColor = mk.textBoxShadowColor;
    op.bgEnabled = mk.textBoxBgEnabled;
    op.bgColor = mk.textBoxBgColor;
    op.bgOpacity = mk.textBoxBgOpacity;
    op.bgPadding = mk.textBoxBgPadding;
    return op;
}

void RetouchTab::setTextFont(const QString &family, double pixelSize, bool bold, bool italic) {
    int mi = textMaskIndex(m_activeText);
    if (mi >= 0) {
        Mask &mk = m_adj.masks[mi];
        mk.textBoxFamily = family;
        mk.textBoxPixelSize = pixelSize;
        mk.textBoxBold = bold;
        mk.textBoxItalic = italic;
        updateTextMarkers();
        retone();
        markEdited();
    } else {
        m_textDefaults.family = family;
        m_textDefaults.pixelSize = pixelSize;
        m_textDefaults.bold = bold;
        m_textDefaults.italic = italic;
    }
    emit textsChanged();
}

void RetouchTab::setTextColor(const QColor &color) {
    int mi = textMaskIndex(m_activeText);
    if (mi >= 0) {
        m_adj.masks[mi].textBoxColor = color;
        retone();
        markEdited();
    } else {
        m_textDefaults.color = color;
    }
    emit textsChanged();
}

void RetouchTab::setTextOutline(bool enabled, const QColor &color, double width) {
    int mi = textMaskIndex(m_activeText);
    if (mi >= 0) {
        Mask &mk = m_adj.masks[mi];
        mk.textBoxOutlineEnabled = enabled;
        mk.textBoxOutlineColor = color;
        mk.textBoxOutlineWidth = width;
        retone();
        markEdited();
    } else {
        m_textDefaults.outlineEnabled = enabled;
        m_textDefaults.outlineColor = color;
        m_textDefaults.outlineWidth = width;
    }
    emit textsChanged();
}

void RetouchTab::setTextShadow(bool enabled, const QPointF &offset, double blur, double opacity,
                               const QColor &color) {
    int mi = textMaskIndex(m_activeText);
    if (mi >= 0) {
        Mask &mk = m_adj.masks[mi];
        mk.textBoxShadowEnabled = enabled;
        mk.textBoxShadowOffset = offset;
        mk.textBoxShadowBlur = blur;
        mk.textBoxShadowOpacity = opacity;
        mk.textBoxShadowColor = color;
        retone();
        markEdited();
    } else {
        m_textDefaults.shadowEnabled = enabled;
        m_textDefaults.shadowOffset = offset;
        m_textDefaults.shadowBlur = blur;
        m_textDefaults.shadowOpacity = opacity;
        m_textDefaults.shadowColor = color;
    }
    emit textsChanged();
}

void RetouchTab::setTextBackground(bool enabled, const QColor &color, double opacity,
                                   double padding) {
    int mi = textMaskIndex(m_activeText);
    if (mi >= 0) {
        Mask &mk = m_adj.masks[mi];
        mk.textBoxBgEnabled = enabled;
        mk.textBoxBgColor = color;
        mk.textBoxBgOpacity = opacity;
        mk.textBoxBgPadding = padding;
        retone();
        markEdited();
    } else {
        m_textDefaults.bgEnabled = enabled;
        m_textDefaults.bgColor = color;
        m_textDefaults.bgOpacity = opacity;
        m_textDefaults.bgPadding = padding;
    }
    emit textsChanged();
}

// ---- Shape tool -------------------------------------------------------

void RetouchTab::setShapeMode(bool on) {
    m_shapeMode = on;
    m_canvas->setShapeMode(on);
    if (on) m_canvas->setFocus();
    else { m_activeShape = -1; m_selectedShapes.clear(); }
}

void RetouchTab::setMoveMode(bool on) {
    m_canvas->setMoveMode(on);
    if (on) m_canvas->setFocus();
}

// Move tool drag start on a Paint/Brush layer: select its mask and snapshot
// its stroke/fill so onPaintLayerMoveDelta can reapply the drag's total
// offset (rather than compounding per-event), mirroring shapeMoved.
void RetouchTab::onPaintLayerMoveStarted(int markerIndex) {
    // If a Paint layer is already selected in the Layers panel, the Move
    // tool always drags that layer, regardless of where on the canvas the
    // click landed -- ignore the spatially hit-tested markerIndex (whose
    // bounds can be misleading, e.g. a bucket-filled layer's hit box covers
    // the whole canvas). Only fall back to the spatial hit when nothing
    // appropriate is currently selected, preserving click-to-select.
    int maskIndex = -1;
    if (m_activeMask >= 0 && m_activeMask < m_adj.masks.size() &&
        m_adj.masks[m_activeMask].type == MaskType::Paint) {
        maskIndex = m_activeMask;
    } else {
        if (markerIndex < 0 || markerIndex >= m_paintMaskIndices.size()) return;
        maskIndex = m_paintMaskIndices[markerIndex];
    }
    if (maskIndex < 0 || maskIndex >= m_adj.masks.size()) return;
    selectMask(maskIndex);
    m_paintMoveMaskIndex = maskIndex;
    const Mask &m = m_adj.masks[maskIndex];
    m_paintMoveStartStroke = m.stroke;
    m_paintMoveStartFillMask = m.fillMask;
    m_paintMoveSelectionOnly = m_canvas->hasActiveSelection();
    m_paintMoveSelectionPath = m_canvas->selectionPathNorm();
    // First move of this mask this session (or cache was invalidated by a
    // fresh fill/reorder/delete elsewhere): snapshot the current fillMask as
    // the untouched master so subsequent moves never compound edge clipping.
    if (m_paintMoveMasterIndex != maskIndex) {
        m_paintMoveMasterIndex = maskIndex;
        m_paintMoveMasterFillMask = m.fillMask;
        m_paintMoveMasterOffsetNorm = QPointF();
    }
    m_paintMoveLastDelta = QPointF();
}

void RetouchTab::onPaintLayerMoveDelta(const QPointF &deltaNorm) {
    if (m_paintMoveMaskIndex < 0 || m_paintMoveMaskIndex >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_paintMoveMaskIndex];
    m.stroke = m_paintMoveStartStroke;
    for (BrushStrokePoint &p : m.stroke) {
        if (!m_paintMoveSelectionOnly || m_paintMoveSelectionPath.contains(p.pt))
            p.pt += deltaNorm;
    }
    // Bucket-fill coverage only moves along when the whole layer moves — a
    // selection-clipped partial move leaves it in place, since splitting the
    // fill raster by the selection path isn't worth the complexity here.
    if (!m_paintMoveSelectionOnly && !m_paintMoveMasterFillMask.isNull() &&
        m_paintMoveMasterIndex == m_paintMoveMaskIndex) {
        m_paintMoveLastDelta = deltaNorm;
        const QPointF totalNorm = m_paintMoveMasterOffsetNorm + deltaNorm;
        QImage translated(m_paintMoveMasterFillMask.size(), m_paintMoveMasterFillMask.format());
        translated.fill(Qt::transparent);
        QPainter tp(&translated);
        const double dxPx = totalNorm.x() * m_paintMoveMasterFillMask.width();
        const double dyPx = totalNorm.y() * m_paintMoveMasterFillMask.height();
        tp.drawImage(QPointF(dxPx, dyPx), m_paintMoveMasterFillMask);
        tp.end();
        m.fillMask = translated;
    }
    retone();
    updateObjectMarkers();
}

void RetouchTab::onPaintLayerMoveFinished() {
    if (m_paintMoveMaskIndex >= 0) markEdited();
    if (m_paintMoveMasterIndex == m_paintMoveMaskIndex)
        m_paintMoveMasterOffsetNorm += m_paintMoveLastDelta;
    m_paintMoveMaskIndex = -1;
    m_paintMoveStartStroke.clear();
    m_paintMoveStartFillMask = QImage();
    m_paintMoveLastDelta = QPointF();
}

void RetouchTab::setActiveShapeType(ShapeType t) {
    m_canvas->setActiveShapeType(t);
    int mi = shapeMaskIndex(m_activeShape);
    if (mi >= 0) {
        m_adj.masks[mi].shapeType = t;
        updateShapeMarkers();
        retone();
        markEdited();
    } else {
        m_shapeDefaults.type = t;
    }
    emit shapesChanged();
}

// A drag-create gesture finished (bounding box in display-image coords,
// pre-crop stored per ShapeOp convention).
void RetouchTab::onShapeCreateRequested(ShapeType type, const QRectF &imageRect) {
    if (m_scaleFromGeom <= 0) return;
    double inv = 1.0 / m_scaleFromGeom;
    // addMask() inserts a brand-new Mask (MaskType::Shape) at masks index 0
    // and points m_activeMask at it; seed its shape* fields from
    // m_shapeDefaults (mirroring the old ShapeOp-from-m_shapeDefaults seed)
    // then overwrite with this drag's final geometry.
    int mi = addMask(MaskType::Shape, type);
    Mask &mk = m_adj.masks[mi];
    mk.shapeType = type;
    mk.shapeSides = m_shapeDefaults.sides;
    mk.shapeInnerRadiusRatio = m_shapeDefaults.innerRadiusRatio;
    mk.shapeFillEnabled = m_shapeDefaults.fillEnabled;
    mk.shapeFillColor = m_shapeDefaults.fillColor;
    mk.shapeStrokeEnabled = m_shapeDefaults.strokeEnabled;
    mk.shapeStrokeColor = m_shapeDefaults.strokeColor;
    mk.shapeStrokeWidth = m_shapeDefaults.strokeWidth;
    mk.opacity = m_shapeDefaults.opacity;
    mk.visible = m_shapeDefaults.visible;
    if (type == ShapeType::Line) {
        mk.shapeP1 = m_geomToOriented.map(imageRect.topLeft() * inv);
        mk.shapeP2 = m_geomToOriented.map(imageRect.bottomRight() * inv);
    } else {
        QPointF center = m_geomToOriented.map(imageRect.center() * inv);
        QSizeF size = imageRect.size() * inv;
        mk.shapeRect = QRectF(center - QPointF(size.width() / 2.0, size.height() / 2.0), size);
        // Drawn axis-aligned on screen; shapeRotation + m_geomRotationDeg
        // must net to 0 so it still shows axis-aligned (see updateShapeMarkers).
        mk.shapeRotation = -m_geomRotationDeg;
    }
    updateShapeMarkers();
    // The new mask is always at masks index 0 (front of stack), i.e. the
    // last entry in the Shape-filtered marker list built by
    // updateShapeMarkers() (which walks masks front-to-back).
    m_activeShape = m_shapeMaskIndices.indexOf(mi);
    m_selectedShapes = (m_activeShape >= 0) ? QSet<int>{m_activeShape} : QSet<int>{};
    updateShapeMarkers();
    retone();
    markEdited();
    emit shapesChanged();
}

// Plain click on a shape not already part of a multi-selection (from the
// canvas): selects it (or its whole group, see selectShape) and captures
// move-drag start geometry for the clicked shape specifically, since
// ImageCanvas re-checks the (possibly just-expanded) selection right after
// this returns and may switch the in-progress drag to a group move instead.
void RetouchTab::onShapeSelected(int index) {
    int mi = shapeMaskIndex(index);
    if (mi < 0) return;
    selectShape(index);
    const Mask &mk = m_adj.masks[mi];
    m_shapeMoveStartRect = mk.shapeRect;
    m_shapeMoveStartP1 = mk.shapeP1;
    m_shapeMoveStartP2 = mk.shapeP2;
}

// Select a shape — from the canvas or the Layers panel. If it belongs to a
// group, every shape sharing that groupId is selected too (grouped shapes
// always act as one unit), matching Illustrator/Photoshop group semantics.
// `index` is a marker index (position within the Shape-filtered view), not
// a raw m_adj.masks index — see m_shapeMaskIndices.
void RetouchTab::selectShape(int index) {
    int mi = shapeMaskIndex(index);
    if (mi < 0) return;
    const QString groupId = m_adj.masks[mi].groupId;
    if (groupId.isEmpty()) {
        m_selectedShapes = {index};
    } else {
        m_selectedShapes.clear();
        for (int mkIdx = 0; mkIdx < m_shapeMaskIndices.size(); ++mkIdx)
            if (m_adj.masks[m_shapeMaskIndices[mkIdx]].groupId == groupId)
                m_selectedShapes.insert(mkIdx);
    }
    m_activeShape = index;
    // Keep m_activeMask (the Layers panel's highlighted row and the target
    // of generic mask edits like opacity/blend/visibility) in lockstep with
    // canvas-driven shape selection -- without this, clicking a shape on
    // the canvas moved the shape gizmo/selection handles but left the
    // Layers panel highlighting whatever mask was last active via
    // selectMask()/addMask(), a stale-selection desync.
    m_activeMask = mi;
    updateShapeMarkers();
    pushMaskGizmo();
    const Mask &mk = m_adj.masks[mi];
    QPointF center;
    if (mk.shapeType == ShapeType::Line) {
        QPointF p1 = m_orientedToGeom.map(mk.shapeP1) * m_scaleFromGeom;
        QPointF p2 = m_orientedToGeom.map(mk.shapeP2) * m_scaleFromGeom;
        center = (p1 + p2) / 2.0;
    } else {
        center = m_orientedToGeom.map(mk.shapeRect.center()) * m_scaleFromGeom;
    }
    m_canvas->centerOnImagePoint(center);
    emit shapesChanged();
}

void RetouchTab::onShapeDeselected() {
    m_activeShape = -1;
    m_selectedShapes.clear();
    updateShapeMarkers();
    emit shapesChanged();
}

// Ctrl+click (no drag) on a shape: toggle its multi-selection membership
// without disturbing the rest of the selection.
void RetouchTab::onShapeToggleSelectRequested(int index) {
    if (index < 0 || index >= m_shapeMaskIndices.size()) return;
    if (m_selectedShapes.contains(index)) {
        m_selectedShapes.remove(index);
        if (m_activeShape == index)
            m_activeShape = m_selectedShapes.isEmpty() ? -1 : *m_selectedShapes.constBegin();
    } else {
        m_selectedShapes.insert(index);
        m_activeShape = index;
    }
    updateShapeMarkers();
    emit shapesChanged();
}

// Press on a shape that's already part of a >1-member selection: capture
// every member's current geometry so the upcoming shapeGroupMoveRequested
// deltas (cumulative from this drag's start) can be applied as absolute
// offsets rather than compounding across move events.
// Shared start-capture for both a group move and a group resize (also used
// via shapeGroupResizeStarted — resize needs the same per-shape reference
// geometry, plus stroke width so it can scale proportionally too).
void RetouchTab::onShapeGroupMoveStarted(const QList<int> &indices) {
    m_shapeGroupStartRect.clear();
    m_shapeGroupStartP1.clear();
    m_shapeGroupStartP2.clear();
    m_shapeGroupStartStrokeWidth.clear();
    for (int idx : indices) {
        int mi = shapeMaskIndex(idx);
        if (mi < 0) continue;
        const Mask &mk = m_adj.masks[mi];
        m_shapeGroupStartRect[idx] = mk.shapeRect;
        m_shapeGroupStartP1[idx] = mk.shapeP1;
        m_shapeGroupStartP2[idx] = mk.shapeP2;
        m_shapeGroupStartStrokeWidth[idx] = mk.shapeStrokeWidth;
    }
}

void RetouchTab::onShapeGroupMoveRequested(const QList<int> &indices, const QPointF &deltaImage) {
    if (m_scaleFromGeom <= 0) return;
    QPointF delta = orientedDelta(deltaImage / m_scaleFromGeom);
    for (int idx : indices) {
        int mi = shapeMaskIndex(idx);
        if (mi < 0 || !m_shapeGroupStartRect.contains(idx)) continue;
        Mask &mk = m_adj.masks[mi];
        if (mk.shapeType == ShapeType::Line) {
            mk.shapeP1 = m_shapeGroupStartP1[idx] + delta;
            mk.shapeP2 = m_shapeGroupStartP2[idx] + delta;
        } else {
            mk.shapeRect = m_shapeGroupStartRect[idx].translated(delta);
        }
    }
    updateShapeMarkers();
    retone();
    markEdited();
}

// `scaleX`/`scaleY` are absolute factors relative to the group's combined
// bounding box at drag start (see shapeGroupResizeStarted → onShapeGroupMoveStarted),
// not incremental — every selected shape's start geometry is scaled about
// the fixed `anchorImage` corner (display-image space, same convention as
// onShapeResized's newImageRect) each call, so results don't compound across
// move events.
void RetouchTab::onShapeGroupResizeRequested(const QList<int> &indices, const QPointF &anchorImage,
                                             double scaleX, double scaleY) {
    if (m_scaleFromGeom <= 0) return;
    double inv = 1.0 / m_scaleFromGeom;
    // anchorImage/scaleX/scaleY are defined in display (geom-local) space —
    // do the actual scaling there, converting each point in and back out via
    // the oriented<->geom affine, so a straightened crop's tilt doesn't skew
    // the scale axes.
    QPointF anchor = anchorImage * inv; // already geom-local (display, unscaled)
    double strokeScale = std::sqrt(std::abs(scaleX * scaleY));
    auto scalePoint = [&](const QPointF &pOriented) {
        QPointF p = m_orientedToGeom.map(pOriented);
        QPointF scaled(anchor.x() + (p.x() - anchor.x()) * scaleX,
                       anchor.y() + (p.y() - anchor.y()) * scaleY);
        return m_geomToOriented.map(scaled);
    };
    for (int idx : indices) {
        int mi = shapeMaskIndex(idx);
        if (mi < 0 || !m_shapeGroupStartRect.contains(idx)) continue;
        Mask &mk = m_adj.masks[mi];
        if (mk.shapeType == ShapeType::Line) {
            mk.shapeP1 = scalePoint(m_shapeGroupStartP1[idx]);
            mk.shapeP2 = scalePoint(m_shapeGroupStartP2[idx]);
        } else {
            QRectF startRect = m_shapeGroupStartRect[idx];
            mk.shapeRect = QRectF(scalePoint(startRect.topLeft()), scalePoint(startRect.bottomRight()))
                          .normalized();
        }
        mk.shapeStrokeWidth = std::max(0.0, m_shapeGroupStartStrokeWidth.value(idx, mk.shapeStrokeWidth) * strokeScale);
    }
    updateShapeMarkers();
    retone();
    markEdited();
}

// `deltaImage` is the total offset from the drag's press point (display-image
// coords), not an incremental step — RetouchTab must apply it against the
// shape's position as of the drag start (captured in onShapeSelected), not
// accumulate it onto the shape's current (already-moved) position.
void RetouchTab::onShapeMoved(int index, const QPointF &deltaImage) {
    int mi = shapeMaskIndex(index);
    if (mi < 0 || m_scaleFromGeom <= 0) return;
    QPointF delta = orientedDelta(deltaImage / m_scaleFromGeom);
    Mask &mk = m_adj.masks[mi];
    if (mk.shapeType == ShapeType::Line) {
        mk.shapeP1 = m_shapeMoveStartP1 + delta;
        mk.shapeP2 = m_shapeMoveStartP2 + delta;
    } else {
        mk.shapeRect = m_shapeMoveStartRect.translated(delta);
    }
    updateShapeMarkers();
    retone();
    markEdited();
}

void RetouchTab::onShapeResized(int index, const QRectF &newImageRect) {
    int mi = shapeMaskIndex(index);
    if (mi < 0 || m_scaleFromGeom <= 0) return;
    double inv = 1.0 / m_scaleFromGeom;
    // newImageRect is unrotated local (marker.rotation is applied separately
    // by the renderer about its center) — map the center through the full
    // affine and keep the (unscaled) local size, mirroring updateShapeMarkers.
    QPointF center = m_geomToOriented.map(newImageRect.center() * inv);
    QSizeF size = newImageRect.size() * inv;
    m_adj.masks[mi].shapeRect = QRectF(center - QPointF(size.width() / 2.0, size.height() / 2.0), size);
    updateShapeMarkers();
    retone();
    markEdited();
}

void RetouchTab::onShapeLineEndpointsChanged(int index, const QPointF &p1, const QPointF &p2) {
    int mi = shapeMaskIndex(index);
    if (mi < 0 || m_scaleFromGeom <= 0) return;
    double inv = 1.0 / m_scaleFromGeom;
    m_adj.masks[mi].shapeP1 = m_geomToOriented.map(p1 * inv);
    m_adj.masks[mi].shapeP2 = m_geomToOriented.map(p2 * inv);
    updateShapeMarkers();
    retone();
    markEdited();
}

void RetouchTab::onShapeRotated(int index, double newRotationDegrees) {
    int mi = shapeMaskIndex(index);
    if (mi < 0) return;
    // The rotate handle isn't offered for Line (see shapeRotateHandlePos's
    // type check), so only non-Line markers ever carry +m_geomRotationDeg
    // baked into the dragged value — matches updateShapeMarkers.
    bool isLine = m_adj.masks[mi].shapeType == ShapeType::Line;
    m_adj.masks[mi].shapeRotation = newRotationDegrees - (isLine ? 0.0 : m_geomRotationDeg);
    updateShapeMarkers();
    retone();
    markEdited();
}

void RetouchTab::onShapeDeleteRequested(int index) {
    int mi = shapeMaskIndex(index);
    if (mi < 0) return;
    m_adj.masks.removeAt(mi);
    if (m_paintMoveMasterIndex == mi) m_paintMoveMasterIndex = -1;
    else if (m_paintMoveMasterIndex > mi) --m_paintMoveMasterIndex;
    if (m_activeMask == mi) m_activeMask = -1;
    else if (m_activeMask > mi) --m_activeMask;
    if (m_activeShape == index) m_activeShape = -1;
    else if (m_activeShape > index) --m_activeShape;
    QSet<int> reselected;
    for (int idx : m_selectedShapes) {
        if (idx == index) continue;
        reselected.insert(idx > index ? idx - 1 : idx);
    }
    m_selectedShapes = reselected;
    updateShapeMarkers();
    retone();
    markEdited();
    emit shapesChanged();
}

// Delete/Backspace with more than one shape selected: remove every selected
// shape in one step (highest index first, so earlier removals don't shift
// the indices of shapes still to be removed).
void RetouchTab::onShapeGroupDeleteRequested(const QList<int> &indices) {
    QList<int> maskIndices;
    for (int idx : indices) {
        int mi = shapeMaskIndex(idx);
        if (mi >= 0) maskIndices.append(mi);
    }
    std::sort(maskIndices.begin(), maskIndices.end(), std::greater<int>());
    for (int mi : maskIndices) {
        m_adj.masks.removeAt(mi);
        if (m_paintMoveMasterIndex == mi) m_paintMoveMasterIndex = -1;
        else if (m_paintMoveMasterIndex > mi) --m_paintMoveMasterIndex;
        if (m_activeMask == mi) m_activeMask = -1;
        else if (m_activeMask > mi) --m_activeMask;
    }
    m_activeShape = -1;
    m_selectedShapes.clear();
    updateShapeMarkers();
    retone();
    markEdited();
    emit shapesChanged();
}

// Ctrl+drag on an existing shape: append an exact copy (same geometry and
// style) and select it, so ImageCanvas's already-in-progress move drag
// continues by dragging the copy away from the untouched original.
void RetouchTab::onShapeDuplicateRequested(int index) {
    int mi = shapeMaskIndex(index);
    if (mi < 0) return;
    Mask copy = m_adj.masks[mi];
    copy.groupId.clear(); // a lone duplicate leaves its group, even if the original had one
    int insertAt = mi + 1;
    m_adj.masks.insert(insertAt, copy);
    if (m_activeMask >= insertAt) ++m_activeMask;
    updateShapeMarkers();
    int newIndex = m_shapeMaskIndices.indexOf(insertAt);
    m_activeShape = newIndex;
    m_selectedShapes = (newIndex >= 0) ? QSet<int>{newIndex} : QSet<int>{};
    m_shapeMoveStartRect = copy.shapeRect;
    m_shapeMoveStartP1 = copy.shapeP1;
    m_shapeMoveStartP2 = copy.shapeP2;
    updateShapeMarkers();
    retone();
    markEdited();
    emit shapesChanged();
}

// Ctrl+drag with a multi-selection: append an exact copy of every selected
// shape, select the copies as the new group, and capture their (identical
// to the originals) start geometry so the in-progress group-move drag
// continues by dragging the copies away — the originals are left untouched.
void RetouchTab::onShapeGroupDuplicateRequested(const QList<int> &indices) {
    m_shapeGroupStartRect.clear();
    m_shapeGroupStartP1.clear();
    m_shapeGroupStartP2.clear();

    // If every duplicated shape shares the same (non-empty) group, the
    // copies form their own new group too, preserving that structure —
    // otherwise (an ad-hoc multi-selection) the copies stay ungrouped.
    int firstMi = indices.isEmpty() ? -1 : shapeMaskIndex(indices.first());
    QString sourceGroupId = firstMi < 0 ? QString() : m_adj.masks[firstMi].groupId;
    bool sameGroup = !sourceGroupId.isEmpty();
    for (int idx : indices) {
        int mi = shapeMaskIndex(idx);
        if (mi < 0 || m_adj.masks[mi].groupId != sourceGroupId) sameGroup = false;
    }
    QString newGroupId = sameGroup ? QUuid::createUuid().toString() : QString();

    // Duplicate each source mask directly above its original, highest masks
    // index first so earlier inserts don't shift the masks indices still to
    // be processed.
    QList<int> maskIndices;
    for (int idx : indices) {
        int mi = shapeMaskIndex(idx);
        if (mi >= 0) maskIndices.append(mi);
    }
    QList<int> sortedDesc = maskIndices;
    std::sort(sortedDesc.begin(), sortedDesc.end(), std::greater<int>());
    QVector<int> newMaskIndices;
    for (int mi : sortedDesc) {
        Mask copy = m_adj.masks[mi];
        copy.groupId = newGroupId;
        int insertAt = mi + 1;
        m_adj.masks.insert(insertAt, copy);
        newMaskIndices.append(insertAt);
        if (m_activeMask >= insertAt) ++m_activeMask;
    }
    updateShapeMarkers();
    QList<int> newIndices;
    for (int mi : newMaskIndices) {
        int newIdx = m_shapeMaskIndices.indexOf(mi);
        if (newIdx < 0) continue;
        newIndices.append(newIdx);
        const Mask &mk = m_adj.masks[mi];
        m_shapeGroupStartRect[newIdx] = mk.shapeRect;
        m_shapeGroupStartP1[newIdx] = mk.shapeP1;
        m_shapeGroupStartP2[newIdx] = mk.shapeP2;
    }
    m_selectedShapes = QSet<int>(newIndices.begin(), newIndices.end());
    m_activeShape = newIndices.isEmpty() ? -1 : newIndices.last();
    updateShapeMarkers();
    retone();
    markEdited();
    emit shapesChanged();
}

void RetouchTab::deleteActiveShape() {
    if (m_selectedShapes.size() > 1) onShapeGroupDeleteRequested(m_selectedShapes.values());
    else if (m_activeShape >= 0) onShapeDeleteRequested(m_activeShape);
}

void RetouchTab::setShapeVisible(int index, bool visible) {
    int mi = shapeMaskIndex(index);
    if (mi < 0) return;
    m_adj.masks[mi].visible = visible;
    retone();
    markEdited();
    emit shapesChanged();
}

// Tags the current multi-selection as one group and moves its members to be
// contiguous in the stack (at the position of the topmost/frontmost member,
// so grouping doesn't change what's drawn on top of what), so the group's
// z-order stays a single contiguous block going forward.
void RetouchTab::groupSelectedShapes() {
    if (m_selectedShapes.size() < 2) return;
    QList<int> maskIndices;
    for (int idx : m_selectedShapes) {
        int mi = shapeMaskIndex(idx);
        if (mi >= 0) maskIndices.append(mi);
    }
    if (maskIndices.size() < 2) return;
    std::sort(maskIndices.begin(), maskIndices.end());
    const int originalTop = maskIndices.last();
    m_paintMoveMasterIndex = -1; // mask indices are shifting; the cache is unsafe across it

    QVector<Mask> members;
    members.reserve(maskIndices.size());
    for (int i = maskIndices.size() - 1; i >= 0; --i) {
        members.prepend(m_adj.masks[maskIndices[i]]);
        m_adj.masks.removeAt(maskIndices[i]);
    }
    const int insertAt = originalTop - (maskIndices.size() - 1);

    const QString groupId = QUuid::createUuid().toString();
    for (Mask &mk : members) mk.groupId = groupId;
    for (int i = 0; i < members.size(); ++i) m_adj.masks.insert(insertAt + i, members[i]);

    updateShapeMarkers();
    m_selectedShapes.clear();
    for (int i = 0; i < members.size(); ++i) {
        int newIdx = m_shapeMaskIndices.indexOf(insertAt + i);
        if (newIdx >= 0) m_selectedShapes.insert(newIdx);
    }
    m_activeShape = m_shapeMaskIndices.indexOf(insertAt + members.size() - 1);

    updateShapeMarkers();
    retone();
    markEdited();
    emit shapesChanged();
}

// Clears the group tag of every shape sharing a group with the current
// selection — the shapes stay exactly where they are, they just stop
// acting as one unit.
void RetouchTab::ungroupSelectedShapes() {
    QSet<QString> groupIds;
    for (int idx : m_selectedShapes) {
        int mi = shapeMaskIndex(idx);
        if (mi >= 0 && !m_adj.masks[mi].groupId.isEmpty())
            groupIds.insert(m_adj.masks[mi].groupId);
    }
    if (groupIds.isEmpty()) return;
    for (Mask &mk : m_adj.masks)
        if (mk.type == MaskType::Shape && groupIds.contains(mk.groupId)) mk.groupId.clear();
    updateShapeMarkers();
    retone();
    markEdited();
    emit shapesChanged();
}

ShapeOp RetouchTab::activeShapeStyle() const {
    int mi = shapeMaskIndex(m_activeShape);
    if (mi < 0) return m_shapeDefaults;
    const Mask &mk = m_adj.masks[mi];
    ShapeOp op;
    op.type = mk.shapeType;
    op.rect = mk.shapeRect;
    op.p1 = mk.shapeP1;
    op.p2 = mk.shapeP2;
    op.rotation = mk.shapeRotation;
    op.sides = mk.shapeSides;
    op.innerRadiusRatio = mk.shapeInnerRadiusRatio;
    op.fillEnabled = mk.shapeFillEnabled;
    op.fillColor = mk.shapeFillColor;
    op.strokeEnabled = mk.shapeStrokeEnabled;
    op.strokeColor = mk.shapeStrokeColor;
    op.strokeWidth = mk.shapeStrokeWidth;
    op.opacity = mk.opacity;
    op.visible = mk.visible;
    op.groupId = mk.groupId;
    return op;
}

void RetouchTab::setShapeSides(int sides) {
    int mi = shapeMaskIndex(m_activeShape);
    if (mi >= 0) {
        m_adj.masks[mi].shapeSides = sides;
        retone();
        markEdited();
    } else {
        m_shapeDefaults.sides = sides;
    }
    emit shapesChanged();
}

void RetouchTab::setShapeInnerRadiusRatio(double ratio) {
    int mi = shapeMaskIndex(m_activeShape);
    if (mi >= 0) {
        m_adj.masks[mi].shapeInnerRadiusRatio = ratio;
        retone();
        markEdited();
    } else {
        m_shapeDefaults.innerRadiusRatio = ratio;
    }
    emit shapesChanged();
}

void RetouchTab::setShapeFill(bool enabled, const QColor &color) {
    int mi = shapeMaskIndex(m_activeShape);
    if (mi >= 0) {
        m_adj.masks[mi].shapeFillEnabled = enabled;
        m_adj.masks[mi].shapeFillColor = color;
        retone();
        markEdited();
    } else {
        m_shapeDefaults.fillEnabled = enabled;
        m_shapeDefaults.fillColor = color;
    }
    emit shapesChanged();
}

void RetouchTab::setShapeStroke(bool enabled, const QColor &color, double width) {
    int mi = shapeMaskIndex(m_activeShape);
    if (mi >= 0) {
        m_adj.masks[mi].shapeStrokeEnabled = enabled;
        m_adj.masks[mi].shapeStrokeColor = color;
        m_adj.masks[mi].shapeStrokeWidth = width;
        retone();
        markEdited();
    } else {
        m_shapeDefaults.strokeEnabled = enabled;
        m_shapeDefaults.strokeColor = color;
        m_shapeDefaults.strokeWidth = width;
    }
    emit shapesChanged();
}

void RetouchTab::clearHeals() {
    if (m_adj.heals.isEmpty()) return;
    m_adj.heals.clear();
    rebuildGeom();
    markEdited();
}

// A heal spot was placed on the canvas (point in display-image coords).
void RetouchTab::onHealAt(const QPoint &imgPoint) {
    if (m_scaleFromGeom <= 0 || m_geomImg.isNull()) return;
    double inv = 1.0 / m_scaleFromGeom; // display(scaled) -> geom(full, cropped)
    // Convert cropped(+straightened)-geom coords to oriented coords (heals
    // live pre-crop, pre-straighten).
    QPointF oriented = m_geomToOriented.map(QPointF(imgPoint) * inv);
    HealOp op;
    op.x = int(oriented.x());
    op.y = int(oriented.y());
    op.radius = qMax(2, int(m_healRadiusDisplay * inv));
    // An active image/duplicated layer has its own independent pixels (see
    // Mask::heals) — heal those instead of the tab's base image so healing a
    // copy doesn't silently no-op (the copy has nothing of its own to show
    // the edit) or bleed into the layer it was duplicated from. The true
    // Background mask mirrors m_base itself, so it keeps using the base path.
    if (m_activeMask >= 0 && m_activeMask < m_adj.masks.size() &&
        m_adj.masks[m_activeMask].isImageLayer()) {
        m_adj.masks[m_activeMask].heals.append(op);
    } else {
        m_adj.heals.append(op);
    }
    rebuildGeom();
    markEdited();
}

// An erase dab was placed on the canvas (point in display-image, width-
// normalized coords — same space as onMaskBrushPoint). Works on any layer
// type — image, background, paint, brush, shape, text, text box, or an
// adjustment mask — since applyMasks applies eraseStrokes to every layer's
// final compositing weight, not just image-layer pixels.
//
// Paint and Brush layers are the exception: they already keep an ORDERED
// dab history (Mask::stroke) that rasterizeBrush replays sequentially, so a
// later paint dab can restore coverage an earlier erase dab removed (see
// stampDab's `erase` branch in Adjustments.cpp) — the same mechanism the
// Brush tool's own alt-erase already uses. Routing the dedicated Erase
// tool's dabs into that same ordered stream (instead of the separate,
// order-independent `eraseStrokes`, which permanently gates the whole
// layer's final weight) lets drawing over a previously-erased spot on a
// Paint/Brush layer make it visible again, instead of staying clear
// forever.
void RetouchTab::onEraseAt(const QPointF &ptNorm, bool newStroke) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    m.selectionClipNorm = m_hasSelection ? m_selectionPath : QPainterPath();
    m.selectionFeatherNorm = m_hasSelection ? m_selectionFeatherNorm : 0.0;
    double radiusNorm = (m_scaled.isNull() || m_scaled.width() <= 0)
                             ? 0.06
                             : m_eraseRadiusDisplay / double(m_scaled.width());
    if (m.type == MaskType::Paint || m.type == MaskType::Brush) {
        // hardness=0.0 reproduces the erase tool's original always-soft,
        // full-radius smoothstep falloff (see stampDab: inner = hardness *
        // rad, so hardness=0 -> falloff spans the whole radius).
        BrushStrokePoint sp{ptNorm, /*erase=*/true, radiusNorm, /*hardness=*/0.0,
                            m.paintColor.rgb(), newStroke};
        // See onMaskBrushPoint: drop the canvas's shared stroke reference
        // first so this append doesn't force a full COW deep-copy.
        m_canvas->releaseActiveMaskStroke();
        m.stroke.append(sp);
        retoneDrag(newStroke);
        return;
    }
    m.eraseStrokes.append(ErasePoint{ptNorm, radiusNorm});
    retone();
}

void RetouchTab::onEraseFinished() {
    markEdited(); // schedule one coalesced undo step for the whole drag
}

// A clone-stamp dab: samples one pixel color from the pre-adjustment source
// image (m_geomImg) at `sourceNorm` and bakes it into a BrushStrokePoint —
// the same per-dab-baked-color mechanism Brush/Pen already use (see
// onMaskBrushPoint), just with a per-dab sampled color instead of one fixed
// foreground color. This reproduces the source's broad color/tone but not
// its fine texture — rasterizeBrush (Adjustments.cpp) samples per-pixel from
// the composite-so-far reference image at (pixel + offset) for isClone dabs,
// rather than filling the whole dab with one flat baked color.
void RetouchTab::onCloneStrokePoint(const QPointF &ptNorm, const QPointF &sourceNorm,
                                    bool newStroke, double pressure) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.type != MaskType::Paint) return;
    // Per-pixel clip to the selection boundary happens in rasterizeBrush via
    // Mask::selectionClipNorm; this center-point check only decides whether
    // re-entering the selection should start a fresh dab (see onMaskBrushPoint).
    if (m_hasSelection && !m_selectionPath.contains(ptNorm)) {
        m_cloneStrokeBroken = true;
    } else if (m_cloneStrokeBroken) {
        newStroke = true;
        m_cloneStrokeBroken = false;
    }
    m.selectionClipNorm = m_hasSelection ? m_selectionPath : QPainterPath();
    m.selectionFeatherNorm = m_hasSelection ? m_selectionFeatherNorm : 0.0;
    BrushStrokePoint sp{ptNorm, false, m.brushRadius, m.hardness, m.paintColor.rgb(), newStroke};
    sp.pressure = pressure;
    sp.isPen = false;
    sp.penGrade = m.penGrade;
    sp.isClone = true;
    sp.cloneSourcePt = sourceNorm;
    // See onMaskBrushPoint: drop the canvas's shared stroke reference first
    // so this append doesn't force a full COW deep-copy.
    m_canvas->releaseActiveMaskStroke();
    m.stroke.append(sp);
    pushMaskGizmo();
    retoneDrag(newStroke);
}

void RetouchTab::onCloneFinished() {
    markEdited(); // schedule one coalesced undo step for the whole drag
}

// A remove-object dab was placed on the canvas (point in display-image,
// width-normalized coords — same space as onEraseAt). Accumulates the stroke
// and paints its coverage into a full-oriented-image-sized mask draft; the
// actual content-aware fill is only computed once, on release, so dragging
// stays smooth and the fill never gets recomputed on repaint.
void RetouchTab::onRemoveObjectAt(const QPointF &ptNorm) {
    if (m_scaleFromGeom <= 0 || m_geomImg.isNull() || m_scaled.isNull()) return;
    double px = ptNorm.x() * m_scaled.width();
    double py = ptNorm.y() * m_scaled.width();
    double inv = 1.0 / m_scaleFromGeom; // display(scaled) -> geom(full, cropped)
    QPointF pt = m_geomToOriented.map(QPointF(px, py) * inv);
    double radius = qMax(2.0, m_removeObjectRadiusDisplay * inv);

    if (m_removeObjectMaskDraft.isNull()) {
        // Oriented (pre-crop) image size — same space heals/removals live in.
        QSize orientedSize = orientedPreCropSource().size();
        m_removeObjectMaskDraft = QImage(orientedSize, QImage::Format_ARGB32);
        m_removeObjectMaskDraft.fill(Qt::transparent);
    }
    m_removeObjectStroke.append(pt);
    m_removeObjectRadiusUsed = radius;

    QPainter mp(&m_removeObjectMaskDraft);
    mp.setRenderHint(QPainter::Antialiasing, true);
    mp.setPen(Qt::NoPen);
    mp.setBrush(Qt::white);
    mp.drawEllipse(pt, radius, radius);
    mp.end();
}

// Drag released: run the content-aware fill once over the accumulated
// stroke's coverage and bake it into a new, non-destructive RemoveObjectOp
// (visible/deletable from the Layers panel, like a Shape layer). The fill
// itself (InpaintTool::inpaint) is slow enough to freeze the UI if run
// inline, so it's kicked off on a QtConcurrent worker thread behind a
// QProgressDialog; ImageCanvas ignores new remove-object presses
// (setRemoveObjectBusy) until it finishes.
void RetouchTab::onRemoveObjectFinished() {
    if (m_removeObjectBusy || m_removeObjectMaskDraft.isNull() || m_removeObjectStroke.isEmpty()) {
        m_removeObjectStroke.clear();
        m_removeObjectMaskDraft = QImage();
        return;
    }

    // Bounding box of the painted coverage, padded a touch so the inpaint
    // has known pixels right at the mask edge to blend against.
    QRect rect;
    {
        const QImage &m = m_removeObjectMaskDraft;
        int minX = m.width(), minY = m.height(), maxX = -1, maxY = -1;
        for (int y = 0; y < m.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(m.constScanLine(y));
            for (int x = 0; x < m.width(); ++x) {
                if (qAlpha(row[x]) > 0) {
                    minX = qMin(minX, x); maxX = qMax(maxX, x);
                    minY = qMin(minY, y); maxY = qMax(maxY, y);
                }
            }
        }
        if (maxX < minX || maxY < minY) {
            m_removeObjectStroke.clear();
            m_removeObjectMaskDraft = QImage();
            return;
        }
        int pad = 2;
        rect = QRect(QPoint(minX - pad, minY - pad), QPoint(maxX + pad, maxY + pad))
                   .intersected(m.rect());
    }

    QImage source = orientedPreCropSource();
    QImage maskDraft = m_removeObjectMaskDraft;
    QVector<QPointF> stroke = m_removeObjectStroke;
    double radiusUsed = m_removeObjectRadiusUsed;
    QImage opMask = maskDraft.copy(rect);

    // Clear the in-progress-stroke state right away so a fresh stroke could
    // in principle start accumulating; ImageCanvas additionally hard-blocks
    // new remove-object presses via setRemoveObjectBusy while this job runs.
    m_removeObjectStroke.clear();
    m_removeObjectMaskDraft = QImage();

    m_removeObjectBusy = true;
    m_canvas->setRemoveObjectBusy(true);

    m_removeObjectProgress = new QProgressDialog(tr("Removing object…"), QString(), 0, 100, this);
    m_removeObjectProgress->setWindowModality(Qt::WindowModal);
    m_removeObjectProgress->setMinimumDuration(300); // don't flash for small/fast removals
    m_removeObjectProgress->setAutoClose(false);
    m_removeObjectProgress->setAutoReset(false);
    m_removeObjectProgress->setValue(0);

    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, rect, stroke, radiusUsed, opMask] {
        QImage fill = watcher->result();
        watcher->deleteLater();
        if (m_removeObjectProgress) {
            m_removeObjectProgress->close();
            m_removeObjectProgress->deleteLater();
            m_removeObjectProgress = nullptr;
        }
        m_removeObjectBusy = false;
        m_canvas->setRemoveObjectBusy(false);

        if (!fill.isNull()) {
            RemoveObjectOp op;
            op.stroke = stroke;
            op.radius = radiusUsed;
            op.rect = rect;
            op.mask = opMask;
            op.fill = fill;
            op.visible = true;
            m_adj.removals.append(op);
            m_activeRemoval = m_adj.removals.size() - 1;

            rebuildGeom();
            markEdited();
            emit removalsChanged();
        }
    });

    // The progress callback runs on the worker thread; marshal it back to
    // the GUI thread via a context-object invokeMethod. `self` is a QPointer
    // (safe to read/null-check from another thread) so this is a no-op if
    // the tab/window is closed while the fill is still computing.
    QPointer<RetouchTab> self(this);
    watcher->setFuture(QtConcurrent::run([self, source, maskDraft, rect]() {
        return InpaintTool::inpaint(source, maskDraft, rect, [self](int percent) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, percent] {
                if (self && self->m_removeObjectProgress) self->m_removeObjectProgress->setValue(percent);
            }, Qt::QueuedConnection);
        });
    }));
}

// ---- Local adjustment masks ------------------------------------------------

void RetouchTab::pushMaskGizmo() {
    if (m_activeMask >= 0 && m_activeMask < m_adj.masks.size()) {
        const Mask &m = m_adj.masks[m_activeMask];
        // Shape and TextBox layers are driven by the canvas's own
        // setShapeMode()/setTextMode() (their own tool, own gizmo, own
        // cursor) rather than the local-mask gizmo — routing them through
        // setMaskMode() here would stomp whatever cursor the actually
        // active tool just set (e.g. the Text tool's I-beam getting reset
        // back to an arrow/cross cursor the instant a Text layer is simply
        // selected in the Layers panel) and would make the canvas paint the
        // Brush-mask gizmo circle over a layer that isn't a brush mask at
        // all. Only route mask-gizmo types through here.
        if (m.type != MaskType::Shape && m.type != MaskType::TextBox)
            m_canvas->setMaskMode(m.type, m_maskMode);
        m_canvas->setActiveMask(true, m);
    } else {
        m_canvas->setActiveMask(false, Mask{});
    }
}

void RetouchTab::setMaskMode(bool on) {
    m_maskMode = on;
    // Enable the canvas for the active mask's type (default Radial if none).
    MaskType kind = (m_activeMask >= 0 && m_activeMask < m_adj.masks.size())
                        ? m_adj.masks[m_activeMask].type
                        : MaskType::Radial;
    m_canvas->setMaskMode(kind, on);
    pushMaskGizmo();
    if (on) m_canvas->setFocus();
}

int RetouchTab::addMask(MaskType type, ShapeType shapeType) {
    Mask m;
    m.type = type;
    m.name = QStringLiteral("Layer %1").arg(m_adj.masks.size() + 1);
    if (type == MaskType::Shape) m.shapeType = shapeType;
    if (type == MaskType::Brush || type == MaskType::Paint)
        m.brushRadius = defaultBrushRadiusNorm(m_base.width());
    m_adj.masks.insert(0, m);
    m_activeMask = 0;
    // Only Radial/Linear/Brush/Paint are edited via canvas drag-mask mode;
    // Shape/TextBox/image layers have their own dedicated tool modes, so
    // leaving this on after they're created would swallow the next drag
    // gesture (mouseMoveEvent short-circuits on m_maskMode, see ImageCanvas).
    bool isDragMask = (type == MaskType::Radial || type == MaskType::Linear ||
                        type == MaskType::Brush || type == MaskType::Paint);
    m_maskMode = isDragMask;
    m_canvas->setMaskMode(type, m_maskMode);
    pushMaskGizmo();
    if (m_maskPreviewEnabled) retone();
    markEdited();
    emit masksChanged();
    return m_activeMask;
}

int RetouchTab::addImageLayer(const QString &path) {
    QString storedPath = copyImageLayerAsset(path);
    Mask m;
    m.type = MaskType::None; // covers the full frame; no shape
    m.name = QFileInfo(path).fileName();
    m.sourceImagePath = storedPath;
    m.sourceImageOffset = QPointF(0.0, 0.0);
    m.sourceImageScale = QPointF(1.0, 1.0);
    m.sourceImageLockRatio = true;
    m_adj.masks.insert(0, m);
    m_activeMask = 0;
    m_maskMode = false;
    m_canvas->setMaskMode(MaskType::None, false);
    pushMaskGizmo();
    if (m_maskPreviewEnabled) retone();
    markEdited();
    emit masksChanged();
    kickoffImageLayerDecode(storedPath);
    return m_activeMask;
}

int RetouchTab::addSvgLayer(const QString &svgPath) {
    QSvgRenderer renderer(svgPath);
    if (!renderer.isValid()) return -1;

    QSize baseSize = renderer.defaultSize();
    if (baseSize.isEmpty()) baseSize = QSize(512, 512);
    // Rasterized once on import, so render well above the likely display
    // size to stay crisp under later scaling.
    const qreal kImportScale = 3.0;
    QSize pixelSize = baseSize * kImportScale;

    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter);
    painter.end();

    QString baseDir = m_path.isEmpty() ? QFileInfo(svgPath).absolutePath()
                                        : QFileInfo(m_path).absolutePath();
    QString baseName = m_path.isEmpty() ? QStringLiteral("layer")
                                         : QFileInfo(m_path).completeBaseName();
    QString uuid = QUuid::createUuid().toString(QUuid::Id128).left(8);
    QString pngPath = QDir(baseDir).filePath(
        QStringLiteral("%1.svg-layer.%2.png").arg(baseName, uuid));
    if (!image.save(pngPath)) return -1;

    int idx = addImageLayer(pngPath);
    if (idx >= 0 && idx < m_adj.masks.size())
        m_adj.masks[idx].name = QFileInfo(svgPath).completeBaseName();
    return idx;
}

int RetouchTab::addImageLayerFromImage(const QImage &image, const QString &suggestedName) {
    QString baseDir = m_path.isEmpty() ? QDir::tempPath() : QFileInfo(m_path).absolutePath();
    QString baseName = m_path.isEmpty() ? QStringLiteral("layer")
                                         : QFileInfo(m_path).completeBaseName();
    QString uuid = QUuid::createUuid().toString(QUuid::Id128).left(8);
    QString pngPath = QDir(baseDir).filePath(
        QStringLiteral("%1.svg-layer.%2.png").arg(baseName, uuid));
    if (!image.save(pngPath)) return -1;

    int idx = addImageLayer(pngPath);
    if (idx >= 0 && idx < m_adj.masks.size())
        m_adj.masks[idx].name = suggestedName.isEmpty() ? QStringLiteral("SVG Layer") : suggestedName;
    return idx;
}

// Copies `sourcePath` into an app-managed file next to the base photo so the
// layer keeps working even if the original file is later moved or deleted.
// Falls back to referencing sourcePath directly if the copy fails.
QString RetouchTab::copyImageLayerAsset(const QString &sourcePath) {
    QString baseDir = m_path.isEmpty() ? QFileInfo(sourcePath).absolutePath()
                                        : QFileInfo(m_path).absolutePath();
    QString baseName = m_path.isEmpty() ? QStringLiteral("layer")
                                         : QFileInfo(m_path).completeBaseName();
    QString ext = QFileInfo(sourcePath).suffix();
    QString uuid = QUuid::createUuid().toString(QUuid::Id128).left(8);
    QString destPath = QDir(baseDir).filePath(
        ext.isEmpty() ? QStringLiteral("%1.layer.%2").arg(baseName, uuid)
                       : QStringLiteral("%1.layer.%2.%3").arg(baseName, uuid, ext));

    if (QFile::copy(sourcePath, destPath)) return destPath;

    qWarning() << "copyImageLayerAsset: failed to copy" << sourcePath << "to" << destPath
               << "- referencing original file location instead";
    return sourcePath;
}

void RetouchTab::kickoffImageLayerDecode(const QString &path) {
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, path, watcher] {
        QImage img = watcher->result();
        watcher->deleteLater();
        bool changed = false;
        for (Mask &m : m_adj.masks) {
            if (m.sourceImagePath == path && m.sourceImageCache.isNull() && !m.sourceMissing) {
                if (img.isNull()) m.sourceMissing = true;
                else m.sourceImageCache = img;
                changed = true;
            }
        }
        if (changed) {
            retone();
            emit masksChanged();
        }
    });
    watcher->setFuture(QtConcurrent::run(RawLoader::loadAny, path, WorkingColorSpace::sRGB));
}

// Asset-stamp cutouts are small app-managed PNGs (not RAW photos), so a
// synchronous decode is cheap enough to do inline -- no need for
// kickoffImageLayerDecode's background-thread machinery.
void RetouchTab::loadShapeImageCache(const QString &path) {
    QImage img(path);
    bool changed = false;
    for (Mask &m : m_adj.masks) {
        if (m.shapeImagePath == path && m.shapeImageCache.isNull() && !img.isNull()) {
            m.shapeImageCache = img;
            changed = true;
        }
    }
    if (changed) {
        retone();
        emit masksChanged();
    }
}

// Places `imagePath` (an AssetStamp's stored cutout) as a new image-filled
// Shape mask, sized to ~25% of the canvas width and matching the asset's own
// aspect ratio, centered on the canvas. Reuses Shape's existing move/resize/
// rotate handle interaction -- no new geometry/handle code needed.
int RetouchTab::insertAssetStamp(const QString &imagePath, const QSize &nativeSize,
                                 const QString &name) {
    if (imagePath.isEmpty()) return -1;
    const QSize canvasSize = previewImage().size();
    const int canvasW = canvasSize.width();
    const int canvasH = canvasSize.height();
    if (canvasW <= 0 || canvasH <= 0) return -1;

    double aspect = (nativeSize.width() > 0 && nativeSize.height() > 0)
                        ? double(nativeSize.height()) / double(nativeSize.width())
                        : 1.0;
    const double w = canvasW * 0.25;
    const double h = w * aspect;

    Mask m;
    m.type = MaskType::Shape;
    m.name = name.isEmpty() ? QStringLiteral("Asset") : name;
    m.shapeType = ShapeType::Rectangle;
    m.shapeRect = QRectF(canvasW / 2.0 - w / 2.0, canvasH / 2.0 - h / 2.0, w, h);
    m.shapeRotation = 0.0;
    m.shapeFillEnabled = true;
    m.shapeStrokeEnabled = false;
    m.shapeImagePath = imagePath;

    int insertAt = 0;
    m_adj.masks.insert(insertAt, m);
    m_activeMask = insertAt;
    loadShapeImageCache(imagePath);
    retone();
    markEdited();
    emit masksChanged();
    return insertAt;
}

int RetouchTab::duplicateActiveMask() {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return -1;
    Mask copy = m_adj.masks[m_activeMask];
    // Only one Background-type mask is ever allowed; a duplicate becomes a
    // regular full-frame layer instead. The Background's own content isn't a
    // real independent buffer (it just mirrors m_base live — see
    // orientedPreCropSource()), so simply re-typing it would leave the
    // "copy" as a pass-through with no pixels of its own to edit. Freeze the
    // layer's current pixels into a real, independently-editable image layer
    // (same on-disk mechanism as addImageLayer) so heals/future edits on the
    // copy stick without touching the original.
    if (copy.type == MaskType::Background) {
        QImage snapshot = orientedPreCropSource(); // full-res, oriented, pre-crop
        QString baseDir = m_path.isEmpty() ? QDir::tempPath() : QFileInfo(m_path).absolutePath();
        QString baseName = m_path.isEmpty() ? QStringLiteral("layer")
                                             : QFileInfo(m_path).completeBaseName();
        QString uuid = QUuid::createUuid().toString(QUuid::Id128).left(8);
        QString pngPath = QDir(baseDir).filePath(
            QStringLiteral("%1.layer.%2.png").arg(baseName, uuid));
        if (!snapshot.isNull()) {
            copy.type = MaskType::None;
            copy.sourceImagePath = pngPath;
            copy.sourceImageCache = snapshot; // already in memory: layer works instantly
            copy.sourceImageOffset = QPointF(0.0, 0.0);
            copy.sourceImageScale = QPointF(1.0, 1.0);
            // The snapshot already has the original's heals baked in (they
            // were applied by orientedPreCropSource() above); starting the
            // copy's own list empty means new heal clicks on the copy affect
            // only the copy, not a phantom re-application of the original's.
            copy.heals.clear();
            // Encoding a full-res PNG synchronously here would freeze the UI
            // for several seconds; write it in the background instead (the
            // in-memory cache above already makes the layer fully usable).
            (void)QtConcurrent::run([snapshot, pngPath] { snapshot.save(pngPath); });
        } else {
            copy.type = MaskType::None; // fallback: old pass-through behavior
        }
    }
    copy.groupId.clear(); // a lone duplicate leaves its group, even if the original had one
    copy.name = copy.name.isEmpty() ? QStringLiteral("Layer copy")
                                    : copy.name + QStringLiteral(" copy");
    // masks[0] is the topmost/frontmost layer (see LayersPanel::doRebuildList),
    // so the copy must land at the original's own index (pushing the
    // original one slot higher/backward) to appear one layer up/in-front —
    // inserting at +1 would instead place it behind the original.
    int insertAt = m_activeMask;
    m_adj.masks.insert(insertAt, copy);
    m_activeMask = insertAt;
    m_maskMode = (copy.type != MaskType::None);
    m_canvas->setMaskMode(copy.type, m_maskMode);
    pushMaskGizmo();
    retone();
    markEdited();
    emit masksChanged();
    return m_activeMask;
}

void RetouchTab::selectMask(int index) {
    if (index < -1 || index >= m_adj.masks.size()) return;
    m_activeMask = index;
    // Mirror the selection into m_activeShape/m_activeText (the canvas's own
    // selection-gizmo/handle state -- see selectShape()/onTextSelected()'s
    // comments on the reverse direction) so picking a Shape/TextBox row in
    // the Layers panel moves the canvas selection handles onto it too,
    // instead of leaving them on whatever shape/text was last clicked on
    // the canvas while property edits and drag-move now silently target the
    // newly panel-selected layer.
    const MaskType t = (index >= 0 && index < m_adj.masks.size())
                            ? m_adj.masks[index].type
                            : MaskType::None;
    // m_shapeMaskIndices/m_textMaskIndices are only rebuilt by
    // updateShapeMarkers()/updateTextMarkers(), so refresh them first --
    // otherwise indexOf() below would look up `index` in a stale (possibly
    // empty) table left over from whatever last called those.
    updateShapeMarkers();
    updateTextMarkers();
    updateObjectMarkers();
    if (t == MaskType::Shape) {
        int markerIdx = m_shapeMaskIndices.indexOf(index);
        m_activeShape = markerIdx;
        m_selectedShapes = (markerIdx >= 0) ? QSet<int>{markerIdx} : QSet<int>{};
        updateShapeMarkers();
    } else if (m_activeShape != -1) {
        m_activeShape = -1;
        m_selectedShapes.clear();
        updateShapeMarkers();
    }
    if (t == MaskType::TextBox) {
        int markerIdx = m_textMaskIndices.indexOf(index);
        m_activeText = markerIdx;
        m_canvas->setActiveTextIndex(markerIdx);
    } else if (m_activeText != -1) {
        m_activeText = -1;
        m_canvas->setActiveTextIndex(-1);
    }
    pushMaskGizmo();
    if (m_maskPreviewEnabled) retone();
    emit masksChanged();
}

void RetouchTab::deleteActiveMask() {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    m_paintMoveMasterIndex = -1; // mask indices are shifting; the cache is unsafe across it
    m_adj.masks.remove(m_activeMask);
    m_activeMask = m_adj.masks.isEmpty() ? -1
                                         : qMin(m_activeMask, m_adj.masks.size() - 1);
    pushMaskGizmo();
    retone();
    markEdited();
    emit masksChanged();
}

int RetouchTab::backgroundMaskIndex() const {
    for (int i = 0; i < m_adj.masks.size(); ++i)
        if (m_adj.masks[i].type == MaskType::Background) return i;
    return -1;
}

bool RetouchTab::backgroundLayerVisible() const {
    const int idx = backgroundMaskIndex();
    return idx < 0 || m_adj.masks[idx].visible;
}

void RetouchTab::ensureBackgroundMask() {
    if (backgroundMaskIndex() >= 0) return;
    Mask bg;
    bg.type = MaskType::Background;
    bg.name = QStringLiteral("Background");
    m_adj.masks.append(bg);
}

void RetouchTab::setActiveMaskType(MaskType type) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.type == type) return;
    m.type = type;
    // Brush strokes default to a 20px-equivalent radius the first time a
    // mask enters Brush/Paint; leave it alone once the user has picked
    // their own size.
    if ((type == MaskType::Brush || type == MaskType::Paint) &&
        std::abs(m.brushRadius - Mask().brushRadius) < 1e-9)
        m.brushRadius = defaultBrushRadiusNorm(m_base.width());
    m_maskMode = (type != MaskType::None);
    m_canvas->setMaskMode(type, m_maskMode);
    pushMaskGizmo();
    retone();
    markEdited();
    emit masksChanged();
}

void RetouchTab::setActiveMaskAdjust(const MaskAdjust &a) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    if (m_adj.masks[m_activeMask].adj == a) return;
    m_adj.masks[m_activeMask].adj = a;
    retone();
    markEdited();
}

void RetouchTab::setActiveMaskImageTransform(double offsetX, double offsetY,
                                            double scaleX, double scaleY,
                                            bool lockRatio) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (!m.isImageLayer()) return;
    QPointF pos(qBound(-1.0, offsetX, 1.0), qBound(-1.0, offsetY, 1.0));
    QPointF scale(qBound(0.10, scaleX, 3.0), qBound(0.10, scaleY, 3.0));
    if (lockRatio) {
        const double s = std::max(scale.x(), scale.y());
        scale = QPointF(s, s);
    }
    if (m.sourceImageOffset == pos && m.sourceImageScale == scale &&
        m.sourceImageLockRatio == lockRatio)
        return;
    m.sourceImageOffset = pos;
    m.sourceImageScale = scale;
    m.sourceImageLockRatio = lockRatio;
    pushMaskGizmo();
    retone();
    markEdited();
    emit masksChanged();
}

void RetouchTab::setActiveMaskOpacity(double opacity) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    double clamped = std::clamp(opacity, 0.0, 1.0);
    if (std::abs(m.opacity - clamped) < 1e-9) return;
    m.opacity = clamped;
    retone();
    markEdited();
}

void RetouchTab::setActiveMaskBlend(BlendMode mode) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.blend == mode) return;
    m.blend = mode;
    retone();
    markEdited();
}

void RetouchTab::setActiveMaskVisible(bool visible) {
    setMaskVisible(m_activeMask, visible);
}

void RetouchTab::setMaskVisible(int index, bool visible) {
    if (index < 0 || index >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[index];
    if (m.visible == visible) return;
    m.visible = visible;
    retone();
    markEdited();
    emit masksChanged();
}

void RetouchTab::setActiveMaskName(const QString &name) {
    setMaskName(m_activeMask, name);
}

void RetouchTab::setMaskName(int index, const QString &name) {
    if (index < 0 || index >= m_adj.masks.size()) return;
    if (m_adj.masks[index].name == name) return;
    m_adj.masks[index].name = name;
    markEdited();
    emit masksChanged();
}

void RetouchTab::moveMask(int from, int to) {
    if (from < 0 || from >= m_adj.masks.size() || to < 0 ||
        to >= m_adj.masks.size() || from == to)
        return;
    m_paintMoveMasterIndex = -1; // mask indices are shifting; the cache is unsafe across it
    m_adj.masks.move(from, to);
    if (m_activeMask == from) m_activeMask = to;
    else if (from < m_activeMask && m_activeMask <= to) --m_activeMask;
    else if (to <= m_activeMask && m_activeMask < from) ++m_activeMask;
    retone();
    markEdited();
    emit masksChanged();
}

// Applies a full new ordering of masks() indices (e.g. from a Layers-panel
// drag), rather than a single from/to pair — needed because a drag can move
// a whole contiguous group as one block.
void RetouchTab::reorderMasks(const QVector<int> &newOrder, const QVector<int> &leftGroupIndices,
                               const QVector<QPair<int, QString>> &joinGroups) {
    // newOrder must be a permutation of 0..size-1: the UI derives it by
    // flattening the tree, and a stale/desynced tree (e.g. after a drag that
    // moved a row out of a group) can otherwise yield a non-bijective order
    // that silently drops or duplicates masks, potentially losing the active
    // mask's index and leaving m_activeMask pointing past the new array.
    // Rejecting it here must still emit masksChanged(): LayersPanel's tree
    // widget has already moved its rows cosmetically (Qt's own drag-drop
    // machinery does that before this call runs), so if we bail out without
    // resyncing, the panel is left showing a reorder that was never applied
    // to m_adj.masks -- silently desynced until some *other* edit forces a
    // rebuild, at which point the tree snaps back to the true (pre-reorder)
    // order and looks like an unrelated action broke the order.
    bool valid = newOrder.size() == m_adj.masks.size();
    if (valid) {
        QVector<bool> seen(m_adj.masks.size(), false);
        for (int idx : newOrder) {
            if (idx < 0 || idx >= m_adj.masks.size() || seen[idx]) { valid = false; break; }
            seen[idx] = true;
        }
    }
    if (!valid) {
        emit masksChanged();
        return;
    }
    m_paintMoveMasterIndex = -1; // mask indices are shifting; the cache is unsafe across it
    for (int idx : leftGroupIndices)
        if (idx >= 0 && idx < m_adj.masks.size()) m_adj.masks[idx].groupId.clear();
    for (const auto &join : joinGroups)
        if (join.first >= 0 && join.first < m_adj.masks.size()) m_adj.masks[join.first].groupId = join.second;
    QVector<Mask> reordered;
    reordered.reserve(newOrder.size());
    int newActive = -1;
    for (int i = 0; i < newOrder.size(); ++i) {
        const int idx = newOrder[i];
        reordered.append(m_adj.masks[idx]);
        if (idx == m_activeMask) newActive = i;
    }
    m_adj.masks = reordered;
    m_activeMask = newActive;
    // A drag can move a Shape/TextBox layer to a new masks index without
    // changing which mask is "active" (e.g. dragging some other, unselected
    // row) -- m_shapeMaskIndices/m_textMaskIndices (the marker<->masks index
    // tables shape/text-specific tools resolve marker indices through, see
    // shapeMaskIndex()/textMaskIndex()) are only rebuilt by
    // updateShapeMarkers()/updateTextMarkers(), which reorderMasks() never
    // called until now. Left stale, any shape/text op issued right after a
    // drag-reorder (raise/lower/select/duplicate/delete, all keyed by marker
    // index from the canvas) would resolve against the *pre-reorder*
    // masks-index layout and silently act on the wrong layer -- e.g.
    // swapping the wrong pair of masks on the very next '+'/'-' nudge.
    // Refresh both tables now, and re-resolve m_activeShape/m_activeText
    // (marker indices, so they've gone stale the same way) the same way
    // selectMask() does, so canvas selection handles stay on the
    // just-reordered active layer instead of snapping onto whatever now
    // occupies the old marker index.
    updateShapeMarkers();
    updateTextMarkers();
    updateObjectMarkers();
    const MaskType activeType = (m_activeMask >= 0 && m_activeMask < m_adj.masks.size())
                                     ? m_adj.masks[m_activeMask].type
                                     : MaskType::None;
    if (activeType == MaskType::Shape) {
        m_activeShape = m_shapeMaskIndices.indexOf(m_activeMask);
    } else {
        m_activeShape = -1;
    }
    m_selectedShapes.clear();
    if (m_activeShape >= 0) m_selectedShapes.insert(m_activeShape);
    if (activeType == MaskType::TextBox) {
        m_activeText = m_textMaskIndices.indexOf(m_activeMask);
    } else {
        m_activeText = -1;
    }
    updateShapeMarkers();
    m_canvas->setActiveTextIndex(m_activeText);
    retone();
    markEdited();
    emit masksChanged();
}

// Tags the given layers as one group and moves them to be contiguous in the
// stack (at the position of the topmost/frontmost member), mirroring
// groupSelectedShapes.
void RetouchTab::groupMasks(const QVector<int> &indices) {
    QSet<int> sel(indices.begin(), indices.end());
    if (sel.size() < 2) return;
    QList<int> sorted = sel.values();
    std::sort(sorted.begin(), sorted.end());
    for (int i : sorted)
        if (i < 0 || i >= m_adj.masks.size()) return;
    const int originalTop = sorted.last();
    m_paintMoveMasterIndex = -1; // mask indices are shifting; the cache is unsafe across it

    QVector<Mask> members;
    members.reserve(sorted.size());
    for (int i = sorted.size() - 1; i >= 0; --i) {
        members.prepend(m_adj.masks[sorted[i]]);
        m_adj.masks.removeAt(sorted[i]);
    }
    const int insertAt = originalTop - (sorted.size() - 1);

    // "Group", then "Group 1", "Group 2", ... — skip any name already in use
    // by an existing group so re-grouping after ungrouping/deleting doesn't
    // collide with a still-live group's name.
    QSet<QString> usedNames;
    for (const Mask &m : m_adj.masks)
        if (!m.groupId.isEmpty()) usedNames.insert(m.groupName);
    QString groupName = QStringLiteral("Group");
    int n = 1;
    while (usedNames.contains(groupName)) groupName = QStringLiteral("Group %1").arg(n++);

    const QString groupId = QUuid::createUuid().toString();
    for (Mask &m : members) { m.groupId = groupId; m.groupName = groupName; }
    for (int i = 0; i < members.size(); ++i) m_adj.masks.insert(insertAt + i, members[i]);
    MaskGroup grp;
    grp.id = groupId;
    grp.name = groupName;
    m_adj.groups.append(grp);

    m_activeMask = insertAt + members.size() - 1;
    retone();
    markEdited();
    emit masksChanged();
}

// Renames every layer sharing this groupId (the group name is mirrored
// across all of a group's members since there's no separate group entity).
void RetouchTab::renameGroup(const QString &groupId, const QString &name) {
    if (groupId.isEmpty()) return;
    bool changed = false;
    for (Mask &m : m_adj.masks) {
        if (m.groupId == groupId && m.groupName != name) {
            m.groupName = name;
            changed = true;
        }
    }
    for (MaskGroup &g : m_adj.groups)
        if (g.id == groupId && g.name != name) { g.name = name; changed = true; }
    if (!changed) return;
    markEdited();
    emit masksChanged();
}

// Clears the group tag of every layer sharing a group with the given
// indices — the layers stay exactly where they are, they just stop acting
// as one unit. Their MaskGroup entry (opacity/visibility/blend) is dropped
// along with them, since nothing references that groupId anymore.
void RetouchTab::ungroupMasks(const QVector<int> &indices) {
    QSet<QString> groupIds;
    for (int idx : indices)
        if (idx >= 0 && idx < m_adj.masks.size() && !m_adj.masks[idx].groupId.isEmpty())
            groupIds.insert(m_adj.masks[idx].groupId);
    if (groupIds.isEmpty()) return;
    for (Mask &m : m_adj.masks)
        if (groupIds.contains(m.groupId)) { m.groupId.clear(); m.groupName.clear(); }
    for (int i = m_adj.groups.size() - 1; i >= 0; --i)
        if (groupIds.contains(m_adj.groups[i].id)) m_adj.groups.removeAt(i);
    markEdited();
    emit masksChanged();
}

void RetouchTab::setGroupProperties(const QString &groupId, double opacity, bool visible,
                                    BlendMode blend) {
    if (groupId.isEmpty()) return;
    MaskGroup *grp = nullptr;
    for (MaskGroup &g : m_adj.groups)
        if (g.id == groupId) { grp = &g; break; }
    if (!grp) {
        // A group created before this field existed (or one whose entry was
        // otherwise lost) — synthesize one now rather than silently no-oping.
        MaskGroup g;
        g.id = groupId;
        for (const Mask &m : m_adj.masks)
            if (m.groupId == groupId) { g.name = m.groupName; break; }
        m_adj.groups.append(g);
        grp = &m_adj.groups.last();
    }
    grp->opacity = std::clamp(opacity, 0.0, 1.0);
    grp->visible = visible;
    grp->blend = blend;
    retone();
    markEdited();
}

void RetouchTab::setActiveMaskShape(bool inverted, double feather,
                                    double hardness, double brushRadius,
                                    bool autoMask) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    m.inverted = inverted;
    m.feather = feather;
    m.hardness = hardness;
    m.brushRadius = brushRadius;
    m.autoMask = autoMask;
    pushMaskGizmo();
    retone();
    markEdited();
}

void RetouchTab::setActiveMaskGradientFill(bool enabled, const QColor &colorA, const QColor &colorB) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.type != MaskType::Radial && m.type != MaskType::Linear) return;
    m.isGradientFill = enabled;
    m.gradientColorA = colorA;
    m.gradientColorB = colorB;
    retone();
    markEdited();
}

void RetouchTab::setPaintColor(const QColor &color) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.type != MaskType::Paint) return;
    m.paintColor = color;
    retone();
    markEdited();
}

void RetouchTab::setActiveMaskText(const QString &text, const QString &family,
                                   double pixelSize, bool bold, bool italic) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.type != MaskType::Text) return;
    m.text = text;
    m.textFamily = family;
    m.textPixelSize = pixelSize;
    m.textBold = bold;
    m.textItalic = italic;
    retone();
    markEdited();
}

void RetouchTab::onMaskRadial(const QPointF &centerNorm, double radiusNorm) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    m.center = centerNorm;
    m.radiusX = m.radiusY = std::max(0.01, radiusNorm);
    pushMaskGizmo();
    retone();
}

void RetouchTab::onMaskLinear(const QPointF &p0Norm, const QPointF &p1Norm) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    m.p0 = p0Norm;
    m.p1 = p1Norm;
    pushMaskGizmo();
    retone();
}

void RetouchTab::onMaskBrushPoint(const QPointF &ptNorm, bool erase, bool newStroke, double pressure) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    // Re-entering the selection after a dab landed fully outside it should
    // start a fresh dab, not connect a straight line across the gap. The
    // actual per-pixel clip to the selection boundary happens in
    // rasterizeBrush (Adjustments.cpp) via Mask::selectionClipNorm below —
    // this center-point check is only used to decide the stroke break.
    if (m_hasSelection && !m_selectionPath.contains(ptNorm)) {
        m_selectionStrokeBroken = true;
    } else if (m_selectionStrokeBroken) {
        newStroke = true;
        m_selectionStrokeBroken = false;
    }
    Mask &m = m_adj.masks[m_activeMask];
    m.selectionClipNorm = m_hasSelection ? m_selectionPath : QPainterPath();
    m.selectionFeatherNorm = m_hasSelection ? m_selectionFeatherNorm : 0.0;
    // Bake in the brush size/hardness/(paint) color at paint time so later
    // changes only affect new dabs, not ones already committed to the stroke.
    // `pressure` (stylus pressure, 1.0 for mouse input) is likewise captured
    // per-dab so Pen dabs can modulate radius/hardness from it later. Pen and
    // Brush share the same Paint-type layer/stroke — `isPen`/`penGrade` tag
    // each dab with which tool painted it (see setPenToolActive), so a user
    // can switch tools mid-layer without needing a separate layer per tool.
    BrushStrokePoint sp{ptNorm, erase, m.brushRadius, m.hardness,
                        m.paintColor.rgb(), newStroke};
    sp.pressure = pressure;
    sp.isPen = m_penToolActive;
    sp.penGrade = m.penGrade;
    // Drop the canvas's shared copy of the stroke before appending, or Qt's
    // QVector copy-on-write forces a full deep-copy of the stroke-so-far on
    // every dab (see releaseActiveMaskStroke).
    m_canvas->releaseActiveMaskStroke();
    m.stroke.append(sp);
    pushMaskGizmo(); // show the painted coverage right away
    retoneDrag(newStroke);
}

void RetouchTab::setActivePenGrade(double grade) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.type != MaskType::Paint) return;
    m.penGrade = std::clamp(grade, -6.0, 5.0);
    retone();
    markEdited();
}

void RetouchTab::onBucketFillRequested(const QPointF &ptNorm) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    if (m_hasSelection && !m_selectionPath.contains(ptNorm)) return;
    Mask &m = m_adj.masks[m_activeMask];
    if (m.type != MaskType::Paint || m_geomImg.isNull()) return;
    // Clips the flood-fill's grown region to the selection boundary, not just
    // the click point, via Mask::selectionClipNorm (see matchesRegion in
    // bucketFillPaintMask, Adjustments.cpp).
    m.selectionClipNorm = m_hasSelection ? m_selectionPath : QPainterPath();
    m.selectionFeatherNorm = m_hasSelection ? m_selectionFeatherNorm : 0.0;
    // Flood-fill at a capped working resolution (full geom resolution can be
    // tens of megapixels; the result is stored in `fillMask` and resampled to
    // whatever the render buffer needs, so this only trades off fill edge
    // precision, not final render quality — see bucketFillPaintMask).
    const int fullW = m_geomImg.width(), fullH = m_geomImg.height();
    if (fullW <= 0 || fullH <= 0) return;
    const double s = fullW > 1600 ? 1600.0 / fullW : 1.0;
    const int w = std::max(1, int(std::lround(fullW * s)));
    const int h = std::max(1, int(std::lround(fullH * s)));
    m.fillMask = bucketFillPaintMask(m, ptNorm, m.paintColor, w, h);
    if (m_paintMoveMasterIndex == m_activeMask) m_paintMoveMasterIndex = -1;
    pushMaskGizmo();
    retone();
    markEdited();
    emit masksChanged();
}

void RetouchTab::fillActiveMask(const QColor &color) {
    if (m_activeMask < 0 || m_activeMask >= m_adj.masks.size()) return;
    Mask &m = m_adj.masks[m_activeMask];
    const MaskType type = m.type;
    if (type == MaskType::Paint) {
        // Route through `fillMask` — the same mechanism the paint bucket
        // uses — instead of appending a giant fully-opaque dab into
        // `m.stroke`. Stroke compositing is max-coverage/last-writer (see
        // rasterizeBrush): once a dab has driven a pixel's coverage to 255,
        // any later dab whose own coverage at that pixel is lower (e.g. the
        // antialiased edge of a soft brush, or most of a Pen stroke, whose
        // grade-driven hardness/opacity are often well under 1.0) can never
        // win that pixel back, so brush/pen strokes on top of a fully-filled
        // layer barely showed. `fillMask` instead composites *underneath*
        // the stroke coverage/color (see applyMasks), so strokes painted
        // after the fill are always visible on top of it regardless of
        // their own hardness/opacity.
        //
        // Deliberately NOT touching m.paintColor here: it doubles as "the
        // color new brush/pen dabs will use" (see onMaskBrushPoint), so
        // setting it to the fill color would silently make every stroke
        // painted after this fill use that color too — e.g. Ctrl+Backspace
        // (fill with background color, usually white) would leave the brush
        // painting invisible white-on-white until the user reselected a
        // color. The fill's own color is fully captured by `fill`/
        // `m.fillMask` below, independent of m.paintColor.
        if (!m_geomImg.isNull()) {
            const int fullW = m_geomImg.width(), fullH = m_geomImg.height();
            const double s = fullW > 1600 ? 1600.0 / fullW : 1.0;
            const int w = std::max(1, int(std::lround(fullW * s)));
            const int h = std::max(1, int(std::lround(fullH * s)));
            QImage fill(w, h, QImage::Format_ARGB32);
            fill.fill(color);
            m.fillMask = fill;
            if (m_paintMoveMasterIndex == m_activeMask) m_paintMoveMasterIndex = -1;
        }
    } else if (type == MaskType::Background) {
        // Full-canvas coverage regardless of aspect ratio: radius is
        // normalized to image width (see BrushStrokePoint), so 3.0
        // comfortably covers the diagonal of any canvas up to ~3x taller
        // than it is wide.
        const BrushStrokePoint fillDab{QPointF(0.5, 0.5), false, 3.0, 1.0,
                                       color.rgb()};
        m.paintColor = color;
        m.stroke.append(fillDab);
    } else {
        return; // no flat-fill mapping for this mask type
    }

    pushMaskGizmo();
    retone();
    markEdited();
    emit masksChanged();
}

void RetouchTab::onMaskEditFinished() {
    // Once a brush stroke is committed, hide its overlay/gizmo again so it
    // doesn't sit on top of the image; it reappears while actively painting
    // (onMaskBrushPoint) or when explicitly reselected from the mask list.
    if (m_activeMask >= 0 && m_activeMask < m_adj.masks.size() &&
        m_adj.masks[m_activeMask].type == MaskType::Brush) {
        m_canvas->setActiveMask(false, Mask{});
    }
    markEdited(); // schedule one coalesced undo step for the whole drag
}

void RetouchTab::zoomFit() { m_canvas->zoomFit(); }
void RetouchTab::setZoomPercent(double percent) { m_canvas->setZoomPercent(percent); }
double RetouchTab::zoomPercent() const { return m_canvas->zoomPercent(); }

void RetouchTab::onColorPicked(const QColor &c) {
    // Neutralise the sampled pixel: gains scale each channel to the mean.
    double r = c.red(), g = c.green(), b = c.blue();
    double mean = (r + g + b) / 3.0;
    if (r < 1) r = 1;
    if (g < 1) g = 1;
    if (b < 1) b = 1;
    m_adj.wbR = mean / r;
    m_adj.wbG = mean / g;
    m_adj.wbB = mean / b;
    retone();
    markEdited();
    emit wbPicked();
}

void RetouchTab::onQuickColorPicked(const QColor &c) {
    // Just bubble it up — RetouchWindow owns the "update swatch + apply as
    // paint color" sequencing already (see ColorSwatchWidget::
    // foregroundColorChanged -> setPaintColor), same as a manual swatch
    // click, so this doesn't call setPaintColor itself to avoid a redundant
    // double-apply.
    emit quickColorPicked(c);
}

void RetouchTab::onCanvasCrop(const QRect &r, double angleDegrees) {
    if (r.isEmpty() || m_scaleFromGeom <= 0) {
        m_pendingCrop = QRect();
        m_pendingCropAngle = 0.0;
        emit cropPending(false);
        return;
    }
    double inv = 1.0 / m_scaleFromGeom; // display(scaled) -> geom(full oriented)
    m_pendingCrop = QRect(int(r.x() * inv), int(r.y() * inv),
                          int(r.width() * inv), int(r.height() * inv))
                        .intersected(m_geomImg.rect());
    m_pendingCropAngle = angleDegrees;
    emit cropPending(!m_pendingCrop.isEmpty());
}

void RetouchTab::applyCrop() {
    if (m_pendingCrop.isEmpty()) return;
    m_adj.cropRect = m_pendingCrop;
    m_adj.cropAngle = m_pendingCropAngle;
    m_pendingCrop = QRect();
    m_pendingCropAngle = 0.0;
    m_cropMode = false;
    m_canvas->setCropMode(false);
    m_canvas->clearSelection();
    emit cropPending(false);
    emit cropModeExited();
    rebuildGeom();
    markEdited();
}

void RetouchTab::resetCrop() {
    m_adj.cropRect = QRect();
    m_adj.cropAngle = 0.0;
    m_pendingCrop = QRect();
    m_pendingCropAngle = 0.0;
    m_canvas->clearSelection();
    emit cropPending(false);
    rebuildGeom();
    markEdited();
}

QImage RetouchTab::renderFullRes() const {
    if (m_geomImg.isNull()) return QImage();
    // m_geomImg is the full-res oriented + healed + cropped base; apply tone
    // and every mask layer (including Shape/TextBox) in true stack order.
    return applyAdjustments(m_geomImg, toneOnly(m_adj), nullptr, -1, nullptr,
                            m_orientedToGeom, m_geomRotationDeg);
}
