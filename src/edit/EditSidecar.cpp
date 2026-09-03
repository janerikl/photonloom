#include "edit/EditSidecar.h"
#include "edit/ZipFile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QByteArray>

namespace {

QJsonObject levelsChannelToJson(const LevelsChannel &c) {
    QJsonObject j;
    j["inBlack"] = c.inBlack;
    j["inWhite"] = c.inWhite;
    j["gamma"] = c.gamma;
    j["outBlack"] = c.outBlack;
    j["outWhite"] = c.outWhite;
    return j;
}

LevelsChannel levelsChannelFromJson(const QJsonObject &j) {
    LevelsChannel c;
    c.inBlack = j["inBlack"].toInt(0);
    c.inWhite = j["inWhite"].toInt(255);
    c.gamma = j["gamma"].toDouble(1.0);
    c.outBlack = j["outBlack"].toInt(0);
    c.outWhite = j["outWhite"].toInt(255);
    return c;
}

QJsonObject levelsToJson(const Levels &lv) {
    QJsonObject j;
    j["rgb"] = levelsChannelToJson(lv.rgb);
    j["r"] = levelsChannelToJson(lv.r);
    j["g"] = levelsChannelToJson(lv.g);
    j["b"] = levelsChannelToJson(lv.b);
    return j;
}

Levels levelsFromJson(const QJsonObject &j) {
    Levels lv;
    lv.rgb = levelsChannelFromJson(j["rgb"].toObject());
    lv.r = levelsChannelFromJson(j["r"].toObject());
    lv.g = levelsChannelFromJson(j["g"].toObject());
    lv.b = levelsChannelFromJson(j["b"].toObject());
    return lv;
}

QJsonArray curveToJson(const QVector<QPointF> &curve) {
    QJsonArray a;
    for (const QPointF &p : curve) a.append(QJsonArray{p.x(), p.y()});
    return a;
}

QVector<QPointF> curveFromJson(const QJsonArray &a) {
    QVector<QPointF> curve;
    for (const QJsonValue &v : a) {
        QJsonArray p = v.toArray();
        if (p.size() == 2) curve.append(QPointF(p[0].toDouble(), p[1].toDouble()));
    }
    return curve;
}

QJsonArray doublesToJson(const QVector<double> &v) {
    QJsonArray a;
    for (double d : v) a.append(d);
    return a;
}

QVector<double> doublesFromJson(const QJsonArray &a) {
    QVector<double> v;
    for (const QJsonValue &jv : a) v.append(jv.toDouble());
    return v;
}

const char *shapeTypeName(ShapeType t) {
    switch (t) {
    case ShapeType::Rectangle: return "rectangle";
    case ShapeType::Ellipse:   return "ellipse";
    case ShapeType::Line:      return "line";
    case ShapeType::Polygon:   return "polygon";
    case ShapeType::Star:      return "star";
    case ShapeType::Heart:     return "heart";
    }
    return "rectangle";
}

ShapeType shapeTypeFromName(const QString &name) {
    if (name == "ellipse") return ShapeType::Ellipse;
    if (name == "line") return ShapeType::Line;
    if (name == "polygon") return ShapeType::Polygon;
    if (name == "star") return ShapeType::Star;
    if (name == "heart") return ShapeType::Heart;
    return ShapeType::Rectangle;
}

// Builds the full Adjustments JSON blob (masks/tone/crop/heals/removals —
// everything except the sidecar-only `rating` field, which lives outside
// Adjustments). Shared by the per-photo sidecar (EditSidecar::save, which
// writes this at the top level) and self-contained project files
// (EditSidecar::saveProject, which nest it under an "adjustments" key
// alongside the embedded base image).
QJsonObject adjustmentsToJson(const Adjustments &a) {
    QJsonObject o;
    o["version"] = 7;
    o["brightness"] = a.brightness;
    o["contrast"] = a.contrast;
    o["highlights"] = a.highlights;
    o["shadows"] = a.shadows;
    o["saturation"] = a.saturation;
    o["vibrance"] = a.vibrance;
    o["temperature"] = a.temperature;
    o["tint"] = a.tint;
    o["wbR"] = a.wbR;
    o["wbG"] = a.wbG;
    o["wbB"] = a.wbB;
    o["denoise"] = a.denoise;
    o["clarity"] = a.clarity;
    o["sharpen"] = a.sharpen;
    o["vignette"] = a.vignette;
    o["lightAngle"] = a.lightAngle;
    o["lightIntensity"] = a.lightIntensity;
    o["flatStyle"] = a.flatStyle;
    o["rotationQuadrants"] = a.rotationQuadrants;
    o["flipH"] = a.flipH;
    o["flipV"] = a.flipV;
    o["backgroundColor"] = a.backgroundColor.name(QColor::HexRgb);
    if (!a.guidesH.isEmpty()) o["guidesH"] = doublesToJson(a.guidesH);
    if (!a.guidesV.isEmpty()) o["guidesV"] = doublesToJson(a.guidesV);
    // Background layer visibility/presence is no longer tracked separately —
    // it's just a normal masks[] entry (MaskType::Background) now, written
    // through the generic masks loop below like any other layer.
    if (!a.cropRect.isNull()) {
        QJsonObject c;
        c["x"] = a.cropRect.x();
        c["y"] = a.cropRect.y();
        c["w"] = a.cropRect.width();
        c["h"] = a.cropRect.height();
        c["angle"] = a.cropAngle;
        o["crop"] = c;
    }
    o["curve"] = curveToJson(a.curve);
    if (!a.levels.isIdentity()) o["levels"] = levelsToJson(a.levels);
    if (!a.colorRanges.isEmpty()) {
        QJsonArray ranges;
        for (const ColorRangeAdjust &cr : a.colorRanges) {
            QJsonObject j;
            j["r"] = cr.r;
            j["g"] = cr.g;
            j["b"] = cr.b;
            j["ch"] = cr.channel;
            j["amt"] = cr.amount;
            ranges.append(j);
        }
        o["colorRanges"] = ranges;
    }

    if (!a.masks.isEmpty()) {
        QJsonArray masks;
        for (const Mask &m : a.masks) {
            QJsonObject j;
            j["name"] = m.name;
            j["visible"] = m.visible;
            j["groupId"] = m.groupId;
            j["groupName"] = m.groupName;
            j["opacity"] = m.opacity;
            j["blend"] = int(m.blend);
            j["sourceImagePath"] = m.sourceImagePath;
            j["sourceImageOffsetX"] = m.sourceImageOffset.x();
            j["sourceImageOffsetY"] = m.sourceImageOffset.y();
            j["sourceImageScaleX"] = m.sourceImageScale.x();
            j["sourceImageScaleY"] = m.sourceImageScale.y();
            j["sourceImageLockRatio"] = m.sourceImageLockRatio;
            j["type"] = int(m.type);
            j["inverted"] = m.inverted;
            j["feather"] = m.feather;
            j["cx"] = m.center.x();
            j["cy"] = m.center.y();
            j["rx"] = m.radiusX;
            j["ry"] = m.radiusY;
            j["angle"] = m.angle;
            j["p0x"] = m.p0.x();
            j["p0y"] = m.p0.y();
            j["p1x"] = m.p1.x();
            j["p1y"] = m.p1.y();
            j["brushRadius"] = m.brushRadius;
            j["hardness"] = m.hardness;
            j["autoMask"] = m.autoMask;
            j["paintColor"] = m.paintColor.name(QColor::HexArgb);
            j["isGradientFill"] = m.isGradientFill;
            j["gradientColorA"] = m.gradientColorA.name(QColor::HexArgb);
            j["gradientColorB"] = m.gradientColorB.name(QColor::HexArgb);
            // Paint bucket result, embedded as base64 PNG (same convention as
            // RemoveObjectOp::mask/fill below) so it round-trips exactly
            // instead of needing to be recomputed from a click point.
            if (!m.fillMask.isNull()) {
                QByteArray fillBytes;
                QBuffer fillBuf(&fillBytes);
                fillBuf.open(QIODevice::WriteOnly);
                m.fillMask.save(&fillBuf, "PNG");
                j["fillMask"] = QString::fromLatin1(fillBytes.toBase64());
            }
            j["text"] = m.text;
            j["textFamily"] = m.textFamily;
            j["textPixelSize"] = m.textPixelSize;
            j["textBold"] = m.textBold;
            j["textItalic"] = m.textItalic;
            j["textPosX"] = m.textPos.x();
            j["textPosY"] = m.textPos.y();
            // Shape (MaskType::Shape) and TextBox (MaskType::TextBox) fields.
            // Always written (not gated on type) to keep the writer simple;
            // harmless for other mask types since these fields are unused there.
            j["shapeType"] = shapeTypeName(m.shapeType);
            j["shapeRectX"] = m.shapeRect.x();
            j["shapeRectY"] = m.shapeRect.y();
            j["shapeRectW"] = m.shapeRect.width();
            j["shapeRectH"] = m.shapeRect.height();
            j["shapeP1x"] = m.shapeP1.x();
            j["shapeP1y"] = m.shapeP1.y();
            j["shapeP2x"] = m.shapeP2.x();
            j["shapeP2y"] = m.shapeP2.y();
            j["shapeRotation"] = m.shapeRotation;
            j["shapeSides"] = m.shapeSides;
            j["shapeInnerRadiusRatio"] = m.shapeInnerRadiusRatio;
            j["shapeFillEnabled"] = m.shapeFillEnabled;
            j["shapeFillColor"] = m.shapeFillColor.name(QColor::HexArgb);
            j["shapeStrokeEnabled"] = m.shapeStrokeEnabled;
            j["shapeStrokeColor"] = m.shapeStrokeColor.name(QColor::HexArgb);
            j["shapeStrokeWidth"] = m.shapeStrokeWidth;
            j["shapeImagePath"] = m.shapeImagePath;

            j["textBoxPosX"] = m.textBoxPos.x();
            j["textBoxPosY"] = m.textBoxPos.y();
            j["textBoxRotation"] = m.textBoxRotation;
            j["textBoxText"] = m.textBoxText;
            j["textBoxFamily"] = m.textBoxFamily;
            j["textBoxPixelSize"] = m.textBoxPixelSize;
            j["textBoxBold"] = m.textBoxBold;
            j["textBoxItalic"] = m.textBoxItalic;
            j["textBoxColor"] = m.textBoxColor.name(QColor::HexArgb);
            j["textBoxOutlineEnabled"] = m.textBoxOutlineEnabled;
            j["textBoxOutlineColor"] = m.textBoxOutlineColor.name(QColor::HexArgb);
            j["textBoxOutlineWidth"] = m.textBoxOutlineWidth;
            j["textBoxShadowEnabled"] = m.textBoxShadowEnabled;
            j["textBoxShadowOffsetX"] = m.textBoxShadowOffset.x();
            j["textBoxShadowOffsetY"] = m.textBoxShadowOffset.y();
            j["textBoxShadowBlur"] = m.textBoxShadowBlur;
            j["textBoxShadowOpacity"] = m.textBoxShadowOpacity;
            j["textBoxShadowColor"] = m.textBoxShadowColor.name(QColor::HexArgb);
            j["textBoxBgEnabled"] = m.textBoxBgEnabled;
            j["textBoxBgColor"] = m.textBoxBgColor.name(QColor::HexArgb);
            j["textBoxBgOpacity"] = m.textBoxBgOpacity;
            j["textBoxBgPadding"] = m.textBoxBgPadding;
            QJsonArray stroke;
            for (const BrushStrokePoint &sp : m.stroke)
                stroke.append(QJsonArray{sp.pt.x(), sp.pt.y(), sp.erase, sp.radius, sp.hardness,
                                          double(sp.color), sp.newStroke, sp.isClone,
                                          sp.cloneSourcePt.x(), sp.cloneSourcePt.y()});
            j["stroke"] = stroke;
            QJsonArray erases;
            for (const ErasePoint &ep : m.eraseStrokes)
                erases.append(QJsonArray{ep.pt.x(), ep.pt.y(), ep.radius});
            j["eraseStrokes"] = erases;
            QJsonArray maskHeals;
            for (const HealOp &hp : m.heals)
                maskHeals.append(QJsonObject{{"x", hp.x}, {"y", hp.y}, {"r", hp.radius}});
            j["heals"] = maskHeals;
            QJsonObject ad;
            ad["brightness"] = m.adj.brightness;
            ad["contrast"] = m.adj.contrast;
            ad["highlights"] = m.adj.highlights;
            ad["shadows"] = m.adj.shadows;
            ad["saturation"] = m.adj.saturation;
            ad["vibrance"] = m.adj.vibrance;
            ad["temperature"] = m.adj.temperature;
            ad["tint"] = m.adj.tint;
            ad["wbR"] = m.adj.wbR;
            ad["wbG"] = m.adj.wbG;
            ad["wbB"] = m.adj.wbB;
            ad["denoise"] = m.adj.denoise;
            ad["clarity"] = m.adj.clarity;
            ad["sharpen"] = m.adj.sharpen;
            ad["vignette"] = m.adj.vignette;
            ad["lightAngle"] = m.adj.lightAngle;
            ad["lightIntensity"] = m.adj.lightIntensity;
            ad["curve"] = curveToJson(m.adj.curve);
            if (!m.adj.levels.isIdentity()) ad["levels"] = levelsToJson(m.adj.levels);
            j["adj"] = ad;
            masks.append(j);
        }
        o["masks"] = masks;
    }

    if (!a.groups.isEmpty()) {
        QJsonArray groups;
        for (const MaskGroup &g : a.groups) {
            QJsonObject j;
            j["id"] = g.id;
            j["name"] = g.name;
            j["opacity"] = g.opacity;
            j["visible"] = g.visible;
            j["blend"] = int(g.blend);
            j["collapsed"] = g.collapsed;
            groups.append(j);
        }
        o["groups"] = groups;
    }

    QJsonArray heals;
    for (const HealOp &hp : a.heals) {
        QJsonObject h;
        h["x"] = hp.x;
        h["y"] = hp.y;
        h["r"] = hp.radius;
        heals.append(h);
    }
    o["heals"] = heals;

    // Legacy "texts"/"shapes" JSON keys are no longer written: shape/text
    // content lives entirely in "masks" now (MaskType::Shape/TextBox), and
    // Adjustments::shapes/texts (the old in-memory arrays) have been removed.
    // Version 7+ sidecars therefore never contain these keys.

    if (!a.removals.isEmpty()) {
        // Cached fill/mask images are embedded as base64-encoded PNG so a
        // reloaded sidecar restores the exact same content-aware result
        // without recomputing InpaintTool::inpaint from scratch (unlike
        // heals, which cheaply re-derive their pixels on every load).
        auto imageToBase64Png = [](const QImage &img) -> QString {
            if (img.isNull()) return QString();
            QByteArray bytes;
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            return QString::fromLatin1(bytes.toBase64());
        };
        QJsonArray removals;
        for (const RemoveObjectOp &r : a.removals) {
            QJsonObject j;
            QJsonArray stroke;
            for (const QPointF &p : r.stroke) stroke.append(QJsonArray{p.x(), p.y()});
            j["stroke"] = stroke;
            j["radius"] = r.radius;
            j["rectX"] = r.rect.x();
            j["rectY"] = r.rect.y();
            j["rectW"] = r.rect.width();
            j["rectH"] = r.rect.height();
            j["mask"] = imageToBase64Png(r.mask);
            j["fill"] = imageToBase64Png(r.fill);
            j["visible"] = r.visible;
            removals.append(j);
        }
        o["removals"] = removals;
    }

    return o;
}

// Reverse of adjustmentsToJson. Shared by EditSidecar::load and
// EditSidecar::loadProject.
Adjustments adjustmentsFromJson(const QJsonObject &o) {
    const int fileVersion = o["version"].toInt(6);

    Adjustments a;
    a.brightness = o["brightness"].toInt();
    a.contrast = o["contrast"].toInt();
    a.highlights = o["highlights"].toInt();
    a.shadows = o["shadows"].toInt();
    a.saturation = o["saturation"].toInt();
    a.vibrance = o["vibrance"].toInt();
    a.temperature = o["temperature"].toInt();
    a.tint = o["tint"].toInt();
    a.wbR = o["wbR"].toDouble(1.0);
    a.wbG = o["wbG"].toDouble(1.0);
    a.wbB = o["wbB"].toDouble(1.0);
    a.denoise = o["denoise"].toInt();
    a.clarity = o["clarity"].toInt();
    a.sharpen = o["sharpen"].toInt();
    a.vignette = o["vignette"].toInt();
    a.lightAngle = o["lightAngle"].toInt();
    a.lightIntensity = o["lightIntensity"].toInt();
    a.flatStyle = o["flatStyle"].toInt();
    a.rotationQuadrants = o["rotationQuadrants"].toInt();
    a.flipH = o["flipH"].toBool();
    a.flipV = o["flipV"].toBool();
    a.backgroundColor = QColor(o["backgroundColor"].toString(QStringLiteral("#1e1e1e")));
    if (!a.backgroundColor.isValid()) a.backgroundColor = QColor(30, 30, 30);
    a.guidesH = doublesFromJson(o["guidesH"].toArray());
    a.guidesV = doublesFromJson(o["guidesV"].toArray());
    if (o.contains("crop")) {
        QJsonObject c = o["crop"].toObject();
        a.cropRect = QRect(c["x"].toInt(), c["y"].toInt(),
                           c["w"].toInt(), c["h"].toInt());
        a.cropAngle = c["angle"].toDouble();
    }
    a.curve = curveFromJson(o["curve"].toArray());
    if (o.contains("levels")) a.levels = levelsFromJson(o["levels"].toObject());
    for (const QJsonValue &v : o["colorRanges"].toArray()) {
        QJsonObject j = v.toObject();
        ColorRangeAdjust cr;
        cr.r = j["r"].toInt();
        cr.g = j["g"].toInt();
        cr.b = j["b"].toInt();
        cr.channel = j["ch"].toInt();
        cr.amount = j["amt"].toInt();
        a.colorRanges.append(cr);
    }
    for (const QJsonValue &v : o["masks"].toArray()) {
        QJsonObject j = v.toObject();
        Mask m;
        m.name = j["name"].toString();
        m.visible = j["visible"].toBool(true);
        m.groupId = j["groupId"].toString();
        m.groupName = j["groupName"].toString();
        m.opacity = j["opacity"].toDouble(1.0);
        m.blend = static_cast<BlendMode>(j["blend"].toInt(0));
        m.sourceImagePath = j["sourceImagePath"].toString();
        m.sourceImageOffset = QPointF(j["sourceImageOffsetX"].toDouble(0.0),
                                      j["sourceImageOffsetY"].toDouble(0.0));
        m.sourceImageScale = QPointF(j["sourceImageScaleX"].toDouble(1.0),
                                     j["sourceImageScaleY"].toDouble(1.0));
        m.sourceImageLockRatio = j["sourceImageLockRatio"].toBool(true);
        m.type = static_cast<MaskType>(j["type"].toInt(0));
        m.inverted = j["inverted"].toBool();
        m.feather = j["feather"].toDouble(0.5);
        m.center = QPointF(j["cx"].toDouble(0.5), j["cy"].toDouble(0.5));
        m.radiusX = j["rx"].toDouble(0.25);
        m.radiusY = j["ry"].toDouble(0.25);
        m.angle = j["angle"].toDouble(0.0);
        m.p0 = QPointF(j["p0x"].toDouble(0.5), j["p0y"].toDouble(0.2));
        m.p1 = QPointF(j["p1x"].toDouble(0.5), j["p1y"].toDouble(0.6));
        m.brushRadius = j["brushRadius"].toDouble(0.06);
        m.hardness = j["hardness"].toDouble(0.5);
        m.autoMask = j["autoMask"].toBool(false);
        m.paintColor = QColor(j["paintColor"].toString(QStringLiteral("#ff000000")));
        m.isGradientFill = j["isGradientFill"].toBool(false);
        m.gradientColorA = QColor(j["gradientColorA"].toString(QStringLiteral("#ffffffff")));
        m.gradientColorB = QColor(j["gradientColorB"].toString(QStringLiteral("#ff000000")));
        if (j.contains("fillMask")) {
            const QByteArray fillBytes = QByteArray::fromBase64(j["fillMask"].toString().toLatin1());
            if (!fillBytes.isEmpty()) m.fillMask.loadFromData(fillBytes, "PNG");
        }
        m.text = j["text"].toString();
        m.textFamily = j["textFamily"].toString(QStringLiteral("Sans Serif"));
        m.textPixelSize = j["textPixelSize"].toDouble(0.08);
        m.textBold = j["textBold"].toBool(false);
        m.textItalic = j["textItalic"].toBool(false);
        m.textPos = QPointF(j["textPosX"].toDouble(0.3), j["textPosY"].toDouble(0.45));

        m.shapeType = shapeTypeFromName(j["shapeType"].toString());
        m.shapeRect = QRectF(j["shapeRectX"].toDouble(0.0), j["shapeRectY"].toDouble(0.0),
                             j["shapeRectW"].toDouble(200.0), j["shapeRectH"].toDouble(200.0));
        m.shapeP1 = QPointF(j["shapeP1x"].toDouble(0.0), j["shapeP1y"].toDouble(0.0));
        m.shapeP2 = QPointF(j["shapeP2x"].toDouble(200.0), j["shapeP2y"].toDouble(0.0));
        m.shapeRotation = j["shapeRotation"].toDouble(0.0);
        m.shapeSides = j["shapeSides"].toInt(5);
        m.shapeInnerRadiusRatio = j["shapeInnerRadiusRatio"].toDouble(0.5);
        m.shapeFillEnabled = j["shapeFillEnabled"].toBool(true);
        m.shapeFillColor = QColor(j["shapeFillColor"].toString(QStringLiteral("#ffffffff")));
        m.shapeStrokeEnabled = j["shapeStrokeEnabled"].toBool(true);
        m.shapeStrokeColor = QColor(j["shapeStrokeColor"].toString(QStringLiteral("#ff000000")));
        m.shapeStrokeWidth = j["shapeStrokeWidth"].toDouble(4.0);
        m.shapeImagePath = j["shapeImagePath"].toString();

        m.textBoxPos = QPointF(j["textBoxPosX"].toDouble(0.0), j["textBoxPosY"].toDouble(0.0));
        m.textBoxRotation = j["textBoxRotation"].toDouble(0.0);
        m.textBoxText = j["textBoxText"].toString();
        m.textBoxFamily = j["textBoxFamily"].toString(QStringLiteral("Sans Serif"));
        m.textBoxPixelSize = j["textBoxPixelSize"].toDouble(48.0);
        m.textBoxBold = j["textBoxBold"].toBool(false);
        m.textBoxItalic = j["textBoxItalic"].toBool(false);
        m.textBoxColor = QColor(j["textBoxColor"].toString(QStringLiteral("#ffffffff")));
        m.textBoxOutlineEnabled = j["textBoxOutlineEnabled"].toBool(false);
        m.textBoxOutlineColor = QColor(j["textBoxOutlineColor"].toString(QStringLiteral("#ff000000")));
        m.textBoxOutlineWidth = j["textBoxOutlineWidth"].toDouble(3.0);
        m.textBoxShadowEnabled = j["textBoxShadowEnabled"].toBool(false);
        m.textBoxShadowOffset = QPointF(j["textBoxShadowOffsetX"].toDouble(8.0),
                                        j["textBoxShadowOffsetY"].toDouble(8.0));
        m.textBoxShadowBlur = j["textBoxShadowBlur"].toDouble(14.0);
        m.textBoxShadowOpacity = j["textBoxShadowOpacity"].toDouble(0.75);
        m.textBoxShadowColor = QColor(j["textBoxShadowColor"].toString(QStringLiteral("#ff000000")));
        m.textBoxBgEnabled = j["textBoxBgEnabled"].toBool(false);
        m.textBoxBgColor = QColor(j["textBoxBgColor"].toString(QStringLiteral("#ff000000")));
        m.textBoxBgOpacity = j["textBoxBgOpacity"].toDouble(0.6);
        m.textBoxBgPadding = j["textBoxBgPadding"].toDouble(10.0);

        for (const QJsonValue &sv : j["stroke"].toArray()) {
            QJsonArray p = sv.toArray();
            if (p.size() >= 2) {
                // radius/hardness/color fall back to the mask's own values
                // for sidecars saved before per-point brush settings were tracked.
                BrushStrokePoint sp{
                    QPointF(p[0].toDouble(), p[1].toDouble()),
                    p.size() >= 3 && p[2].toBool(),
                    p.size() >= 4 ? p[3].toDouble() : m.brushRadius,
                    p.size() >= 5 ? p[4].toDouble() : m.hardness,
                    p.size() >= 6 ? QRgb(qint64(p[5].toDouble())) : m.paintColor.rgb(),
                    // Sidecars saved before stroke-boundary tracking have no
                    // marker here; treat every loaded point as a fresh stroke
                    // start so reopening a file never draws a phantom
                    // connecting line between what were separate strokes.
                    p.size() >= 7 ? p[6].toBool() : true};
                if (p.size() >= 10) {
                    sp.isClone = p[7].toBool();
                    sp.cloneSourcePt = QPointF(p[8].toDouble(), p[9].toDouble());
                }
                m.stroke.append(sp);
            }
        }
        for (const QJsonValue &ev : j["eraseStrokes"].toArray()) {
            QJsonArray p = ev.toArray();
            if (p.size() >= 3)
                m.eraseStrokes.append(ErasePoint{
                    QPointF(p[0].toDouble(), p[1].toDouble()), p[2].toDouble()});
        }
        for (const QJsonValue &hv : j["heals"].toArray()) {
            QJsonObject h = hv.toObject();
            m.heals.append(HealOp{h["x"].toInt(), h["y"].toInt(), h["r"].toInt()});
        }
        QJsonObject ad = j["adj"].toObject();
        m.adj.brightness = ad["brightness"].toInt();
        m.adj.contrast = ad["contrast"].toInt();
        m.adj.highlights = ad["highlights"].toInt();
        m.adj.shadows = ad["shadows"].toInt();
        m.adj.saturation = ad["saturation"].toInt();
        m.adj.vibrance = ad["vibrance"].toInt();
        m.adj.temperature = ad["temperature"].toInt();
        m.adj.tint = ad["tint"].toInt();
        m.adj.wbR = ad["wbR"].toDouble(1.0);
        m.adj.wbG = ad["wbG"].toDouble(1.0);
        m.adj.wbB = ad["wbB"].toDouble(1.0);
        m.adj.denoise = ad["denoise"].toInt();
        m.adj.clarity = ad["clarity"].toInt();
        m.adj.sharpen = ad["sharpen"].toInt();
        m.adj.vignette = ad["vignette"].toInt();
        m.adj.lightAngle = ad["lightAngle"].toInt();
        m.adj.lightIntensity = ad["lightIntensity"].toInt();
        m.adj.curve = curveFromJson(ad["curve"].toArray());
        if (ad.contains("levels")) m.adj.levels = levelsFromJson(ad["levels"].toObject());
        a.masks.append(m);
    }
    for (const QJsonValue &v : o["groups"].toArray()) {
        QJsonObject j = v.toObject();
        MaskGroup g;
        g.id = j["id"].toString();
        g.name = j["name"].toString();
        g.opacity = j["opacity"].toDouble(1.0);
        g.visible = j["visible"].toBool(true);
        g.blend = static_cast<BlendMode>(j["blend"].toInt(0));
        g.collapsed = j["collapsed"].toBool(false);
        a.groups.append(g);
    }
    for (const QJsonValue &v : o["heals"].toArray()) {
        QJsonObject h = v.toObject();
        HealOp hp;
        hp.x = h["x"].toInt();
        hp.y = h["y"].toInt();
        hp.radius = h["r"].toInt();
        a.heals.append(hp);
    }
    // Legacy "texts"/"shapes" JSON keys (pre-version-7 sidecars) are parsed
    // into local, transient vectors here — used only below to synthesize
    // equivalent masks entries — rather than into Adjustments::shapes/texts,
    // which no longer exist.
    QVector<TextOp> legacyTexts;
    QVector<ShapeOp> legacyShapes;
    for (const QJsonValue &v : o["texts"].toArray()) {
        QJsonObject j = v.toObject();
        TextOp t;
        t.pos = QPointF(j["x"].toDouble(0.0), j["y"].toDouble(0.0));
        t.rotation = j["rotation"].toDouble(0.0);
        t.text = j["text"].toString();
        t.family = j["family"].toString(QStringLiteral("Sans Serif"));
        t.pixelSize = j["pixelSize"].toDouble(48.0);
        t.bold = j["bold"].toBool(false);
        t.italic = j["italic"].toBool(false);
        t.color = QColor(j["color"].toString(QStringLiteral("#ffffffff")));
        t.outlineEnabled = j["outlineEnabled"].toBool(false);
        t.outlineColor = QColor(j["outlineColor"].toString(QStringLiteral("#ff000000")));
        t.outlineWidth = j["outlineWidth"].toDouble(3.0);
        t.shadowEnabled = j["shadowEnabled"].toBool(false);
        t.shadowOffset = QPointF(j["shadowOffsetX"].toDouble(4.0), j["shadowOffsetY"].toDouble(4.0));
        t.shadowBlur = j["shadowBlur"].toDouble(6.0);
        t.shadowOpacity = j["shadowOpacity"].toDouble(0.6);
        t.shadowColor = QColor(j["shadowColor"].toString(QStringLiteral("#ff000000")));
        t.bgEnabled = j["bgEnabled"].toBool(false);
        t.bgColor = QColor(j["bgColor"].toString(QStringLiteral("#ff000000")));
        t.bgOpacity = j["bgOpacity"].toDouble(0.6);
        t.bgPadding = j["bgPadding"].toDouble(10.0);
        legacyTexts.append(t);
    }
    for (const QJsonValue &v : o["shapes"].toArray()) {
        QJsonObject j = v.toObject();
        ShapeOp s;
        s.type = shapeTypeFromName(j["type"].toString());
        s.rect = QRectF(j["rectX"].toDouble(0.0), j["rectY"].toDouble(0.0),
                        j["rectW"].toDouble(200.0), j["rectH"].toDouble(200.0));
        s.p1 = QPointF(j["p1x"].toDouble(0.0), j["p1y"].toDouble(0.0));
        s.p2 = QPointF(j["p2x"].toDouble(200.0), j["p2y"].toDouble(0.0));
        s.rotation = j["rotation"].toDouble(0.0);
        s.sides = j["sides"].toInt(5);
        s.innerRadiusRatio = j["innerRadiusRatio"].toDouble(0.5);
        s.fillEnabled = j["fillEnabled"].toBool(true);
        s.fillColor = QColor(j["fillColor"].toString(QStringLiteral("#ffffffff")));
        s.strokeEnabled = j["strokeEnabled"].toBool(true);
        s.strokeColor = QColor(j["strokeColor"].toString(QStringLiteral("#ff000000")));
        s.strokeWidth = j["strokeWidth"].toDouble(4.0);
        s.opacity = j["opacity"].toDouble(1.0);
        s.visible = j["visible"].toBool(true);
        s.groupId = j["groupId"].toString();
        legacyShapes.append(s);
    }
    auto imageFromBase64Png = [](const QString &b64) -> QImage {
        if (b64.isEmpty()) return QImage();
        QByteArray bytes = QByteArray::fromBase64(b64.toLatin1());
        QImage img;
        img.loadFromData(bytes, "PNG");
        return img;
    };
    for (const QJsonValue &v : o["removals"].toArray()) {
        QJsonObject j = v.toObject();
        RemoveObjectOp r;
        for (const QJsonValue &pv : j["stroke"].toArray()) {
            QJsonArray p = pv.toArray();
            r.stroke.append(QPointF(p.at(0).toDouble(), p.at(1).toDouble()));
        }
        r.radius = j["radius"].toDouble(20.0);
        r.rect = QRect(j["rectX"].toInt(0), j["rectY"].toInt(0),
                       j["rectW"].toInt(0), j["rectH"].toInt(0));
        r.mask = imageFromBase64Png(j["mask"].toString());
        r.fill = imageFromBase64Png(j["fill"].toString());
        r.visible = j["visible"].toBool(true);
        if (!r.rect.isEmpty() && !r.fill.isNull()) a.removals.append(r);
    }

    // Load-time migration (pre-version-7 sidecars): synthesize equivalent
    // MaskType::Shape/TextBox entries in a.masks from the legacy
    // legacyTexts/legacyShapes vectors parsed above. Best-effort z-order
    // reconstruction only — old files never had a true interleaved
    // masks/texts/shapes/paint concept, so this approximates the legacy
    // composite order (non-Paint masks, then texts, then shapes, then Paint
    // masks) by inserting migrated text-masks then migrated shape-masks
    // immediately above all existing non-Paint masks and below any Paint
    // masks. This is now the ONLY place shape/text content ends up:
    // Adjustments::shapes/texts no longer exist, so masks is the sole
    // representation, avoiding the old double-render.
    if (fileVersion < 7 && (!legacyTexts.isEmpty() || !legacyShapes.isEmpty())) {
        int insertAt = a.masks.size();
        for (int i = 0; i < a.masks.size(); ++i) {
            if (a.masks[i].type == MaskType::Paint) { insertAt = i; break; }
        }
        QVector<Mask> migrated;
        for (const TextOp &t : legacyTexts) {
            Mask m;
            m.type = MaskType::TextBox;
            m.visible = true;
            m.textBoxPos = t.pos;
            m.textBoxRotation = t.rotation;
            m.textBoxText = t.text;
            m.textBoxFamily = t.family;
            m.textBoxPixelSize = t.pixelSize;
            m.textBoxBold = t.bold;
            m.textBoxItalic = t.italic;
            m.textBoxColor = t.color;
            m.textBoxOutlineEnabled = t.outlineEnabled;
            m.textBoxOutlineColor = t.outlineColor;
            m.textBoxOutlineWidth = t.outlineWidth;
            m.textBoxShadowEnabled = t.shadowEnabled;
            m.textBoxShadowOffset = t.shadowOffset;
            m.textBoxShadowBlur = t.shadowBlur;
            m.textBoxShadowOpacity = t.shadowOpacity;
            m.textBoxShadowColor = t.shadowColor;
            m.textBoxBgEnabled = t.bgEnabled;
            m.textBoxBgColor = t.bgColor;
            m.textBoxBgOpacity = t.bgOpacity;
            m.textBoxBgPadding = t.bgPadding;
            migrated.append(m);
        }
        for (const ShapeOp &s : legacyShapes) {
            Mask m;
            m.type = MaskType::Shape;
            m.visible = s.visible;
            m.opacity = s.opacity;
            m.groupId = s.groupId;
            m.shapeType = s.type;
            m.shapeRect = s.rect;
            m.shapeP1 = s.p1;
            m.shapeP2 = s.p2;
            m.shapeRotation = s.rotation;
            m.shapeSides = s.sides;
            m.shapeInnerRadiusRatio = s.innerRadiusRatio;
            m.shapeFillEnabled = s.fillEnabled;
            m.shapeFillColor = s.fillColor;
            m.shapeStrokeEnabled = s.strokeEnabled;
            m.shapeStrokeColor = s.strokeColor;
            m.shapeStrokeWidth = s.strokeWidth;
            migrated.append(m);
        }
        for (int i = 0; i < migrated.size(); ++i)
            a.masks.insert(insertAt + i, migrated[i]);
    }

    // Load-time migration: sidecars written before the Background layer
    // became a normal masks[] entry carry the old standalone
    // "backgroundHidden"/"backgroundDeleted" fields instead. Synthesize an
    // equivalent MaskType::Background entry at the bottom of the stack (its
    // old pinned visual position) so the rest of the app never has to know
    // the difference. `backgroundDeleted` was a permanent one-way flag, so a
    // deleted background simply gets no entry synthesized (matching the old
    // "row disappears forever" behaviour) — the user can always add a fresh
    // one being genuinely reorderable from here on is moot since it no
    // longer exists, same as before.
    {
        bool hasBackgroundMask = false;
        for (const Mask &m : a.masks)
            if (m.type == MaskType::Background) { hasBackgroundMask = true; break; }
        if (!hasBackgroundMask && !o["backgroundDeleted"].toBool(false)) {
            Mask bg;
            bg.type = MaskType::Background;
            bg.name = QStringLiteral("Background");
            bg.visible = !o["backgroundHidden"].toBool(false);
            a.masks.append(bg);
        }
    }

    return a;
}

} // namespace

