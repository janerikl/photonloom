#pragma once

#include <QWidget>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QTransform>
#include <QColor>
#include <QSet>
#include <QMap>
#include <QList>
#include <QPainterPath>
#include <QElapsedTimer>

#include "edit/Adjustments.h"
#include "edit/ColorSpace.h"

class ImageCanvas;
class QTimer;
class QThread;
class QProgressDialog;
template <typename T> class QFutureWatcher;

// Runs applyAdjustments off the GUI thread. Lives on its own QThread; one job
// at a time (the tab coalesces to the latest request).
class RenderWorker : public QObject {
    Q_OBJECT
public slots:
    // `maskSnapshotIndex >= 0` additionally requests the cumulative composite
    // through that mask index, returned as `maskSnapshot` in `done` (used to
    // feed the per-layer Levels histogram; see RetouchTab::maskPreviewImage).
    // `orientedToGeom`/`geomRotationDeg`/`scale`: geometry mapping for
    // Shape/TextBox layers, snapshotted at request time (see
    // RetouchTab::m_orientedToGeom).
    void render(const QImage &src, const Adjustments &adj, int maskSnapshotIndex,
               const QTransform &orientedToGeom, double geomRotationDeg, double scale,
               int belowSnapshotIndex = -1);
    // Fast drag-frame render: resumes compositing from `belowSnapshot` (the
    // composite through everything below `dragMaskIndex`, previously
    // captured by render()'s belowSnapshotIndex) instead of redoing the
    // whole stack. See RetouchTab::retoneDrag.
    void renderDragFrame(const QImage &belowSnapshot, int dragMaskIndex, const Adjustments &adj,
                         const QTransform &orientedToGeom, double geomRotationDeg, double scale);
signals:
    // `dirtyRect`: for a drag frame where applyMasks' Paint-layer dirty-rect
    // fast path was used, the exact sub-rect of `result` that actually
    // changed from the previous frame (image pixel space) - an invalid/
    // default QRect (the only value render() ever sends) means "unknown,
    // assume the whole image changed." See ImageCanvas::setImage.
    void done(const QImage &result, const QImage &maskSnapshot,
             const QImage &belowSnapshot, int belowSnapshotIndex,
             const QRect &dirtyRect = QRect());

private:
    // Persists across calls (this worker's queued slot invocations run
    // serially on one thread) so brush/paint mask coverage only needs to be
    // rasterized incrementally as a stroke grows; see BrushRasterCache.
    QVector<BrushRasterCache> m_brushCache;
};

// One open photo in the retouch window. Decodes its RAW asynchronously, then
// shows an interactive preview: geometry (rotate/flip/crop) is cached at full
// res and downscaled once; tone/colour sliders re-render only the small copy,
// so dragging stays smooth. Full-res render is produced on demand for export.
class RetouchTab : public QWidget {
    Q_OBJECT
public:
    // `defaultSpace` is the working color space to decode with when there's
    // no sidecar override yet (i.e. the current global Preferences default at
    // open time) — see ColorSpace.h.
    explicit RetouchTab(const QString &path, QWidget *parent = nullptr,
                        WorkingColorSpace defaultSpace = WorkingColorSpace::sRGB);
    explicit RetouchTab(const QSize &blankSize, QWidget *parent = nullptr,
                        WorkingColorSpace defaultSpace = WorkingColorSpace::sRGB); // File > New
    ~RetouchTab() override;

    QString path() const { return m_path; }
    WorkingColorSpace workingColorSpace() const { return m_workingColorSpace; }
    ImageCanvas *canvas() const { return m_canvas; }
    void assignPath(const QString &path); // File > New's first save: adopt a real backing path
    Adjustments adjustments() const { return m_adj; }
    bool isReady() const { return !m_base.isNull(); }
    int imageWidth() const { return m_base.isNull() ? 0 : m_base.width(); }

    void setAdjustments(const Adjustments &a); // from the dock
    void setCropMode(bool on);
    void setCropAspect(double widthOverHeight);
    void applyCrop();
    void resetCrop();

