#pragma once
#include "project.h"
#include <QPlainTextEdit>
#include <QRegularExpression>

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
    // Returns -1 when the supplied regular expression is invalid.
    int setFindHighlights(const QString &query, bool caseSensitive, bool wholeWords, bool regex);
    bool findText(const QString &query, bool caseSensitive, bool wholeWords, bool regex,
                  bool backwards);
    void clearFindHighlights();
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
  signals:
    void navigateRequested();
    void referencesRequested();
    void callGraphRequested();
    void renameRequested();

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    void highlightCurrentLine();
    void updateHighlights();
    QRegularExpression findExpression(const QString &query, bool caseSensitive, bool wholeWords,
                                      bool regex) const;
    QString selectedIdentifier() const;
    bool goToLocalDeclaration();
    bool light_ = false;
    class QSyntaxHighlighter *highlighter_;
    QVector<SourceSpan> spans_;
    QWidget *gutter_;
    QString findQuery_;
    bool findCaseSensitive_ = false, findWholeWords_ = false, findRegex_ = false;
};
