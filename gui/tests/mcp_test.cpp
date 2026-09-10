#include "mainwindow.h"
#include "mcpserver.h"
#include <QJsonDocument>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSettings>
#include <QtTest>

class McpTest : public QObject {
    Q_OBJECT
    int id_ = 0;
    QJsonObject call(QLocalSocket &socket, const QString &method, const QJsonObject &params = {}) {
        const int id = ++id_;
        socket.write(
            QJsonDocument(
                QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})
                .toJson(QJsonDocument::Compact) +
            "\n");
        socket.flush();
        QElapsedTimer timer;
        timer.start();
        while (!socket.canReadLine() && timer.elapsed() < 30000) {
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        if (!socket.canReadLine())
            return {{"timeout", true}};
        return QJsonDocument::fromJson(socket.readLine()).object();
    }
    QJsonObject tool(QLocalSocket &socket, const QString &name, const QJsonObject &args = {}) {
        auto reply = call(socket, "tools/call", {{"name", name}, {"arguments", args}})
                         .value("result")
                         .toObject();
        const auto text =
            reply.value("content").toArray().first().toObject().value("text").toString();
        auto result = QJsonDocument::fromJson(text.toUtf8()).object();
        result["isError"] = reply.value("isError");
        return result;
    }
  private slots:
    void httpTransport() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("McpTest");
        QSettings().clear();
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        auto port = reservation.serverPort();
        reservation.close();
        auto settings = window.backend()->settings();
        settings.mcpEnabled = true;
        settings.mcpTransport = "http";
        settings.mcpPort = port;
        window.applySettings(settings);
        auto server = window.findChild<McpServer *>();
        QVERIFY(server);
        auto config =
            server->httpConfig().value("mcpServers").toObject().value("garlic").toObject();
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(config.value("url").toString()));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QByteArray body =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})";
        auto denied = manager.post(request, body);
        QTRY_VERIFY_WITH_TIMEOUT(denied->isFinished(), 5000);
        QCOMPARE(denied->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
        denied->deleteLater();
        request.setRawHeader(
            "Authorization",
            config.value("headers").toObject().value("Authorization").toString().toUtf8());
        auto reply = manager.post(request, body);
        QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        auto json = QJsonDocument::fromJson(reply->readAll()).object();
        QVERIFY(json.value("result").toObject().contains("serverInfo"));
        reply->deleteLater();
        request.setRawHeader("Origin", "https://untrusted.example");
        auto origin = manager.post(request, body);
        QTRY_VERIFY_WITH_TIMEOUT(origin->isFinished(), 5000);
        QCOMPARE(origin->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
        origin->deleteLater();
        request.setRawHeader("Origin", {});
        auto notification =
            manager.post(request, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        QTRY_VERIFY_WITH_TIMEOUT(notification->isFinished(), 5000);
        QCOMPARE(notification->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 202);
    }
    void toolsAndBridge() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("McpTest");
        QSettings().clear();
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings();
        settings.mcpEnabled = true;
        settings.background = false;
        window.applySettings(settings);
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        QLocalSocket socket;
        socket.connectToServer(window.mcpEndpoint());
        QVERIFY(socket.waitForConnected(3000));
        auto initialized = call(socket, "initialize",
                                {{"protocolVersion", "2024-11-05"},
                                 {"capabilities", QJsonObject{}},
                                 {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}});
        QCOMPARE(initialized.value("result").toObject().value("protocolVersion").toString(),
                 QString("2024-11-05"));
        auto list = call(socket, "tools/list").value("result").toObject().value("tools").toArray();
        QVERIFY(list.size() >= 20);
        auto classes = tool(socket, "get_all_classes");
        QVERIFY(!classes.value("isError").toBool());
        QVERIFY(classes.value("total").toInt() >= 5);
        auto code = tool(socket, "get_class_source", {{"class_name", "demo.Main"}});
        QVERIFY2(code.value("code").toString().contains("class Main"),
                 qPrintable(QJsonDocument(code).toJson()));
        auto ambiguous =
            tool(socket, "rename_method",
                 {{"class_name", "demo.Main"}, {"method_name", "greet"}, {"new_name", "welcome"}});
        QVERIFY(ambiguous.value("isError").toBool());
        auto rename = tool(socket, "rename_method",
                           {{"class_name", "demo.Main"},
                            {"method_name", "greet"},
                            {"method_signature", "(I)Ljava/lang/String;"},
                            {"new_name", "welcome"}});
        QVERIFY(!rename.value("isError").toBool());
        auto method = tool(socket, "get_method_by_name",
                           {{"class_name", "demo.Main"},
                            {"method_name", "welcome"},
                            {"method_signature", "(I)Ljava/lang/String;"}});
        QVERIFY(method.value("code").toString().contains("welcome(int"));
        QVERIFY(!method.value("code").toString().contains("greet(String"));
        auto refs = tool(socket, "get_xrefs_to_method",
                         {{"class_name", "demo.Main"},
                          {"method_name", "welcome"},
                          {"method_signature", "(I)Ljava/lang/String;"}});
        QVERIFY(refs.value("total").toInt() > 0);
        auto search =
            tool(socket, "search_classes_by_keyword",
                 {{"search_term", "literal greet should not change"}, {"search_in", "code"}});
        QVERIFY(!search.value("results").toArray().isEmpty());
        auto changed = tool(socket, "set_settings",
                            {{"settings", QJsonObject{{"threads", 1}, {"escapeUnicode", true}}}});
        QCOMPARE(changed.value("threads").toInt(), 1);
        QCoreApplication::processEvents();
        QProcess bridge;
        bridge.start(qEnvironmentVariable("GARLIC_TEST_GUI"),
                     {"--mcp", "--socket", window.mcpEndpoint()});
        QVERIFY(bridge.waitForStarted(3000));
        bridge.write("{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/list\"}\n");
        bridge.closeWriteChannel();
        QElapsedTimer timer;
        timer.start();
        while (bridge.state() != QProcess::NotRunning && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        QCOMPARE(bridge.exitCode(), 0);
        auto response = QJsonDocument::fromJson(bridge.readAllStandardOutput().trimmed()).object();
        QCOMPARE(response.value("id").toInt(), 42);
        QVERIFY(response.value("result").toObject().value("tools").toArray().size() >= 20);
        settings.mcpEnabled = false;
        window.applySettings(settings);
        window.close();
    }
};
QTEST_MAIN(McpTest)
#include "mcp_test.moc"
