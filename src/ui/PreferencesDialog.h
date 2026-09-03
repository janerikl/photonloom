#pragma once

#include <QDialog>

#include "edit/ColorSpace.h"

class QComboBox;
class QSpinBox;
class QPushButton;
class QWidget;
class QTreeWidget;
class QTreeWidgetItem;

// File → Preferences… dialog. Two-column layout: a category list on the
// left, a QStackedWidget of pages on the right. Currently two pages:
// "General" (camera-model dropdown and the AF coordinate frame size used by
// click-to-focus — AF frame is remembered per model in QSettings, this
// dialog is the single writer) and "Keyboard Shortcuts" (live, editable —
// backed by ShortcutRegistry; double-click a shortcut cell to rebind it).
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

public slots:
    // Select a model by id (used for auto-detect). No-op if id is empty or
    // already the current selection, so a manual override survives reconnects.
    void selectModelById(const QString &id);
    // Push calibrated values into the fields and persist them for the model.
    void setAfFrame(int w, int h);

signals:
    void afFrameSizeChanged(int w, int h);
    void calibrationRequested();

private:
    void onModelChanged();
    void onFrameEdited();
    void loadFrameForCurrentModel();
    QString currentModelId() const;
    QWidget *buildGeneralPage();
    QWidget *buildShortcutsPage();
    void reloadShortcutsTree();
    void beginEditShortcut(QTreeWidgetItem *item);

    QComboBox *m_model = nullptr;
    QSpinBox *m_frameW = nullptr;
    QSpinBox *m_frameH = nullptr;
    QPushButton *m_calibrate = nullptr;
    QComboBox *m_workingColorSpace = nullptr;
    QTreeWidget *m_shortcutsTree = nullptr;
    QPushButton *m_resetShortcut = nullptr;
    QPushButton *m_resetAllShortcuts = nullptr;
};

// Returns the persisted AF frame for a model id (per-model override, else the
// model's built-in default, else 640x426). Shared by the dialog and startup
// seeding in RetouchWindow.
void afFrameForModel(const QString &id, int &w, int &h);

// Global default working color space applied to newly-opened/newly-decoded
// RAW files (does not retroactively affect already-open tabs). Shared by the
// dialog and RetouchWindow's open/new-tab flows.
WorkingColorSpace defaultWorkingColorSpace();
void setDefaultWorkingColorSpace(WorkingColorSpace space);
