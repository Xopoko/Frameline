#pragma once

#include <QObject>

#include <memory>

class GlobalShortcuts : public QObject {
    Q_OBJECT

public:
    explicit GlobalShortcuts(QObject* parent = nullptr);
    ~GlobalShortcuts() override;
    void start();
    QString description(bool region) const;

signals:
    void captureRequested(bool region);
    void statusChanged(bool full, bool region, const QString& message);

private:
    class Backend;
    std::unique_ptr<Backend> backend_;
};
