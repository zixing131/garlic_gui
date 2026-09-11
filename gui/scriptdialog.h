#pragma once
#include <QDialog>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QStringDecoder>
#include <memory>
class MainWindow;
class McpServer;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class ScriptDialog : public QDialog {
    Q_OBJECT
  public:
    explicit ScriptDialog(MainWindow *window, const QString &host = {});
    ~ScriptDialog() override;
    static QString interpreter(const QString &configured, bool python);
  protected:
    void closeEvent(QCloseEvent *event) override;
  private:
    void run();
    void stop();
    void append(const QString &text);
    void saveDraft();
    MainWindow *window_;
    QString host_;
    McpServer *server_;
    QComboBox *language_;
    QLineEdit *arguments_, *file_;
    QPlainTextEdit *code_, *output_;
    QPushButton *run_, *stop_;
    QString draftLanguage_ = "python";
    std::unique_ptr<QTemporaryDir> temporary_;
    QProcess process_;
    QTimer timeout_, killTimer_;
    QStringDecoder stdoutDecoder_{QStringDecoder::Utf8}, stderrDecoder_{QStringDecoder::Utf8};
};
