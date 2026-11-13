#pragma once

#include <QMainWindow>

#include <memory>

class LibraryClient;
class QShowEvent;

class LibraryWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit LibraryWindow(LibraryClient &client, QWidget *parent = nullptr);
    ~LibraryWindow() override;

signals:
    void settingsRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    struct Ui;
    std::unique_ptr<Ui> ui;
};
