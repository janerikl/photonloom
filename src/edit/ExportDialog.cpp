#include "edit/ExportDialog.h"

#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>

ExportDialog::ExportDialog(ExportPresetStore *store, QWidget *parent)
    : QDialog(parent), m_store(store) {
    setWindowTitle("Export");
    setModal(true);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    m_presetCombo = new QComboBox;
    form->addRow("Preset:", m_presetCombo);

    m_formatCombo = new QComboBox;
    m_formatCombo->addItem("JPEG", int(ExportPreset::JPEG));
    m_formatCombo->addItem("PNG", int(ExportPreset::PNG));
    m_formatCombo->addItem("TIFF (16-bit)", int(ExportPreset::TIFF16));
    form->addRow("Format:", m_formatCombo);

    m_longEdge = new QSpinBox;
    m_longEdge->setRange(0, 20000);
    m_longEdge->setSingleStep(128);
    m_longEdge->setSpecialValueText("Full size"); // shown when value == 0
    m_longEdge->setSuffix(" px");
    form->addRow("Long edge:", m_longEdge);

    m_quality = new QSpinBox;
    m_quality->setRange(1, 100);
    form->addRow("JPEG quality:", m_quality);

    layout->addLayout(form);

    auto *presetBtns = new QHBoxLayout;
    m_saveBtn = new QPushButton("Save as preset…");
    m_deleteBtn = new QPushButton("Delete preset");
    presetBtns->addWidget(m_saveBtn);
    presetBtns->addWidget(m_deleteBtn);
    presetBtns->addStretch(1);
    layout->addLayout(presetBtns);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    box->addButton("Export", QDialogButtonBox::AcceptRole);
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_presetCombo, &QComboBox::currentIndexChanged, this,
            &ExportDialog::loadPresetIntoFields);
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this,
            &ExportDialog::onFormatChanged);
    connect(m_saveBtn, &QPushButton::clicked, this, &ExportDialog::onSavePreset);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ExportDialog::onDeletePreset);

    refreshPresetList();
}

void ExportDialog::refreshPresetList(const QString &selectName) {
    QSignalBlocker block(m_presetCombo);
    m_presetCombo->clear();
    for (const ExportPreset &p : m_store->all()) {
        QString label = p.builtIn ? p.name : p.name + "  (custom)";
        m_presetCombo->addItem(label, p.name);
    }
    int idx = 0;
    if (!selectName.isEmpty()) {
        int found = m_presetCombo->findData(selectName);
        if (found >= 0) idx = found;
    }
    m_presetCombo->setCurrentIndex(idx);
    loadPresetIntoFields(idx);
}

void ExportDialog::loadPresetIntoFields(int index) {
    if (index < 0) return;
    const QList<ExportPreset> all = m_store->all();
    if (index >= all.size()) return;
    const ExportPreset &p = all[index];

    QSignalBlocker b1(m_formatCombo), b2(m_longEdge), b3(m_quality);
    m_formatCombo->setCurrentIndex(m_formatCombo->findData(int(p.format)));
    m_longEdge->setValue(p.longEdge);
    m_quality->setValue(p.quality);
    onFormatChanged();

    m_deleteBtn->setEnabled(m_store->isCustom(p.name));
}

void ExportDialog::onFormatChanged() {
    bool isJpeg = m_formatCombo->currentData().toInt() == int(ExportPreset::JPEG);
    m_quality->setEnabled(isJpeg);
}

ExportPreset ExportDialog::selectedPreset() const {
    ExportPreset p;
    p.name = m_presetCombo->currentData().toString();
    p.format = static_cast<ExportPreset::Format>(m_formatCombo->currentData().toInt());
    p.longEdge = m_longEdge->value();
    p.quality = m_quality->value();
    return p;
}

void ExportDialog::onSavePreset() {
    bool ok = false;
    QString name = QInputDialog::getText(this, "Save preset", "Preset name:",
                                         QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    ExportPreset p = selectedPreset();
    p.name = name.trimmed();
    p.builtIn = false;
    m_store->addOrUpdate(p);
    refreshPresetList(p.name);
}

void ExportDialog::onDeletePreset() {
    QString name = m_presetCombo->currentData().toString();
    if (!m_store->isCustom(name)) return;
    m_store->remove(name);
    refreshPresetList();
}