    void setWbPickMode(bool on);
    void setColorRangePickMode(bool on); // Levels targeted color adjustment
    void setHealMode(bool on);
    void setZoomMode(bool on); // zoom tool: marquee-drag + Ctrl+wheel zoom
    void setHealBrush(int radiusDisplayPx);
    void clearHeals();
    void setEraseMode(bool on);
    void setMaskForceErase(bool on); // E toggle while a Paint mask is active
    void setEraseBrush(int radiusDisplayPx);
    void setBucketMode(bool on); // paint bucket: single click flood-fills the active Paint layer

    // Selection tools: marquee/lasso/magic-wand build an active selection
    // region on the canvas; paint/erase/bucket clip their writes to it (see
    // onMaskBrushPoint/onEraseAt/onBucketFillRequested).
    void setSelectMarqueeMode(bool on);
    void setSelectLassoMode(bool on);
    void setSelectMagicWandMode(bool on);
    void setMagicWandTolerance(int tolerance);
    void setSelectBrushMode(bool on);
    void setSelectBrushRadius(double normRadius);
    void setSelectionFeather(double normRadius);
    double selectionFeatherNorm() const { return m_selectionFeatherNorm; }
    void clearActiveSelection();
    bool hasActiveSelection() const { return m_hasSelection; }

    // Clone stamp: paints into the active Paint-type layer (auto-created if
    // needed, same as Pen/Brush), sampling per-dab color from the pre-
    // adjustment source image (m_geomImg) at the offset source point.
    void setCloneMode(bool on);

    // Remove Object tool: paint a stroke over an unwanted object; on stroke
    // release, a content-aware fill (InpaintTool) is computed once and
    // cached as a new, non-destructive RemoveObjectOp layer.
    void setRemoveObjectMode(bool on);
    void setRemoveObjectBrush(int radiusDisplayPx);
    const QVector<RemoveObjectOp> &removals() const { return m_adj.removals; }
    int activeRemovalIndex() const { return m_activeRemoval; }
    void selectRemoval(int index);
    void setRemovalVisible(int index, bool visible);
    void deleteRemoval(int index);

    // Move tool.
    void setMoveMode(bool on);

    // Text tool.
    void setTextMode(bool on);
    void deleteActiveText();
    int activeTextIndex() const { return m_activeText; }
    // Style of the active text, or the "next new text" defaults if none is
    // selected — what the options bar should display.
    TextOp activeTextStyle() const;
    // Each setter applies to the active text if one is selected, otherwise
    // updates the defaults used for the next newly-placed text.
    void setTextFont(const QString &family, double pixelSize, bool bold, bool italic);
    void setTextColor(const QColor &color);
    void setTextOutline(bool enabled, const QColor &color, double width);
    void setTextShadow(bool enabled, const QPointF &offset, double blur, double opacity,
                       const QColor &color);
    void setTextBackground(bool enabled, const QColor &color, double opacity, double padding);

    // Shape tool.
    void setShapeMode(bool on);
    void setActiveShapeType(ShapeType t);
    void deleteActiveShape();
    int activeShapeIndex() const { return m_activeShape; }
    // Style of the active shape, or the "next new shape" defaults if none is
    // selected — what the options bar should display.
    ShapeOp activeShapeStyle() const;
    // Each setter applies to the active shape if one is selected, otherwise
    // updates the defaults used for the next newly-created shape.
    void setShapeSides(int sides);
    void setShapeInnerRadiusRatio(double ratio);
    void setShapeFill(bool enabled, const QColor &color);
    void setShapeStroke(bool enabled, const QColor &color, double width);
    const QSet<int> &selectedShapes() const { return m_selectedShapes; }
    // Select a shape (e.g. a Layers-panel row click). If it belongs to a
    // group, the whole group is selected, matching a canvas click on a
    // grouped shape — see onShapeSelected.
    void selectShape(int index);
    void setShapeVisible(int index, bool visible);
    void groupSelectedShapes();   // Ctrl+G: tag the current multi-selection as one group
    void ungroupSelectedShapes(); // Ctrl+Shift+G: clear the group tag of every selected shape

