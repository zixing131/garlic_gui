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
    bool goToSymbol(const QString &id, int line = 0);
    bool goToHit(const QJsonObject &hit);
    void goToLine(int line);
    // Returns -1 when the supplied regular expression is invalid.
    int setFindHighlights(const QString &query, bool caseSensitive, bool wholeWords, bool regex);
    bool findText(const QString &query, bool caseSensitive, bool wholeWords, bool regex,
                  bool backwards);
    void clearFindHighlights();
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
  signals:
    void modeSwitchRequested();
    void localJumpStarted();
    void localJumpFinished();
    void navigateRequested();
    void referencesRequested();
    void callGraphRequested();
    void renameRequested();

  protected:
    bool event(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    void scheduleHighlight();
    int highlightGeneration_ = 0;
    void highlightCurrentLine();
    void updateHighlights();
    QRegularExpression findExpression(const QString &query, bool caseSensitive, bool wholeWords,
                                      bool regex) const;
    QString selectedIdentifier() const;
    bool light_ = false;
    class QSyntaxHighlighter *highlighter_;
    QVector<SourceSpan> spans_;
    QWidget *gutter_ = nullptr;
    QString findQuery_;
    bool findCaseSensitive_ = false, findWholeWords_ = false, findRegex_ = false;
};
