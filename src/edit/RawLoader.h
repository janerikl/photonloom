#pragma once

#include <QImage>
#include <QString>

#include "edit/ColorSpace.h"

// Decodes a RAW file (NEF) to a full-resolution RGB QImage using LibRaw
// (demosaic + camera white balance + gamma for the requested working space).
// This is CPU-heavy and blocking — call it on a worker thread (e.g. via
// QtConcurrent::run), never on the GUI thread. Returns a null QImage on
// failure. The returned QImage is tagged via QImage::setColorSpace() to
// match `space`.
namespace RawLoader {

QImage load(const QString &rawPath, WorkingColorSpace space = WorkingColorSpace::sRGB);

// Like load(), but also handles regular image formats (JPEG/PNG/TIFF/etc):
// tries the RAW decoder first, and falls back to QImage's built-in reader if
// that fails. Still CPU-heavy/blocking — same threading rules as load().
// Non-RAW sources are tagged sRGB regardless of `space` (Qt's built-in
// decoders don't do LibRaw-style gamut conversion).
QImage loadAny(const QString &path, WorkingColorSpace space = WorkingColorSpace::sRGB);

} // namespace RawLoader
