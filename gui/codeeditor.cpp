#include "codeeditor.h"
#include "settings.h"
#include <limits>
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
    setProperty("monoFont", smali);
    auto font = AppSettings::load().codeFont(smali);
    setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    setUndoRedoEnabled(false);
    highlighter_ = new Highlighter(document(), smali);
    connect(this, &QPlainTextEdit::blockCountChanged, this,
            [this] { setViewportMargins(gutterWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy) {
            gutter_->scroll(0, dy);
            updateHighlights();
        }
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

void CodeEditor::changeEvent(QEvent *event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
        if (gutter_) {
            setViewportMargins(gutterWidth(), 0, 0, 0);
            const auto cr = contentsRect();
            gutter_->setGeometry(cr.left(), cr.top(), gutterWidth(), cr.height());
            gutter_->update();
        }
    }
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
    locations_ = document.locations;
    for (auto &location : locations_) {
        location.start -= std::lower_bound(removed.begin(), removed.end(), location.start) - removed.begin();
        location.end -= std::lower_bound(removed.begin(), removed.end(), location.end) - removed.begin();
    }
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
bool CodeEditor::goToSymbol(const QString &id, int line) {
    for (const auto &s : spans_)
        if ((s.id == id) && (line > 0 ? document()->findBlock(s.start).blockNumber() + 1 == line : s.declaration)) {
            auto cursor = textCursor();
            cursor.setPosition(s.start);
            cursor.setPosition(s.end, QTextCursor::KeepAnchor);
            setTextCursor(cursor);
            centerCursor();
            return true;
        }
    return false;
}
bool CodeEditor::goToHit(const QJsonObject &hit) {
    int start = -1, length = 0;
    if (hit.value("mappedLocation").toBool()) {
        const SourceLocation *best = nullptr;
        const int offset = hit.value("offset").toInt(-1);
        for (const auto &location : locations_) {
            if (location.scope != hit.value("scope").toString()) continue;
            if (!best || qAbs(qint64(location.offset) - offset) < qAbs(qint64(best->offset) - offset)) best = &location;
        }
        if (!best) return false;
        auto cursor = textCursor(); cursor.setPosition(best->start);
        setTextCursor(cursor); centerCursor(); return true;
    }
    if (hit.contains("symbol")) {
        int occurrence = hit.value("occurrence").toInt();
        int scopeStart = 0, scopeEnd = document()->characterCount();
        const auto scope = hit.value("scope").toString();
        if (!scope.isEmpty()) {
            scopeStart = -1;
            for (const auto &span : spans_) {
                if (span.declaration && span.id == scope) scopeStart = span.start;
                else if (scope.contains("->") && scopeStart >= 0 && span.start > scopeStart && span.declaration &&
                         span.id.contains("->") && !span.id.contains("@local:")) {
                    scopeEnd = span.start; break;
                }
            }
            if (scopeStart < 0) return false;
        }
        if (hit.value("smali").toBool() && hit.value("offset").toInt(-1) >= 0) {
            const QString marker = "# @offset " + QString::number(hit.value("offset").toInt());
            for (auto block = document()->findBlock(scopeStart); block.isValid() && block.position() < scopeEnd; block = block.next()) {
                if (block.text().trimmed() != marker) continue;
                const auto instruction = block.next();
                if (!instruction.isValid()) return false;
                for (const auto &span : spans_)
                    if (span.start >= instruction.position() && span.end <= instruction.position() + instruction.length() &&
                        span.id == hit.value("symbol").toString()) {
                        auto cursor = textCursor(); cursor.setPosition(span.start);
                        cursor.setPosition(span.end, QTextCursor::KeepAnchor); setTextCursor(cursor); centerCursor(); return true;
                    }
                auto cursor = QTextCursor(instruction);
                cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                setTextCursor(cursor); centerCursor(); return true;
            }
            return false;
        }
        for (const auto &span : spans_)
            if (!span.declaration && span.start >= scopeStart && span.start < scopeEnd &&
                span.id == hit.value("symbol").toString() && occurrence-- == 0) {
                start = span.start; length = span.end - span.start; break;
            }
    } else if (hit.contains("sourceLine")) {
        int occurrence = hit.value("lineOccurrence").toInt();
        auto text = hit.value("sourceLine").toString();
        if (text.endsWith('\r')) text.chop(1);
        for (auto block = document()->begin(); block.isValid(); block = block.next())
            if (block.text() == text && occurrence-- == 0) {
                const int column = hit.value("column").toInt();
                length = hit.value("length").toInt();
                if (column >= 0 && length >= 0 && column + length <= text.size())
                    start = block.position() + column;
                break;
            }
    }
    if (start < 0) return false;
    auto cursor = textCursor();
    cursor.setPosition(start);
    cursor.setPosition(start + length, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
    centerCursor();
    return true;
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
    const auto id = symbolAtCursor();
    // Unmapped words are plain text, not candidates for guessed declarations.
    if (!id.isEmpty()) {
        emit localJumpStarted();
        const bool found = goToSymbol(id);
        emit localJumpFinished();
        if (!found) emit navigateRequested();
    }
}
void CodeEditor::mousePressEvent(QMouseEvent *event) {
    QPlainTextEdit::mousePressEvent(event);
    if (event->button() == Qt::LeftButton && !(event->modifiers() & Qt::ShiftModifier)) {
        auto cursor = textCursor();
        const auto text = cursor.block().text();
        int at = cursor.positionInBlock(), first = at, last = at;
        auto word = [](QChar c) { return c.isLetterOrNumber() || c == '_' || c == '$'; };
        if (at < text.size() && word(text[at])) {
            while (first > 0 && word(text[first - 1])) --first;
            while (last < text.size() && word(text[last])) ++last;
            cursor.setPosition(cursor.block().position() + first);
            cursor.setPosition(cursor.block().position() + last, QTextCursor::KeepAnchor);
            setTextCursor(cursor);
        }
    }
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
                                          int limit, bool visible = false) {
        if (!expression.isValid() || expression.pattern().isEmpty())
            return;
        int offset = 0;
        QString text;
        if (visible) {
            auto block = firstVisibleBlock();
            offset = block.position();
            const int last = cursorForPosition(viewport()->rect().bottomRight()).blockNumber() + 1;
            while (block.isValid() && block.blockNumber() <= last) {
                text += block.text() + '\n'; block = block.next();
            }
        } else text = toPlainText();
        auto matches = expression.globalMatch(text);
        int count = 0;
        while (matches.hasNext() && count++ < limit) {
            const auto match = matches.next();
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(color);
            selection.cursor = textCursor();
            selection.cursor.setPosition(offset + match.capturedStart());
            selection.cursor.setPosition(offset + match.capturedEnd(), QTextCursor::KeepAnchor);
            selections << selection;
        }
    };
    const auto selected = selectedIdentifier();
    if (!selected.isEmpty())
        addMatches(findExpression(selected, true, true, false),
                   QColor(light_ ? "#ffe082" : "#725f1d"), std::numeric_limits<int>::max(), true);
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

bool CodeEditor::event(QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        auto key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier) {
            emit modeSwitchRequested();
            return true;
        }
    }
    return QPlainTextEdit::event(event);
}

QJsonObject CodeEditor::locationAtCursor() const {
    const auto cursor = textCursor();
    const int position = cursor.selectionStart();
    const auto block = document()->findBlock(position);
    const SourceLocation *best = nullptr;
    for (const auto &location : locations_) {
        if (location.start > block.position() + block.length()) break;
        if (location.end <= block.position()) continue;
        if (!best || (location.start <= position && position < location.end)) best = &location;
    }
    if (!best) {
        QString scope;
        for (const auto &span : spans_) {
            if (span.start > position) break;
            if (span.declaration && span.id.contains('(') && !span.id.contains("@local:")) scope = span.id;
        }
        for (const auto &location : locations_) {
            if (scope.isEmpty() || location.scope != scope) continue;
            if (!best || qAbs(location.start - position) < qAbs(best->start - position)) best = &location;
        }
    }
    if (!best) return {};
    return {{"mappedLocation", true}, {"scope", best->scope}, {"offset", best->offset}};
}
