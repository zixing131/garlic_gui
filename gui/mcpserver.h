#pragma once
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QTcpServer>
class MainWindow;
class Backend;
class QLocalSocket;
class McpServer : public QObject {
    Q_OBJECT
  public:
    McpServer(MainWindow *window, QObject *parent = nullptr);
    McpServer(Backend *backend, QObject *parent = nullptr);
    ~McpServer() override { stop(); }
    bool start(QString *error = nullptr, bool privateSession = false);
    void stop();
    QString endpoint() const { return endpoint_; }
    QJsonObject clientConfig() const;
    QJsonObject httpConfig(int port = 0) const;
    QString httpUrl() const;
    bool isHttpListening() const { return http_.isListening(); }

  private:
    void dispatch(QLocalSocket *socket, const QJsonObject &request);
    MainWindow *window_ = nullptr;
    Backend *backend_;
    QLocalServer server_;
    QString endpoint_;
    QTcpServer http_;
    int requestedHttpPort_ = -1;
    void acceptHttp();
};
int runHeadless(int argc, char **argv);
int runMcpBridge(int argc, char **argv);

// Asynchronous stdio transport shared by GUI bridges and headless sessions.
void startMcpStdio(const QString &endpoint);
