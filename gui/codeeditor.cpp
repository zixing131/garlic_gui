#include "codeeditor.h"
#include "hookcode.h"
#include <QApplication>
#include <QClipboard>
#include "nodeicons.h"
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <algorithm>
#include <QTimer>
#include <QScrollBar>
#include <QElapsedTimer>

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
        if (document()->property("deferHighlight").toBool() &&
            !document()->property("highlightChunk").toBool()) {
            setCurrentBlockState(0);
            return;
        }
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
        static const QRegularExpression types(QStringLiteral("\\b[A-Z][A-Za-z0-9_$]*\\b"));
        static const QRegularExpression calls(
            QStringLiteral("\\b[\\p{L}_$][\\p{L}\\p{N}_$]*(?=\\s*\\()"));
        static const QRegularExpression constants(QStringLiteral("\\b[A-Z][A-Z0-9_]{2,}\\b"));
        static const QRegularExpression primitives(
            QStringLiteral("\\b(?:boolean|byte|char|short|int|long|float|double|void)\\b"));
        apply(types, color("#7dcfff", "#006b8f"));
        apply(calls, color("#82aaff", "#2556a8"));
        apply(constants, color("#e0af68", "#935400"));
        apply(keywords, color("#c4a3ff", "#6530a3"));
        apply(primitives, color("#56b6c2", "#007b83"));
        apply(numbers, color("#e6b878", "#925800"));
        apply(annotations, color("#72cbd2", "#006e87"));
        if (smali_)
            apply(smaliOps, color("#c4a3ff", "#6530a3"));
        if (smali_) {
            static const QRegularExpression labels(":[A-Za-z0-9_]+"), descriptors("L[\\w/$]+;"),
                registers("\\b[vp][0-9]+\\b");
            apply(labels, color("#e0af68", "#935400"));
            apply(descriptors, color("#7dcfff", "#006b8f"));
            apply(registers, color("#ff9e64", "#a44a16"));
        }
        if (document()->property("language") == "xml") {
            static const QRegularExpression tags("</?[\\w:.-]+|/?>"), attrs("[\\w:.-]+(?=\\s*=)"),
                values("\"[^\"]*\"|'[^']*'");
            setFormat(0, text.size(), color("#dce5ee", "#243446"));
            apply(tags, color("#c4a3ff", "#6530a3"));
            apply(attrs, color("#7dcfff", "#006b8f"));
            apply(values, color("#a4e4bd", "#267650"));
            int a = previousBlockState() == 2 ? 0 : text.indexOf("<!--");
            setCurrentBlockState(0);
            while (a >= 0) {
                int b = text.indexOf("-->", a);
                setFormat(a, b < 0 ? text.size() - a : b + 3 - a, color("#758698", "#5c7483"));
                if (b < 0) {
                    setCurrentBlockState(2);
                    break;
                }
                a = text.indexOf("<!--", b + 3);
            }
            return;
        }
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
    setAttribute(Qt::WA_InputMethodEnabled, false);
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
    connect(this, &QPlainTextEdit::selectionChanged, this, &CodeEditor::highlightCurrentLine);
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

