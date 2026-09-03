#include "edit/TiffExport.h"

#include <QColorSpace>
#include <QImage>
#include <QVector>

#include <tiffio.h>

bool writeTiff16(const QImage &imgIn, const QString &path) {
    if (imgIn.isNull()) return false;
    QImage img = imgIn.format() == QImage::Format_RGBA64
                     ? imgIn
                     : imgIn.convertToFormat(QImage::Format_RGBA64);

    const uint32_t width = uint32_t(img.width());
    const uint32_t height = uint32_t(img.height());
    const bool hasAlpha = img.hasAlphaChannel();
    const uint16_t samplesPerPixel = hasAlpha ? 4 : 3;

    TIFF *tif = TIFFOpen(path.toUtf8().constData(), "w");
    if (!tif) return false;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samplesPerPixel);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, uint32_t(-1)));
    if (hasAlpha) {
        uint16_t extra[1] = {EXTRASAMPLE_UNASSALPHA};
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, extra);
    }
    if (img.colorSpace().isValid()) {
        const QByteArray icc = img.colorSpace().iccProfile();
        if (!icc.isEmpty())
            TIFFSetField(tif, TIFFTAG_ICCPROFILE, uint32_t(icc.size()), icc.constData());
    }

    QVector<quint16> row(int(width) * samplesPerPixel);
    bool ok = true;
    for (uint32_t y = 0; y < height && ok; ++y) {
        const QRgba64 *src = reinterpret_cast<const QRgba64 *>(img.constScanLine(int(y)));
        quint16 *dst = row.data();
        for (uint32_t x = 0; x < width; ++x) {
            const QRgba64 px = src[x];
            *dst++ = px.red();
            *dst++ = px.green();
            *dst++ = px.blue();
            if (hasAlpha) *dst++ = px.alpha();
        }
        if (TIFFWriteScanline(tif, row.data(), y, 0) < 0) ok = false;
    }

    TIFFClose(tif);
    return ok;
}
