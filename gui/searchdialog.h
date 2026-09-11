#pragma once
#include "search.h"
#include <QDialog>
class MainWindow;
class QLineEdit;
class QCheckBox;
class QTableView;
class QLabel;
class QTimer;
class QBoxLayout;
class QAbstractTableModel;
class SearchDialog : public QDialog {
    Q_OBJECT
  public:
    explicit SearchDialog(MainWindow *window);
    void startSearch(int limit = 50);
    void setPackage(const QString &name);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

  private:
    void navigate();
    MainWindow *window_;
    QBoxLayout *filters_ = nullptr;
    QLineEdit *query_, *package_;
    QCheckBox *resources_, *classes_, *methods_, *fields_, *code_, *comments_, *regex_, *sensitive_, *automatic_,
        *keep_;
    QTableView *table_;
    QLabel *status_;
    QTimer *debounce_;
    QAbstractTableModel *model_;
    int request_ = -1, limit_ = 50;
};
