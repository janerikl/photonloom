#include "edit/RawLoader.h"

#include <libraw/libraw.h>

#include <QDataStream>
#include <QFile>
#include <QFileInfo>

namespace RawLoader {
namespace {

// Full-resolution demosaic is the single most CPU-heavy step in opening a
// RAW file. The decoded pixels never change for a given source file (there's
// no half_size/quality shortcut here — see RawLoader.h), so cache the result
// next to the source and skip LibRaw entirely on repeat opens. Invalidated by
// source size+mtime, so re-exporting/replacing the NEF picks up fresh data.
QString cachePathFor(const QString &rawPath) { return rawPath + QStringLiteral(".nte.rawcache"); }

// Bumped from NRC1: the cache now also keys on working color space (a stale
// NRC1 file has no such field and would otherwise be silently reused for
// whichever space is requested first, mismatching its actual sRGB decode).
constexpr quint32 kCacheMagic = 0x4E524332; // "NRC2"

QImage loadFromCache(const QString &rawPath, WorkingColorSpace space) {
    const QFileInfo srcInfo(rawPath);
    QFile f(cachePathFor(rawPath));
    if (!f.open(QIODevice::ReadOnly)) return QImage();

    QDataStream in(&f);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    qint64 srcSize = 0, srcMTime = 0;
    qint32 width = 0, height = 0, bytesPerLine = 0, format = 0, cachedSpace = 0;
    in >> magic >> srcSize >> srcMTime >> width >> height >> bytesPerLine >> format >> cachedSpace;
    if (in.status() != QDataStream::Ok || magic != kCacheMagic) return QImage();
    if (srcSize != srcInfo.size() || srcMTime != srcInfo.lastModified().toMSecsSinceEpoch())
        return QImage(); // source changed since the cache was written
    if (cachedSpace != static_cast<qint32>(space)) return QImage(); // wrong working space

    QImage img(width, height, static_cast<QImage::Format>(format));
    if (img.isNull()) return QImage();
    for (int y = 0; y < height; ++y) {
        if (in.readRawData(reinterpret_cast<char *>(img.scanLine(y)), bytesPerLine) != bytesPerLine)
            return QImage();
    }
    img.setColorSpace(toQColorSpace(space));
    return img;
}

void saveToCache(const QString &rawPath, const QImage &img, WorkingColorSpace space) {
    const QFileInfo srcInfo(rawPath);
    QFile f(cachePathFor(rawPath));
    if (!f.open(QIODevice::WriteOnly)) return;

    QDataStream out(&f);
    out.setVersion(QDataStream::Qt_6_0);
    out << kCacheMagic << static_cast<qint64>(srcInfo.size())
        << static_cast<qint64>(srcInfo.lastModified().toMSecsSinceEpoch())
        << static_cast<qint32>(img.width()) << static_cast<qint32>(img.height())
        << static_cast<qint32>(img.bytesPerLine()) << static_cast<qint32>(img.format())
        << static_cast<qint32>(space);
    for (int y = 0; y < img.height(); ++y)
        out.writeRawData(reinterpret_cast<const char *>(img.constScanLine(y)), img.bytesPerLine());
}

QImage decode(const QString &rawPath, WorkingColorSpace space) {
    LibRaw raw;

    // 16-bit output with camera white balance — a natural-looking base.
    // 16-bit preserves shadow tonal resolution that 8-bit throws away, which
    // otherwise reappears as banding when shadows/brightness are pushed later.
    raw.imgdata.params.output_bps = 16;
    raw.imgdata.params.output_color = libRawOutputColor(space);
    raw.imgdata.params.use_camera_wb = 1;
    raw.imgdata.params.no_auto_bright = 0;

    if (raw.open_file(rawPath.toUtf8().constData()) != LIBRAW_SUCCESS)
        return QImage();
    if (raw.unpack() != LIBRAW_SUCCESS)
        return QImage();
    if (raw.dcraw_process() != LIBRAW_SUCCESS)
        return QImage();

    int errc = 0;
    libraw_processed_image_t *out = raw.dcraw_make_mem_image(&errc);
    if (!out) return QImage();

    QImage result;
    if (out->type == LIBRAW_IMAGE_BITMAP && out->colors == 3 && out->bits == 16) {
        // LibRaw's mem image is native-endian, interleaved RGB (no alpha),
        // scaled to the full 16-bit range. QImage has no packed-3x16 format,
        // so expand into RGBA64 (alpha forced opaque) — 8 bytes/px, twice
        // RGB888's footprint, accepted in exchange for eliminating 8-bit
        // shadow banding through the whole editing pipeline.
        QImage img(out->width, out->height, QImage::Format_RGBA64);
        const auto *src = reinterpret_cast<const quint16 *>(out->data);
        for (int y = 0; y < out->height; ++y) {
            auto *dst = reinterpret_cast<QRgba64 *>(img.scanLine(y));
            const quint16 *row = src + static_cast<size_t>(y) * out->width * 3;
            for (int x = 0; x < out->width; ++x) {
                const quint16 r = row[x * 3 + 0];
                const quint16 g = row[x * 3 + 1];
                const quint16 b = row[x * 3 + 2];
                dst[x] = qRgba64(r, g, b, 0xFFFF);
            }
        }
        result = img.copy(); // detach from the loop-local buffer lifetime
        result.setColorSpace(toQColorSpace(space));
    }

    LibRaw::dcraw_clear_mem(out);
    raw.recycle();
    return result;
}

} // namespace

QImage load(const QString &rawPath, WorkingColorSpace space) {
    QImage cached = loadFromCache(rawPath, space);
    if (!cached.isNull()) return cached;

    QImage img = decode(rawPath, space);
    if (!img.isNull()) saveToCache(rawPath, img, space);
    return img;
}

QImage loadAny(const QString &path, WorkingColorSpace space) {
    QImage img = load(path, space);
    if (!img.isNull()) return img;
    // Qt's JPEG/PNG decoders only ever yield 8-bit data, but upconvert to
    // RGBA64 so every image reaching the Adjustments pipeline is uniformly
    // 16-bit — callers never need to branch on source bit depth. These
    // sources aren't run through LibRaw's gamut conversion, so tag sRGB
    // regardless of the requested working space.
    QImage img8 = QImage(path).convertToFormat(QImage::Format_RGBA64);
    img8.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return img8;
}

} // namespace RawLoader
