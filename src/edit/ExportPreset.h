#pragma once

#include <QString>
#include <QList>
#include <QImage>

// A named export configuration: output format, an optional long-edge size limit
// (0 = keep full resolution), and JPEG quality.
struct ExportPreset {
    enum Format { JPEG, PNG, TIFF16 };

    QString name;
    Format format = JPEG;
    int longEdge = 0;   // longer side in px; 0 = full size (never upscales)
    int quality = 90;   // JPEG quality 1..100 (ignored for PNG/TIFF16)
    bool builtIn = false;

    QString extension() const {
        switch (format) {
        case PNG: return "png";
        case TIFF16: return "tif";
        default: return "jpg";
        }
    }
};

// Downscale `img` so its longer edge is at most preset.longEdge (aspect
// preserved); returns `img` unchanged when longEdge is 0 or already smaller.
QImage applyExportResize(const QImage &img, const ExportPreset &preset);

// Persists custom presets via QSettings and exposes them alongside the
// non-editable built-ins.
class ExportPresetStore {
public:
    ExportPresetStore() { load(); }

    static QList<ExportPreset> builtins();
    QList<ExportPreset> all() const;      // builtins + custom
    const QList<ExportPreset> &custom() const { return m_custom; }
    bool isCustom(const QString &name) const;

    void addOrUpdate(const ExportPreset &p); // by name; persists
    void remove(const QString &name);        // custom only; persists

private:
    void load();
    void save() const;

    QList<ExportPreset> m_custom;
};