    // Local adjustment masks.
    void setMaskMode(bool on);              // enter/leave mask editing on the canvas
    // append + select; returns its index. shapeType only applies when
    // type == MaskType::Shape (sets Mask::shapeType on the new layer).
    int addMask(MaskType type, ShapeType shapeType = ShapeType::Rectangle);
    int addImageLayer(const QString &path); // append an image layer; returns its index
    // Rasterizes an .svg file via QSvgRenderer and adds it as an image layer
    // through the same path as addImageLayer(); returns its index, or -1 if
    // the file couldn't be parsed/rendered.
    int addSvgLayer(const QString &svgPath);
    // Adds an already-rendered image (e.g. from SvgEditorWindow's "Send to
    // Retouch as Layer") as an image layer, saving it as a managed PNG asset
    // first so it goes through the same addImageLayer path as any other
    // image layer.
    int addImageLayerFromImage(const QImage &image, const QString &suggestedName);
    // Places an AssetStamp's cutout as a new image-filled Shape mask, sized
    // to a fraction of canvas width matching the asset's aspect ratio;
    // returns its index, or -1 if there's no image/canvas to place it on.
    int insertAssetStamp(const QString &imagePath, const QSize &nativeSize, const QString &name);
    // Photoshop-style Ctrl/Alt+Backspace: flat-fills the active layer with
    // `color`. Background -> inserts a new full-canvas Paint layer above it;
    // Paint -> appends full coverage to its existing stroke. No-op for mask
    // types that don't represent flat raster content (Shape/Text/Image/etc).
    void fillActiveMask(const QColor &color);
    int duplicateActiveMask();              // copy + insert above; returns its index
    void selectMask(int index);             // -1 = none
    void deleteActiveMask();
    void setActiveMaskType(MaskType type);  // add/remove/change the layer's mask
    void setActiveMaskAdjust(const MaskAdjust &a);
    void setActiveMaskImageTransform(double offsetX, double offsetY, double scaleX,
                                     double scaleY, bool lockRatio);
    void setActiveMaskShape(bool inverted, double feather, double hardness,
                            double brushRadius, bool autoMask);
    // No-op unless the active layer is Radial/Linear. See Mask::isGradientFill.
    void setActiveMaskGradientFill(bool enabled, const QColor &colorA, const QColor &colorB);
    void setPaintColor(const QColor &color); // no-op unless the active layer is MaskType::Paint
    // Sets the pencil-grade to bake into new Pen-tool dabs on this Paint
    // layer (see BrushStrokePoint::penGrade); no-op unless the active layer
    // is MaskType::Paint. -6.0(6B)..5.0(5H).
    void setActivePenGrade(double grade);
    // Whether the next painted dabs on the active Paint layer are tagged as
    // Pen (grade-driven hardness/opacity/grain) vs. plain Brush dabs — set by
    // RetouchWindow when the Pen/Brush toolbar toggle changes so both tools
    // can freely share one Paint-type layer (see onMaskBrushPoint).
    void setPenToolActive(bool on) { m_penToolActive = on; }
    void setActiveMaskText(const QString &text, const QString &family, double pixelSize,
                           bool bold, bool italic); // no-op unless the active layer is MaskType::Text
    void setActiveMaskOpacity(double opacity);       // 0..1
    void setActiveMaskBlend(BlendMode mode);
    void setActiveMaskVisible(bool visible);
    void setMaskVisible(int index, bool visible); // toggle any layer, not just the active one
    void setActiveMaskName(const QString &name);
    void setMaskName(int index, const QString &name); // renames any layer, not just the active one
    void moveMask(int from, int to);                 // reorder within the stack
    // Applies a full new ordering of masks() indices (original indices, e.g.
    // from a Layers-panel drag); leftGroupIndices (also original indices)
    // are layers whose drag pulled them out of their group's nested rows, so
    // their groupId is cleared before the reorder is applied.
    void reorderMasks(const QVector<int> &newOrder, const QVector<int> &leftGroupIndices = {},
                       const QVector<QPair<int, QString>> &joinGroups = {});
    void groupMasks(const QVector<int> &indices);     // tag layers as one group; kept contiguous
    void ungroupMasks(const QVector<int> &indices);   // clear the group tag of the given layers' groups
    void renameGroup(const QString &groupId, const QString &name); // rename every layer's group
    // Sets a group's own opacity/visibility/blend (see MaskGroup in
    // Adjustments.h), creating its MaskGroup entry on first use (a group
    // with no entry yet behaves as fully-visible/opacity-1.0/Normal-blend).
    void setGroupProperties(const QString &groupId, double opacity, bool visible, BlendMode blend);
    const QVector<Mask> &masks() const { return m_adj.masks; }
    const QVector<MaskGroup> &groups() const { return m_adj.groups; }
    int activeMaskIndex() const { return m_activeMask; }

