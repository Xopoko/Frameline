#include "annotations.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int blurRadius = 15;
constexpr int blurDiameter = blurRadius * 2 + 1;

// Keep the channels in separate 16-bit lanes. A 31-pixel sum fits in each lane.
quint64 expandChannels(QRgb pixel)
{
    constexpr quint64 mask = 0x00ff00ff;
    return (pixel & mask) | (((pixel >> 8) & mask) << 32);
}

QRgb averageChannels(quint64 sum)
{
    return qRgba(((sum >> 16) & 0xffff) / blurDiameter,
                 ((sum >> 32) & 0xffff) / blurDiameter,
                 (sum & 0xffff) / blurDiameter,
                 (sum >> 48) / blurDiameter);
}

void blurRows(const QImage &source, QImage &target)
{
    const int width = source.width();
    for (int y = 0; y < source.height(); ++y) {
        const auto *in = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(target.scanLine(y));
        quint64 sum = expandChannels(in[0]) * (blurRadius + 1);
        for (int x = 1; x <= blurRadius; ++x) {
            sum += expandChannels(in[std::min(x, width - 1)]);
        }
        for (int x = 0; x < width; ++x) {
            out[x] = averageChannels(sum);
            sum += expandChannels(in[std::min(x + blurRadius + 1, width - 1)]);
            sum -= expandChannels(in[std::max(x - blurRadius, 0)]);
        }
    }
}

void blurColumns(const QImage &source, QImage &target, std::vector<quint64> &sums)
{
    const int width = source.width();
    const int last = source.height() - 1;
    const auto *first = reinterpret_cast<const QRgb *>(source.constScanLine(0));
    for (int x = 0; x < width; ++x) {
        sums[x] = expandChannels(first[x]) * (blurRadius + 1);
    }
    for (int y = 1; y <= blurRadius; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(source.constScanLine(std::min(y, last)));
        for (int x = 0; x < width; ++x) {
            sums[x] += expandChannels(row[x]);
        }
    }
    // Walk rows in memory order instead of stepping by the full image stride per pixel.
    auto *sum = sums.data();
    for (int y = 0; y <= last; ++y) {
        const auto *entering = reinterpret_cast<const QRgb *>(source.constScanLine(std::min(y + blurRadius + 1, last)));
        const auto *leaving = reinterpret_cast<const QRgb *>(source.constScanLine(std::max(y - blurRadius, 0)));
        auto *out = reinterpret_cast<QRgb *>(target.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const quint64 current = sum[x];
            out[x] = averageChannels(current);
            sum[x] = current + expandChannels(entering[x]) - expandChannels(leaving[x]);
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
    std::vector<quint64> sums(pixels.width());
    for (int repeat = 0; repeat < 3; ++repeat) {
        blurRows(pixels, pass);
        blurColumns(pass, pixels, sums);
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

} // namespace

QRectF Annotation::bounds() const
{
    if (points.size() < 2) {
        return {};
    }
    return QRectF(points.first(), points.last()).normalized();
}

QRect Annotation::affectedPixels() const
{
    if (!hasMark()) {
        return {};
    }
    if (tool == Tool::Blur || tool == Tool::Redact) {
        return bounds().toAlignedRect();
    }

    QRectF area = bounds();
    if (tool == Tool::Pen) {
        QPointF topLeft = points.first();
        QPointF bottomRight = topLeft;
        for (const QPointF &point : points) {
            topLeft.setX(std::min(topLeft.x(), point.x()));
            topLeft.setY(std::min(topLeft.y(), point.y()));
            bottomRight.setX(std::max(bottomRight.x(), point.x()));
            bottomRight.setY(std::max(bottomRight.y(), point.y()));
        }
        area = QRectF(topLeft, bottomRight);
    }
    const qreal margin = (tool == Tool::Arrow ? std::max(10.0, width * 3.5) : width) / 2 + 2;
    return area.adjusted(-margin, -margin, margin, margin).toAlignedRect();
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
    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_RGB32) {
        image.convertTo(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    }
    image.setDevicePixelRatio(1);
    if (annotation.tool == Tool::Blur) {
        const QRect area = annotation.affectedPixels().intersected(image.rect());
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
