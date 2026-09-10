#include "mainwindow.h"
#include "mcpserver.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>
#include <QTimer>
#include <cstring>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--mcp") == 0)
            return runMcpBridge(argc, argv);
    QApplication app(argc, argv);
    app.setApplicationName("Garlic GUI");
    app.setOrganizationName("Garlic");
    app.setApplicationVersion("0.3.0");
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
    app.setStyleSheet(R"(
        QMainWindow { background: #17212d; }
        QToolBar { border: 0; background: #1b2937; spacing: 12px; padding: 12px 10px; }
        QToolButton { padding: 6px 10px; border-radius: 5px; }
        QToolButton:hover { background: #304354; }
        QLabel#brand { color: #a4e4bd; font-size: 19px; font-weight: 700; letter-spacing: 3px; }
        QLabel#muted { color: #8c9dad; line-height: 1.5; }
        QLabel#sectionTitle { color: #b2c3d3; font-weight: 600; padding-bottom: 8px; }
        QLabel#welcomeTitle { font-size: 25px; font-weight: 600; color: #e0ebf3; }
        QWidget#fileBar { background: #14202b; border-bottom: 1px solid #2a3949; }
        QLineEdit { padding: 8px 10px; border: 1px solid #354454; border-radius: 5px; background: #111b26; }
        QLineEdit:focus { border-color: #69b9a2; }
        QPushButton { padding: 8px 12px; border: 1px solid #354454; border-radius: 5px; }
        QPushButton:hover { background: #304354; }
        QPushButton#primary { background: #97d6b1; color: #132d23; font-weight: 600; border: 0; padding: 12px; }
        QComboBox { padding: 7px; border: 1px solid #354454; border-radius: 5px; }
        QTreeView { border: 0; background: transparent; padding-top: 6px; }
        QTreeView::item { padding: 5px 2px; }
        QTreeView::item:selected { background: #28534d; border-radius: 4px; }
        QPlainTextEdit { border: 0; padding: 4px; selection-background-color: #38635b; }
        QTabWidget::pane { border: 1px solid #2a3949; }
        QTabBar::tab { padding: 10px 14px; background: #1b2937; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { background: #111b26; border-bottom: 2px solid #97d6b1; }
        QStatusBar { color: #95a8b9; background: #14202b; padding: 3px; }
        QSplitter::handle { background: #2a3949; width: 1px; }
        QProgressBar { border: 0; background: #243446; border-radius: 4px; }
        QProgressBar::chunk { background: #97d6b1; }
    )");
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
                           [&] { window.openPath(parser.positionalArguments().first()); });
    return app.exec();
}