void CodeEditor::setSource(const SourceDocument &source) {
    // QTextDocument collapses CRLF; adjust symbol positions before handing text to Qt.
    SourceDocument document = source;
    QVector<int> removed;
    for (int i = 0; i + 1 < source.text.size(); ++i)
        if (source.text[i] == '\r' && source.text[i + 1] == '\n')
            removed << i;
    if (!removed.isEmpty()) {
        document.text.replace("\r\n", "\n");
        for (auto &span : document.spans) {
            span.start -=
                std::lower_bound(removed.begin(), removed.end(), span.start) - removed.begin();
            span.end -=
                std::lower_bound(removed.begin(), removed.end(), span.end) - removed.begin();
        }
    }
    const int position = textCursor().position();
    const int vertical = verticalScrollBar()->value(), horizontal = horizontalScrollBar()->value();
    highlighter_->setDocument(nullptr);
    setPlainText(document.text);
    this->document()->setProperty("deferHighlight", document.text.size() > 512 * 1024);
    highlighter_->setDocument(this->document());
    if (this->document()->property("deferHighlight").toBool()) scheduleHighlight();
    else ++highlightGeneration_;
    spans_ = document.spans;
    auto cursor = textCursor();
    cursor.setPosition(qMin(position, int(document.text.size())));
    setTextCursor(cursor);
    verticalScrollBar()->setValue(vertical);
    horizontalScrollBar()->setValue(horizontal);
    updateHighlights();
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
    if (event->button() != Qt::LeftButton)
        return;
    if (!symbolAtCursor().isEmpty())
        emit navigateRequested();
    else
        goToLocalDeclaration();
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
    auto graph = menu->addAction(NodeIcons::icon("methodReference"), tr("查看函数调用图  G"),
                                 this, &CodeEditor::callGraphRequested);
    auto rename = menu->addAction(tr("重命名  N"), this, &CodeEditor::renameRequested);
    const auto symbol = symbolAtCursor();
    const bool known = !symbol.isEmpty();
    const bool method = symbol.contains("->") && symbol.contains('(');
    go->setEnabled(known);
    refs->setEnabled(known);
    graph->setEnabled(method);
    for (bool xposed : {false, true}) {
        const auto snippet = HookCode::generate(symbol, xposed);
        auto action = menu->addAction(xposed ? tr("复制为 Xposed Hook") : tr("复制为 Frida Hook"),
                                     this, [snippet] { QApplication::clipboard()->setText(snippet); });
        action->setEnabled(!snippet.isEmpty());
    }
    rename->setEnabled(known);
    menu->exec(event->globalPos());
    delete menu;
}

void CodeEditor::highlightCurrentLine() {
    updateHighlights();
}

QString CodeEditor::selectedIdentifier() const {
    const auto value = textCursor().selectedText();
    static const QRegularExpression identifier(
        "^[\\p{L}_$][\\p{L}\\p{N}_$]*$", QRegularExpression::UseUnicodePropertiesOption);
    return identifier.match(value).hasMatch() ? value : QString();
}

bool CodeEditor::goToLocalDeclaration() {
    const auto name = selectedIdentifier();
    const int before = textCursor().selectionStart();
    if (name.isEmpty() || before <= 0)
        return false;
    // Local slots do not have a cross-class bytecode identity. Within this editor we can still
    // provide the useful IDE behavior: select the latest typed declaration before the use.
    const auto escaped = QRegularExpression::escape(name);
    const QRegularExpression declaration(
        "(?:^|[;{}(,])\\s*(?:(?:final|volatile|transient)\\s+)*(?:[\\p{L}_$][\\p{L}\\p{N}_$]*"
        "(?:\\s*<[^{;}()]*>)?(?:\\s*\\[\\])?\\s+)+(" +
            escaped + ")(?![\\p{L}\\p{N}_$])",
        QRegularExpression::UseUnicodePropertiesOption);
    auto matches = declaration.globalMatch(toPlainText().left(before));
    int start = -1, end = -1;
    while (matches.hasNext()) {
        const auto match = matches.next();
        start = match.capturedStart(1);
        end = match.capturedEnd(1);
    }
    if (start < 0 || end <= start)
        return false;
    auto cursor = textCursor();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
    centerCursor();
    return true;
}

QRegularExpression CodeEditor::findExpression(const QString &query, bool caseSensitive,
                                               bool wholeWords, bool regex) const {
    if (query.isEmpty())
        return {};
    QString pattern = regex ? query : QRegularExpression::escape(query);
    if (wholeWords)
        pattern = "(?<![\\p{L}\\p{N}_$])(?:" + pattern + ")(?![\\p{L}\\p{N}_$])";
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (!caseSensitive)
        options |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(pattern, options);
}

