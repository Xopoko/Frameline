#pragma once

#include <QMainWindow>

#include <memory>

class QImage;
class QCloseEvent;
class QShowEvent;

class EditorWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit EditorWindow(QWidget *parent = nullptr);
    ~EditorWindow() override;

    void setCapture(const QImage &image);
    QImage currentImage() const;
    void setShortcuts(const QString &full, const QString &region, const QString &warning = {});
    void setLibraryStatus(const QString &message);
    bool hasUnsavedChanges() const;
    bool confirmReplace();

signals:
    void fullCaptureRequested();
    void regionCaptureRequested();
    void libraryRequested();
    void closeAccepted();

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Ui;
    std::unique_ptr<Ui> ui;
};
