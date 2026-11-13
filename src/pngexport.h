#pragma once

class QImage;
class QString;

bool savePng(const QImage &image, const QString &path, QString *error);
