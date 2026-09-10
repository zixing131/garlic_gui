#pragma once

#include "project.h"
#include <QDialog>

class QComboBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QSpinBox;

class CallGraphDialog : public QDialog {
    Q_OBJECT
  public:
    CallGraphDialog(const std::shared_ptr<Project> &project, const QString &method,
                    QWidget *parent = nullptr);

  signals:
    void navigationRequested(const QString &id);

  private:
    void rebuild();
    void render(const QJsonObject &graph);
    std::shared_ptr<Project> project_;
    QString method_;
    QSpinBox *depth_ = nullptr;
    QComboBox *direction_ = nullptr;
    QLabel *status_ = nullptr;
    QGraphicsView *view_ = nullptr;
    QGraphicsScene *scene_ = nullptr;
    int generation_ = 0;
};
