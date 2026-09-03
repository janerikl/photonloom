#pragma once

#include <QString>
#include <QImage>

#include "edit/Adjustments.h"
#include "edit/ColorSpace.h"

// Non-destructive edits are stored in a JSON sidecar next to the RAW file
// (`<image>.nte.json`), so reopening a photo or session restores them.
namespace EditSidecar {

QString pathFor(const QString &imagePath);
bool exists(const QString &imagePath);
bool save(const QString &imagePath, const Adjustments &adj);
// Fills `out` and returns true if a sidecar was read successfully.
bool load(const QString &imagePath, Adjustments &out);

// Self-contained project file (base pixels + full Adjustments in one file,
// no dependency on an external photo on disk) — see EditSidecar.cpp for
// details. Path is used as given, unlike save()/load() which derive
// `<imagePath>.nte.json` via pathFor().
bool saveProject(const QString &path, const QImage &base, const Adjustments &adj);
bool loadProject(const QString &path, QImage &base, Adjustments &out);

// 1-5 star rating, stored alongside the adjustments in the same sidecar JSON
// (preserved across save() calls even though it isn't part of Adjustments).
// 0 = unrated. loadRating returns 0 if no sidecar or no rating field exists.
int loadRating(const QString &imagePath);
bool saveRating(const QString &imagePath, int rating);

// The working color space the photo was decoded with, stored alongside the
// adjustments in the same sidecar JSON (preserved across save() calls, like
// rating). Defaults to sRGB if no sidecar or no field exists — i.e. for
// photos decoded before this field existed.
WorkingColorSpace loadWorkingColorSpace(const QString &imagePath);
bool saveWorkingColorSpace(const QString &imagePath, WorkingColorSpace space);

// A small cached JPEG rendering of the edited photo (`<image>.nte.thumb.jpg`),
// so the filmstrip can show the edited look without re-decoding the RAW and
// re-applying adjustments on every session load.
QString thumbnailPathFor(const QString &imagePath);
bool saveThumbnail(const QString &imagePath, const QImage &image);
// Returns the cached edited thumbnail, or a null QImage if none exists.
QImage loadThumbnail(const QString &imagePath);

} // namespace EditSidecar