    // True when there is a currently-selected layer and its mask type matches
    // requiredType exactly. Used to gate the Brush/Paint, Radial, and Linear
    // tool toggles: those tools operate on the existing selection rather than
    // always creating a new layer, so they must only be enabled when the
    // selection is of the matching type.
    bool canActivateTool(MaskType requiredType) const {
        return m_activeMask >= 0 && m_activeMask < m_adj.masks.size() &&
               m_adj.masks[m_activeMask].type == requiredType;
    }

    // The Background layer (the tab's own loaded base photo) is a normal
    // MaskType::Background entry in m_adj.masks now — no separate
    // hidden/deleted bookkeeping. These are thin convenience lookups; hiding/
    // deleting/reordering it goes through the exact same generic
    // selectMask()/deleteActiveMask()/Mask::visible flow as any other layer.
    int backgroundMaskIndex() const;
    bool hasBackgroundLayer() const { return backgroundMaskIndex() >= 0; }
    bool backgroundLayerVisible() const;
    void showOriginal(bool on); // press-and-hold before/after
    void zoomFit();
    void setZoomPercent(double percent);
    double zoomPercent() const;

    bool isDirty() const { return m_dirty; }
    bool hasEdits() const;
    void saveEdits(); // write the sidecar and mark clean
    // Write a self-contained .ploom project file (base pixels + all edits)
    // at `path` and re-key this tab to it. Returns false on I/O failure.
    bool saveProjectFile(const QString &path);

    void undo();
    void redo();
    bool canUndo() const { return m_histIndex > 0; }
    bool canRedo() const { return m_histIndex >= 0 && m_histIndex < m_history.size() - 1; }

    const QVector<Adjustments> &history() const { return m_history; }
    int historyIndex() const { return m_histIndex; }
    void jumpToHistory(int index); // set position to index and apply it

    QImage renderFullRes() const; // for export

    // Latest toned preview render (display-scaled). Empty until first render.
    QImage previewImage() const { return m_lastEdited; }

