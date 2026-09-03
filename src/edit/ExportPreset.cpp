#include "edit/ExportPreset.h"

#include <QSettings>

QImage applyExportResize(const QImage &img, const ExportPreset &preset) {
    if (img.isNull() || preset.longEdge <= 0) return img;
    int longer = qMax(img.width(), img.height());
    if (longer <= preset.longEdge) return img; // never upscale
    return img.scaled(preset.longEdge, preset.longEdge, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

QList<ExportPreset> ExportPresetStore::builtins() {
    QList<ExportPreset> b;
    auto add = [&](const QString &name, ExportPreset::Format fmt, int longEdge, int q) {
        ExportPreset p;
        p.name = name;
        p.format = fmt;
        p.longEdge = longEdge;
        p.quality = q;
        p.builtIn = true;
        b.append(p);
    };
    add("Full size / max quality", ExportPreset::JPEG, 0, 100);
    add("Web 2048px", ExportPreset::JPEG, 2048, 85);
    add("Email 1024px", ExportPreset::JPEG, 1024, 80);
    add("Print (long edge 3000px)", ExportPreset::JPEG, 3000, 95);
    add("Full size / 16-bit TIFF", ExportPreset::TIFF16, 0, 100);
    return b;
}

QList<ExportPreset> ExportPresetStore::all() const {
    return builtins() + m_custom;
}

bool ExportPresetStore::isCustom(const QString &name) const {
    for (const ExportPreset &p : m_custom)
        if (p.name == name) return true;
    return false;
}

void ExportPresetStore::addOrUpdate(const ExportPreset &p) {
    ExportPreset entry = p;
    entry.builtIn = false;
    for (ExportPreset &e : m_custom) {
        if (e.name == entry.name) {
            e = entry;
            save();
            return;
        }
    }
    m_custom.append(entry);
    save();
}

void ExportPresetStore::remove(const QString &name) {
    for (int i = 0; i < m_custom.size(); ++i) {
        if (m_custom[i].name == name) {
            m_custom.removeAt(i);
            save();
            return;
        }
    }
}

void ExportPresetStore::load() {
    m_custom.clear();
    QSettings s;
    int n = s.beginReadArray("exportPresets");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        ExportPreset p;
        p.name = s.value("name").toString();
        p.format = static_cast<ExportPreset::Format>(s.value("format").toInt());
        p.longEdge = s.value("longEdge").toInt();
        p.quality = s.value("quality").toInt();
        p.builtIn = false;
        if (!p.name.isEmpty()) m_custom.append(p);
    }
    s.endArray();
}

void ExportPresetStore::save() const {
    QSettings s;
    s.beginWriteArray("exportPresets");
    for (int i = 0; i < m_custom.size(); ++i) {
        s.setArrayIndex(i);
        const ExportPreset &p = m_custom[i];
        s.setValue("name", p.name);
        s.setValue("format", int(p.format));
        s.setValue("longEdge", p.longEdge);
        s.setValue("quality", p.quality);
    }
    s.endArray();
}
