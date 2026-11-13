#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QVector>

class QPainter;

namespace screenframe {

enum class Tool { Arrow, Pen, Rectangle, Blur, Redact };

struct Annotation {
    Tool tool;
    QColor color;
    qreal width;
    QVector<QPointF> points;

    QRectF bounds() const;
    bool hasMark() const;
};

void paintAnnotation(QPainter &painter, const Annotation &annotation);
void applyAnnotation(QImage &image, const Annotation &annotation);
QImage renderAnnotations(const QImage &source, const QVector<Annotation> &annotations, qsizetype count);

}
