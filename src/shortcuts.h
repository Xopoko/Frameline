#pragma once

#include <QObject>

#include <memory>

enum class RegionShortcutPreset { Standard, System };

class GlobalShortcuts : public QObject {
    Q_OBJECT

public:
    explicit GlobalShortcuts(QObject *parent = nullptr);
    ~GlobalShortcuts() override;
    void start(RegionShortcutPreset preset = RegionShortcutPreset::Standard);
    QString description(bool region) const;

signals:
    void captureRequested(bool region);
    void statusChanged(bool full, bool region, const QString &message);

private:
    class Backend;
    std::unique_ptr<Backend> backend_;
};
