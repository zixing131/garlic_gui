#include "hexviewer.h"
#include <QFontDatabase>
#include <QPainter>
#include <QScrollBar>
#include <limits>

HexViewer::HexViewer(const QString &path, int previewKiB, QWidget *parent)
    : QAbstractScrollArea(parent), file_(path), pageBytes_(qint64(qBound(1, previewKiB, 16384)) * 1024) {
    setObjectName("hexViewer");
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    file_.open(QIODevice::ReadOnly);
    rows_ = (file_.size() + 15) / 16;
    setToolTip(tr("只读十六进制预览；滚动时按需读取文件。"));
    updateRange();
}
void HexViewer::updateRange() {
    const int visible = qMax(1, viewport()->height() / fontMetrics().height());
    verticalScrollBar()->setRange(0, int(qMin<qint64>(qMax<qint64>(0, rows_ - visible), std::numeric_limits<int>::max())));
    verticalScrollBar()->setPageStep(visible);
    horizontalScrollBar()->setRange(0, qMax(0, fontMetrics().horizontalAdvance(QString(86, '0')) - viewport()->width()));
}
void HexViewer::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    updateRange();
}
void HexViewer::paintEvent(QPaintEvent *) {
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), palette().base());
    painter.setPen(palette().text().color());
    if (!file_.isOpen()) { painter.drawText(8, 20, file_.errorString()); return; }
    const int height = fontMetrics().height(), visible = qMax(1, viewport()->height() / height);
    const qint64 maximum = qMax<qint64>(0, rows_ - visible);
    const qint64 first = verticalScrollBar()->maximum() ?
        qint64(static_cast<long double>(verticalScrollBar()->value()) * maximum / verticalScrollBar()->maximum()) : 0;
    for (int row = 0; row <= visible && first + row < rows_; ++row) {
        const qint64 offset = (first + row) * 16;
        if (offset < pageOffset_ || offset >= pageOffset_ + page_.size()) {
            pageOffset_ = offset / pageBytes_ * pageBytes_;
            if (!file_.seek(pageOffset_)) break;
            page_ = file_.read(pageBytes_);
        }
        const auto bytes = page_.mid(offset - pageOffset_, 16);
        QString ascii;
        for (auto byte : bytes) ascii += uchar(byte) >= 32 && uchar(byte) < 127 ? QChar(uchar(byte)) : QChar('.');
        painter.drawText(8 - horizontalScrollBar()->value(), row * height + fontMetrics().ascent(),
            QString("%1  %2  %3").arg(offset, 12, 16, QChar('0')).arg(QString::fromLatin1(bytes.toHex(' ')), -47).arg(ascii));
    }
}
