#include "classview.h"
#include <QSignalBlocker>
#include <QTabWidget>
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
}
void ClassView::applySettings(const AppSettings &settings) {
    for (int i = 0; i < 2; i++) {
        auto code = editor(i);
        code->setTheme(settings.theme == "light");
        auto font = code->font();
        font.setPointSize(settings.fontSize);
        code->setFont(font);
        code->setLineWrapMode(settings.wordWrap ? QPlainTextEdit::WidgetWidth
                                                : QPlainTextEdit::NoWrap);
    }
}
void ClassView::selectMode(bool smali) { modes_->setCurrentIndex(smali ? 1 : 0); }
