#pragma once
#include <QPlainTextEdit>

class CodeEditor : public QPlainTextEdit {
public:
    explicit CodeEditor(bool smali, QWidget *parent = nullptr);
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    QWidget *gutter_;
};