    // Per-layer Levels histogram feed: while enabled (Layers dock visible),
    // the render pipeline additionally produces the cumulative composite
    // through the active mask, pushed to whoever wants to draw its histogram.
    void setMaskPreviewEnabled(bool on);
    QImage maskPreviewImage() const { return m_maskPreviewImage; }

signals:
    void decoded(bool ok);
    void cropPending(bool hasSelection);
    void cropModeExited(); // crop applied internally (e.g. via Enter)
    void wbPicked();       // white balance was set from the eyedropper
    void quickColorPicked(const QColor &color); // right-click-hold wheel committed a paint color
    // Emitted when the save/edit state changes (for the thumbnail badge).
    void editStateChanged(bool dirty, bool hasEdits);
    void zoomChanged(double percent);
    void historyChanged(bool canUndo, bool canRedo);
    void historyListChanged(); // history entries or current index changed
    void adjustmentsReplaced(); // undo/redo swapped the whole adjustment set
    void healBrushChanged(int radiusDisplayPx); // ctrl+wheel resized the brush
    void eraseBrushChanged(int radiusDisplayPx); // ctrl+wheel resized the erase brush
    void previewUpdated(); // a new toned preview render is available
    void maskPreviewUpdated(); // a new per-layer histogram source image is available
    void masksChanged();   // mask list or active-mask geometry changed
    void maskBrushChanged(double radiusNorm); // ctrl+wheel resized the mask brush
    void selectBrushChanged(double radiusNorm); // ctrl+wheel resized the selection brush
    void textsChanged();   // text list, active text, or its style changed
    void shapesChanged();  // shape list, active shape, or its style changed
    void removeObjectBrushChanged(int radiusDisplayPx); // ctrl+wheel resized the brush
    void removalsChanged(); // removal list or active removal changed
    // A canvas click-to-select fallback (ImageCanvas::objectClicked) picked
    // a layer and this tab already selected it (selectMask) — RetouchWindow
    // listens to flip the matching toolbar tool button on (mirroring a
    // manual click on that button), so the toolbar stays in sync.
    void objectToolRequested(MaskType type);

private slots:
    void onDecodeFinished();
    void onCanvasCrop(const QRect &r, double angleDegrees);
    void onColorPicked(const QColor &c);
    void onColorRangePickStarted(const QColor &c);
    void onColorRangeDragged(int dxPixels);
    void onColorRangeReleased();
    void onHealAt(const QPoint &imgPoint);
    void onTextPlaceRequested(const QPoint &imgPoint);
    void onTextSelected(int index);
    void onTextDeselected();
    void onTextMoved(int index, const QPointF &newImgPos);
    void onTextRotated(int index, double newRotationDegrees);
    void onTextEditRequested(int index);
    void onTextEditCommitted(int index, const QString &text);
    void onTextEditCancelled(int index);
    void onTextLiveContentChanged(int index, const QString &text);
    void onTextDeleteRequested(int index);
    void onTextResizeStarted(int index);
    void onTextResized(int index, double ratio);
    void onShapeCreateRequested(ShapeType type, const QRectF &imageRect);
    void onShapeSelected(int index);
    void onObjectClicked(MaskType type, int markerIndex);
    void onPaintLayerMoveStarted(int markerIndex);
    void onPaintLayerMoveDelta(const QPointF &deltaNorm);
    void onPaintLayerMoveFinished();
    void onShapeDeselected();
    void onShapeMoved(int index, const QPointF &deltaImage);
    void onShapeResized(int index, const QRectF &newImageRect);
    void onShapeLineEndpointsChanged(int index, const QPointF &p1, const QPointF &p2);
    void onShapeRotated(int index, double newRotationDegrees);
    void onShapeDeleteRequested(int index);
    void onShapeGroupDeleteRequested(const QList<int> &indices);
    void onShapeDuplicateRequested(int index);
    void onShapeGroupDuplicateRequested(const QList<int> &indices);
    void onShapeToggleSelectRequested(int index);
    void onShapeGroupMoveStarted(const QList<int> &indices);
    void onShapeGroupMoveRequested(const QList<int> &indices, const QPointF &deltaImage);
    void onShapeGroupResizeRequested(const QList<int> &indices, const QPointF &anchorImage,
                                     double scaleX, double scaleY);
    void onEraseAt(const QPointF &ptNorm, bool newStroke);
    void onEraseFinished();
    void onRemoveObjectAt(const QPointF &ptNorm);
    void onRemoveObjectFinished();
    void onRenderDone(const QImage &result, const QImage &maskSnapshot,
                      const QImage &belowSnapshot, int belowSnapshotIndex,
                      const QRect &dirtyRect = QRect());
    void onMaskRadial(const QPointF &centerNorm, double radiusNorm);
    void onMaskLinear(const QPointF &p0Norm, const QPointF &p1Norm);
    void onMaskBrushPoint(const QPointF &ptNorm, bool erase, bool newStroke, double pressure = 1.0);
    void onBucketFillRequested(const QPointF &ptNorm);
    void onMaskEditFinished();
    void onQuickColorPicked(const QColor &c);
    void onSelectionPathChanged(const QPainterPath &pathNorm, bool hasSelection);
    void onSelectionFeatherChanged(double normRadius);
    void onCloneStrokePoint(const QPointF &ptNorm, const QPointF &sourceNorm, bool newStroke,
                            double pressure);
    void onCloneFinished();

private:
    void rebuildGeom();  // recompute oriented(+crop) full image + display base
    // Rotates a geom-space (display, unscaled) delta vector back into
    // oriented-space by -m_geomRotationDeg — unlike a point, a delta has no
    // translation component to run through m_geomToOriented.
    QPointF orientedDelta(const QPointF &geomDelta) const;
    void retone();       // fast preview (defers clarity/sharpen while dragging)
    void retoneFull();   // full preview incl. clarity/sharpen (after idle)
    // Fast per-move brush/erase-stroke preview: reuses a cached composite of
    // everything below the active mask (captured by a prior retone()/
    // requestRender belowSnapshotIndex capture) and only recomposites the
    // active mask and the layers above it, instead of the full stack. Falls
    // back to a normal retone() (which also (re-)captures that cache) when
    // there's no valid cache yet for the current active mask, `newStroke` is
    // true, or a Background-type mask sits at or above the active mask.
    void retoneDrag(bool newStroke);
    void requestRender(const QImage &src, const Adjustments &adj, int maskSnapshotIndex = -1,
                       int belowSnapshotIndex = -1); // coalesced, async, uses current geometry members
    void requestDragRender(const QImage &belowSnapshot, int dragMaskIndex, const Adjustments &adj);
    int maskPreviewIndex() const { return m_maskPreviewEnabled ? m_activeMask : -1; }
    void markEdited(); // set dirty + emit editStateChanged
    void commitHistory();     // snapshot current adjustments (coalesced)
    void applyHistoryState(); // apply m_history[m_histIndex]
    void updateHealSpots();   // push heal-op markers (display coords) to the canvas
    void updateTextMarkers(); // push text-op markers (display coords) to the canvas
    void updateShapeMarkers(); // push shape-op markers (display coords) to the canvas
    // Marker index (position within the Shape-filtered view ImageCanvas sees)
    // -> real index into m_adj.masks, rebuilt every updateShapeMarkers() call.
    // Returns -1 for an out-of-range marker index.
    int shapeMaskIndex(int markerIndex) const;
    // Marker index (position within the TextBox-filtered view ImageCanvas
    // sees) -> real index into m_adj.masks, rebuilt every updateTextMarkers()
    // call. Returns -1 for an out-of-range marker index.
    int textMaskIndex(int markerIndex) const;
    void updateRemovalMarkers(); // push removal-op markers (display coords) to the canvas
    // Push Paint-layer/image-layer bounding-box markers (display coords) to
    // the canvas for the click-to-select fallback; rebuilds
    // m_paintMaskIndices/m_imageLayerMaskIndices. See ImageCanvas::objectClicked.
    void updateObjectMarkers();
    // Oriented (rotate/flip), pre-crop image with heals AND already-committed
    // removals baked in — the "source" a new removal's inpaint reads from,
    // and the same image rebuildGeom() paints new removals onto.
    QImage orientedPreCropSource() const;
    void pushMaskGizmo();     // sync active mask geometry to the canvas
    void kickoffImageLayerDecode(const QString &path); // async-decode an image layer's source
    void loadShapeImageCache(const QString &path); // sync-decode an image-filled Shape's asset PNG
    QString copyImageLayerAsset(const QString &sourcePath); // copy a layer source next to m_path so it survives move/delete
    void setupCanvasAndWiring(); // shared canvas creation + connect()s for both constructors
    // Inserts a MaskType::Background entry (sourced from this tab's own base
    // photo) at the bottom of m_adj.masks if one isn't already there —
    // called once, right after the sidecar is loaded/a blank canvas is
    // created, before the undo history is seeded, so it never itself counts
    // as an "edit". A no-op for a sidecar that already restored one (fresh
    // open) or that explicitly had it migrated in (see EditSidecar::load).
    void ensureBackgroundMask();