namespace EditSidecar {

QString pathFor(const QString &imagePath) {
    return imagePath + ".nte.json";
}

bool exists(const QString &imagePath) {
    return QFileInfo::exists(pathFor(imagePath));
}

bool save(const QString &imagePath, const Adjustments &a) {
    // Rating and working color space live in the same sidecar file but
    // aren't part of Adjustments, so carry over whatever was there before
    // this rewrite (saveWorkingColorSpace() is the only writer of the latter
    // — see RetouchTab::saveEdits()).
    int existingRating = loadRating(imagePath);
    bool hadSidecar = exists(imagePath);
    WorkingColorSpace existingSpace = loadWorkingColorSpace(imagePath);
    QJsonObject o = adjustmentsToJson(a);
    if (existingRating > 0) o["rating"] = existingRating;
    if (hadSidecar) o["workingColorSpace"] = int(existingSpace);

    QFile f(pathFor(imagePath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

bool load(const QString &imagePath, Adjustments &out) {
    QFile f(pathFor(imagePath));
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    out = adjustmentsFromJson(doc.object());
    return true;
}

// Self-contained project file: a real zip archive (see ZipFile.h) holding
// "project.json" (the same Adjustments blob as a sidecar, nested under
// "adjustments") and "base.png" (the tab's own base pixels as plain binary
// PNG, not base64-inflated) — so the file has no dependency on an external
// photo on disk (used for File > New documents, and as a "Save As project"
// option for any document). Both entries are deflate-compressed by libzip,
// and storing the PNG as raw bytes rather than base64 avoids the ~33% size
// penalty base64 would add on top of PNG's own compression.
bool saveProject(const QString &path, const QImage &base, const Adjustments &a) {
    QJsonObject root;
    root["projectFormat"] = 1;
    root["adjustments"] = adjustmentsToJson(a);

    QByteArray pngBytes;
    QBuffer buf(&pngBytes);
    buf.open(QIODevice::WriteOnly);
    if (!base.save(&buf, "PNG")) return false;

    QMap<QString, QByteArray> entries;
    entries["project.json"] = QJsonDocument(root).toJson(QJsonDocument::Indented);
    entries["base.png"] = pngBytes;
    return ZipFile::write(path, entries);
}

// Fills `base`/`out` and returns true if a project file was read successfully.
bool loadProject(const QString &path, QImage &base, Adjustments &out) {
    QMap<QString, QByteArray> entries;
    if (!ZipFile::read(path, entries)) return false;
    if (!entries.contains("project.json") || !entries.contains("base.png")) return false;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(entries["project.json"], &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;

    QImage img;
    if (!img.loadFromData(entries["base.png"], "PNG")) return false;

    base = img;
    out = adjustmentsFromJson(doc.object()["adjustments"].toObject());
    return true;
}

QString thumbnailPathFor(const QString &imagePath) {
    return imagePath + ".nte.thumb.jpg";
}

bool saveThumbnail(const QString &imagePath, const QImage &image) {
    if (image.isNull()) return false;
    // Cap the cached thumbnail's size — the filmstrip icon is tiny, so a modest
    // JPEG keeps the sidecar footprint small.
    QImage scaled = image.width() > 320
                        ? image.scaledToWidth(320, Qt::SmoothTransformation)
                        : image;
    return ditherTo8Bit(scaled).save(thumbnailPathFor(imagePath), "JPEG", 85);
}

QImage loadThumbnail(const QString &imagePath) {
    QImage img;
    img.load(thumbnailPathFor(imagePath), "JPEG");
    return img;
}

int loadRating(const QString &imagePath) {
    QFile f(pathFor(imagePath));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return 0;
    return doc.object()["rating"].toInt(0);
}

bool saveRating(const QString &imagePath, int rating) {
    QFile f(pathFor(imagePath));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) o = doc.object();
        f.close();
    }
    if (rating > 0)
        o["rating"] = rating;
    else
        o.remove("rating");
    if (!o.contains("version")) o["version"] = 7;

    QFile out(pathFor(imagePath));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    out.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

WorkingColorSpace loadWorkingColorSpace(const QString &imagePath) {
    QFile f(pathFor(imagePath));
    if (!f.open(QIODevice::ReadOnly)) return WorkingColorSpace::sRGB;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return WorkingColorSpace::sRGB;
    return static_cast<WorkingColorSpace>(
        doc.object()["workingColorSpace"].toInt(int(WorkingColorSpace::sRGB)));
}

bool saveWorkingColorSpace(const QString &imagePath, WorkingColorSpace space) {
    QFile f(pathFor(imagePath));
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) o = doc.object();
        f.close();
    }
    o["workingColorSpace"] = int(space);
    if (!o.contains("version")) o["version"] = 7;

    QFile out(pathFor(imagePath));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    out.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace EditSidecar
