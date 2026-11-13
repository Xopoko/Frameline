#include "annotations.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace screenframe {
namespace {

// A running window keeps the work proportional to the number of pixels, even for large blurs.
void boxPass(const QImage &source, QImage &target, int radius, bool horizontal)
{
    const int length = horizontal ? source.width() : source.height();
    const int lines = horizontal ? source.height() : source.width();
    const int diameter = radius * 2 + 1;
    const auto *input = reinterpret_cast<const QRgb *>(source.constBits());
    auto *output = reinterpret_cast<QRgb *>(target.bits());
    const int sourcePitch = source.bytesPerLine() / sizeof(QRgb);
    const int targetPitch = target.bytesPerLine() / sizeof(QRgb);
    const int sourceLine = horizontal ? sourcePitch : 1;
    const int sourceStep = horizontal ? 1 : sourcePitch;
    const int targetLine = horizontal ? targetPitch : 1;
    const int targetStep = horizontal ? 1 : targetPitch;

    for (int line = 0; line < lines; ++line) {
        const QRgb *in = input + line * sourceLine;
        QRgb *out = output + line * targetLine;
        int red = 0, green = 0, blue = 0, alpha = 0;
        auto accumulate = [&](int offset, int sign) {
            const QRgb pixel = in[std::clamp(offset, 0, length - 1) * sourceStep];
            red += sign * qRed(pixel);
            green += sign * qGreen(pixel);
            blue += sign * qBlue(pixel);
            alpha += sign * qAlpha(pixel);
        };
        for (int offset = -radius; offset <= radius; ++offset) {
            accumulate(offset, 1);
        }
        for (int index = 0; index < length; ++index) {
            out[index * targetStep] = qRgba(red / diameter, green / diameter,
                                            blue / diameter, alpha / diameter);
            accumulate(index - radius, -1);
            accumulate(index + radius + 1, 1);
        }
    }
}

QImage blurredRegion(const QImage &image, const QRect &area)
{
    QImage pixels = image.copy(area);
    if (pixels.isNull()) {
        return {};
    }
    QImage pass(pixels.size(), QImage::Format_ARGB32_Premultiplied);
    if (pass.isNull()) {
        return {};
    }
    constexpr int radius = 15;
    for (int repeat = 0; repeat < 3; ++repeat) {
        boxPass(pixels, pass, radius, true);
        boxPass(pass, pixels, radius, false);
    }
    return pixels;
}

void paintArrow(QPainter &painter, const Annotation &annotation)
{
    const QPointF start = annotation.points.first();
    const QPointF end = annotation.points.last();
    const QPointF delta = end - start;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (length < 1) {
        return;
    }

    const QPointF axis = delta / length;
    const QPointF perpendicular(-axis.y(), axis.x());
    const qreal headLength = std::min(length * 0.55, std::max(10.0, annotation.width * 3.5));
    const QPointF headBase = end - axis * headLength;
    const QPointF headHalfWidth = perpendicular * (headLength * 0.48);

    painter.drawLine(start, end - axis * (headLength * 0.65));
    painter.setPen(Qt::NoPen);
    painter.setBrush(annotation.color);
    painter.drawPolygon(QPolygonF{end, headBase + headHalfWidth, headBase - headHalfWidth});
}

}

QRectF Annotation::bounds() const
{
    if (points.size() < 2) {
        return {};
    }
    return QRectF(points.first(), points.last()).normalized();
}

bool Annotation::hasMark() const
{
    if (points.isEmpty()) {
        return false;
    }
    switch (tool) {
    case Tool::Pen:
        return true;
    case Tool::Arrow:
        return QLineF(points.first(), points.last()).length() >= 2;
    case Tool::Rectangle:
    case Tool::Blur:
    case Tool::Redact:
        return bounds().width() >= 2 && bounds().height() >= 2;
    }
    return false;
}

void paintAnnotation(QPainter &painter, const Annotation &annotation)
{
    if (!annotation.hasMark() || annotation.tool == Tool::Blur) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(annotation.color, annotation.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (annotation.tool) {
    case Tool::Arrow:
        paintArrow(painter, annotation);
        break;
    case Tool::Pen: {
        QPainterPath path(annotation.points.first());
        for (const QPointF &point : annotation.points) {
            path.lineTo(point);
        }
        if (path.isEmpty()) {
            painter.drawPoint(annotation.points.first());
        } else {
            painter.drawPath(path);
        }
        break;
    }
    case Tool::Rectangle:
        painter.drawRect(annotation.bounds());
        break;
    case Tool::Redact: {
        QColor opaqueColor = annotation.color;
        opaqueColor.setAlpha(255);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(annotation.bounds().toAlignedRect(), opaqueColor);
        break;
    }
    case Tool::Blur:
        break;
    }
    painter.restore();
}

void applyAnnotation(QImage &image, const Annotation &annotation)
{
    if (image.isNull() || !annotation.hasMark()) {
        return;
    }
    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        image.convertTo(QImage::Format_ARGB32_Premultiplied);
    }
    image.setDevicePixelRatio(1);
    if (annotation.tool == Tool::Blur) {
        const QRect area = annotation.bounds().toAlignedRect().intersected(image.rect());
        if (area.isEmpty()) {
            return;
        }
        const QImage blur = blurredRegion(image, area);
        if (blur.isNull()) {
            return;
        }
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(area.topLeft(), blur);
    } else {
        QPainter painter(&image);
        paintAnnotation(painter, annotation);
    }
}

QImage renderAnnotations(const QImage &source, const QVector<Annotation> &annotations, qsizetype count)
{
    QImage result = source;
    result.setDevicePixelRatio(1);
    for (qsizetype index = 0; index < std::min(count, annotations.size()); ++index) {
        applyAnnotation(result, annotations.at(index));
    }
    return result;
}

}
