#pragma once

#include <QMainWindow>

#include <memory>

class QImage;
class QShowEvent;

class EditorWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit EditorWindow(QWidget *parent = nullptr);
    ~EditorWindow() override;

    void setCapture(const QImage &image);
    void setShortcuts(const QString &full, const QString &region, const QString &warning = {});
    bool hasUnsavedChanges() const;
    bool confirmReplace();

signals:
    void fullCaptureRequested();
    void regionCaptureRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    struct Ui;
    std::unique_ptr<Ui> ui;
};
