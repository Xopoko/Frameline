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
    // The keys of a shortcut as shown to the user. A Wayland desktop may assign other keys.
    QString description(bool region) const;
    // The keys Frameline asks for, in the platform's notation.
    static QString defaultKeys(bool region, RegionShortcutPreset preset);

signals:
    void captureRequested(bool region);
    void statusChanged(bool full, bool region, const QString &message);

private:
    class Backend;
    std::unique_ptr<Backend> backend_;
};
