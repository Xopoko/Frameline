#pragma once

#include <QDialog>

class Button;
class QLabel;
class QShowEvent;
class WelcomePreview;

class WelcomeWindow final : public QDialog {
public:
    enum Result { OpenApp = QDialog::Accepted, TryCapture = 2 };

    explicit WelcomeWindow(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void showStep(int step);

    WelcomePreview *preview_;
    QLabel *heading_;
    QLabel *description_;
    Button *back_;
    Button *next_;
    int step_ = 0;
};
