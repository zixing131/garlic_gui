#pragma once
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
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

  private:
    void dispatch(QLocalSocket *socket, const QJsonObject &request);
    MainWindow *window_;
    QLocalServer server_;
    QString endpoint_;
};
int runMcpBridge(int argc, char **argv);
