#include "mainwindow.h"
#include "theme.h"
#include "mcpserver.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>
#include <QTimer>
#include <cstring>
#include <cstdio>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mcp") == 0)
            return runMcpBridge(argc, argv);
        if (std::strcmp(argv[i], "--version") == 0) {
            // QCommandLineParser shows a blocking message box for Windows GUI applications.
            std::puts("Garlic GUI 0.5.3");
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
    app.setApplicationVersion("0.5.3");
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
    parser.addOption({"engine", "Path to the garlic engine executable", "path"});
    parser.addPositionalArgument("file", "APK, DEX, JAR, WAR or CLASS file", "[file]");
    parser.process(app);
    MainWindow window(parser.value("engine"));
    window.show();
    if (!parser.positionalArguments().isEmpty())
        QTimer::singleShot(0, &window,
                           [&] { window.openPaths(parser.positionalArguments()); });
    return app.exec();
}
