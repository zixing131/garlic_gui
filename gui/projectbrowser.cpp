#include "classview.h"
#include "mainwindow.h"
#include "nodeicons.h"
#include "resources.h"
#include "hexviewer.h"
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
    resourceRoot_ = new QStandardItem(NodeIcons::icon("resresourcesRoot"), tr("资源文件"));
    root->appendRow(resourceRoot_);
    for (const auto &path : backend_.inputs()) {
        auto item = new QStandardItem(NodeIcons::resource(path), QFileInfo(path).fileName());
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
                                        new QStandardItem(NodeIcons::icon("resfolder"), parts[j]);
                                    parent->appendRow(child);
                                    state->dirs[dir] = child;
                                }
                                parent = state->dirs[dir];
                            }
                            auto child = new QStandardItem(NodeIcons::resource(n), parts.last());
                            const bool table = n.endsWith(".arsc", Qt::CaseInsensitive);
                            child->setData(table ? "resource-table" : "resource", Qt::UserRole + 4);
                            if (table)
                                child->appendRow(new QStandardItem(tr("展开解析资源表…")));
                            child->setData(entry.value("sourcePath").toString(path), Qt::UserRole + 5);
                            child->setData(entry.value("sourceEntry").toString(n), Qt::UserRole + 6);
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
        const auto canceled = backend_.project()->cancellationToken();
        watcher->setFuture(QtConcurrent::run([path, canceled] { return Resources::inspect(path, canceled); }));
    }
    tree_->expand(proxy_->mapFromSource(root->index()));
    tree_->expand(proxy_->mapFromSource(sourceRoot_->index()));
}
void MainWindow::openResource(const QString &path, const QString &entry, const QString &generated, int line, const QJsonObject &hit) {
    const QString key = path + "!" + entry + (generated.isEmpty() ? "" : "!" + generated);
    const bool decoded = !generated.isEmpty();
    const QString decodedText = decodedResources_.value(path + "!" + entry).value(generated);
    const QString displayEntry = decoded ? generated : entry;
    for (int i = 0; i < tabs_->count(); i++)
        if (tabs_->widget(i)->property("resourceKey") == key) {
            tabs_->widget(i)->setProperty("navigationHit", hit);
            tabs_->widget(i)->setProperty("navigationLine", line);
            tabs_->setCurrentIndex(i);
            if (auto code = tabs_->widget(i)->findChild<CodeEditor *>()) { if (hit.contains("sourceLine")) code->goToHit(hit); else if (line) code->goToLine(line); }
            syncEditor();
            return;
        }
    while (tabs_->count() >= backend_.settings().maxTabs)
        delete tabs_->widget(0);
    auto page = new QWidget;
    page->setProperty("resourceKey", key);
    page->setProperty("navigationHit", hit);
    page->setProperty("navigationLine", line);
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto code = new CodeEditor(false);
    code->setTheme(backend_.settings().theme == "light");
    code->document()->setProperty(
        "language", displayEntry.endsWith(".xml", Qt::CaseInsensitive) ? "xml" : "java");
    code->setPlainText(tr("正在加载资源…"));
    layout->addWidget(code);
    tabs_->setCurrentIndex(
        tabs_->addTab(page, NodeIcons::resource(displayEntry),
                      QFileInfo(displayEntry.isEmpty() ? path : displayEntry).fileName()));
    auto exportButton = new QPushButton(tr("导出资源…"));
    layout->addWidget(exportButton);
    connect(exportButton, &QPushButton::clicked, page,
            [this, path, entry, decoded, generated, displayEntry] {
                auto target = QFileDialog::getSaveFileName(this, tr("导出资源"),
                                                           QFileInfo(displayEntry).fileName());
                if (target.isEmpty())
                    return;
                auto task = new QFutureWatcher<QString>(this);
                connect(task, &QFutureWatcher<QString>::finished, this, [this, task] {
                    status_->setText(task->result().isEmpty() ? tr("资源已导出") : task->result());
                    task->deleteLater();
                });
                const auto exportText = decoded ? decodedResources_.value(path + "!" + entry).value(generated) : QString();
                task->setFuture(QtConcurrent::run([path, entry, target, decoded, exportText] {
                    QString error;
                    auto data = decoded ? exportText.toUtf8()
                                        : Resources::read(path, entry, 512LL * 1024 * 1024, &error);
                    if (!error.isEmpty())
                        return error;
                    QSaveFile f(target);
                    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() ||
                        !f.commit())
                        return f.errorString();
                    return QString();
                }));
            });
    struct Preview {
        QString text, error, binaryPath;
        QMap<QString, QString> files;
        QImage image;
    };
    auto task = new QFutureWatcher<Preview>(page);
    auto limit = backend_.settings().sourceMiB;
    const int hexKiB = backend_.settings().hexPreviewKiB;
    connect(task, &QFutureWatcher<Preview>::finished, page, [this, task, code, layout, hexKiB, path, entry, page] {
        auto result = task->result();
        task->deleteLater();
        if (!result.binaryPath.isEmpty()) {
            auto hex = new HexViewer(result.binaryPath, hexKiB);
            delete layout->replaceWidget(code, hex);
            code->deleteLater();
        } else if (!result.image.isNull()) {
            auto label = new QLabel;
            label->setPixmap(QPixmap::fromImage(result.image));
            auto scroll = new QScrollArea;
            scroll->setWidget(label);
            delete layout->replaceWidget(code, scroll);
            code->deleteLater();
        } else {
            if (!result.files.isEmpty()) decodedResources_[path + "!" + entry] = result.files;
            code->setSource({result.error.isEmpty() ? result.text : result.error, {}});
            const auto hit = page->property("navigationHit").toJsonObject();
            if (hit.contains("sourceLine")) code->goToHit(hit);
            else if (const int line = page->property("navigationLine").toInt()) code->goToLine(line);
            syncEditor();
        }
    });
    const auto canceled = backend_.project()->cancellationToken();
    task->setFuture(QtConcurrent::run([path, entry, limit, decoded, decodedText, canceled, generated] {
        Preview result;
        if (decoded) {
            QString text = decodedText;
            if (text.isEmpty()) {
                const auto bytes = Resources::read(path, entry, 128LL * 1048576, &result.error);
                if (result.error.isEmpty()) Resources::describeTable(bytes, &result.files, false, canceled);
                text = result.files.value(generated);
            }
            result.text = text.size() * 2LL > qint64(limit) * 1024 * 1024
                              ? QString("资源预览超过大小限制，请导出文件查看。")
                              : text;
            return result;
        }
        const auto local = Resources::materialize(path, entry, &result.error, canceled);
        QFile source(local);
        if (!source.open(QIODevice::ReadOnly)) { result.error = source.errorString(); return result; }
        const auto prefix = source.peek(65536);
        const bool structured = entry.endsWith(".xml", Qt::CaseInsensitive) || entry.endsWith(".arsc", Qt::CaseInsensitive);
        QImageReader imageProbe(local);
        if (!structured && !imageProbe.canRead() && (prefix.contains('\0') || source.size() > qint64(limit) * 1024 * 1024)) {
            result.binaryPath = local;
            return result;
        }
        auto b = Resources::read(local, {}, qint64(limit) * 1024 * 1024, &result.error);
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
            result.binaryPath = local;
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
    auto report = new QTextBrowser;
    report->setObjectName("overviewReport");
    report->setOpenExternalLinks(false);
    layout->addWidget(report);
    report->setPlainText(tr("正在读取…"));
    tabs_->setCurrentIndex(
        tabs_->addTab(page, NodeIcons::icon(signature ? "toolandroidManifest" : "resdetailView"),
                      signature ? tr("APK signature") : tr("总览")));
    auto task = new QFutureWatcher<QString>(page);
    connect(task, &QFutureWatcher<QString>::finished, page, [task, report] {
        QString html;
        const QSet<QString> headings{"输入",         "代码来源",          "Native 库",
                                     "统计",         "反编译状态",        "问题",
                                     "APK 签名信息", "警告 / v1 覆盖范围"};
        for (const auto &line : task->result().split('\n')) {
            auto escaped = line.toHtmlEscaped();
            if (headings.contains(line))
                html += "<h2>" + escaped + "</h2>";
            else if (line.startsWith("签名方案:") || line.startsWith("v1 签名条目:"))
                html += "<h3>" + escaped + "</h3>";
            else
                html += "<div style='white-space:pre-wrap;margin:3px 0'>" + escaped + "</div>";
        }
        report->setHtml("<body style='font-family:sans-serif'>" + html + "</body>");
        task->deleteLater();
    });
    int opened = 0;
    auto project = backend_.project()->snapshot();
    for (int i = 0; i < tabs_->count(); i++)
        if (auto v = qobject_cast<ClassView *>(tabs_->widget(i)))
            if (backend_.classInput(v->name()) == path)
                opened++;
    const auto cache = backend_.cachedSources();
    const auto work = backend_.workspacePath();
    const auto inputs = backend_.inputs();
    const auto errors = backend_.property("errorCount").toInt();
    const auto warnings = backend_.property("warningCount").toInt();
    const auto allReady = backend_.projectReady();
    task->setFuture(QtConcurrent::run([path, signature, project, opened, cache, work, inputs,
                                       errors, warnings, allReady] {
        if (signature)
            return Resources::signature(path);
        int classes = 0, methods = 0, fields = 0, topLevel = 0;
        qint64 units = 0;
        QStringList owners;
        QMap<QString, int> dex;
        for (const auto &n : project->classes()) {
            auto c = project->info(n);
            if (c.value("input").toString() != path)
                continue;
            classes++;
            if (!c.value("inner").toBool())
                topLevel++;
            units += qint64(c.value("instruction_units").toDouble());
            owners << project->owner(n);
            methods += c.value("methods").toArray().size();
            fields += c.value("fields").toArray().size();
            dex[c.value("origin").toString()]++;
        }
        owners.removeDuplicates();
        auto info = Resources::inspect(path, project->cancellationToken());
        QString origins;
        for (auto it = dex.begin(); it != dex.end(); ++it)
            origins += QString("  %1: %2 classes\n").arg(it.key()).arg(it.value());
        QMap<QString, QStringList> native;
        for (const auto &v : info.value("entries").toArray()) {
            auto name = v.toObject().value("name").toString();
            if (name.endsWith(".so"))
                native[name.section('/', 1, 1)] << name;
        }
        QString libs;
        int nativeCount = 0;
        for (auto it = native.begin(); it != native.end(); ++it) {
            nativeCount += it.value().size();
            libs += "  " + it.key() + ":\n    " + it.value().join("\n    ") + '\n';
        }
        int generated = 0;
        for (const auto &name : owners) {
            const auto cached = cache.value(name + ":java");
            if (allReady || cached.startsWith("memory:") || QFileInfo::exists(cached) ||
                QFileInfo::exists(Project::sourcePath(work + "/all-java", name, ".java")))
                generated++;
        }
        auto ratio = [owners](int n) { return owners.isEmpty() ? 0. : n * 100. / owners.size(); };
        return QString(
                   "输入\n文件列表:\n%1\n\n当前文件: %2\n大小: %3 MiB\n包名: %4\n版本: "
                   "%5\nApplication: %6\n\n代码来源\nCount: %7\n%8\nNative 库\nTotal count: "
                   "%9\n%10\n统计\n类: %11\n方法: %12\n字段: %13\n指令存储单元: %14（DEX: 16-bit "
                   "units；JVM: bytes）\n资源条目: %15\n\n反编译状态\n顶层类: "
                   "%16\n可生成源码的类单元: %17\n当前打开的类标签: %18\n已缓存源码: %19 "
                   "(%20%)\n尚未缓存: %21 (%22%)\n\n问题\n当前项目任务错误: %23\n引擎日志警告行数: "
                   "%24\n方法级错误数及成功率：引擎未提供，不能据此推算。\n")
            .arg(inputs.join("\n"))
            .arg(path)
            .arg(info.value("bytes").toDouble() / 1048576., 0, 'f', 2)
            .arg(info.value("package").toString())
            .arg(info.value("version").toString())
            .arg(info.value("application").toString().isEmpty()
                     ? QString("android.app.Application（Manifest 未声明自定义类）")
                     : info.value("application").toString())
            .arg(dex.size())
            .arg(origins)
            .arg(nativeCount)
            .arg(libs)
            .arg(classes)
            .arg(methods)
            .arg(fields)
            .arg(units)
            .arg(info.value("entries").toArray().size())
            .arg(topLevel)
            .arg(owners.size())
            .arg(opened)
            .arg(generated)
            .arg(ratio(generated), 0, 'f', 1)
            .arg(owners.size() - generated)
            .arg(ratio(owners.size() - generated), 0, 'f', 1)
            .arg(errors)
            .arg(warnings);
    }));
}
void MainWindow::goManifest() {
    for (const auto &p : backend_.inputs())
        for (const auto &v : resourceInfo_.value(p).value("entries").toArray()) {
            const auto entry = v.toObject();
            if (entry.value("name").toString() != "AndroidManifest.xml") continue;
            openResource(entry.value("sourcePath").toString(p),
                         entry.value("sourceEntry").toString("AndroidManifest.xml"));
            syncEditor();
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
    const auto candidates = backend_.project()->applicationCandidates();
    if (candidates.size() == 1) {
        openClass(candidates.first());
        status_->setText(
            tr("Manifest 未声明 Application；根据继承关系定位候选类（不代表运行时入口）。"));
    } else if (!candidates.isEmpty()) {
        bool ok = false;
        auto selected = QInputDialog::getItem(this, tr("Application 候选类"),
                                              tr("Manifest 未声明名称，以下类继承 Application："),
                                              candidates, 0, false, &ok);
        if (ok)
            openClass(selected);
    } else {
        auto dialog = new QMessageBox(
            QMessageBox::Information, tr("Application"),
            tr("Manifest 未声明自定义 Application，当前输入中也没有继承 Application "
               "的候选类。\nAndroid 将使用系统 android.app.Application；它不打包在 APK 中。"),
            QMessageBox::Ok, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    }
}

void MainWindow::expandResourceTable(const QModelIndex &index) {
    const auto source = proxy_->mapToSource(index);
    auto item = model_->itemFromIndex(source);
    if (!item || item->data(Qt::UserRole + 8).toBool())
        return;
    item->setData(true, Qt::UserRole + 8);
    item->child(0)->setText(tr("正在后台解析资源表…"));
    const auto path = index.data(Qt::UserRole + 5).toString(),
               entry = index.data(Qt::UserRole + 6).toString();
    const QPersistentModelIndex root(source);
    using Result = QPair<QMap<QString, QString>, QString>;
    auto task = new QFutureWatcher<Result>(this);
    connect(task, &QFutureWatcher<Result>::finished, this, [this, task, root, path, entry] {
        const auto result = task->result();
        task->deleteLater();
        if (!root.isValid())
            return;
        auto item = model_->itemFromIndex(root);
        item->removeRows(0, item->rowCount());
        if (!result.second.isEmpty()) {
            item->setData(false, Qt::UserRole + 8);
            item->appendRow(new QStandardItem(result.second));
            return;
        }
        decodedResources_[path + "!" + entry] = result.first;
        struct State {
            QStringList files;
            int i = 0;
            QHash<QString, QPersistentModelIndex> dirs;
        };
        auto state = std::make_shared<State>();
        state->files = result.first.keys();
        state->dirs[""] = root;
        auto step = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak = step;
        *step = [this, state, root, path, entry, weak] {
            if (!root.isValid())
                return;
            QElapsedTimer clock;
            clock.start();
            while (state->i < state->files.size() && clock.elapsed() < 6) {
                auto file = state->files[state->i++];
                auto parts = file.split('/');
                QString dir;
                auto parent = model_->itemFromIndex(root);
                for (int n = 0; n < parts.size() - 1; n++) {
                    dir += parts[n] + '/';
                    if (!state->dirs.contains(dir)) {
                        auto child = new QStandardItem(NodeIcons::icon("resfolder"), parts[n]);
                        parent->appendRow(child);
                        state->dirs[dir] = child->index();
                    }
                    parent = model_->itemFromIndex(state->dirs[dir]);
                }
                auto child = new QStandardItem(NodeIcons::resource(file), parts.last());
                child->setData("decoded-resource", Qt::UserRole + 4);
                child->setData(path, Qt::UserRole + 5);
                child->setData(entry, Qt::UserRole + 6);
                child->setData(file, Qt::UserRole + 7);
                child->setData(file, Qt::UserRole + 2);
                parent->appendRow(child);
            }
            if (state->i < state->files.size())
                if (auto next = weak.lock())
                    QTimer::singleShot(0, this, [next] { (*next)(); });
            if (state->i == state->files.size() && !pendingSyncResource_.isEmpty()) {
                auto key = pendingSyncResource_;
                pendingSyncResource_.clear();
                if (tabs_->currentWidget() &&
                    tabs_->currentWidget()->property("resourceKey") == key)
                    syncEditor();
            }
        };
        (*step)();
    });
    const auto cached = decodedResources_.value(path + "!" + entry);
    task->setFuture(QtConcurrent::run([path, entry, cached] {
        if (!cached.isEmpty()) return Result{cached, {}};
        QString error;
        auto bytes = Resources::read(path, entry, 128LL * 1024 * 1024, &error);
        QMap<QString, QString> files;
        if (error.isEmpty()) {
            const auto description = Resources::describeTable(bytes, &files);
            if (files.isEmpty())
                error = description;
        }
        return Result{files, error};
    }));
}

void MainWindow::goMainActivity() {
    QStringList names;
    for (const auto &path : backend_.inputs())
        for (const auto &value : resourceInfo_.value(path).value("main_activities").toArray()) {
            const auto name = value.toString();
            if (!names.contains(name))
                names << name;
        }
    if (names.isEmpty()) {
        status_->setText(tr("未找到声明 MAIN + LAUNCHER 的 Activity 入口。"));
        return;
    }
    QString name = names.first();
    if (names.size() > 1) {
        bool ok = false;
        name = QInputDialog::getItem(this, tr("主 Activity"), tr("选择启动入口："), names, 0, false,
                                     &ok);
        if (!ok)
            return;
    }
    name.replace('.', '/');
    if (backend_.project()->info(name).isEmpty()) {
        status_->setText(tr("入口类不在当前输入中：%1").arg(name));
        return;
    }
    openClass(name);
}
void MainWindow::syncEditor() {
    auto page = tabs_->currentWidget();
    if (!page)
        return;
    // Reveal even when an active filter hides the current editor's file.
    filter_->clear();
    filterTree({});
    const auto cls = qobject_cast<ClassView *>(page);
    QString member;
    if (cls && editor()) {
        // The tree follows the caret's enclosing member, not a reference under the caret.
        // For example, placing the caret on a call to foo() inside bar() must select bar().
        member = editor()->scopeSymbolAtCursor();
        if (member.contains("@local:"))
            member = member.section("@local:", 0, 0);
        if (!member.contains("->"))
            member.clear();
    }
    const auto key = page->property("resourceKey").toString();
    QModelIndex found, resourceTable;
    std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
        for (int row = 0; row < model_->rowCount(parent) && !found.isValid(); row++) {
            auto index = model_->index(row, 0, parent);
            const auto kind = index.data(Qt::UserRole + 4).toString();
            QString resourceKey = index.data(Qt::UserRole + 5).toString();
            if (kind == "resource" || kind == "resource-table" || kind == "decoded-resource") {
                resourceKey += "!" + index.data(Qt::UserRole + 6).toString();
                if (kind == "decoded-resource")
                    resourceKey += "!" + index.data(Qt::UserRole + 7).toString();
            } else if (kind == "summary")
                resourceKey += "!overview";
            else if (kind == "signature")
                resourceKey += "!signature";
            if (!cls && kind == "resource-table" && key.startsWith(resourceKey + "!"))
                resourceTable = index;
            if ((cls && index.data(Qt::UserRole + 1).toString() == Project::classId(cls->name())) ||
                (!cls && !key.isEmpty() && resourceKey == key))
                found = index;
            else
                visit(index);
        }
    };
    visit({});
    if (!found.isValid() && resourceTable.isValid()) {
        pendingSyncResource_ = key;
        auto visible = proxy_->mapFromSource(resourceTable);
        for (auto parent = visible.parent(); parent.isValid(); parent = parent.parent())
            tree_->expand(parent);
        tree_->expand(visible);
        tree_->scrollTo(visible);
        return;
    }
    if (!found.isValid()) {
        status_->setText(tr("当前文件的目录节点尚未加载。"));
        return;
    }
    pendingSyncResource_.clear();
    auto visible = proxy_->mapFromSource(found);
    for (auto parent = visible.parent(); parent.isValid(); parent = parent.parent())
        tree_->expand(parent);
    tree_->setCurrentIndex(visible);
    tree_->scrollTo(visible, QAbstractItemView::PositionAtCenter);
    if (member.isEmpty()) {
        pendingSyncMember_.clear();
        pendingSyncAttempts_ = 0;
        return;
    }
    pendingSyncMember_ = member;
    if (waitForMetadata([this] { syncEditor(); }))
        return;
    auto source = found;
    populateMembers(visible);
    std::function<QModelIndex(const QModelIndex &)> findMember = [&](const QModelIndex &parent) {
        for (int row = 0; row < model_->rowCount(parent); ++row) {
            const auto child = model_->index(row, 0, parent);
            if (child.data(Qt::UserRole + 1).toString() == pendingSyncMember_)
                return child;
        }
        return QModelIndex{};
    };
    const auto child = findMember(source);
    if (child.isValid()) {
        const auto memberVisible = proxy_->mapFromSource(child);
        tree_->expand(visible);
        tree_->setCurrentIndex(memberVisible);
        tree_->scrollTo(memberVisible, QAbstractItemView::PositionAtCenter);
        pendingSyncMember_.clear();
        pendingSyncAttempts_ = 0;
    } else if (++pendingSyncAttempts_ < 30) {
        QTimer::singleShot(10, this, &MainWindow::syncEditor);
    } else {
        pendingSyncMember_.clear();
        pendingSyncAttempts_ = 0;
    }
}
