#pragma once
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QHash>
#include <memory>

class Backend : public QObject {
    Q_OBJECT
public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;
    void setEngine(const QString &path) { engine_ = path; }
    QString engine() const { return engine_; }
    QString input() const { return input_; }
    bool busy() const { return job_ != Job::None; }
    bool supportsSmali() const;
    void open(const QString &path);
    void request(const QString &name, bool smali);
    void exportSources(const QString &directory, bool smali);
    void cancel();
    static bool safeClassName(const QString &name);
signals:
    void indexed(const QStringList &classes);
    void sourceReady(const QString &name, bool smali, const QString &path);
    void busyChanged(bool busy);
    void log(const QString &text);
    void failed(const QString &message);
    void exported(const QString &directory);
private:
    enum class Job { None, Index, Source, Export };
    void start(Job job, const QStringList &arguments);
    void finish(int code, QProcess::ExitStatus status);
    void readOutput();
    QProcess process_;
    QString engine_, input_, currentName_, jobDir_, exportDir_;
    QString errorTail_;
    std::unique_ptr<QTemporaryDir> workspace_;
    QHash<QString, QString> cache_;
    Job job_ = Job::None;
    bool smali_ = false;
    bool canceled_ = false;
};
