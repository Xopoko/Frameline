#pragma once

#include <QByteArray>
#include <QString>
#include <QVariant>

namespace Autostart {

struct State {
    QVariant entry;

    bool isEnabled() const { return entry.isValid(); }
};

State snapshot(QString *error = nullptr);
bool restore(const State &state, QString *error = nullptr);
bool isEnabled(QString *error = nullptr);
bool setEnabled(bool enabled, QString *error = nullptr);
// Updates startup entries created by earlier releases for this installation.
void refreshAtStartup();

namespace Detail {

QString windowsCommand(const QString &executable);
QByteArray macAgent(const QString &executable);
QByteArray linuxEntry(const QString &executable);
bool matchesPreviousEntry(const State &state, const QString &executable, bool screenFrame);
State fileSnapshot(const QString &path, QString *error = nullptr);
bool restoreFile(const QString &path, const State &state, QString *error = nullptr);

} // namespace Detail
} // namespace Autostart
