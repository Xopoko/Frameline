#pragma once

#include <QDialog>

#include <memory>

struct AppSettings;
struct LibrarySettings;
struct TailscaleStatus;

class SettingsWindow final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsWindow(const AppSettings &settings, QWidget *parent = nullptr);
    ~SettingsWindow() override;

    AppSettings settings() const;
    bool autostartEditable() const;
    // An empty error means the connection succeeded.
    void setTestResult(const QString &error);
    void setSaveResult(const QString &error, const AppSettings &appliedSettings);
    void setTailscaleStatus(const TailscaleStatus &status);
    void setHostedStatus(bool running, const QString &shareAddress = {}, const QString &error = {});

signals:
    void testRequested(const LibrarySettings &settings);
    void saveRequested(const AppSettings &settings, bool autostart);
    void refreshTailscaleRequested();

protected:
    void accept() override;

private:
    struct Ui;
    std::unique_ptr<Ui> ui;
};
