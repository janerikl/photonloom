// Covers writeTiff16() writing genuine 16-bit-per-channel TIFF data (not
// dithered to 8-bit like the JPEG/PNG export path), added when TIFF export
// was introduced so a future regression that routes TIFF through
// ditherTo8Bit() or otherwise loses precision gets caught.
#include "edit/TiffExport.h"

#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#include <cassert>

#include <tiffio.h>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    QImage img(4, 3, QImage::Format_RGBA64);
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            // Values that would visibly collapse if downsampled to 8-bit.
            quint16 r = quint16((x + 1) * 12345);
            quint16 g = quint16((y + 1) * 6789);
            quint16 b = 40000;
            quint16 a = 65535;
            img.setPixelColor(x, y, QColor::fromRgba64(r, g, b, a));
        }
    }

    QTemporaryDir dir;
    assert(dir.isValid());
    QString path = dir.filePath("out.tif");

    assert(writeTiff16(img, path));

    TIFF *tif = TIFFOpen(path.toUtf8().constData(), "r");
    assert(tif);

    uint32_t width = 0, height = 0;
    uint16_t bps = 0, spp = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    assert(width == 4);
    assert(height == 3);
    assert(bps == 16);
    assert(spp == 4); // image has an alpha channel

    QVector<quint16> row(int(width) * spp);
    for (uint32_t y = 0; y < height; ++y) {
        assert(TIFFReadScanline(tif, row.data(), y, 0) >= 0);
        for (uint32_t x = 0; x < width; ++x) {
            quint16 r = quint16((x + 1) * 12345);
            quint16 g = quint16((y + 1) * 6789);
            quint16 b = 40000;
            quint16 a = 65535;
            const quint16 *px = row.constData() + x * spp;
            assert(px[0] == r);
            assert(px[1] == g);
            assert(px[2] == b);
            assert(px[3] == a);
        }
    }

    TIFFClose(tif);
    return 0;
}
