#pragma once

class QDateTime;
class QImage;
class QString;
class QWidget;

bool savePng(const QImage &image, const QString &path, QString *error);

// Asks for a destination in Pictures and saves the image there. The capture time names the file.
// Returns false if the user cancels or the save fails; failures are reported in a message box.
bool saveImageAs(QWidget *parent, const QImage &image, const QDateTime &capturedAt);
