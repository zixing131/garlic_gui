#pragma once

#include <QDialog>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QComboBox;

class NativeAnalysisDialog : public QDialog {
    Q_OBJECT
  public:
    NativeAnalysisDialog(const QString &engine, const QString &sourcePath,
                         const QString &archiveEntry = {}, QWidget *parent = nullptr);
    ~NativeAnalysisDialog() override;

  private:
    void prepare();
    void startAnalysis();
    void startGhidra();
    void loadResults(int code, QProcess::ExitStatus status);
    void findNext();
    QString engine_, sourcePath_, archiveEntry_, analysisPath_;
    std::shared_ptr<QTemporaryDir> workspace_;
    QProcess process_;
    QLabel *status_ = nullptr;
    QLineEdit *find_ = nullptr;
    QPushButton *export_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QComboBox *backend_ = nullptr;
    QPushButton *run_ = nullptr;
    bool ghidra_ = false;
    QByteArray log_;
};