    QString m_path;
    QImage m_base;   // full-res decoded RAW (immutable)
    Adjustments m_adj;
    // Working color space this tab's RAW was decoded with (or sRGB for
    // blank/project tabs) — set once at construction, read by export.
    WorkingColorSpace m_workingColorSpace = WorkingColorSpace::sRGB;

    bool m_cropMode = false;
    bool m_maskMode = false;
    bool m_penToolActive = false; // see setPenToolActive
    int m_activeMask = -1; // index into m_adj.masks, or -1
    bool m_textMode = false;
    int m_activeText = -1;   // marker index (position within the TextBox-filtered
                              // view of m_adj.masks, see m_textMaskIndices), or -1
    int m_newTextIndex = -1; // marker index of a just-placed, not-yet-committed draft, or -1
    int m_textEditIndex = -1; // marker index currently open in the inline editor, or -1
    // marker index -> m_adj.masks index, rebuilt each updateTextMarkers() call.
    QVector<int> m_textMaskIndices;
    TextOp m_textDefaults;   // style applied to the next newly-placed text
    double m_textResizeStartPixelSize = 48.0; // captured at corner-drag start

    bool m_shapeMode = false;
    int m_activeShape = -1;   // marker index (position within the Shape-filtered
                               // view of m_adj.masks, see m_shapeMaskIndices), or -1
    // marker index -> m_adj.masks index, rebuilt each updateShapeMarkers() call.
    QVector<int> m_shapeMaskIndices;
    // marker index -> m_adj.masks index for the click-to-select fallback,
    // rebuilt each updateObjectMarkers() call.
    QVector<int> m_paintMaskIndices;
    QVector<int> m_imageLayerMaskIndices;

