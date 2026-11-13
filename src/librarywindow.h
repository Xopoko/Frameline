#pragma once

#include <QMainWindow>

#include <memory>

class LibraryClient;
class QShowEvent;

class LibraryWindow final : public QMainWindow {
public:
    explicit LibraryWindow(LibraryClient &client, QWidget *parent = nullptr);
    ~LibraryWindow() override;

protected:
    void showEvent(QShowEvent *event) override;

private:
    struct Ui;
    std::unique_ptr<Ui> ui;
};
