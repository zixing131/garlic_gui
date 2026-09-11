#pragma once
#include <QAbstractScrollArea>
#include <QFile>

// Read-only, paged view: the text document never grows with the file.
class HexViewer : public QAbstractScrollArea {
  public:
    HexViewer(const QString &path, int previewKiB, QWidget *parent = nullptr);
  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void scrollContentsBy(int, int) override { viewport()->update(); }
  private:
    void updateRange();
    QFile file_;
    QByteArray page_;
    qint64 pageOffset_ = -1, pageBytes_, rows_ = 0;
};