    // Move tool: Paint/Brush layer drag state, captured at
    // paintLayerMoveStarted and reapplied (not compounded) on every
    // paintLayerMoveDelta, mirroring shapeMoved's start-snapshot convention.
    int m_paintMoveMaskIndex = -1;
    QVector<BrushStrokePoint> m_paintMoveStartStroke;
    QImage m_paintMoveStartFillMask;
    bool m_paintMoveSelectionOnly = false;
    QPainterPath m_paintMoveSelectionPath;
    // Session-only cache of the fill mask as it was before ANY move this
    // session, plus how far it's been moved since. onPaintLayerMoveDelta
    // always redraws from this untouched master (not from the possibly
    // already-edge-clipped m.fillMask), so repeated moves don't compound
    // pixel loss at the frame edges. Invalidated (index reset to -1) by
    // anything that reassigns fillMask outside the move path, or reorders/
    // removes masks, since it's not safe across index shifts.
    int m_paintMoveMasterIndex = -1;
    QImage m_paintMoveMasterFillMask;
    QPointF m_paintMoveMasterOffsetNorm; // cumulative committed offset applied to the master
    QPointF m_paintMoveLastDelta; // this gesture's most recent delta, folded into the offset on finish
    ShapeOp m_shapeDefaults;  // style/type applied to the next newly-created shape
    QRectF m_shapeMoveStartRect;   // active shape's rect at move-drag start
    QPointF m_shapeMoveStartP1, m_shapeMoveStartP2; // active Line's endpoints at move-drag start
    QSet<int> m_selectedShapes;   // multi-selection; superset of m_activeShape when non-empty
    QMap<int, QRectF> m_shapeGroupStartRect;   // per-shape rect at group-move/resize-drag start
    QMap<int, QPointF> m_shapeGroupStartP1, m_shapeGroupStartP2; // per-Line endpoints, same
    QMap<int, double> m_shapeGroupStartStrokeWidth; // per-shape stroke width, same (for group resize)
    bool m_dirty = false; // unsaved changes since last save/load
    int m_healRadiusDisplay = 20; // brush radius in display pixels
    int m_eraseRadiusDisplay = 20; // erase brush radius in display pixels
    int m_removeObjectRadiusDisplay = 24; // remove-object brush radius in display pixels
    int m_activeRemoval = -1; // index into m_adj.removals, or -1
    // In-progress remove-object stroke (oriented-image coords, pre-crop),
    // accumulated between press and release, and the mask painted for it so
    // far; committed as a new RemoveObjectOp on release.
    QVector<QPointF> m_removeObjectStroke;
    QImage m_removeObjectMaskDraft; // full oriented-image size, alpha = coverage so far
    // Mirrors ImageCanvas's active selection (see onSelectionPathChanged),
    // width-normalized. Paint/erase/bucket handlers clip to this when set.
    QPainterPath m_selectionPath;
    bool m_hasSelection = false;
    double m_selectionFeatherNorm = 0.0; // mirrors ImageCanvas::selectionFeatherNorm()
    bool m_selectionStrokeBroken = false; // a brush/pen dab was just skipped for being outside the selection
    bool m_cloneStrokeBroken = false; // same idea as m_selectionStrokeBroken, for Clone dabs
    bool m_removeObjectDragging = false;
    double m_removeObjectRadiusUsed = 24.0; // last dab's radius (oriented-image px), for the new op
    // The content-aware fill for a committed stroke runs on a QtConcurrent
    // worker thread (InpaintTool::inpaint is too slow to run on the GUI
    // thread without freezing it); these track that in-flight job so a
    // second stroke can't be started until it finishes, and drive a progress
    // dialog fed by InpaintTool's onProgress callback.
    bool m_removeObjectBusy = false;
    QProgressDialog *m_removeObjectProgress = nullptr;
    int m_crIndex = -1;      // colorRanges entry being dragged, or -1
    int m_crBaseAmount = 0;  // that entry's amount at drag start
    QRect m_pendingCrop; // in oriented-image coords, awaiting Apply
    double m_pendingCropAngle = 0.0; // straighten angle for m_pendingCrop, awaiting Apply

