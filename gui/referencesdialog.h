#pragma once
#include <QDialog>
class MainWindow;
class ReferencesDialog : public QDialog {
    Q_OBJECT
  public:
    ReferencesDialog(MainWindow *window, const QString &id);
};
