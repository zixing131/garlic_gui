#include "mcpserver.h"
#include "mainwindow.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>
#include <iostream>

namespace {
QJsonObject schema(const QStringList &required, const QJsonObject &properties) {
    return {{"type", "object"},
            {"required", QJsonArray::fromStringList(required)},
            {"properties", properties},
            {"additionalProperties", false}};
}
QJsonObject prop(const QString &type) { return {{"type", type}}; }
QJsonArray toolsList() {
    QJsonArray out;
    auto add = [&](const QString &name, const QString &description,
                   const QStringList &required = QStringList(),
                   const QJsonObject &properties = QJsonObject()) {
        out.append(QJsonObject{{"name", name},
                               {"description", description},
                               {"inputSchema", schema(required, properties)}});
    };
    const QJsonObject clazz{{"class_name", prop("string")}};
    add("fetch_current_class", "Read the selected GUI class and current code view.");
    add("get_selected_text", "Read text selected in the active code editor.");
    add("get_all_classes", "List classes, interfaces, enums and annotations with pagination.", {},
        {{"offset", prop("integer")}, {"count", prop("integer")}});
    add("get_package_tree", "Read package names and class counts.");
    for (const auto &name :
         {"get_class_source", "get_smali_of_class", "get_methods_of_class", "get_fields_of_class"})
        add(name,
            "Read the requested class's source or declared members. Class names may use dots or "
            "slashes.",
            {"class_name"}, clazz);
    add("search_method_by_name",
        "Search declared method symbols across the project without decompiling.", {"method_name"},
        {{"method_name", prop("string")}});
    add("get_method_by_name",
        "Read a method declaration and source. Supply method_signature to disambiguate overloads.",
        {"class_name", "method_name"},
        {{"class_name", prop("string")},
         {"method_name", prop("string")},
         {"method_signature", prop("string")}});
    add("search_classes_by_keyword",
        "Search class/member names or all generated Java source. Code search prepares project "
        "sources asynchronously; results are capped at 1000.",
        {"search_term"},
        {{"search_term", prop("string")},
         {"search_in", prop("string")},
         {"offset", prop("integer")},
         {"count", prop("integer")},
         {"regex", prop("boolean")},
         {"case_sensitive", prop("boolean")}});
    for (const auto &kind : {QString("class"), QString("method"), QString("field")}) {
        auto args = clazz;
        QStringList required{"class_name"};
        if (kind != "class") {
            args[kind + "_name"] = prop("string");
            required << kind + "_name";
        }
        args["method_signature"] = prop("string");
        args["offset"] = prop("integer");
        args["count"] = prop("integer");
        add("get_xrefs_to_" + kind,
            "Read bytecode references (DEX) or constant-pool references (JVM). Dynamic/reflection "
            "references are not inferred.",
            required, args);
        args.remove("offset");
        args.remove("count");
        args["new_name"] = prop("string");
        required << "new_name";
        add("rename_" + kind,
            "Set a reversible project symbol alias and update mapped source occurrences. Does not "
            "modify the original APK/JAR. Use signature for overloaded methods.",
            required, args);
    }
    add("undo_rename", "Undo the last project alias change.");
    add("get_cache_stats", "Read source cache and project generation state.");
    add("clear_cache", "Clear generated sources when the engine is idle.");
    add("get_settings", "Read effective Garlic GUI settings.");
    add("set_settings",
        "Update supported GUI settings, including threads and Unicode escaping. Engine settings "
        "apply to subsequent decompilations.",
        {"settings"}, {{"settings", prop("object")}});
    add("save_project", "Save input identity and aliases to a Garlic project file.", {"path"},
        {{"path", prop("string")}});
    add("cancel_task", "Stop foreground/background decompilation and pending search.");
    return out;
}
QJsonArray paginate(const QJsonArray &data, const QJsonObject &args) {
    QJsonArray out;
    int offset = qMax(0, args.value("offset").toInt()),
        count = qBound(1, args.value("count").toInt(100), 1000);
    for (int i = offset; i < data.size() && out.size() < count; i++)
        out.append(data[i]);
    return out;
}
QString memberId(Project *project, const QString &kind, const QJsonObject &args, QString *error) {
    const auto clazz = Project::normalize(args.value("class_name").toString());
    if (project->info(clazz).isEmpty()) {
        *error = "Class not found";
        return {};
    }
    if (kind == "class")
        return Project::classId(clazz);
    QStringList matches;
    for (const auto &v : project->members(clazz, kind == "method")) {
        auto m = v.toObject();
        const auto id = m.value("id").toString();
        if (m.value("name").toString() != args.value(kind + "_name").toString() &&
            project->symbolName(id) != args.value(kind + "_name").toString())
            continue;
        if (kind == "method" && args.contains("method_signature") &&
            m.value("descriptor") != args.value("method_signature"))
            continue;
        matches << id;
    }
    if (matches.size() != 1) {
        *error = matches.isEmpty() ? "Member not found"
                                   : "Ambiguous overloaded method; supply method_signature";
        return {};
    }
    return matches.first();
}
} // namespace
McpServer::McpServer(MainWindow *window, QObject *parent) : QObject(parent), window_(window) {
    endpoint_ =
        "garlic-gui-" + QString::fromLatin1(QCryptographicHash::hash(QDir::homePath().toUtf8(),
                                                                     QCryptographicHash::Sha256)
                                                .toHex()
                                                .left(12));
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (server_.hasPendingConnections()) {
            auto socket = server_.nextPendingConnection();
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
                if (buffer.size() > 4 * 1024 * 1024) {
                    socket->disconnectFromServer();
                    return;
                }
                int newline;
                while ((newline = buffer.indexOf('\n')) >= 0) {
                    const auto line = buffer.left(newline);
                    buffer.remove(0, newline + 1);
                    QJsonParseError error;
                    auto doc = QJsonDocument::fromJson(line, &error);
                    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                        socket->write("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
                                      "\"message\":\"Parse error\"}}\n");
                        continue;
                    }
                    dispatch(socket, doc.object());
                }
                socket->setProperty("buffer", buffer);
            });
        }
    });
}
bool McpServer::start(QString *error) {
    if (server_.isListening())
        return true;
    if (server_.listen(endpoint_))
        return true;
    QLocalSocket probe;
    probe.connectToServer(endpoint_);
    if (probe.waitForConnected(100)) {
        endpoint_ += "-" + QString::number(QCoreApplication::applicationPid());
    } else
        QLocalServer::removeServer(endpoint_);
    if (server_.listen(endpoint_))
        return true;
    if (error)
        *error = tr("MCP 启动失败：%1").arg(server_.errorString());
    return false;
}
void McpServer::stop() {
    server_.close();
    for (auto socket : server_.findChildren<QLocalSocket *>())
        socket->disconnectFromServer();
}
QJsonObject McpServer::clientConfig() const {
    return {{"mcpServers",
             QJsonObject{
                 {"garlic", QJsonObject{{"command", QCoreApplication::applicationFilePath()},
                                        {"args", QJsonArray{"--mcp", "--socket", endpoint_}}}}}}};
}
void McpServer::dispatch(QLocalSocket *socket, const QJsonObject &request) {
    if (!request.contains("id"))
        return;
    QPointer<QLocalSocket> client(socket);
    const auto id = request.value("id");
    auto respond = [client, id](const QJsonValue &value, bool tool = false, bool error = false) {
        if (!client)
            return;
        QJsonObject result;
        if (tool)
            result = {
                {"content",
                 QJsonArray{QJsonObject{
                     {"type", "text"},
                     {"text", QString::fromUtf8(QJsonDocument(value.isObject()
                                                                  ? value.toObject()
                                                                  : QJsonObject{{"result", value}})
                                                    .toJson(QJsonDocument::Compact))}}}},
                {"isError", error}};
        else
            result = value.toObject();
        client->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}})
                          .toJson(QJsonDocument::Compact) +
                      "\n");
    };
    auto fail = [respond](const QString &message) {
        respond(QJsonObject{{"error", message}}, true, true);
    };
    const auto method = request.value("method").toString();
    if (method == "initialize") {
        const auto version = request.value("params").toObject().value("protocolVersion").toString();
        respond(
            QJsonObject{{"protocolVersion",
                         QStringList{"2024-11-05", "2025-03-26", "2025-06-18"}.contains(version)
                             ? version
                             : "2024-11-05"},
                        {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                        {"serverInfo", QJsonObject{{"name", "garlic-gui"}, {"version", "0.2.0"}}}});
        return;
    }
    if (method == "ping") {
        respond(QJsonObject{});
        return;
    }
    if (method == "tools/list") {
        respond(QJsonObject{{"tools", toolsList()}});
        return;
    }
    if (method != "tools/call") {
        client->write(
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                      {"id", id},
                                      {"error", QJsonObject{{"code", -32601},
                                                            {"message", "Method not found"}}}})
                .toJson(QJsonDocument::Compact) +
            "\n");
        return;
    }
    auto params = request.value("params").toObject();
    const auto name = params.value("name").toString();
    const auto args = params.value("arguments").toObject();
    auto backend = window_->backend();
    auto project = backend->project();
    QJsonObject toolSchema;
    for (const auto &v : toolsList()) {
        auto t = v.toObject();
        if (t.value("name").toString() == name)
            toolSchema = t.value("inputSchema").toObject();
    }
    if (toolSchema.isEmpty()) {
        fail("Unknown tool");
        return;
    }
    for (const auto &v : toolSchema.value("required").toArray())
        if (!args.contains(v.toString())) {
            fail("Missing argument: " + v.toString());
            return;
        }
    const auto properties = toolSchema.value("properties").toObject();
    for (auto it = args.begin(); it != args.end(); ++it) {
        const auto type = properties.value(it.key()).toObject().value("type").toString();
        const auto value = it.value();
        const bool valid = (type == "string" && value.isString()) ||
                           (type == "boolean" && value.isBool()) ||
                           (type == "object" && value.isObject()) ||
                           (type == "integer" && value.isDouble() &&
                            value.toDouble() == double(value.toInteger()));
        if (!valid) {
            fail("Invalid argument: " + it.key());
            return;
        }
    }
    if (name == "cancel_task") {
        backend->cancel();
        respond(QJsonObject{{"canceled", true}}, true);
        return;
    }
    if (name == "get_settings") {
        respond(backend->settings().toJson(), true);
        return;
    }
    if (name == "set_settings") {
        if (backend->busy() || backend->preparing()) {
            fail("Engine busy; retry after completion");
            return;
        }
        auto j = backend->settings().toJson();
        const auto changes = args.value("settings").toObject();
        for (auto i = changes.begin(); i != changes.end(); ++i) {
            if (!j.contains(i.key())) {
                fail("Unsupported setting: " + i.key());
                return;
            }
            if (j.value(i.key()).type() != i.value().type()) {
                fail("Invalid setting type: " + i.key());
                return;
            }
            j[i.key()] = i.value();
        }
        auto settings = AppSettings::fromJson(j);
        settings.save();
        respond(settings.toJson(), true);
        QTimer::singleShot(0, window_, [this, settings] { window_->applySettings(settings); });
        return;
    }
    if (name == "get_cache_stats") {
        respond(backend->cacheStats(), true);
        return;
    }
    if (name == "clear_cache") {
        if (backend->busy() || backend->preparing()) {
            fail("Engine busy");
            return;
        }
        backend->clearCache();
        respond(backend->cacheStats(), true);
        return;
    }
    if (name == "fetch_current_class") {
        respond(
            QJsonObject{{"class_name", window_->selectedClass()},
                        {"code", window_->editor() ? window_->editor()->toPlainText() : QString()}},
            true);
        return;
    }
    if (name == "get_selected_text") {
        respond(
            QJsonObject{{"text", window_->editor() ? window_->editor()->textCursor().selectedText()
                                                   : QString()}},
            true);
        return;
    }
    if (project->classes().isEmpty()) {
        fail("Open an input file in Garlic GUI first");
        return;
    }
    if (name == "get_all_classes") {
        QJsonArray values;
        for (const auto &n : project->classes())
            values.append(QJsonObject{{"name", n},
                                      {"display_name", project->displayName(n)},
                                      {"kind", project->info(n).value("kind")}});
        respond(QJsonObject{{"classes", paginate(values, args)}, {"total", values.size()}}, true);
        return;
    }
    if (name == "get_package_tree") {
        QJsonObject packages;
        for (const auto &n : project->classes()) {
            auto p = n.section('/', 0, -2);
            packages[p] = packages.value(p).toInt() + 1;
        }
        respond(QJsonObject{{"packages", packages}}, true);
        return;
    }
    if (name == "get_methods_of_class" || name == "get_fields_of_class") {
        respond(QJsonObject{{"members", project->members(args.value("class_name").toString(),
                                                         name == "get_methods_of_class")}},
                true);
        return;
    }
    if (name == "search_method_by_name") {
        QJsonArray found;
        for (const auto &v : project->symbols(args.value("method_name").toString()))
            if (v.toObject().value("kind") == "method")
                found.append(v);
        respond(QJsonObject{{"methods", found}}, true);
        return;
    }
    if (name == "undo_rename") {
        project->undoRename();
        respond(QJsonObject{{"aliases", project->aliases()}}, true);
        return;
    }
    if (name == "save_project") {
        QString error;
        if (!project->save(args.value("path").toString(), &error)) {
            fail(error);
            return;
        }
        respond(QJsonObject{{"saved", true}}, true);
        return;
    }
    if (name.startsWith("rename_") || name.startsWith("get_xrefs_to_")) {
        const auto kind = name.section('_', -1);
        QString error;
        const auto symbol = memberId(project, kind, args, &error);
        if (symbol.isEmpty()) {
            fail(error);
            return;
        }
        if (name.startsWith("rename_")) {
            error = project->rename(symbol, args.value("new_name").toString());
            if (!error.isEmpty()) {
                fail(error);
                return;
            }
            respond(QJsonObject{{"id", symbol}, {"alias", project->symbolName(symbol)}}, true);
        } else {
            auto snapshot = project->snapshot();
            auto watcher = new QFutureWatcher<QJsonArray>(socket);
            connect(watcher, &QFutureWatcher<QJsonArray>::finished, socket,
                    [watcher, respond, args] {
                        auto refs = watcher->result();
                        watcher->deleteLater();
                        respond(QJsonObject{{"references", paginate(refs, args)},
                                            {"total", refs.size()}},
                                true);
                    });
            watcher->setFuture(
                QtConcurrent::run([snapshot, symbol] { return snapshot->xrefs(symbol); }));
        }
        return;
    }
    // Asynchronous replies are tied to a context, disconnected on completion or client exit.
    auto pending = new QObject(this);
    QPointer<QObject> guard(pending);
    auto done = [respond, guard](QJsonObject result, bool error = false) {
        if (!guard || guard->property("done").toBool())
            return;
        guard->setProperty("done", true);
        respond(result, true, error);
        guard->deleteLater();
    };
    connect(socket, &QLocalSocket::disconnected, pending, &QObject::deleteLater);
    QTimer::singleShot(300000, pending, [done] {
        done({{"error", "Operation timed out; cancel_task can stop the engine"}}, true);
    });
    connect(backend, &Backend::failed, pending,
            [done](const QString &error) { done({{"error", error}}, true); });
    if (name == "search_classes_by_keyword") {
        if (args.value("search_in").toString("code") != "code") {
            const auto values = project->symbols(args.value("search_term").toString());
            done({{"results", paginate(values, args)}, {"total", values.size()}});
            return;
        }
        SearchOptions options;
        options.query = args.value("search_term").toString();
        options.regex = args.value("regex").toBool();
        options.caseSensitive = args.value("case_sensitive").toBool();
        const int request = backend->search(options);
        connect(backend, &Backend::searchCompleted, pending,
                [done, args, request](int id, const SearchResult &result) {
                    if (id != request)
                        return;
                    if (result.canceled) {
                        done({{"error", "Search canceled or replaced by a newer query"}}, true);
                        return;
                    }
                    if (!result.error.isEmpty()) {
                        done({{"error", result.error}}, true);
                        return;
                    }
                    done({{"results", paginate(result.hits, args)},
                          {"total", result.hits.size()},
                          {"truncated", result.truncated},
                          {"missing_files", result.missing},
                          {"skipped_files", result.skipped}});
                });
        return;
    }
    const auto clazz = Project::normalize(args.value("class_name").toString());
    if (project->info(clazz).isEmpty()) {
        done({{"error", "Class not found"}}, true);
        return;
    }
    bool smali = name == "get_smali_of_class";
    QString methodId;
    if (name == "get_method_by_name") {
        QString error;
        methodId = memberId(project, "method", args, &error);
        if (methodId.isEmpty()) {
            done({{"error", error}}, true);
            return;
        }
    }
    const auto deliver = [this, done, project, clazz, smali, methodId](const QString &path) {
        if (QFileInfo(path).size() > 8 * 1024 * 1024) {
            done({{"error", "Source exceeds MCP 8 MiB response limit"}}, true);
            return;
        }
        auto snapshot = project->snapshot();
        auto watcher = new QFutureWatcher<QJsonObject>(this);
        connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [watcher, done] {
            auto result = watcher->result();
            watcher->deleteLater();
            done(result);
        });
        watcher->setFuture(QtConcurrent::run([snapshot, clazz, smali, methodId, path] {
            const auto document = snapshot->document(clazz, smali, path);
            QJsonObject result{{"class_name", clazz}, {"code", document.text}};
            if (!methodId.isEmpty()) {
                int position = -1;
                for (const auto &span : document.spans)
                    if (span.id == methodId && span.declaration) {
                        position = span.start;
                        break;
                    }
                result["method_id"] = methodId;
                result["declaration_offset"] = position;
                result["code"] = snapshot->methodSource(document, methodId);
            }
            return result;
        }));
    };
    const auto cached = backend->cachedPath(clazz, smali);
    if (!cached.isEmpty()) {
        deliver(cached);
        return;
    }
    if (backend->busy()) {
        done({{"error", "Engine busy; retry after the current request"}}, true);
        return;
    }
    connect(backend, &Backend::sourceReady, pending,
            [deliver, clazz, smali](const QString &name, bool mode, const QString &path) {
                if (name == clazz && mode == smali)
                    deliver(path);
            });
    backend->request(clazz, smali);
}
int runMcpBridge(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    int index = args.indexOf("--socket");
    if (index < 0 || index + 1 >= args.size()) {
        std::cerr << "Use --mcp --socket <endpoint from GUI settings>\n";
        return 2;
    }
    QLocalSocket socket;
    socket.connectToServer(args[index + 1]);
    if (!socket.waitForConnected(3000)) {
        std::cerr << "Garlic GUI MCP is unavailable. Open the GUI and enable MCP in settings.\n";
        return 1;
    }
    std::string line;
    QByteArray buffer;
    while (std::getline(std::cin, line)) {
        if (line.size() > 4 * 1024 * 1024) {
            std::cerr << "MCP request too large\n";
            return 2;
        }
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        socket.write(QByteArray::fromStdString(line) + "\n");
        socket.waitForBytesWritten(3000);
        if (!doc.object().contains("id"))
            continue;
        int elapsed = 0;
        while (!buffer.contains('\n')) {
            if (!socket.waitForReadyRead(1000)) {
                if (socket.state() != QLocalSocket::ConnectedState || ++elapsed > 310) {
                    std::cerr << "GUI MCP disconnected or timed out\n";
                    return 1;
                }
            }
            buffer += socket.readAll();
        }
        int newline = buffer.indexOf('\n');
        std::cout << buffer.left(newline).constData() << std::endl;
        buffer.remove(0, newline + 1);
    }
    return 0;
}
