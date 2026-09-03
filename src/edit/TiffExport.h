#pragma once

#include <QString>

class QImage;

// Writes `img` (expected Format_RGBA64 / Format_RGBA64_Premultiplied) as an
// uncompressed 16-bit-per-channel TIFF via libtiff, preserving full bit depth
// (no dithering to 8-bit). Returns false on failure to open/write the file.
bool writeTiff16(const QImage &img, const QString &path);
