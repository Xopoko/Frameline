#pragma once

#include <QDialog>

#include <memory>

struct AppSettings;
struct LibrarySettings;

class SettingsWindow final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsWindow(const AppSettings &settings, QWidget *parent = nullptr);
    ~SettingsWindow() override;

    AppSettings settings() const;
    // An empty error means the connection succeeded.
    void setTestResult(const QString &error);

signals:
    void testRequested(const LibrarySettings &settings);

protected:
    void accept() override;

private:
    struct Ui;
    std::unique_ptr<Ui> ui;
};
