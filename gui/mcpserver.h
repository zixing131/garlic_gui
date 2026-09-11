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
    bool start(QString *error = nullptr);
    void stop();
    QString endpoint() const { return endpoint_; }
    QJsonObject clientConfig() const;
    QJsonObject httpConfig(int port = 0) const;
    QString httpUrl() const;

  private:
    void dispatch(QLocalSocket *socket, const QJsonObject &request);
    MainWindow *window_ = nullptr;
    Backend *backend_;
    QLocalServer server_;
    QString endpoint_;
    QTcpServer http_;
    void acceptHttp();
};
int runHeadless(int argc, char **argv);
int runMcpBridge(int argc, char **argv);
