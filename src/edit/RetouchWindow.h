#pragma once

#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QString>

#include "edit/Adjustments.h"
#include "edit/AdjustmentPreset.h"
#include "edit/AssetStamp.h"
#include "edit/ViewTemplate.h"
#include "edit/ExportPreset.h"
#include "ui/ColorSwatchWidget.h"

class AssetsPanel;

class RetouchTab;
class FilmstripWidget;
class CurveEditor;
class QTabWidget;
class QSlider;
class QPushButton;
class QToolButton;
class QAbstractButton;
class QCheckBox;
class FlyoutToolButton;
class QLabel;
class QToolBar;
class QStackedWidget;
class QDockWidget;
class QListWidget;
class TetherView;
class PreferencesDialog;
class LevelsPanel;
class LayersPanel;
class LayerAdjustmentsPanel;
class BrowseTab;

// Separate top-level window for retouching photos. Own filmstrip selector plus
// one tab per open photo, an adjustments dock, and JPEG/PNG export.
class RetouchWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit RetouchWindow(QWidget *parent = nullptr);

    // Add a photo to the selector filmstrip (thumbnail via embedded JPEG). No-op
    // if already present.
    void addToFilmstrip(const QString &path);
    // Open the photo in a tab (or activate its existing tab).
    void openPhoto(const QString &path);
    // Create a new blank/untitled tab (File > New). Never touches the filmstrip.
    void createUntitledTab(const QSize &size);

    enum class Mode { Retouch, Tether, Svg, Browse };
    void setMode(Mode mode);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onOpenSession();
    void onOpenPhotos();
    void onOpenProject();
    void onNewDocument();
    void onSave();
    void onSaveAll();
    void onSaveAsProject();
    void reKeyTab(RetouchTab *tab, const QString &path);
    void onFilmstripSelected(const QString &path);
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onDeleteRequested(const QStringList &paths);
    void onRenameRequested(const QString &path);
    void onRatingChanged(const QString &path, int rating);
    void onToneChanged();
    void onExport();

