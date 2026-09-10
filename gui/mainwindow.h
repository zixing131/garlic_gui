#pragma once
#include "backend.h"
#include "classview.h"
#include <QMainWindow>
class QTreeView;
class QStandardItemModel;
class QSortFilterProxyModel;
class QTabWidget;
class QLineEdit;
class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QStackedWidget;
class QTableWidget;
class QDockWidget;
class McpServer;
class SearchDialog;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(const QString &engine = {}, QWidget *parent = nullptr);
    void openPath(const QString &path);
    Backend *backend() { return &backend_; }
    QString selectedClass() const;
    CodeEditor *editor() const;
    void openClass(const QString &name, bool smali = false);
    void navigateTo(const QString &id, int line = 0);
    void showReferences(const QString &id);
    void applySettings(const AppSettings &settings);
    QString mcpEndpoint() const;

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

  private:
    ClassView *view() const;
    void chooseFile();
    void populate(const QStringList &classes);
    void populateMembers(const QModelIndex &index);
    int treeGeneration_ = 0;
    void showSource(const QString &name, bool smali, const QString &path);
    void loadCurrent();
    void find(bool backwards);
    void updateBusy();
    void exportAll();
    void settingsDialog();
    void searchDialog();
    void renameSymbol(const QString &id);
    void refreshAliases();
    SourceDocument present(const SourceDocument &raw, bool smali) const;
    void saveProject();
    void openProject();
    void recordHistory();
    QIcon classIcon(const QString &kind) const;
    Backend backend_;
    McpServer *mcp_;
    SearchDialog *searchDialog_ = nullptr;
    QTreeView *tree_;
    QStandardItemModel *model_;
    QSortFilterProxyModel *proxy_;
    QTabWidget *tabs_;
    QLineEdit *filter_, *find_;
    QLabel *fileLabel_, *countLabel_, *status_;
    QProgressBar *progress_;
    QPlainTextEdit *logs_;
    QStackedWidget *pages_;
    QTableWidget *results_;
    QDockWidget *resultDock_, *logDock_;
    QAction *openAction_, *exportAction_, *stopAction_, *engineAction_, *settingsAction_;
    QString pendingId_, pendingProject_;
    int pendingLine_ = 0;
    QVector<QPair<QString, int>> history_;
    int historyIndex_ = -1;
    bool restoringHistory_ = false;
};
