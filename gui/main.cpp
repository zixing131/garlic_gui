#include "mainwindow.h"
#include "theme.h"
#include "mcpserver.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>
#include <QTimer>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <QDir>
#include <QFileInfo>
#include <QThread>

// Only engine-owned temporary roots may be removed. This helper outlives the UI
// so Windows file handles and large cache trees never hold up window shutdown.
static int cleanupWorkspaces(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto root = QFileInfo(QDir::tempPath()).canonicalFilePath();
    const auto paths = app.arguments().mid(2);
    for (const auto &path : paths) {
        QFileInfo info(path);
        if (info.isSymLink() || !info.fileName().startsWith("garlic-gui-") ||
            QFileInfo(info.absolutePath()).canonicalFilePath() != root) return 2;
    }
    for (int retry = 0; retry < 50; ++retry) {
        bool remaining = false;
        for (const auto &path : paths) {
            QFileInfo info(path);
            if (info.isSymLink()) continue;
            if (info.exists() && !QDir(path).removeRecursively()) remaining = true;
        }
        if (!remaining) return 0;
        QThread::msleep(200);
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--cleanup-workspaces") == 0) return cleanupWorkspaces(argc, argv);
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--headless") == 0)
            return runHeadless(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mcp") == 0)
            return runMcpBridge(argc, argv);
        if (std::strcmp(argv[i], "--version") == 0) {
            // QCommandLineParser shows a blocking message box for Windows GUI applications.
            std::puts("Garlic GUI " GARLIC_GUI_VERSION);
            return 0;
        }
    }
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/app/garlic.png"));
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--smoke-test") == 0) {
            QWidget probe;
            probe.show();
            app.processEvents();
            return 0;
        }
    app.setApplicationName("Garlic GUI");
    app.setOrganizationName("Garlic");
    app.setApplicationVersion(GARLIC_GUI_VERSION);
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#17212d"));
    palette.setColor(QPalette::WindowText, QColor("#dce5ee"));
    palette.setColor(QPalette::Base, QColor("#111b26"));
    palette.setColor(QPalette::AlternateBase, QColor("#1c2937"));
    palette.setColor(QPalette::Text, QColor("#dce5ee"));
    palette.setColor(QPalette::PlaceholderText, QColor("#8195a7"));
    palette.setColor(QPalette::Button, QColor("#243446"));
    palette.setColor(QPalette::ButtonText, QColor("#dce5ee"));
    palette.setColor(QPalette::Highlight, QColor("#285f59"));
    palette.setColor(QPalette::HighlightedText, QColor("#effff9"));
    palette.setColor(QPalette::ToolTipBase, QColor("#243446"));
    palette.setColor(QPalette::ToolTipText, QColor("#dce5ee"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#637386"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#637386"));
    app.setPalette(palette);
    app.setStyleSheet(garlicStyleSheet());
    QCommandLineParser parser;
    parser.setApplicationDescription("Garlic native bytecode browser");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption shutdownProbe("shutdown-probe", "Internal lifecycle regression probe", "milliseconds");
    shutdownProbe.setFlags(QCommandLineOption::HiddenFromHelp); parser.addOption(shutdownProbe);
    parser.addOption({"engine", "Path to the garlic engine executable", "path"});
    parser.addPositionalArgument("file", "APK, DEX, JAR, WAR, ZIP or CLASS file", "[file]");
    parser.process(app);
    if (parser.isSet(shutdownProbe)) { app.setOrganizationName("GarlicTests"); app.setApplicationName("ShutdownProbe"); }
    MainWindow window(parser.value("engine"));
    window.show();
    if (!parser.positionalArguments().isEmpty())
        QTimer::singleShot(0, &window,
                           [&] { window.openPaths(parser.positionalArguments()); });
    if (parser.isSet(shutdownProbe)) {
        const auto close = [&window] { std::fprintf(stderr, "SHUTDOWN_BEGIN %s\n", qPrintable(window.backend()->workspacePath())); std::fflush(stderr); window.close(); };
        const auto delay = parser.value(shutdownProbe).toInt();
        if (delay > 0) QTimer::singleShot(delay, &window, close);
        else {
            QObject::connect(window.backend(), &Backend::metadataCompleted, &window, close);
            QObject::connect(window.backend(), &Backend::indexed, &window, [&window, close] {
                if (window.backend()->metadataReady()) QTimer::singleShot(0, &window, close);
            });
        }
    }
    const int result = app.exec();
    QSettings().sync();
    if (window.backend()->prepareExit(QCoreApplication::applicationFilePath())) {
        // All producers are stopped; the helper owns disk cleanup. Let the OS
        // reclaim the multi-gigabyte immutable index instead of synchronously
        // destroying millions of Qt nodes or waiting on global pool destructors.
        std::fflush(nullptr);
        std::_Exit(result);
    }
    return result;
}
