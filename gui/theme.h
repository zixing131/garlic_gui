#pragma once
#include <QString>
inline QString garlicStyleSheet() { return R"(
        QMainWindow { background: #17212d; }
        QToolBar { border: 0; background: #1b2937; spacing: 3px; padding: 3px 6px; }
        QToolButton { padding: 4px 5px; border-radius: 5px; }
        QToolButton:hover { background: #304354; }
        QToolButton:checked { background: #28534d; color: #b8f0d0; }
        QToolButton:focus, QPushButton:focus, QTabBar::tab:focus, QTreeView::item:focus { outline: 0; }
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
        QTreeView::branch:focus { outline: 0; }
        QPlainTextEdit { border: 0; padding: 4px; selection-background-color: #38635b; }
        QTabWidget::pane { border: 1px solid #2a3949; }
        QTabBar::tab { padding: 6px 10px; background: #1b2937; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { background: #111b26; border-bottom: 2px solid #97d6b1; }
        QStatusBar { color: #95a8b9; background: #14202b; padding: 3px; }
        QSplitter::handle { background: #2a3949; width: 1px; }
        QProgressBar { border: 0; background: #243446; border-radius: 4px; }
        QProgressBar::chunk { background: #97d6b1; }
    )"; }
