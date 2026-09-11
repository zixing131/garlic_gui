#include "classview.h"
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTextBlock>
#include <QVBoxLayout>
ClassView::ClassView(const QString &name, bool hasSmali, const AppSettings &settings,
                     QWidget *parent)
    : QWidget(parent), name_(name) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    modes_ = new QTabWidget;
    modes_->setObjectName("codeModes");
    modes_->setTabPosition(QTabWidget::South);
    modes_->setDocumentMode(true);
    for (int i = 0; i < 2; i++) {
        auto code = new CodeEditor(i == 1);
        code->setPlainText(i ? tr("切换到 Smali 后按需加载。") : tr("正在加载 Java 源码…"));
        modes_->addTab(code, i ? "Smali" : tr("代码"));
        connect(code, &CodeEditor::modeSwitchRequested, this, [this, code] {
            if (!modes_->isTabEnabled(1) || !loaded(smali())) return;
            const int position = code->textCursor().selectionStart();
            QString scope, symbol;
            bool declaration = false;
            const auto spans = code->spans();
            for (const auto &span : spans) {
                if (span.start > position) break;
                if (span.declaration && !span.id.contains("@local:")) scope = span.id;
                if (position < span.end) { symbol = span.id; declaration = span.declaration; }
            }
            // Native methods have no body and users commonly press Tab while the cursor is on
            // the `native` modifier, before the declaration-name span.  Keep the declaration on
            // the same source line as the switching scope so it can be located in the other view.
            if (scope.isEmpty()) {
                const auto block = code->document()->findBlock(position);
                for (const auto &span : spans) {
                    if (!span.declaration || span.id.contains("@local:")) continue;
                    if (code->document()->findBlock(span.start) != block) continue;
                    scope = span.id;
                    break;
                }
            }
            int occurrence = 0;
            for (const auto &span : spans) {
                if (span.start >= position) break;
                if (span.declaration && span.id == scope) occurrence = 0;
                if (!span.declaration && span.id == symbol && span.end <= position) ++occurrence;
            }
            modePosition_ = {{"symbol", symbol}, {"scope", scope}, {"occurrence", occurrence}, {"declaration", declaration}};
            const auto location = code->locationAtCursor();
            if (!location.isEmpty() && (!declaration || symbol.contains("@local:"))) {
                modePosition_["mappedLocation"] = true;
                modePosition_["offset"] = location.value("offset");
                modePosition_["scope"] = location.value("scope");
            }
            selectMode(!smali());
            restoreModePosition();
            editor()->setFocus();
        });
    }
    modes_->setTabEnabled(1, hasSmali);
    layout->addWidget(modes_);
    applySettings(settings);
    connect(modes_, &QTabWidget::currentChanged, this, &ClassView::modeChanged);
}
bool ClassView::smali() const { return modes_->currentIndex() == 1; }
CodeEditor *ClassView::editor() const { return editor(smali()); }
CodeEditor *ClassView::editor(bool smali) const {
    return static_cast<CodeEditor *>(modes_->widget(smali ? 1 : 0));
}
void ClassView::setSource(bool smali, const SourceDocument &document) {
    editor(smali)->setSource(document);
    loaded_[smali ? 1 : 0] = true;
    if (this->smali() == smali) restoreModePosition();
}
void ClassView::applySettings(const AppSettings &settings) {
    for (int i = 0; i < 2; i++) {
        auto code = editor(i);
        code->setTheme(settings.theme == "light");
        code->setFont(settings.codeFont(i == 1));
        code->setLineWrapMode(settings.wordWrap ? QPlainTextEdit::WidgetWidth
                                                : QPlainTextEdit::NoWrap);
    }
}
void ClassView::selectMode(bool smali) { modes_->setCurrentIndex(smali ? 1 : 0); }

void ClassView::invalidate() {
    setProperty("sourceGeneration", property("sourceGeneration").toInt() + 1);
    for (int i=0; i<2; ++i) {
        loaded_[i] = false;
        raw_[i] = {};
        setProperty(i ? "loadingSmali" : "loadingJava", false);
        editor(i)->setSource({tr("源码缓存已清理。重新选择该标签或代码模式以加载。"), {}});
    }
}

void ClassView::restoreModePosition() {
    if (modePosition_.isEmpty() || !loaded(smali())) return;
    auto hit = modePosition_; modePosition_ = {};
    if (hit.value("mappedLocation").toBool()) {
        if (editor()->goToHit(hit)) return;
        hit.remove("mappedLocation");
    }
    const auto symbol = hit.value("symbol").toString();
    if (!symbol.isEmpty() && (hit.value("declaration").toBool()
            ? editor()->goToSymbol(symbol) : editor()->goToHit(hit))) return;
    editor()->goToSymbol(hit.value("scope").toString());
}
