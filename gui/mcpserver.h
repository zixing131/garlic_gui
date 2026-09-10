#pragma once
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QTcpServer>
class MainWindow;
class QLocalSocket;
class McpServer : public QObject {
    Q_OBJECT
  public:
    McpServer(MainWindow *window, QObject *parent = nullptr);
    bool start(QString *error = nullptr);
    void stop();
    QString endpoint() const { return endpoint_; }
    QJsonObject clientConfig() const;
    QJsonObject httpConfig(int port = 0) const;
    QString httpUrl() const;

  private:
    void dispatch(QLocalSocket *socket, const QJsonObject &request);
    MainWindow *window_;
    QLocalServer server_;
    QString endpoint_, httpToken_;
    QTcpServer http_;
    void acceptHttp();
};
int runMcpBridge(int argc, char **argv);
