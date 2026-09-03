#include "ui/PreferencesDialog.h"

#include "camera/CameraModels.h"
#include "ui/ShortcutRegistry.h"

#include <QComboBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QListWidget>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QFont>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QMap>

void afFrameForModel(const QString &id, int &w, int &h) {
    QSettings s;
    const cammodel::Model *m = cammodel::byId(id.toStdString());
    int dw = m ? m->afFrameW : 640;
    int dh = m ? m->afFrameH : 426;
    w = s.value(QString("af/models/%1/frameWidth").arg(id), dw).toInt();
    h = s.value(QString("af/models/%1/frameHeight").arg(id), dh).toInt();
}

WorkingColorSpace defaultWorkingColorSpace() {
    QSettings s;
    return static_cast<WorkingColorSpace>(
        s.value("colorSpace/workingSpace", int(WorkingColorSpace::sRGB)).toInt());
}

void setDefaultWorkingColorSpace(WorkingColorSpace space) {
    QSettings s;
    s.setValue("colorSpace/workingSpace", int(space));
}

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Preferences");
    resize(560, 420);

    auto *outer = new QVBoxLayout(this);

    auto *row = new QHBoxLayout;
    outer->addLayout(row, /*stretch=*/1);

    auto *nav = new QListWidget;
    nav->setFixedWidth(150);
    nav->addItem("General");
    nav->addItem("Keyboard Shortcuts");
    row->addWidget(nav);

    auto *pages = new QStackedWidget;
    pages->addWidget(buildGeneralPage());
    pages->addWidget(buildShortcutsPage());
    row->addWidget(pages, /*stretch=*/1);

    connect(nav, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    nav->setCurrentRow(0);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    outer->addWidget(buttons);

    // Restore the last-used model.
    QSettings s;
    QString cur = s.value("af/currentModel", "custom").toString();
    int idx = m_model->findData(cur);
    if (idx < 0) idx = m_model->findData("custom");
    {
        QSignalBlocker b(m_model);
        m_model->setCurrentIndex(idx);
    }
    loadFrameForCurrentModel();

    connect(m_model, &QComboBox::currentIndexChanged, this,
            [this](int) { onModelChanged(); });
    connect(m_frameW, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) { onFrameEdited(); });
    connect(m_frameH, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) { onFrameEdited(); });
}

QWidget *PreferencesDialog::buildGeneralPage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *form = new QFormLayout;
    layout->addLayout(form);

    m_model = new QComboBox;
    for (const cammodel::Model &m : cammodel::models())
        m_model->addItem(m.display, QString::fromLatin1(m.id));
    form->addRow("Camera model:", m_model);

    m_frameW = new QSpinBox;
    m_frameH = new QSpinBox;
    m_frameW->setRange(1, 20000);
    m_frameH->setRange(1, 20000);
    form->addRow("AF frame width:", m_frameW);
    form->addRow("AF frame height:", m_frameH);

    m_calibrate = new QPushButton("Calibrate…");
    form->addRow(QString(), m_calibrate);
    connect(m_calibrate, &QPushButton::clicked, this,
            [this] { emit calibrationRequested(); });

    auto *hint = new QLabel(
        "Click-to-focus calibration. Click Calibrate…, then click the point "
        "you want in focus, then click where it actually snapped sharp -- "
        "the AF frame size is solved automatically from those two clicks. "
        "Values are remembered per model.");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_workingColorSpace = new QComboBox;
    m_workingColorSpace->addItem(workingColorSpaceLabel(WorkingColorSpace::sRGB),
                                  int(WorkingColorSpace::sRGB));
    m_workingColorSpace->addItem(workingColorSpaceLabel(WorkingColorSpace::AdobeRGB),
                                  int(WorkingColorSpace::AdobeRGB));
    m_workingColorSpace->addItem(workingColorSpaceLabel(WorkingColorSpace::ProPhotoRGB),
                                  int(WorkingColorSpace::ProPhotoRGB));
    m_workingColorSpace->setCurrentIndex(
        m_workingColorSpace->findData(int(defaultWorkingColorSpace())));
    form->addRow("Working color space:", m_workingColorSpace);
    connect(m_workingColorSpace, &QComboBox::currentIndexChanged, this, [this](int) {
        setDefaultWorkingColorSpace(
            static_cast<WorkingColorSpace>(m_workingColorSpace->currentData().toInt()));
    });

    auto *colorHint = new QLabel(
        "Applies to newly-opened or newly-decoded RAW files only — already-open "
        "tabs keep the working space they were decoded with.");
    colorHint->setWordWrap(true);
    layout->addWidget(colorHint);

    layout->addStretch(1);
    return page;
}

namespace {
// Role used to stash the registry id on each leaf row so we know which
// binding a given tree item refers to.
constexpr int kShortcutIdRole = Qt::UserRole + 1;
} // namespace

