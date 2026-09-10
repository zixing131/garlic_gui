#pragma once
#include "codeeditor.h"
#include "project.h"
#include "settings.h"
#include <QWidget>
class QTabWidget;
class ClassView : public QWidget {
    Q_OBJECT
  public:
    ClassView(const QString &name, bool hasSmali, const AppSettings &settings,
              QWidget *parent = nullptr);
    QString name() const { return name_; }
    bool smali() const;
    CodeEditor *editor() const;
    CodeEditor *editor(bool smali) const;
    bool loaded(bool smali) const { return loaded_[smali ? 1 : 0]; }
    SourceDocument rawDocument(bool smali) const { return raw_[smali ? 1 : 0]; }
    void setRawDocument(bool smali, const SourceDocument &document) {
        raw_[smali ? 1 : 0] = document;
    }
    void setSource(bool smali, const SourceDocument &document);
    void applySettings(const AppSettings &settings);
    void selectMode(bool smali);
    void invalidate();
  signals:
    void modeChanged();

  private:
    SourceDocument raw_[2];
    QString name_;
    QTabWidget *modes_;
    bool loaded_[2] = {false, false};
};
