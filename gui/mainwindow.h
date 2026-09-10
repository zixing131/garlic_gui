#pragma once
#include <QMainWindow>
#include "backend.h"
class QTreeView;
class QStandardItemModel;
class QSortFilterProxyModel;
class QTabWidget;
class QLineEdit;
class QLabel;
class QProgressBar;
class QComboBox;
class QPlainTextEdit;
class QStackedWidget;
class CodeEditor;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &engine = {}, QWidget *parent = nullptr);
    void openPath(const QString &path);
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    void chooseFile();
    void populate(const QStringList &classes);
    void openSelected();
    void showSource(const QString &name, bool smali, const QString &path);
    void find(bool backwards);
    void updateBusy(bool busy);
    void exportAll();
    CodeEditor *editor() const;
    Backend backend_;
    QTreeView *tree_;
    QStandardItemModel *model_;
    QSortFilterProxyModel *proxy_;
    QTabWidget *tabs_;
    QLineEdit *filter_, *find_;
    QLabel *fileLabel_, *countLabel_, *status_;
    QProgressBar *progress_;
    QComboBox *language_;
    QPlainTextEdit *logs_;
    QStackedWidget *pages_;
    QAction *openAction_, *exportAction_, *stopAction_, *engineAction_;
    QString selectedName_;
};
