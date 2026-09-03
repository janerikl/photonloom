#pragma once

#include <QColorSpace>

// The RAW decode working space: what gamut LibRaw demosaics into, and what
// gets tagged on exported images so JPEG/PNG/TIFF carry a matching embedded
// ICC profile. Persisted as a plain int (this ordinal) in both QSettings
// (the global default) and EditSidecar (the per-project value) — never
// reorder these, only append.
enum class WorkingColorSpace { sRGB = 0, AdobeRGB = 1, ProPhotoRGB = 2 };

// LibRaw imgdata.params.output_color value for this space (LIBRAW_COLORSPACE_*).
inline int libRawOutputColor(WorkingColorSpace space) {
    switch (space) {
    case WorkingColorSpace::AdobeRGB: return 2;
    case WorkingColorSpace::ProPhotoRGB: return 4;
    default: return 1; // sRGB
    }
}

inline QColorSpace toQColorSpace(WorkingColorSpace space) {
    switch (space) {
    case WorkingColorSpace::AdobeRGB: return QColorSpace(QColorSpace::AdobeRgb);
    case WorkingColorSpace::ProPhotoRGB: return QColorSpace(QColorSpace::ProPhotoRgb);
    default: return QColorSpace(QColorSpace::SRgb);
    }
}

inline QString workingColorSpaceLabel(WorkingColorSpace space) {
    switch (space) {
    case WorkingColorSpace::AdobeRGB: return QStringLiteral("Adobe RGB");
    case WorkingColorSpace::ProPhotoRGB: return QStringLiteral("ProPhoto RGB");
    default: return QStringLiteral("sRGB");
    }
}