void CodeEditor::updateHighlights() {
    QList<QTextEdit::ExtraSelection> selections;
    QTextEdit::ExtraSelection line;
    line.format.setBackground(QColor(light_ ? "#eaf0f7" : "#202d3b"));
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    selections << line;
    auto addMatches = [this, &selections](const QRegularExpression &expression, const QColor &color,
                                          int limit) {
        if (!expression.isValid() || expression.pattern().isEmpty())
            return;
        auto matches = expression.globalMatch(toPlainText());
        int count = 0;
        while (matches.hasNext() && count++ < limit) {
            const auto match = matches.next();
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(color);
            selection.cursor = textCursor();
            selection.cursor.setPosition(match.capturedStart());
            selection.cursor.setPosition(match.capturedEnd(), QTextCursor::KeepAnchor);
            selections << selection;
        }
    };
    const auto selected = selectedIdentifier();
    if (!selected.isEmpty())
        addMatches(findExpression(selected, true, true, false),
                   QColor(light_ ? "#ffe082" : "#725f1d"), 3000);
    if (!findQuery_.isEmpty())
        addMatches(findExpression(findQuery_, findCaseSensitive_, findWholeWords_, findRegex_),
                   QColor(light_ ? "#b9d9f5" : "#315a75"), 3000);
    setExtraSelections(selections);
}

int CodeEditor::setFindHighlights(const QString &query, bool caseSensitive, bool wholeWords,
                                  bool regex) {
    findQuery_ = query;
    findCaseSensitive_ = caseSensitive;
    findWholeWords_ = wholeWords;
    findRegex_ = regex;
    const auto expression = findExpression(query, caseSensitive, wholeWords, regex);
    if (!expression.isValid()) {
        updateHighlights();
        return -1;
    }
    int count = 0;
    auto matches = expression.globalMatch(toPlainText());
    while (matches.hasNext() && count < 10000) {
        matches.next();
        ++count;
    }
    updateHighlights();
    return count;
}

bool CodeEditor::findText(const QString &query, bool caseSensitive, bool wholeWords, bool regex,
                          bool backwards) {
    const auto expression = findExpression(query, caseSensitive, wholeWords, regex);
    if (!expression.isValid() || expression.pattern().isEmpty())
        return false;
    auto flags = backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags();
    auto cursor = document()->find(expression, textCursor(), flags);
    if (cursor.isNull()) {
        cursor = textCursor();
        cursor.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        cursor = document()->find(expression, cursor, flags);
    }
    if (cursor.isNull())
        return false;
    setTextCursor(cursor);
    centerCursor();
    return true;
}

void CodeEditor::clearFindHighlights() {
    findQuery_.clear();
    updateHighlights();
}
void CodeEditor::setTheme(bool light) {
    const bool changed = light_ != light;
    light_ = light;
    document()->setProperty("lightTheme", light);
    setStyleSheet(light ? "QPlainTextEdit { background: #ffffff; color: #243446; "
                          "selection-background-color: #bdd9f7; selection-color: #12293d; }"
                        : "QPlainTextEdit { background: #111b26; color: #dce5ee; "
                          "selection-background-color: #38635b; }");
    if (changed && highlighter_->document()) scheduleHighlight();
    highlightCurrentLine();
    gutter_->update();
}

void CodeEditor::scheduleHighlight() {
    const int generation = ++highlightGeneration_;
    if (!document()->property("deferHighlight").toBool()) {
        if (highlighter_->document()) highlighter_->rehighlight();
        return;
    }
    auto block = std::make_shared<QTextBlock>(document()->begin());
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = step;
    *step = [this, block, generation, weak] {
        if (generation != highlightGeneration_) return;
        QElapsedTimer budget; budget.start();
        document()->setProperty("highlightChunk", true);
        while (block->isValid() && budget.elapsed() < 5) {
            highlighter_->rehighlightBlock(*block);
            *block = block->next();
        }
        document()->setProperty("highlightChunk", false);
        if (block->isValid())
            if (auto next = weak.lock()) QTimer::singleShot(0, this, [next] { (*next)(); });
    };
    QTimer::singleShot(0, this, [step] { (*step)(); });
}