private:
    void buildDock();
    void loadSession(const QString &dir);      // scan a folder for NEFs into the filmstrip
    void rebuildRecentSessionsMenu();          // repopulate the recent-session items in the File menu
    void rebuildRecentFilesMenu();             // repopulate the recent-file items in the File menu
    void rebuildRecentProjectsMenu();          // repopulate the recent-project items in the File menu
    void buildToolPanel(); // narrow left icon toolbar: Zoom / Crop / Spot Heal tools
    void deselectAllTools(); // uncheck all left-bar tools and exit their modes
    void buildToolOptionsBar(); // contextual per-tool options row under the main toolbar
    RetouchTab *currentTab() const;
    void syncDockFromTab();
    void setDockEnabled(bool enabled);
    void applyModeChrome(Mode mode);
    class QAction *m_tetherModeAction = nullptr;
    class QAction *m_retouchModeAction = nullptr;
    class QAction *m_svgModeAction = nullptr;
    class QAction *m_browseModeAction = nullptr;
    void onBrowseOpenRequested(const QStringList &paths);

    QTabWidget *m_tabs = nullptr;
    FilmstripWidget *m_filmstrip = nullptr;
    QSet<QString> m_filmstripPaths;
    QMap<QString, RetouchTab *> m_openTabs;
    int m_untitledCounter = 0;

    // Unified window: central stack swaps editing tabs (page 0) / tether
    // (page 1) / the SVG icon/logo editor (page 2).
    QStackedWidget *m_modeStack = nullptr;
    TetherView *m_tetherView = nullptr;
    class SvgEditorTab *m_svgEditorTab = nullptr;
    BrowseTab *m_browseTab = nullptr;
    PreferencesDialog *m_prefsDialog = nullptr;
    QDockWidget *m_controlsDock = nullptr; // camera controls, shown in Tether mode
    QToolBar *m_tetherToolBar = nullptr;   // Connect/Disconnect/LiveView/Capture/…

    // Promoted from constructor locals so mode chrome can enable/disable them.
    class QAction *m_saveAction = nullptr;
    class QAction *m_saveAllAction = nullptr;
    class QAction *m_saveAsProjectAction = nullptr;
    class QAction *m_exportAction = nullptr;

    // File menu + anchors for the rebuildable recent-sessions section. The
    // recent items live between these two separators (inserted before
    // m_recentEndSeparator); both separators are hidden when the list is empty.
    class QMenu *m_fileMenu = nullptr;
    class QAction *m_recentBeginSeparator = nullptr; // separator above the recent items
    class QAction *m_recentEndSeparator = nullptr;   // separator below the recent items
    QList<class QAction *> m_recentActions;          // current recent-session menu entries

    // Anchors for the rebuildable recent-files section (individually opened
    // photos, as distinct from recent session folders above).
    class QAction *m_recentFilesBeginSeparator = nullptr;
    class QAction *m_recentFilesEndSeparator = nullptr;
    QList<class QAction *> m_recentFileActions;

    // Anchors for the rebuildable recent-projects section (Photonloom .ploom
    // project files, opened or saved).
    class QAction *m_recentProjectsBeginSeparator = nullptr;
    class QAction *m_recentProjectsEndSeparator = nullptr;
    QList<class QAction *> m_recentProjectActions;

    // View menu togglable panels.
    QToolBar *m_toolsBar = nullptr; // left icon bar: Zoom / Crop / Spot Heal
    QDockWidget *m_adjustmentsDock = nullptr;
    QDockWidget *m_orientationDock = nullptr;
    QDockWidget *m_historyDock = nullptr;
    QListWidget *m_historyList = nullptr;
    QDockWidget *m_levelsDock = nullptr;
    LevelsPanel *m_levelsPanel = nullptr;
    void buildViewMenu();
    void applyDefaultDockLayout(); // re-apply the default dock arrangement (used on first launch + Reset Panels)
    class QAction *m_filmstripAction = nullptr; // View menu: promoted so templates can save/restore its checked state
    class QAction *m_rulersAction = nullptr;    // View menu: promoted so templates can save/restore its checked state

    // Named view templates (dock/toolbar visibility snapshots), reachable from
    // View > Layouts. Two built-in presets (Painting, Photo Editing) are
    // synthesized on demand; user templates persist via m_viewTemplateStore.
    ViewTemplateStore m_viewTemplateStore;
    class QMenu *m_layoutsMenu = nullptr;
    void rebuildLayoutsMenu();
    void applyViewTemplate(const ViewTemplate &t); // restoreState() + filmstrip/rulers + re-assert app-controlled visibility
    void applyBuiltInPaintingLayout();
    void applyBuiltInPhotoEditingLayout();
    void onSaveViewTemplate();
    void onDeleteViewTemplate();
    void restoreWindowState(); // deferred restoreGeometry()/restoreState(), run after the window is shown
    void buildOrientationDock();
    void buildHistoryDock();
    void buildLevelsDock();
    void refreshHistoryPanel();  // rebuild the list from the current tab
    void refreshLevels();        // push the current tab's preview + levels into the panel

    // Dock controls.
    QSlider *m_brightness = nullptr;
    QSlider *m_contrast = nullptr;
    QSlider *m_highlights = nullptr;
    QSlider *m_shadows = nullptr;
    QSlider *m_saturation = nullptr;
    QSlider *m_vibrance = nullptr;
    QSlider *m_temperature = nullptr;
    QSlider *m_tint = nullptr;
    QSlider *m_denoise = nullptr;
    QSlider *m_clarity = nullptr;
    QSlider *m_sharpen = nullptr;
    QSlider *m_vignette = nullptr;
    QSlider *m_lightAngle = nullptr;
    QSlider *m_lightIntensity = nullptr;
    QSlider *m_flatStyle = nullptr;
    CurveEditor *m_curve = nullptr;
    QPushButton *m_wbPick = nullptr;
    QToolButton *m_beforeAfter = nullptr; // curtain icon beside the filmstrip: hold to reveal the original
    QSlider *m_zoomSlider = nullptr;
    QPushButton *m_zoomFit = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QPushButton *m_rotLeft = nullptr;
    QPushButton *m_rotRight = nullptr;
    QPushButton *m_flipH = nullptr;
    QPushButton *m_flipV = nullptr;
    QToolButton *m_moveToggle = nullptr; // left icon bar: move tool (drag layers/selection content)
    QToolButton *m_toolZoom = nullptr;  // left icon bar: zoom tool (marquee/Ctrl+wheel)
    QToolButton *m_cropToggle = nullptr; // left icon bar: crop tool
    QPushButton *m_cropApply = nullptr;
    QPushButton *m_cropReset = nullptr;
    class QComboBox *m_cropAspect = nullptr;
    QToolButton *m_healToggle = nullptr; // left icon bar: spot-heal tool
    QSlider *m_healBrush = nullptr;
    QPushButton *m_healClear = nullptr;
    QToolButton *m_brushToggle = nullptr; // left icon bar: paint-brush tool (requires a Paint layer already selected)
    QToolButton *m_bucketToggle = nullptr; // left icon bar: paint bucket (flood-fill), requires a Paint layer already selected
    QSlider *m_paintSize = nullptr;
    QLabel *m_paintSizePx = nullptr; // live "NNpx" readout next to m_paintSize
    bool m_paintSizeCustomized = false; // once the user drags m_paintSize, stop auto-defaulting it
    bool m_syncingPaintSize = false; // true while auto-defaulting m_paintSize (not a user edit)
    void updatePaintSizePxLabel();
    QSlider *m_paintHardness = nullptr;
    QSlider *m_paintOpacity = nullptr;
    class BrushPresetMenuButton *m_brushToolPresets = nullptr;
    FlyoutToolButton *m_maskToggle = nullptr; // left icon bar: local-mask tool
    QToolButton *m_eraseToggle = nullptr; // left icon bar: erase tool
    QSlider *m_eraseBrush = nullptr;
    QToolButton *m_removeObjectToggle = nullptr; // left icon bar: remove-object tool
    QSlider *m_removeObjectBrush = nullptr;
    QToolButton *m_penToggle = nullptr; // left icon bar: pen/pencil tool (requires a Pen layer already selected)
    QSlider *m_penSize = nullptr;
    bool m_penSizeCustomized = false; // once the user drags m_penSize, stop auto-defaulting it
    QSlider *m_penGrade = nullptr; // -6(6B)..5(5H), drives hardness/opacity/grain (see rasterizeBrush)
    QLabel *m_penGradeLabel = nullptr; // live grade readout ("HB", "2B", "3H", ...) next to m_penGrade
    void updatePenGradeLabel();
    QToolButton *m_textToggle = nullptr; // left icon bar: text tool
    class QFontComboBox *m_textFont = nullptr;
    class QSpinBox *m_textSize = nullptr;
    QToolButton *m_textBold = nullptr;
    QToolButton *m_textItalic = nullptr;
    QPushButton *m_textColorBtn = nullptr;
    QPushButton *m_textOutlineColorBtn = nullptr;
    class QDoubleSpinBox *m_textOutlineWidth = nullptr;
    QPushButton *m_textShadowColorBtn = nullptr;
    class QDoubleSpinBox *m_textShadowBlur = nullptr;
    class QDoubleSpinBox *m_textShadowOpacity = nullptr;
    QPushButton *m_textBgColorBtn = nullptr;
    class QDoubleSpinBox *m_textBgOpacity = nullptr;
    class QDoubleSpinBox *m_textBgPadding = nullptr;
    QPushButton *m_textDelete = nullptr;
    void updateTextOptionsFromTab(); // refresh the text options row from the active/selected text
    void setColorSwatchButton(QPushButton *btn, const QColor &color);

    QToolButton *m_shapeToggle = nullptr; // left icon bar: shape tool
    class QComboBox *m_shapeType = nullptr;
    class QSpinBox *m_shapeSides = nullptr;
    class QDoubleSpinBox *m_shapeInnerRatio = nullptr;
    QCheckBox *m_shapeFillEnabled = nullptr;
    QPushButton *m_shapeFillColorBtn = nullptr;
    QCheckBox *m_shapeStrokeEnabled = nullptr;
    QPushButton *m_shapeStrokeColorBtn = nullptr;
    class QDoubleSpinBox *m_shapeStrokeWidth = nullptr;
    QPushButton *m_shapeDelete = nullptr;
    void updateShapeOptionsFromTab(); // refresh the shape options row from the active/selected shape

    QToolButton *m_selectMarqueeToggle = nullptr; // left icon bar: rectangular selection
    QToolButton *m_selectLassoToggle = nullptr;   // left icon bar: freehand-polygon selection
    QToolButton *m_selectWandToggle = nullptr;    // left icon bar: magic-wand (color-similarity) selection
    QSlider *m_wandTolerance = nullptr;
    QToolButton *m_selectBrushToggle = nullptr;   // left icon bar: paint to add/subtract from selection
    QSlider *m_selectBrushSize = nullptr;
    QToolButton *m_cloneToggle = nullptr; // left icon bar: clone-stamp tool (requires a Paint layer already selected)
    QSlider *m_cloneSize = nullptr;
    ColorSwatchWidget *m_colorSwatch = nullptr; // left icon bar: fg/bg color swatch
    QDockWidget *m_layersDock = nullptr;
    LayersPanel *m_layersPanel = nullptr;
    void buildLayersDock();
    void refreshMaskPanel(); // refreshes the Layers panel (all sections) from the active tab
    // Hosts the per-layer editing sections (Tone/Colour/Tone Curve/Levels/
    // Detail & Effects/Masks/Remove Object), one at a time. Created lazily,
    // split next to m_layersDock, the first time a section is requested via
    // LayersPanel's right-click context menu (see LayersPanel::sectionRequested).
    QDockWidget *m_layerAdjustmentsDock = nullptr;
    LayerAdjustmentsPanel *m_layerAdjustmentsPanel = nullptr;
    void showLayerAdjustmentsSection(int section);
    QDockWidget *m_assetsDock = nullptr;
    void buildAssetsDock();
    void refreshAssetsPanel(); // rebuilds the Assets panel from m_assetStampStore
    void wireTabSignals(RetouchTab *tab);
    // Local-mask subtool selected via the tool's Photoshop-style flyout; a plain
    // click on the mask tool creates a mask of this type.
    MaskType m_activeMaskSubtool = MaskType::Radial;
    void openMaskFlyout();       // pop the subtool strip next to the mask button
    void setMaskSubtool(MaskType t); // set active subtool + refresh tool glyph
    void addActiveMask();        // create a mask of the active subtool
    // Shared mutual-exclusion helpers used by every left-bar tool toggle's
    // toggled(true) handler, replacing what used to be an identical
    // QSignalBlocker/setXMode(false) block copy-pasted per tool.
    void deactivateOtherToolButtons(QAbstractButton *keep);
    void deactivateAllToolModes(RetouchTab *tab);
    QLabel *m_statusLabel = nullptr;
    class QAction *m_undoAction = nullptr;
    class QAction *m_redoAction = nullptr;

    // Copy/paste/sync of portable edits (tone/colour/detail/curve/levels) across
    // photos. Geometry and spot-heals are image-specific and never copied.
    class QAction *m_copyEditsAction = nullptr;
    class QAction *m_pasteEditsAction = nullptr;
    class QAction *m_syncEditsAction = nullptr;
    class QAction *m_groupShapesAction = nullptr;
    class QAction *m_ungroupShapesAction = nullptr;

    // Pixel-selection actions (Deselect/Invert/Copy/Paste), operating on the
    // marquee/lasso/magic-wand selection, distinct from Copy/Paste Edits above.
    class QAction *m_deselectAction = nullptr;
    class QAction *m_invertSelectionAction = nullptr;
    class QAction *m_featherSelectionAction = nullptr;
    class QAction *m_copySelectionAction = nullptr;
    class QAction *m_pasteSelectionAction = nullptr;
    class QAction *m_saveSelectionAsAssetAction = nullptr;
    QImage m_selectionClipboard;         // extracted pixels, transparent outside the copied region
    QPoint m_selectionClipboardOffsetPx; // top-left of m_selectionClipboard within the full image
    int m_lastFeatherPx = 0; // remembered across invocations, like Photoshop's Feather dialog
    void onDeselect();
    void onInvertSelection();
    void onFeatherSelection();
    void onCopySelection();
    void onPasteSelection();
    void onSaveSelectionAsAsset();

    Adjustments m_editClipboard;
    bool m_hasEditClipboard = false;
    void onCopyEdits();
    void onPasteEdits();
    void onSyncEdits();
    void updateEditClipboardActions();
    // Overwrite dst's portable fields from src, preserving dst geometry & heals.
    static void mergePortable(const Adjustments &src, Adjustments &dst);
    // Apply the clipboard to one photo (open tab or closed sidecar). Returns
    // true if the photo changed. Reports counts to the caller.
    bool applyClipboardTo(const QString &path);

    // Develop presets: built-in templates + user-saved snapshots of the
    // portable adjustment fields, applied to the current tab from the
    // "Presets" menu.
    class QMenu *m_presetsMenu = nullptr;
    QList<class QAction *> m_presetActions;
    AdjustmentPresetStore m_adjustmentPresetStore;
    void rebuildPresetsMenu();
    void applyAdjustmentPreset(const AdjustmentPreset &preset);
    void onSaveAdjustmentPreset();
    void onDeleteAdjustmentPreset();

    // Contextual per-tool options row (shown under the main toolbar only while
    // a left-bar tool is selected).
    QToolBar *m_toolOptionsBar = nullptr;
    QStackedWidget *m_toolOptionsStack = nullptr;

    ExportPresetStore m_presetStore;
    bool m_syncing = false; // guard against feedback while loading dock from tab

    // Asset stamps: user-saved cut-out objects (Select a region, "Save
    // Selection as Asset"), reusable across any document via the Assets dock.
    AssetStampStore m_assetStampStore;
    AssetsPanel *m_assetsPanel = nullptr;
};
