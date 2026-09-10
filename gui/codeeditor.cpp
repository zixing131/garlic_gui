#include "codeeditor.h"
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>

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
        if (text.size() > 20000)
            return;
        const bool light = document()->property("lightTheme").toBool();
        auto color = [light](const char *dark, const char *bright) {
            return QColor(light ? bright : dark);
        };
        auto apply = [this, &text](const QRegularExpression &re, const QColor &color) {
            auto it = re.globalMatch(text);
            while (it.hasNext()) {
                const auto m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), color);
            }
        };
        static const QRegularExpression keywords(QStringLiteral(
            "\\b(abstract|assert|boolean|break|byte|case|catch|char|class|const|continue|default|"
            "do|double|else|enum|extends|final|finally|float|for|if|implements|import|instanceof|"
            "int|interface|long|native|new|package|private|protected|public|return|short|static|"
            "strictfp|super|switch|synchronized|this|throw|throws|transient|try|void|volatile|"
            "while|true|false|null|record|sealed|var)\\b"));
        static const QRegularExpression numbers(
            QStringLiteral("\\b(?:0x[0-9a-fA-F]+|[0-9]+(?:\\.[0-9]+)?)[fFdDlL]?\\b"));
        static const QRegularExpression annotations(QStringLiteral("@[\\w.]+"));
        static const QRegularExpression smaliOps(
            QStringLiteral("(?:^\\s*\\.[\\w-]+|\\b(?:[vp][0-9]+|invoke-[\\w/-]+|move[\\w/"
                           "-]*|return[\\w/-]*|const[\\w/-]*|iget[\\w/-]*|iput[\\w/-]*)\\b)"));
        apply(keywords, color("#c4a3ff", "#6530a3"));
        apply(numbers, color("#e6b878", "#925800"));
        apply(annotations, color("#72cbd2", "#006e87"));
        if (smali_)
            apply(smaliOps, color("#c4a3ff", "#6530a3"));
        setCurrentBlockState(0);
        int i = 0;
        if (!smali_ && previousBlockState() == 1) {
            const int end = text.indexOf("*/");
            if (end < 0) {
                setFormat(0, text.size(), color("#758698", "#5c7483"));
                setCurrentBlockState(1);
                return;
            }
            setFormat(0, end + 2, color("#758698", "#5c7483"));
            i = end + 2;
        }
        while (i < text.size()) {
            if ((smali_ && text[i] == '#') || (!smali_ && text.mid(i, 2) == "//")) {
                setFormat(i, text.size() - i, color("#758698", "#5c7483"));
                break;
            }
            if (!smali_ && text.mid(i, 2) == "/*") {
                int end = text.indexOf("*/", i + 2);
                if (end < 0) {
                    setFormat(i, text.size() - i, color("#758698", "#5c7483"));
                    setCurrentBlockState(1);
                    break;
                }
                setFormat(i, end + 2 - i, color("#758698", "#5c7483"));
                i = end + 2;
                continue;
            }
            if (text[i] == '"' || text[i] == '\'') {
                const auto quote = text[i];
                const int start = i++;
                while (i < text.size()) {
                    if (text[i] == '\\') {
                        i = qMin(i + 2, int(text.size()));
                        continue;
                    }
                    if (text[i++] == quote)
                        break;
                }
                setFormat(start, i - start, color("#93d5ac", "#237b35"));
            } else
                ++i;
        }
    }

  private:
    bool smali_;
};
} // namespace

