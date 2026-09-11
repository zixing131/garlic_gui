#pragma once
#include <QAbstractScrollArea>
#include <QFile>
#include <QTimer>

// Read-only, paged view. Only copying a selection materializes its bytes.
class HexViewer : public QAbstractScrollArea {
  public:
    HexViewer(const QString &path, int previewKiB, QWidget *parent = nullptr);
    qint64 selectionStart() const { return qMin(anchor_, cursor_); }
    qint64 selectionEnd() const { return qMax(anchor_, cursor_); }
    void copySelection(const QString &format = "text");
  protected:
    void paintEvent(QPaintEvent *) override;
    void changeEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void scrollContentsBy(int, int) override { viewport()->update(); }
  private:
    void updateRange();
    void ensureFixedFont();
    qint64 firstRow() const;
    qint64 byteAt(const QPoint &point) const;
    void dragSelection();
    int cellWidth() const;
    QFile file_;
    QByteArray page_;
    qint64 pageOffset_ = -1, pageBytes_, rows_ = 0;
    qint64 anchor_ = -1, cursor_ = -1, dragOrigin_ = -1;
    bool dragging_ = false;
    QPoint dragPoint_;
    QTimer dragTimer_;
};
