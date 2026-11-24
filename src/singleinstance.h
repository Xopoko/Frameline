#pragma once

#include <QObject>

#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;

class SingleInstance final : public QObject {
    Q_OBJECT

public:
    enum class Request { Editor, Library, Settings, Quiet, Welcome };
    Q_ENUM(Request)

    enum class Result { Primary, Forwarded, Failed };

    explicit SingleInstance(const QString &runtimeDirectory = {}, QObject *parent = nullptr);
    ~SingleInstance() override;

    Result start(Request request, QString *error = nullptr);
    static bool requestSettings();

signals:
    void activationRequested(SingleInstance::Request request);

private:
    void readRequest(QLocalSocket *socket);

    QString runtimeDirectory_;
    std::unique_ptr<QLockFile> lock_;
    std::unique_ptr<QLocalServer> server_;
};