CodeEditor::CodeEditor(bool smali, QWidget *parent)
    : QPlainTextEdit(parent), gutter_(new Gutter(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(13);
    setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    setUndoRedoEnabled(false);
    highlighter_ = new Highlighter(document(), smali);
    connect(this, &QPlainTextEdit::blockCountChanged, this,
            [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy)
            gutter_->scroll(0, dy);
        else
            gutter_->update(0, rect.y(), gutter_->width(), rect.height());
        if (rect.contains(viewport()->rect()))
            setViewportMargins(gutterWidth(), 0, 0, 0);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

int CodeEditor::gutterWidth() const {
    return 22 +
           fontMetrics().horizontalAdvance('9') * QString::number(qMax(1, blockCount())).size();
}

void CodeEditor::resizeEvent(QResizeEvent *event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    gutter_->setGeometry(cr.left(), cr.top(), gutterWidth(), cr.height());
}

void CodeEditor::paintGutter(QPaintEvent *event) {
    QPainter painter(gutter_);
    painter.fillRect(event->rect(), QColor(light_ ? "#f1f4f8" : "#17212d"));
    auto block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(QColor("#66798d"));
            painter.drawText(0, top, gutter_->width() - 10, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void CodeEditor::setSource(const SourceDocument &document) {
    const int position = textCursor().position();
    highlighter_->setDocument(nullptr);
    setPlainText(document.text);
    // Very large documents remain navigable without an expensive synchronous rehighlight.
    if (document.text.size() <= 512 * 1024)
        highlighter_->setDocument(this->document());
    spans_ = document.spans;
    auto cursor = textCursor();
    cursor.setPosition(qMin(position, int(document.text.size())));
    setTextCursor(cursor);
}
QString CodeEditor::symbolAtCursor() const {
    int position = textCursor().selectionStart();
    for (const auto &s : spans_)
        if (position >= s.start && position < s.end)
            return s.id;
    return {};
}
bool CodeEditor::goToSymbol(const QString &id) {
    for (const auto &s : spans_)
        if (s.id == id && s.declaration) {
            auto cursor = textCursor();
            cursor.setPosition(s.start);
            cursor.setPosition(s.end, QTextCursor::KeepAnchor);
            setTextCursor(cursor);
            centerCursor();
            return true;
        }
    return false;
}
void CodeEditor::goToLine(int line) {
    auto block = document()->findBlockByNumber(qMax(0, line - 1));
    if (block.isValid()) {
        setTextCursor(QTextCursor(block));
        centerCursor();
    }
}
void CodeEditor::mouseDoubleClickEvent(QMouseEvent *event) {
    QPlainTextEdit::mouseDoubleClickEvent(event);
    if (event->button() == Qt::LeftButton && !symbolAtCursor().isEmpty()) emit navigateRequested();
}
void CodeEditor::mousePressEvent(QMouseEvent *event) {
    QPlainTextEdit::mousePressEvent(event);
    if (event->button() == Qt::LeftButton &&
        (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))
        emit navigateRequested();
}
void CodeEditor::contextMenuEvent(QContextMenuEvent *event) {
    if (!textCursor().hasSelection())
        setTextCursor(cursorForPosition(event->pos()));
    auto menu = createStandardContextMenu();
    menu->addSeparator();
    auto go = menu->addAction(tr("跳转到声明  F12"), this, &CodeEditor::navigateRequested);
    auto refs = menu->addAction(tr("查找引用  X"), this, &CodeEditor::referencesRequested);
    auto rename = menu->addAction(tr("重命名  N"), this, &CodeEditor::renameRequested);
    const bool known = !symbolAtCursor().isEmpty();
    go->setEnabled(known);
    refs->setEnabled(known);
    rename->setEnabled(known);
    menu->exec(event->globalPos());
    delete menu;
}

void CodeEditor::highlightCurrentLine() {
    QTextEdit::ExtraSelection line;
    line.format.setBackground(QColor(light_ ? "#eaf0f7" : "#202d3b"));
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    setExtraSelections({line});
}
void CodeEditor::setTheme(bool light) {
    light_ = light;
    document()->setProperty("lightTheme", light);
    setStyleSheet(light ? "QPlainTextEdit { background: #ffffff; color: #243446; "
                          "selection-background-color: #bdd9f7; selection-color: #12293d; }"
                        : "QPlainTextEdit { background: #111b26; color: #dce5ee; "
                          "selection-background-color: #38635b; }");
    if (highlighter_->document())
        highlighter_->rehighlight();
    highlightCurrentLine();
    gutter_->update();
}
