#include "codeeditor.h"
#include <QPainter>
#include <QTextBlock>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QFontDatabase>

namespace {
class Gutter : public QWidget {
public:
    explicit Gutter(CodeEditor *editor) : QWidget(editor), editor_(editor) {}
    QSize sizeHint() const override { return {editor_->gutterWidth(), 0}; }
    void paintEvent(QPaintEvent *event) override { editor_->paintGutter(event); }
private:
    CodeEditor *editor_;
};

class Highlighter : public QSyntaxHighlighter {
public:
    Highlighter(QTextDocument *doc, bool smali) : QSyntaxHighlighter(doc), smali_(smali) {}
protected:
    void highlightBlock(const QString &text) override {
        // Bound work for pathological single-line decompiler output.
        if (text.size() > 20000) return;
        auto apply = [this, &text](const QRegularExpression &re, const QColor &color) {
            auto it = re.globalMatch(text);
            while (it.hasNext()) { const auto m = it.next(); setFormat(m.capturedStart(), m.capturedLength(), color); }
        };
        static const QRegularExpression keywords(QStringLiteral("\\b(abstract|assert|boolean|break|byte|case|catch|char|class|const|continue|default|do|double|else|enum|extends|final|finally|float|for|if|implements|import|instanceof|int|interface|long|native|new|package|private|protected|public|return|short|static|strictfp|super|switch|synchronized|this|throw|throws|transient|try|void|volatile|while|true|false|null|record|sealed|var)\\b"));
        static const QRegularExpression numbers(QStringLiteral("\\b(?:0x[0-9a-fA-F]+|[0-9]+(?:\\.[0-9]+)?)[fFdDlL]?\\b"));
        static const QRegularExpression annotations(QStringLiteral("@[\\w.]+"));
        static const QRegularExpression smaliOps(QStringLiteral("(?:^\\s*\\.[\\w-]+|\\b(?:[vp][0-9]+|invoke-[\\w/-]+|move[\\w/-]*|return[\\w/-]*|const[\\w/-]*|iget[\\w/-]*|iput[\\w/-]*)\\b)"));
        apply(keywords, QColor("#c4a3ff"));
        apply(numbers, QColor("#e6b878"));
        apply(annotations, QColor("#72cbd2"));
        if (smali_) apply(smaliOps, QColor("#c4a3ff"));
        setCurrentBlockState(0);
        int i = 0;
        if (!smali_ && previousBlockState() == 1) {
            const int end = text.indexOf("*/");
            if (end < 0) { setFormat(0, text.size(), QColor("#758698")); setCurrentBlockState(1); return; }
            setFormat(0, end + 2, QColor("#758698")); i = end + 2;
        }
        while (i < text.size()) {
            if ((smali_ && text[i] == '#') || (!smali_ && text.mid(i, 2) == "//")) {
                setFormat(i, text.size() - i, QColor("#758698")); break;
            }
            if (!smali_ && text.mid(i, 2) == "/*") {
                int end = text.indexOf("*/", i + 2);
                if (end < 0) { setFormat(i, text.size() - i, QColor("#758698")); setCurrentBlockState(1); break; }
                setFormat(i, end + 2 - i, QColor("#758698")); i = end + 2; continue;
            }
            if (text[i] == '"' || text[i] == '\'') {
                const auto quote = text[i]; const int start = i++;
                while (i < text.size()) {
                    if (text[i] == '\\') { i = qMin(i + 2, int(text.size())); continue; }
                    if (text[i++] == quote) break;
                }
                setFormat(start, i - start, QColor("#93d5ac"));
            } else ++i;
        }
    }
private:
    bool smali_;
};
}

CodeEditor::CodeEditor(bool smali, QWidget *parent) : QPlainTextEdit(parent), gutter_(new Gutter(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(13);
    setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    setUndoRedoEnabled(false);
    new Highlighter(document(), smali);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy) gutter_->scroll(0, dy); else gutter_->update(0, rect.y(), gutter_->width(), rect.height());
        if (rect.contains(viewport()->rect())) setViewportMargins(gutterWidth(), 0, 0, 0);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        QTextEdit::ExtraSelection line;
        line.format.setBackground(QColor("#202d3b"));
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor(); line.cursor.clearSelection();
        setExtraSelections({line});
    });
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

int CodeEditor::gutterWidth() const {
    return 22 + fontMetrics().horizontalAdvance('9') * QString::number(qMax(1, blockCount())).size();
}

void CodeEditor::resizeEvent(QResizeEvent *event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    gutter_->setGeometry(cr.left(), cr.top(), gutterWidth(), cr.height());
}

void CodeEditor::paintGutter(QPaintEvent *event) {
    QPainter painter(gutter_);
    painter.fillRect(event->rect(), QColor("#17212d"));
    auto block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(QColor("#66798d"));
            painter.drawText(0, top, gutter_->width() - 10, fontMetrics().height(), Qt::AlignRight, QString::number(number + 1));
        }
        block = block.next(); top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height()); ++number;
    }
}
