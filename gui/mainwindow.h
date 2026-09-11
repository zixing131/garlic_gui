#pragma once
#include "backend.h"
#include "classview.h"
#include <QMainWindow>
#include <QSet>
#include <QPersistentModelIndex>
#include <functional>
class QMenu;
class QStandardItem;
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
class QToolButton;
class QWidget;
class McpServer;
class SearchDialog;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(const QString &engine = {}, QWidget *parent = nullptr);
    void openPath(const QString &path);
    void openPaths(const QStringList &paths);
    Backend *backend() { return &backend_; }
    QString selectedClass() const;
    CodeEditor *editor() const;
    void openClass(const QString &name, bool smali = false);
    void navigateTo(const QString &id, int line = 0, const QString &target = {}, const QJsonObject &hit = {});
    void showReferences(const QString &id);
    void openResource(const QString &path, const QString &entry, const QString &generated = {}, int line = 0, const QJsonObject &hit = {});
    void showCallGraph(const QString &id);
    void applySettings(const AppSettings &settings, bool preserveAnalysis = false);
    QString mcpEndpoint() const;

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

  private:
    bool waitForMetadata(std::function<void()> action);
    QSet<QString> pendingMemberClasses_;
    ClassView *view() const;
    void chooseFile();
    void filterTree(const QString &text);
    QSet<QPersistentModelIndex> expandedNodes_;
    bool filtering_ = false;
    void projectNodes();
    void showNativeAnalysis(const QString &path, const QString &entry = {});
    void expandResourceTable(const QModelIndex &index);
    QHash<QString, QMap<QString, QString>> decodedResources_;
    void showOverview(const QString &path, bool signature = false);
    void goApplication();
    void goMainActivity();
    void syncEditor();
    bool flatPackages_ = true;
    QString pendingSyncResource_;
    void goManifest();
    void refreshRecent();
    void closeTabs(int index, const QString &mode);
    QMenu *recentMenu_;
    QStandardItem *sourceRoot_ = nullptr, *resourceRoot_ = nullptr;
    QHash<QString, QJsonObject> resourceInfo_;
    void populate(const QStringList &classes);
    void populateMembers(const QModelIndex &index);
    int treeGeneration_ = 0;
    QHash<QString, QStandardItem *> classItems_;
    void showSource(const QString &name, bool smali, const QString &path);
    void loadCurrent();
    void find(bool backwards);
    void showFindBar();
    void hideFindBar();
    void refreshFindHighlights();
    void updateBusy();
    void exportAll();
    void settingsDialog();
    void searchDialog();
    void renameSymbol(const QString &id);
    void refreshAliases();
    void applyAliasChanges(const QHash<QString, QString> &aliases, const QSet<QString> &changed, QSet<QString> owners, QSet<QString> renamedClasses);
    int aliasPlanGeneration_ = 0;
    QHash<QString, QString> displayedAliases_;
    int aliasRefreshGeneration_ = 0;
    QSet<QString> pendingAliasOwners_;
    SourceDocument present(const SourceDocument &raw, bool smali) const;
    void saveProject();
    void openProject();
    void recordHistory();
    void updateHistoryPosition();
    void moveHistory(int direction);
    void updateHistoryActions();
    Backend backend_;
    McpServer *mcp_;
    SearchDialog *searchDialog_ = nullptr;
    QTreeView *tree_;
    QStandardItemModel *model_;
    QSortFilterProxyModel *proxy_;
    QTabWidget *tabs_;
    QLineEdit *filter_, *find_;
    QWidget *findBar_ = nullptr;
    QLabel *findCount_ = nullptr;
    QToolButton *findCase_ = nullptr, *findWord_ = nullptr, *findRegex_ = nullptr;
    QLabel *memoryLabel_;
    quint64 peakMemory_ = 0;
    QLabel *countLabel_, *status_;
    QProgressBar *progress_;
    QPlainTextEdit *logs_;
    QStackedWidget *pages_;
    QDockWidget *logDock_;
    QAction *openAction_, *exportAction_, *stopAction_, *settingsAction_;
    QString pendingId_, pendingProject_;
    int pendingLine_ = 0;
    QString pendingClass_;
    QJsonObject pendingHit_;
    struct HistoryPosition {
        QString name;
        bool smali = false;
        int position = 0, anchor = 0, vertical = 0, horizontal = 0;
    };
    HistoryPosition currentPosition() const;
    QVector<HistoryPosition> history_;
    bool navigating_ = false, pendingSmali_ = false, openingClass_ = false;
    int historyIndex_ = -1;
    bool restoringHistory_ = false;
};