QWidget *PreferencesDialog::buildShortcutsPage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    m_shortcutsTree = new QTreeWidget;
    m_shortcutsTree->setColumnCount(2);
    m_shortcutsTree->setHeaderLabels({"Action", "Shortcut"});
    m_shortcutsTree->setRootIsDecorated(true);
    m_shortcutsTree->setAlternatingRowColors(true);
    m_shortcutsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_shortcutsTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_shortcutsTree);

    auto *hint = new QLabel("Double-click a shortcut to rebind it.");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *buttons = new QHBoxLayout;
    m_resetShortcut = new QPushButton("Reset to Default");
    m_resetShortcut->setEnabled(false);
    m_resetAllShortcuts = new QPushButton("Reset All");
    buttons->addWidget(m_resetShortcut);
    buttons->addWidget(m_resetAllShortcuts);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    reloadShortcutsTree();

    connect(m_shortcutsTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int column) {
                if (column != 1) return;
                beginEditShortcut(item);
            });
    connect(m_shortcutsTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        auto items = m_shortcutsTree->selectedItems();
        m_resetShortcut->setEnabled(!items.isEmpty() &&
                                     !items.first()->data(0, kShortcutIdRole).toString().isEmpty());
    });
    connect(m_resetShortcut, &QPushButton::clicked, this, [this] {
        auto items = m_shortcutsTree->selectedItems();
        if (items.isEmpty()) return;
        const QString id = items.first()->data(0, kShortcutIdRole).toString();
        if (id.isEmpty()) return;
        ShortcutRegistry::instance().resetToDefault(id);
        reloadShortcutsTree();
    });
    connect(m_resetAllShortcuts, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Reset All Shortcuts",
                                   "Reset every keyboard shortcut back to its default?") !=
            QMessageBox::Yes)
            return;
        ShortcutRegistry::instance().resetAllToDefaults();
        reloadShortcutsTree();
    });

    return page;
}

void PreferencesDialog::reloadShortcutsTree() {
    m_shortcutsTree->clear();

    QMap<QString, QTreeWidgetItem *> headings;
    for (const ShortcutRegistry::Entry &e : ShortcutRegistry::instance().entries()) {
        QTreeWidgetItem *heading = headings.value(e.category, nullptr);
        if (!heading) {
            heading = new QTreeWidgetItem(m_shortcutsTree, {e.category});
            QFont f = heading->font(0);
            f.setBold(true);
            heading->setFont(0, f);
            heading->setFlags(heading->flags() & ~Qt::ItemIsSelectable);
            heading->setExpanded(true);
            headings.insert(e.category, heading);
        }
        auto *row = new QTreeWidgetItem(heading, {e.label, e.get().toString()});
        row->setData(0, kShortcutIdRole, e.id);
    }
    m_resetShortcut->setEnabled(false);
}

void PreferencesDialog::beginEditShortcut(QTreeWidgetItem *item) {
    const QString id = item->data(0, kShortcutIdRole).toString();
    if (id.isEmpty()) return; // category heading row

    auto *editor = new QKeySequenceEdit(QKeySequence::fromString(item->text(1)));
    m_shortcutsTree->setItemWidget(item, 1, editor);
    editor->setFocus();

    connect(editor, &QKeySequenceEdit::editingFinished, this, [this, item, id, editor] {
        const QKeySequence seq = editor->keySequence();
        m_shortcutsTree->setItemWidget(item, 1, nullptr);
        editor->deleteLater();

        const QString conflictId = ShortcutRegistry::instance().idBoundTo(seq);
        if (!conflictId.isEmpty() && conflictId != id) {
            QString conflictLabel;
            for (const ShortcutRegistry::Entry &e : ShortcutRegistry::instance().entries())
                if (e.id == conflictId) conflictLabel = e.label;
            if (QMessageBox::question(
                    this, "Shortcut Already In Use",
                    QString("\"%1\" is already used by \"%2\". Reassign it to this action?")
                        .arg(seq.toString(), conflictLabel)) != QMessageBox::Yes) {
                reloadShortcutsTree();
                return;
            }
        }

        ShortcutRegistry::instance().rebind(id, seq);
        reloadShortcutsTree();
    });
}

QString PreferencesDialog::currentModelId() const {
    return m_model->currentData().toString();
}

void PreferencesDialog::loadFrameForCurrentModel() {
    int w = 0, h = 0;
    afFrameForModel(currentModelId(), w, h);
    QSignalBlocker bw(m_frameW);
    QSignalBlocker bh(m_frameH);
    m_frameW->setValue(w);
    m_frameH->setValue(h);
}

void PreferencesDialog::onModelChanged() {
    QSettings s;
    s.setValue("af/currentModel", currentModelId());
    loadFrameForCurrentModel();
    emit afFrameSizeChanged(m_frameW->value(), m_frameH->value());
}

void PreferencesDialog::onFrameEdited() {
    QSettings s;
    const QString id = currentModelId();
    s.setValue(QString("af/models/%1/frameWidth").arg(id), m_frameW->value());
    s.setValue(QString("af/models/%1/frameHeight").arg(id), m_frameH->value());
    emit afFrameSizeChanged(m_frameW->value(), m_frameH->value());
}

void PreferencesDialog::selectModelById(const QString &id) {
    if (id.isEmpty()) return;
    int idx = m_model->findData(id);
    if (idx < 0 || idx == m_model->currentIndex()) return;
    m_model->setCurrentIndex(idx); // triggers onModelChanged()
}

void PreferencesDialog::setAfFrame(int w, int h) {
    {
        QSignalBlocker bw(m_frameW);
        QSignalBlocker bh(m_frameH);
        m_frameW->setValue(w);
        m_frameH->setValue(h);
    }
    onFrameEdited(); // persist for the current model + emit afFrameSizeChanged
}
