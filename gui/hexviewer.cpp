#include "hexviewer.h"
#include "settings.h"
#include <QtWidgets>
#include <limits>

HexViewer::HexViewer(const QString &path, int previewKiB, QWidget *parent)
    : QAbstractScrollArea(parent), file_(path), pageBytes_(qint64(qBound(1, previewKiB, 16384)) * 1024) {
    setObjectName("hexViewer");
    setFocusPolicy(Qt::StrongFocus);
    setFont(AppSettings::load().codeFont(true));
    ensureFixedFont();
    file_.open(QIODevice::ReadOnly);
    rows_ = (file_.size() + 15) / 16;
    setToolTip(tr("只读十六进制预览；滚动时按需读取文件。"));
    dragTimer_.setInterval(40);
    connect(&dragTimer_, &QTimer::timeout, this, [this] {
        if (!dragging_) return;
        if (dragPoint_.y() < 0) verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
        else if (dragPoint_.y() >= viewport()->height()) verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        dragSelection();
    });
    updateRange();
}
void HexViewer::ensureFixedFont() {
    auto fixed = font();
    if (!QFontDatabase::isFixedPitch(fixed.family())) {
        fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        for (const auto &family : {"Consolas", "Cascadia Mono", "Menlo", "DejaVu Sans Mono", "Liberation Mono"})
            if (QFontDatabase::families().contains(family) && QFontDatabase::isFixedPitch(family)) {
                fixed.setFamily(family); break;
            }
        fixed.setPointSizeF(font().pointSizeF() > 0 ? font().pointSizeF() : 12);
    }
    fixed.setStyleHint(QFont::Monospace);
    fixed.setFixedPitch(true);
    if (font() != fixed) setFont(fixed);
}
int HexViewer::cellWidth() const {
    return qMax(1, qMax(fontMetrics().horizontalAdvance('W'), fontMetrics().horizontalAdvance('0')));
}
qint64 HexViewer::firstRow() const {
    const int visible = qMax(1, viewport()->height() / fontMetrics().height());
    const qint64 maximum = qMax<qint64>(0, rows_ - visible);
    return verticalScrollBar()->maximum() ?
        qint64(static_cast<long double>(verticalScrollBar()->value()) * maximum / verticalScrollBar()->maximum()) : 0;
}
void HexViewer::updateRange() {
    const int visible = qMax(1, viewport()->height() / fontMetrics().height());
    verticalScrollBar()->setRange(0, int(qMin<qint64>(qMax<qint64>(0, rows_ - visible), std::numeric_limits<int>::max())));
    verticalScrollBar()->setPageStep(visible);
    horizontalScrollBar()->setRange(0, qMax(0, 8 + 80 * cellWidth() - viewport()->width()));
}
void HexViewer::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    updateRange();
}
void HexViewer::paintEvent(QPaintEvent *) {
    QPainter painter(viewport());
    painter.setFont(font());
    painter.fillRect(viewport()->rect(), palette().base());
    if (!file_.isOpen()) { painter.setPen(palette().text().color()); painter.drawText(8, 20, file_.errorString()); return; }
    const bool light = palette().base().color().lightness() > 128;
    const QColor address = light ? QColor("#6b7280") : QColor("#94a3b8");
    const QColor zero = light ? QColor("#9a7272") : QColor("#b08a8a");
    const QColor printable = light ? QColor("#146b43") : QColor("#8fd5ac");
    const QColor binary = light ? QColor("#245da8") : QColor("#8dbaff");
    const int height = fontMetrics().height(), visible = qMax(1, viewport()->height() / height), cell = cellWidth();
    const qint64 first = firstRow();
    const int left = 8 - horizontalScrollBar()->value();
    auto draw = [&](int col, int row, const QString &text, QColor color, bool selected, int width) {
        const QRect area(left + col * cell, row * height, width * cell, height);
        if (selected) painter.fillRect(area, palette().highlight());
        painter.setPen(selected ? palette().highlightedText().color() : color);
        painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, text);
    };
    for (int row = 0; row <= visible && first + row < rows_; ++row) {
        const qint64 offset = (first + row) * 16;
        if (offset < pageOffset_ || offset >= pageOffset_ + page_.size()) {
            pageOffset_ = offset / pageBytes_ * pageBytes_;
            if (!file_.seek(pageOffset_)) break;
            page_ = file_.read(pageBytes_);
        }
        const auto bytes = page_.mid(offset - pageOffset_, 16);
        draw(0, row, QString("%1").arg(offset, 12, 16, QChar('0')), address, false, 12);
        for (int i = 0; i < bytes.size(); ++i) {
            const auto byte = uchar(bytes[i]);
            const bool ascii = byte >= 32 && byte < 127;
            const QColor color = byte == 0 ? zero : ascii ? printable : binary;
            const bool selected = anchor_ >= 0 && offset + i >= selectionStart() && offset + i < selectionEnd();
            draw(14 + 3 * i, row, QString("%1").arg(byte, 2, 16, QChar('0')), color, selected, 2);
            draw(64 + i, row, ascii ? QString(QChar(byte)) : QString("."), color, selected, 1);
        }
    }
}
qint64 HexViewer::byteAt(const QPoint &point) const {
    const int x = point.x() + horizontalScrollBar()->value() - 8;
    const int column = x / cellWidth();
    if (x < 0 || column < 14 || (column >= 62 && column < 64) || column >= 80) return -1;
    const int byte = column >= 64 ? column - 64 : (column - 14) / 3;
    const qint64 offset = (firstRow() + qBound(0, point.y(), qMax(0, viewport()->height() - 1)) / fontMetrics().height()) * 16 + byte;
    return offset < file_.size() ? offset : -1;
}
void HexViewer::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) { QAbstractScrollArea::mousePressEvent(event); return; }
    const auto at = byteAt(event->pos());
    if (at < 0) return;
    setFocus();
    if (!(event->modifiers() & Qt::ShiftModifier) || dragOrigin_ < 0) dragOrigin_ = at;
    anchor_ = at >= dragOrigin_ ? dragOrigin_ : dragOrigin_ + 1;
    cursor_ = at >= dragOrigin_ ? at + 1 : at;
    dragging_ = true; dragPoint_ = event->pos(); dragTimer_.start();
    viewport()->update();
}
void HexViewer::dragSelection() {
    const auto at = byteAt(dragPoint_);
    if (at < 0) return;
    anchor_ = at >= dragOrigin_ ? dragOrigin_ : dragOrigin_ + 1;
    cursor_ = at >= dragOrigin_ ? at + 1 : at;
    viewport()->update();
}
void HexViewer::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_) { dragPoint_ = event->pos(); dragSelection(); }
}
void HexViewer::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) { dragging_ = false; dragTimer_.stop(); event->accept(); return; }
    QAbstractScrollArea::mouseReleaseEvent(event);
}
void HexViewer::copySelection(const QString &format) {
    const qint64 size = selectionEnd() - selectionStart();
    if (anchor_ < 0 || size <= 0) return;
    if (size > 64 * 1024 * 1024) {
        QMessageBox::information(this, tr("复制"), tr("所选内容超过 64 MiB，请缩小选择范围后复制。"));
        return;
    }
    if (!file_.seek(selectionStart())) return;
    const auto bytes = file_.read(size);
    QString text;
    if (format == "hex") text = QString::fromLatin1(bytes.toHex(' '));
    else if (format == "base64") text = QString::fromLatin1(bytes.toBase64());
    else text = QString::fromUtf8(bytes);
    QApplication::clipboard()->setText(text);
}
void HexViewer::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::Copy)) { copySelection(); return; }
    if (event->matches(QKeySequence::SelectAll)) { anchor_ = 0; cursor_ = file_.size(); viewport()->update(); return; }
    QAbstractScrollArea::keyPressEvent(event);
}
void HexViewer::contextMenuEvent(QContextMenuEvent *event) {
    QMenu menu(this);
    for (const auto &item : {qMakePair(tr("复制"), QString("text")),
                             qMakePair(tr("复制为 Hex"), QString("hex")),
                             qMakePair(tr("复制为 Base64"), QString("base64"))}) {
        auto action = menu.addAction(item.first, this, [this, format = item.second] { copySelection(format); });
        action->setEnabled(anchor_ >= 0 && selectionEnd() > selectionStart());
    }
    menu.exec(event->globalPos());
}
void HexViewer::changeEvent(QEvent *event) {
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) { ensureFixedFont(); updateRange(); }
    if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange) viewport()->update();
}