    QImage m_geomImg;            // oriented (+crop/straighten unless cropMode), full res, untoned
    // Maps oriented (pre-crop) pixel coords <-> m_geomImg pixel coords (unscaled).
    // Identity when uncropped or in crop mode; a rotate-about-crop-center-then-
    // translate affine when a straightened crop is committed. m_geomRotationDeg
    // is that same crop's angle (0 otherwise), added to heal/text/shape ops'
    // own rotation so their orientation follows the straightened content.
    QTransform m_orientedToGeom, m_geomToOriented;
    double m_geomRotationDeg = 0.0;
    QImage m_scaled;             // display base, untoned
    double m_scaleFromGeom = 1.0;

    ImageCanvas *m_canvas = nullptr;
    QFutureWatcher<QImage> *m_watcher = nullptr;
    QTimer *m_fullRenderTimer = nullptr; // fires the full render after dragging stops
    QTimer *m_commitTimer = nullptr;     // coalesces edits into one undo step
    QVector<Adjustments> m_history;      // committed adjustment snapshots
    int m_histIndex = -1;

    QThread *m_renderThread = nullptr;
    RenderWorker *m_renderWorker = nullptr;
    bool m_rendering = false;   // a render is in flight
    bool m_hasPending = false;  // a newer request arrived while rendering
    QImage m_pendingSrc;
    Adjustments m_pendingAdj;
    int m_pendingMaskIdx = -1;
    int m_pendingBelowIdx = -1;
    bool m_pendingIsDrag = false; // pending request is a requestDragRender, not requestRender
    QImage m_pendingDragBelow;
    int m_pendingDragIndex = -1;
    // Caps how often drag-preview renders are dispatched: each one recomposites
    // the full active-layer buffer (see applyMasks), so without this a fast
    // mouse re-queues a new full render the instant the previous one finishes,
    // pinning a CPU core for the whole stroke. Final (non-drag) renders are
    // unthrottled. See requestDragRender()/onRenderDone().
    QElapsedTimer m_lastDragRenderTime;
    bool m_dragThrottlePending = false; // a delayed dispatch is already scheduled
    static constexpr int kDragRenderMinIntervalMs = 16; // ~60fps cap

    // Cache for retoneDrag(): the composite through everything below
    // m_dragSnapshotIndex, captured the last time a full render requested it.
    // Valid only while m_dragSnapshotIndex still matches the active mask;
    // see retoneDrag()/onRenderDone().
    QImage m_dragBelowSnapshot;
    int m_dragSnapshotIndex = -1;
    bool m_dragSnapshotValid = false;

    QImage m_lastEdited;          // most recent edited render (for before/after)
    bool m_showingOriginal = false;

    bool m_maskPreviewEnabled = false; // Layers dock visible -> compute per-layer histogram source
    QImage m_maskPreviewImage;
};
