#include "mainwindow.h"
#include "mcpserver.h"
#include <QJsonDocument>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSettings>
#include <QtTest>
#include <QtWidgets>

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
    void idleBridgeDisconnect() {
        Backend backend;
        McpServer server(&backend);
        QString error;
        QVERIFY(server.start(&error, true));
        for (int i = 0; i < 4; ++i) {
            QProcess bridge;
            bridge.start(qEnvironmentVariable("GARLIC_TEST_GUI"), {"--mcp", "--socket", server.endpoint()});
            QVERIFY(bridge.waitForStarted(3000));
            bridge.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n");
            QTRY_VERIFY_WITH_TIMEOUT(bridge.bytesAvailable() > 0, 5000);
            QVERIFY(bridge.readAllStandardOutput().contains("tools"));
            // Keep stdin open, including an unfinished line: disconnect must
            // still terminate the bridge without waiting for another request.
            bridge.write("{\"jsonrpc\":");
            server.stop();
            QTRY_COMPARE_WITH_TIMEOUT(bridge.state(), QProcess::NotRunning, 3000);
            QVERIFY(bridge.exitCode() != 0);
            QVERIFY(server.start(&error, true));
        }
    }
    void defaultHttpAddress() {
        auto settings = AppSettings::fromJson({});
        QCOMPARE(settings.mcpHost, QString("127.0.0.1"));
        settings.mcpTransport = "http"; settings.mcpPort = 0;
        Backend backend; backend.configure(settings);
        McpServer server(&backend);
        QString error;
        QVERIFY2(server.start(&error), qPrintable(error));
        QVERIFY(server.httpUrl().startsWith("http://127.0.0.1:"));
        QCOMPARE(AppSettings::fromJson({{"mcpHost", "0.0.0.0"}}).mcpHost, QString("0.0.0.0"));
    }
    void privateSessionEndpoints() {
        Backend backend;
        McpServer first(&backend), second(&backend);
        QString error;
        QVERIFY2(first.start(&error, true), qPrintable(error));
        QVERIFY2(second.start(&error, true), qPrintable(error));
        QVERIFY(first.endpoint() != second.endpoint());
#ifndef Q_OS_WIN
        QVERIFY(first.endpoint().toUtf8().size() < 104);
#endif
        QLocalSocket socket;
        socket.connectToServer(first.endpoint());
        QVERIFY(socket.waitForConnected(3000));
        QVERIFY(call(socket, "initialize").contains("result"));
        socket.abort();
        first.stop();
        // Stopping one script session must not invalidate another session.
        socket.connectToServer(second.endpoint());
        QVERIFY(socket.waitForConnected(3000));
        QVERIFY(call(socket, "initialize").contains("result"));
    }
    void multipleGuiHttpPorts() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("McpTest");
        QSettings().clear();
        // Find a free consecutive range without assuming developer ports are free.
        QTcpServer reservations[4];
        int base = 0;
        for (int attempt = 0; attempt < 100 && !base; ++attempt) {
            for (auto &reservation : reservations) reservation.close();
            QVERIFY(reservations[0].listen(QHostAddress::LocalHost, 0));
            const int candidate = reservations[0].serverPort();
            if (candidate > 65532) continue;
            bool available = true;
            for (int i = 1; i < 4; ++i)
                available = reservations[i].listen(QHostAddress::LocalHost, candidate + i) && available;
            if (available) base = candidate;
        }
        QVERIFY(base);
        for (auto &reservation : reservations) reservation.close();
        MainWindow first, second, third;
        auto settings = first.backend()->settings();
        settings.mcpEnabled = true;
        settings.mcpTransport = "http";
        settings.mcpPort = base;
        settings.save();
        auto a = first.findChild<McpServer *>();
        auto b = second.findChild<McpServer *>();
        auto c = third.findChild<McpServer *>();
        QVERIFY(a && b && c);
        first.applySettings(settings);
        second.applySettings(settings);
        third.applySettings(settings);
        QCOMPARE(QUrl(a->httpUrl()).port(), base);
        QCOMPARE(QUrl(b->httpUrl()).port(), base + 1);
        QCOMPARE(QUrl(c->httpUrl()).port(), base + 2);
        QVERIFY(a->endpoint() != b->endpoint());
        QVERIFY(a->endpoint() != c->endpoint());
        QVERIFY(b->endpoint() != c->endpoint());
        for (auto server : {a, b, c}) {
            QLocalSocket socket;
            socket.connectToServer(server->endpoint());
            QVERIFY(socket.waitForConnected(3000));
            QVERIFY(call(socket, "initialize").contains("result"));
        }
        // A headless service must still honor its explicitly requested port.
        Backend backend;
        backend.configure(settings);
        McpServer headless(&backend);
        QString error;
        QVERIFY(!headless.start(&error));
        QVERIFY(!error.isEmpty());
        a->stop();
        settings.showMemory = !settings.showMemory;
        second.applySettings(settings);
        QCOMPARE(QUrl(b->httpUrl()).port(), base + 1);
        QCOMPARE(second.backend()->settings().mcpPort, base);
        // The copied configuration must retain the actual port after reopening preferences.
        bool inspected = false;
        QTimer::singleShot(0, &second, [&] {
            auto dialog = second.findChild<QDialog *>();
            if (!dialog) return;
            auto config = dialog->findChild<QPlainTextEdit *>("mcpClientConfig");
            auto port = dialog->findChild<QSpinBox *>("mcpPort");
            auto activeUrl = dialog->findChild<QLabel *>("mcpActiveUrl");
            if (config && port && activeUrl) {
                const auto url = QJsonDocument::fromJson(config->toPlainText().toUtf8()).object()
                    .value("mcpServers").toObject().value("garlic").toObject().value("url").toString();
                inspected = url == b->httpUrl() && activeUrl->text() == url && port->value() == base;
            }
            dialog->accept();
        });
        second.findChild<QAction *>("settings")->trigger();
        QVERIFY(inspected);
        QCOMPARE(AppSettings::load().mcpPort, base);
        QCOMPARE(QUrl(b->httpUrl()).port(), base + 1);
        // Changing the starting port retries from the new base and skips a live instance.
        settings.mcpPort = base + 2;
        second.applySettings(settings);
        QCOMPARE(QUrl(b->httpUrl()).port(), base + 3);
        QNetworkAccessManager manager;
        for (auto server : {b, c}) {
            QNetworkRequest request{QUrl(server->httpUrl())};
            request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            auto reply = manager.post(request, R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
            QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            QVERIFY(QJsonDocument::fromJson(reply->readAll()).object().contains("result"));
            reply->deleteLater();
        }
        b->stop();
        settings.mcpPort = base;
        second.applySettings(settings);
        QCOMPARE(QUrl(b->httpUrl()).port(), base);
        QSettings().clear();
    }
    void httpPortExhaustion() {
        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 65535))
            QSKIP("Port 65535 unavailable for boundary test");
        MainWindow window;
        auto settings = window.backend()->settings();
        settings.mcpTransport = "http";
        settings.mcpPort = 65535;
        window.backend()->configure(settings);
        auto server = window.findChild<McpServer *>();
        QString error;
        QVERIFY(!server->start(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!server->isHttpListening());
        reservation.close();
        QVERIFY2(server->start(&error), qPrintable(error));
        QCOMPARE(QUrl(server->httpUrl()).port(), 65535);
        server->stop();
        settings.mcpHost = "invalid-ip";
        window.backend()->configure(settings);
        error.clear();
        QVERIFY(!server->start(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!server->isHttpListening());
    }
    void progressiveMembers() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("McpTest");
        QSettings().clear();
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings();
        settings.mcpEnabled = true; settings.background = false;
        window.applySettings(settings);
        window.backend()->setProperty("fastOpen", true);
        QLocalSocket socket;
        socket.connectToServer(window.mcpEndpoint());
        QVERIFY(socket.waitForConnected(3000));
        bool completed = false;
        connect(window.backend(), &Backend::indexed, &window, [&] {
            QVERIFY(!window.backend()->metadataReady());
            const auto result = tool(socket, "get_methods_of_class", {{"class_name", "demo/cases/foo"}});
            QVERIFY(!result.value("isError").toBool());
            QCOMPARE(result.value("members").toArray().size(), 1);
            QVERIFY(window.backend()->metadataReady());
            completed = true;
        });
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/cases.dex");
        QTRY_VERIFY_WITH_TIMEOUT(completed, 15000);
    }
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
        settings.mcpHost = "0.0.0.0";
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
        QVERIFY(!config.contains("headers"));
        auto reply = manager.post(request, body);
        QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        auto json = QJsonDocument::fromJson(reply->readAll()).object();
        QVERIFY(json.value("result").toObject().contains("serverInfo"));
        reply->deleteLater();
        auto futureBody = body;
        futureBody.replace("2025-03-26", "9999-01-01");
        auto negotiated = manager.post(request, futureBody);
        QTRY_VERIFY_WITH_TIMEOUT(negotiated->isFinished(), 5000);
        QCOMPARE(QJsonDocument::fromJson(negotiated->readAll()).object().value("result").toObject().value("protocolVersion").toString(), QString("2025-06-18"));
        negotiated->deleteLater();
        request.setRawHeader("MCP-Protocol-Version", "2025-06-18");
        auto tools = manager.post(request, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        QTRY_VERIFY_WITH_TIMEOUT(tools->isFinished(), 5000);
        QCOMPARE(tools->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY(!QJsonDocument::fromJson(tools->readAll()).object().value("result").toObject().value("tools").toArray().isEmpty());
        tools->deleteLater();
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
        QTemporaryDir alternateTemp;
        QProcess bridge;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("TMPDIR", alternateTemp.path());
        bridge.setProcessEnvironment(environment);
        bridge.start(qEnvironmentVariable("GARLIC_TEST_GUI"),
                     {"--mcp", "--socket", "garlic-gui-legacy-missing-endpoint"});
        QVERIFY(bridge.waitForStarted(3000));
        QVERIFY(bridge.waitForFinished(4000));
        QVERIFY(bridge.exitCode() != 0); // Explicit endpoints must not attach to another project.
        QVERIFY(bridge.readAllStandardOutput().isEmpty());
        bridge.start(qEnvironmentVariable("GARLIC_TEST_GUI"), {"--mcp"});
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
