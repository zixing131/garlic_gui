#include "backend.h"
#include "mcpserver.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <cstdio>

int runHeadless(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("Garlic Headless");
    app.setOrganizationName("Garlic");
    app.setApplicationVersion(GARLIC_GUI_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription("Garlic headless APK analysis and MCP server (no display required)");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions({{"headless", "Run without GUI"},
                       {"mcp", "Serve MCP over stdio (default)"},
                       {"apk", "Input APK/APKS/DEX/JAR file", "path"},
                       {"engine", "Garlic engine executable", "path"},
                       {"threads", "Engine threads (1-64)", "count", "4"},
                       {"deobfuscate", "Enable deobfuscation"},
                       {"simplify-control-flow", "Simplify control flow"},
                       {"unflatten", "Recover flattened control flow"},
                       {"background", "Generate all sources in the background"},
                       {"http-port", "Serve HTTP MCP on loopback instead of stdio (0 selects a free port)", "port"},
                       {"print-mcp-config", "Print stdio MCP client JSON and exit"}});
    parser.addPositionalArgument("file", "Input file (alternative to --apk)", "[file]");
    parser.process(app);
    auto input = parser.value("apk");
    const auto positional = parser.positionalArguments();
    auto error = [](const QString &message) {
        fprintf(stderr, "%s\n", qPrintable(message));
        return 2;
    };
    if (positional.size() > 1 || (!input.isEmpty() && !positional.isEmpty()))
        return error("Specify exactly one input using --apk or a positional path");
    if (input.isEmpty() && !positional.isEmpty()) input = positional.first();
    if (input.isEmpty() || !QFileInfo(input).isFile())
        return error("Input file does not exist; pass --apk /absolute/path/to/app.apk");
    input = QFileInfo(input).absoluteFilePath();
    bool valid;
    AppSettings settings;
    settings.threads = parser.value("threads").toInt(&valid);
    if (!valid || settings.threads < 1 || settings.threads > 64)
        return error("--threads must be between 1 and 64");
    settings.deobfuscate = parser.isSet("deobfuscate");
    settings.simplifyControlFlow = parser.isSet("simplify-control-flow");
    settings.unflatten = parser.isSet("unflatten");
    settings.background = parser.isSet("background");
    if (parser.isSet("http-port")) {
        auto port = parser.value("http-port").toInt(&valid);
        if (!valid || port < 0 || port > 65535) return error("Invalid HTTP port");
        settings.mcpTransport = "http";
        settings.mcpPort = port;
    }
    if (parser.isSet("print-mcp-config")) {
        if (parser.isSet("http-port")) return error("--print-mcp-config generates stdio configuration; omit --http-port");
        QJsonArray args{"--headless", "--apk", input, "--threads", QString::number(settings.threads)};
        for (const auto &flag : {"deobfuscate", "simplify-control-flow", "unflatten", "background"})
            if (parser.isSet(flag)) args.append(QString("--") + flag);
        if (parser.isSet("engine")) {
            args.append("--engine"); args.append(QFileInfo(parser.value("engine")).absoluteFilePath());
        }
        auto config = QJsonObject{{"mcpServers", QJsonObject{{"garlic", QJsonObject{
            {"command", QCoreApplication::applicationFilePath()}, {"args", args}}}}}};
        auto json = QJsonDocument(config).toJson();
        fwrite(json.constData(), 1, size_t(json.size()), stdout);
        return 0;
    }
    Backend backend;
    backend.configure(settings);
    if (parser.isSet("engine")) backend.setEngine(QFileInfo(parser.value("engine")).absoluteFilePath());
    McpServer server(&backend);
    QString message;
    if (!server.start(&message)) return error(message);
    QObject::connect(&backend, &Backend::log, &app, [](const QString &s) { fprintf(stderr, "%s\n", qPrintable(s)); });
    QObject::connect(&backend, &Backend::failed, &app, [&app, &backend](const QString &s) {
        fprintf(stderr, "Analysis failed: %s\n", qPrintable(s));
        if (backend.project()->classCount() == 0) app.exit(1);
    });
    QObject::connect(&backend, &Backend::indexed, &app, [&backend, settings](const QStringList &classes) {
        fprintf(stderr, "Indexed %lld classes\n", static_cast<long long>(classes.size()));
        if (settings.background) backend.prepareSources();
    });
    // Reuse the portable synchronous stdio bridge in a child. The parent owns
    // the event loop/backend; EOF terminates the bridge and this session together.
    QProcess bridge;
    if (settings.mcpTransport == "stdio") {
        bridge.setInputChannelMode(QProcess::ForwardedInputChannel);
        bridge.setProcessChannelMode(QProcess::ForwardedChannels);
        QObject::connect(&bridge, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         &app, [&app](int code, QProcess::ExitStatus status) {
                             app.exit(status == QProcess::NormalExit ? code : 1);
                         });
        bridge.start(QCoreApplication::applicationFilePath(), {"--mcp", "--socket", server.endpoint()});
        if (!bridge.waitForStarted(5000)) return error(bridge.errorString());
    } else {
        fprintf(stderr, "MCP URL: %s\n", qPrintable(server.httpUrl()));
    }
    QTimer::singleShot(0, &backend, [&backend, input] { backend.open(input); });
    const int result = app.exec();
    bridge.kill();
    if (bridge.state() != QProcess::NotRunning) bridge.waitForFinished(3000);
    backend.cancel();
    return result;
}
