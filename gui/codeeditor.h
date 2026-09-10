#pragma once
#include "project.h"
#include <QPlainTextEdit>

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
  public:
    explicit CodeEditor(bool smali, QWidget *parent = nullptr);
    void setTheme(bool light);
    void setSource(const SourceDocument &document);
    QString symbolAtCursor() const;
    QVector<SourceSpan> spans() const { return spans_; }
    bool goToSymbol(const QString &id);
    void goToLine(int line);
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
  signals:
    void navigateRequested();
    void referencesRequested();
    void renameRequested();

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    void highlightCurrentLine();
    bool light_ = false;
    class QSyntaxHighlighter *highlighter_;
    QVector<SourceSpan> spans_;
    QWidget *gutter_;
};
