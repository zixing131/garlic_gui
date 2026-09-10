#include "mainwindow.h"
#include "nodeicons.h"
#include "resources.h"
#include <QtConcurrent>
#include <QtWidgets>

void MainWindow::refreshRecent() {
    recentMenu_->clear();
    for (const auto &p : QSettings().value("recentFiles").toStringList()) {
        auto a = recentMenu_->addAction(QFileInfo(p).fileName(), this, [this, p] { openPath(p); });
        a->setToolTip(p);
    }
    recentMenu_->addSeparator();
    recentMenu_->addAction(tr("清空历史记录"), this, [this] {
        QSettings().remove("recentFiles");
        refreshRecent();
    });
}
void MainWindow::closeTabs(int index, const QString &mode) {
    for (int i = tabs_->count() - 1; i >= 0; --i)
        if (mode == "all" || (mode == "one" && i == index) || (mode == "others" && i != index) ||
            (mode == "left" && i < index) || (mode == "right" && i > index))
            delete tabs_->widget(i);
}
void MainWindow::projectNodes() {
    const int generation = treeGeneration_;
    auto root = new QStandardItem(NodeIcons::icon("package"),
                                  backend_.inputs().size() > 1
                                      ? tr("项目（%1 个输入）").arg(backend_.inputs().size())
                                      : QFileInfo(backend_.input()).fileName());
    model_->appendRow(root);
    auto inputs = new QStandardItem(NodeIcons::icon("package"), tr("输入"));
    root->appendRow(inputs);
    auto files = new QStandardItem(NodeIcons::icon("package"), tr("文件"));
    inputs->appendRow(files);
    sourceRoot_ = new QStandardItem(NodeIcons::icon("package"), tr("源代码"));
    root->appendRow(sourceRoot_);
    resourceRoot_ = new QStandardItem(NodeIcons::icon("package"), tr("资源文件"));
    root->appendRow(resourceRoot_);
    for (const auto &path : backend_.inputs()) {
        auto item = new QStandardItem(style()->standardIcon(QStyle::SP_FileIcon),
                                      QFileInfo(path).fileName());
        item->setData("input", Qt::UserRole + 4);
        item->setData(path, Qt::UserRole + 5);
        item->setToolTip(path);
        files->appendRow(item);
        auto resourceParent = resourceRoot_;
        if (backend_.inputs().size() > 1) {
            resourceParent =
                new QStandardItem(NodeIcons::icon("package"), QFileInfo(path).fileName());
            resourceRoot_->appendRow(resourceParent);
        }
        auto signature = new QStandardItem(
            style()->standardIcon(QStyle::SP_FileDialogInfoView),
            tr("APK signature") +
                (backend_.inputs().size() > 1 ? " — " + QFileInfo(path).fileName() : ""));
        signature->setData("signature", Qt::UserRole + 4);
        signature->setData(path, Qt::UserRole + 5);
        if (QFileInfo(path).suffix().toLower() == "apk")
            root->appendRow(signature);
        else
            delete signature;
        auto summary = new QStandardItem(
            style()->standardIcon(QStyle::SP_FileDialogDetailedView),
            tr("总览") + (backend_.inputs().size() > 1 ? " — " + QFileInfo(path).fileName() : ""));
        summary->setData("summary", Qt::UserRole + 4);
        summary->setData(path, Qt::UserRole + 5);
        root->appendRow(summary);
        auto watcher = new QFutureWatcher<QJsonObject>(this);
        connect(watcher, &QFutureWatcher<QJsonObject>::finished, this,
                [this, watcher, path, resourceParent, generation] {
                    auto info = watcher->result();
                    watcher->deleteLater();
                    if (treeGeneration_ != generation)
                        return;
                    resourceInfo_[path] = info;
                    struct State {
                        QJsonArray entries;
                        int i = 0;
                        QHash<QString, QStandardItem *> dirs;
                    };
                    auto state = std::make_shared<State>();
                    state->entries = info.value("entries").toArray();
                    state->dirs[""] = resourceParent;
                    auto step = std::make_shared<std::function<void()>>();
                    std::weak_ptr<std::function<void()>> weak = step;
                    *step = [this, state, path, generation, weak] {
                        if (generation != treeGeneration_)
                            return;
                        QElapsedTimer time;
                        time.start();
                        while (state->i < state->entries.size() && time.elapsed() < 8) {
                            auto entry = state->entries[state->i++].toObject();
                            QString n = entry.value("name").toString();
                            auto parts = n.split('/');
                            QString dir;
                            auto parent = state->dirs[""];
                            for (int j = 0; j < parts.size() - 1; j++) {
                                dir += parts[j] + '/';
                                if (!state->dirs.contains(dir)) {
                                    auto child =
                                        new QStandardItem(NodeIcons::icon("package"), parts[j]);
                                    parent->appendRow(child);
                                    state->dirs[dir] = child;
                                }
                                parent = state->dirs[dir];
                            }
                            auto child = new QStandardItem(
                                style()->standardIcon(QStyle::SP_FileIcon), parts.last());
                            child->setData("resource", Qt::UserRole + 4);
                            child->setData(path, Qt::UserRole + 5);
                            child->setData(n, Qt::UserRole + 6);
                            child->setData(n, Qt::UserRole + 2);
                            child->setToolTip(
                                n + QString(" · %1 bytes").arg(entry.value("size").toDouble()));
                            parent->appendRow(child);
                        }
                        if (state->i < state->entries.size())
                            if (auto next = weak.lock())
                                QTimer::singleShot(0, this, [next] { (*next)(); });
                    };
                    (*step)();
                });
        watcher->setFuture(QtConcurrent::run([path] { return Resources::inspect(path); }));
    }
    tree_->expand(proxy_->mapFromSource(root->index()));
    tree_->expand(proxy_->mapFromSource(sourceRoot_->index()));
}
void MainWindow::openResource(const QString &path, const QString &entry) {
    const QString key = path + "!" + entry;
    for (int i = 0; i < tabs_->count(); i++)
        if (tabs_->widget(i)->property("resourceKey") == key) {
            tabs_->setCurrentIndex(i);
            return;
        }
    while (tabs_->count() >= backend_.settings().maxTabs)
        delete tabs_->widget(0);
    auto page = new QWidget;
    page->setProperty("resourceKey", key);
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto code = new CodeEditor(false);
    code->setTheme(backend_.settings().theme == "light");
    code->setPlainText(tr("正在加载资源…"));
    layout->addWidget(code);
    tabs_->setCurrentIndex(tabs_->addTab(page, style()->standardIcon(QStyle::SP_FileIcon),
                                         QFileInfo(entry.isEmpty() ? path : entry).fileName()));
    auto exportButton = new QPushButton(tr("导出资源…"));
    layout->addWidget(exportButton);
    connect(exportButton, &QPushButton::clicked, page, [this, path, entry] {
        auto target =
            QFileDialog::getSaveFileName(this, tr("导出资源"), QFileInfo(entry).fileName());
        if (target.isEmpty())
            return;
        auto task = new QFutureWatcher<QString>(this);
        connect(task, &QFutureWatcher<QString>::finished, this, [this, task] {
            status_->setText(task->result().isEmpty() ? tr("资源已导出") : task->result());
            task->deleteLater();
        });
        task->setFuture(QtConcurrent::run([path, entry, target] {
            QString error;
            auto data = Resources::read(path, entry, 512LL * 1024 * 1024, &error);
            if (!error.isEmpty())
                return error;
            QSaveFile f(target);
            if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
                return f.errorString();
            return QString();
        }));
    });
    struct Preview {
        QString text, error;
        QImage image;
    };
    auto task = new QFutureWatcher<Preview>(page);
    auto limit = backend_.settings().sourceMiB;
    connect(task, &QFutureWatcher<Preview>::finished, page, [task, code, layout] {
        auto result = task->result();
        task->deleteLater();
        if (!result.image.isNull()) {
            auto label = new QLabel;
            label->setPixmap(QPixmap::fromImage(result.image));
            auto scroll = new QScrollArea;
            scroll->setWidget(label);
            delete layout->replaceWidget(code, scroll);
            code->deleteLater();
        } else
            code->setSource({result.error.isEmpty() ? result.text : result.error, {}});
    });
    task->setFuture(QtConcurrent::run([path, entry, limit] {
        Preview result;
        auto b = Resources::read(path, entry, qint64(limit) * 1024 * 1024, &result.error);
        if (!result.error.isEmpty())
            return result;
        QBuffer buffer(&b);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        if (reader.canRead()) {
            const auto size = reader.size();
            if (!size.isValid() || qint64(size.width()) * size.height() > 32000000) {
                result.error = QObject::tr("图片超过预览大小限制，可导出后查看。");
                return result;
            }
            result.image = reader.read();
            if (!result.image.isNull())
                return result;
        }
        if (entry.endsWith(".xml", Qt::CaseInsensitive))
            result.text = Resources::decodeXml(b);
        else if (entry.endsWith(".arsc", Qt::CaseInsensitive))
            result.text = Resources::describeTable(b);
        else if (b.contains('\0'))
            result.text = Resources::hex(b);
        else
            result.text = QString::fromUtf8(b);
        return result;
    }));
}
void MainWindow::showOverview(const QString &path, bool signature) {
    const auto key = path + (signature ? "!signature" : "!overview");
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->widget(i)->property("resourceKey") == key) {
            tabs_->setCurrentIndex(i);
            return;
        }
    while (tabs_->count() >= backend_.settings().maxTabs)
        delete tabs_->widget(0);
    auto page = new QWidget;
    page->setProperty("resourceKey", key);
    auto layout = new QVBoxLayout(page);
    auto code = new CodeEditor(false);
    code->setTheme(backend_.settings().theme == "light");
    layout->addWidget(code);
    code->setPlainText(tr("正在读取…"));
    tabs_->setCurrentIndex(tabs_->addTab(page, signature ? tr("APK signature") : tr("总览")));
    auto task = new QFutureWatcher<QString>(page);
    connect(task, &QFutureWatcher<QString>::finished, page, [task, code] {
        code->setPlainText(task->result());
        task->deleteLater();
    });
    int classes = 0, methods = 0, fields = 0;
    QMap<QString, int> dex;
    for (const auto &n : backend_.project()->classes()) {
        auto c = backend_.project()->info(n);
        if (c.value("input").toString() != path)
            continue;
        classes++;
        methods += c.value("methods").toArray().size();
        fields += c.value("fields").toArray().size();
        dex[c.value("origin").toString()]++;
    }
    task->setFuture(QtConcurrent::run([path, signature, classes, methods, fields, dex] {
        if (signature)
            return Resources::signature(path);
        auto info = Resources::inspect(path);
        QString origins;
        for (auto it = dex.begin(); it != dex.end(); ++it)
            origins += QString("  %1: %2 classes\n").arg(it.key()).arg(it.value());
        return QString("文件: %1\n大小: %2 MiB\n包名: %3\n版本: %4\nApplication: %5\n\n类: "
                       "%6\n方法: %7\n字段: %8\n资源条目: %9\n\n输入来源:\n%10")
            .arg(path)
            .arg(info.value("bytes").toDouble() / 1048576., 0, 'f', 2)
            .arg(info.value("package").toString())
            .arg(info.value("version").toString())
            .arg(info.value("application").toString())
            .arg(classes)
            .arg(methods)
            .arg(fields)
            .arg(info.value("entries").toArray().size())
            .arg(origins);
    }));
}
void MainWindow::goManifest() {
    for (const auto &p : backend_.inputs())
        if (QFileInfo(p).suffix().toLower() == "apk") {
            openResource(p, "AndroidManifest.xml");
            return;
        }
    status_->setText(tr("当前输入没有 AndroidManifest.xml"));
}
void MainWindow::goApplication() {
    for (const auto &p : backend_.inputs()) {
        auto name = resourceInfo_.value(p).value("application").toString();
        if (!name.isEmpty()) {
            name.replace('.', '/');
            if (!backend_.project()->info(name).isEmpty()) {
                openClass(name);
                return;
            }
            status_->setText(tr("Application 类不在当前输入中：%1").arg(name));
            return;
        }
    }
    status_->setText(tr("Manifest 未声明自定义 Application，或资源目录仍在加载。"));
}
