#include "mainwindow.h"
#include "mcpserver.h"
#include "nodeicons.h"
#include "searchdialog.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtWidgets>

MainWindow::MainWindow(const QString &engine, QWidget *parent)
    : QMainWindow(parent), backend_(this) {
    setWindowTitle("Garlic — 代码浏览器");
    setMinimumSize(880, 600);
    resize(1360, 860);
    setAcceptDrops(true);
    QSettings prefs;
    restoreGeometry(prefs.value("geometry").toByteArray());
    const auto saved = prefs.value("engine").toString();
    if (QFileInfo(saved).isExecutable())
        backend_.setEngine(saved);
    if (!engine.isEmpty())
        backend_.setEngine(QFileInfo(engine).absoluteFilePath());
    auto file = menuBar()->addMenu(tr("文件"));
    openAction_ =
        file->addAction(tr("打开文件…"), QKeySequence::Open, this, &MainWindow::chooseFile);
    file->addAction(tr("打开项目…"), this, &MainWindow::openProject);
    file->addAction(tr("保存项目…"), QKeySequence::Save, this, &MainWindow::saveProject);
    exportAction_ = file->addAction(tr("导出源码…"), QKeySequence("Ctrl+Shift+E"), this,
                                    &MainWindow::exportAll);
    engineAction_ = file->addAction(tr("选择引擎…"), this, [this] {
        auto path = QFileDialog::getOpenFileName(this, tr("选择 garlic 引擎"));
        if (!path.isEmpty()) {
            backend_.setEngine(path);
            QSettings().setValue("engine", path);
        }
    });
    settingsAction_ =
        file->addAction(tr("设置…"), QKeySequence::Preferences, this, &MainWindow::settingsDialog);
    file->addSeparator();
    file->addAction(tr("退出"), QKeySequence::Quit, this, &QWidget::close);
    auto edit = menuBar()->addMenu(tr("导航"));
    edit->addAction(tr("跳转到声明"), QKeySequence(Qt::Key_F12), this, [this] {
        if (editor())
            navigateTo(editor()->symbolAtCursor());
    });
    edit->addAction(tr("查找引用"), QKeySequence("Shift+F12"), this, [this] {
        if (editor())
            showReferences(editor()->symbolAtCursor());
    });
    edit->addAction(tr("重命名"), QKeySequence(Qt::Key_F2), this, [this] {
        if (editor())
            renameSymbol(editor()->symbolAtCursor());
    });
    edit->addAction(tr("撤销重命名"), QKeySequence::Undo, backend_.project(), &Project::undoRename);
    auto back = edit->addAction(tr("后退"), QKeySequence("Alt+Left"), this, [this] {
        if (historyIndex_ <= 0)
            return;
        --historyIndex_;
        restoringHistory_ = true;
        const auto at = history_[historyIndex_];
        navigateTo(at.first, at.second);
        restoringHistory_ = false;
    });
    auto forward = edit->addAction(tr("前进"), QKeySequence("Alt+Right"), this, [this] {
        if (historyIndex_ + 1 >= history_.size())
            return;
        ++historyIndex_;
        restoringHistory_ = true;
        const auto at = history_[historyIndex_];
        navigateTo(at.first, at.second);
        restoringHistory_ = false;
    });
    auto viewMenu = menuBar()->addMenu(tr("视图"));
    viewMenu->addAction(tr("查找当前代码"), QKeySequence::Find, this, [this] {
        find_->setFocus();
        find_->selectAll();
    });
    viewMenu->addAction(tr("项目搜索…"), QKeySequence("Ctrl+Shift+F"), this,
                        &MainWindow::searchDialog);
    viewMenu->addAction(tr("过滤类"), QKeySequence("Ctrl+L"), this, [this] {
        filter_->setFocus();
        filter_->selectAll();
    });
    viewMenu->addAction(tr("关闭标签"), QKeySequence::Close, this, [this] {
        if (view())
            delete view();
    });
    viewMenu->addAction(tr("放大代码"), QKeySequence::ZoomIn, this, [this] {
        if (editor())
            editor()->zoomIn();
    });
    viewMenu->addAction(tr("缩小代码"), QKeySequence::ZoomOut, this, [this] {
        if (editor())
            editor()->zoomOut();
    });
    auto toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    auto brand = new QLabel("  GARLIC  ");
    brand->setObjectName("brand");
    toolbar->addWidget(brand);
    toolbar->addSeparator();
    toolbar->addAction(openAction_);
    toolbar->addAction(exportAction_);
    toolbar->addAction(tr("项目搜索"), this, &MainWindow::searchDialog);
    toolbar->addAction(back);
    toolbar->addAction(forward);
    toolbar->addSeparator();
    stopAction_ = toolbar->addAction(tr("停止"), &backend_, &Backend::cancel);
    toolbar->addAction(settingsAction_);
    auto central = new QWidget;
    auto layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    auto fileRow = new QHBoxLayout;
    fileLabel_ = new QLabel(tr("打开 APK / DEX / JAR / CLASS"));
    fileLabel_->setTextFormat(Qt::PlainText);
    fileLabel_->setContentsMargins(12, 6, 0, 6);
    countLabel_ = new QLabel;
    countLabel_->setObjectName("muted");
    fileRow->addWidget(fileLabel_, 1);
    fileRow->addWidget(countLabel_);
    layout->addLayout(fileRow);
    auto splitter = new QSplitter;
    layout->addWidget(splitter, 1);
    auto left = new QWidget;
    auto leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(12, 6, 6, 6);
    filter_ = new QLineEdit;
    filter_->setObjectName("classFilter");
    filter_->setPlaceholderText(tr("过滤类、接口、枚举或成员…"));
    filter_->setClearButtonEnabled(true);
    leftLayout->addWidget(filter_);
    tree_ = new QTreeView;
    tree_->setIconSize(QSize(16, 16));
    tree_->setObjectName("classTree");
    tree_->setUniformRowHeights(true);
    connect(tree_, &QTreeView::expanded, this, &MainWindow::populateMembers);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    model_ = new QStandardItemModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setRecursiveFilteringEnabled(true);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterRole(Qt::UserRole + 2);
    tree_->setModel(proxy_);
    leftLayout->addWidget(tree_, 1);
    splitter->addWidget(left);
    auto right = new QWidget;
    auto rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 6, 6, 0);
    auto search = new QHBoxLayout;
    find_ = new QLineEdit;
    find_->setObjectName("codeFind");
    find_->setPlaceholderText(tr("查找当前代码…"));
    find_->setClearButtonEnabled(true);
    search->addWidget(find_, 1);
    auto previous = new QPushButton(tr("上一个")), next = new QPushButton(tr("下一个"));
    search->addWidget(previous);
    search->addWidget(next);
    rightLayout->addLayout(search);
    connect(previous, &QPushButton::clicked, this, [this] { find(true); });
    connect(next, &QPushButton::clicked, this, [this] { find(false); });
    connect(find_, &QLineEdit::returnPressed, this, [this] { find(false); });
    pages_ = new QStackedWidget;
    auto welcome = new QWidget;
    auto welcomeLayout = new QVBoxLayout(welcome);
    welcomeLayout->addStretch();
    auto title = new QLabel(tr("选择一个类，开始阅读。"));
    title->setObjectName("welcomeTitle");
    title->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(title);
    auto desc = new QLabel(tr("Java / Smali 底部切换 · F12 跳转 · Shift+F12 引用 · F2 重命名"));
    desc->setObjectName("muted");
    desc->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(desc);
    auto open = new QPushButton(tr("打开文件"));
    open->setObjectName("primary");
    open->setFixedWidth(160);
    welcomeLayout->addWidget(open, 0, Qt::AlignHCenter);
    welcomeLayout->addStretch();
    connect(open, &QPushButton::clicked, this, &MainWindow::chooseFile);
    tabs_ = new QTabWidget;
    tabs_->setIconSize(QSize(16, 16));
    tabs_->setObjectName("sourceTabs");
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    pages_->addWidget(welcome);
    pages_->addWidget(tabs_);
    rightLayout->addWidget(pages_, 1);
    splitter->addWidget(right);
    splitter->setSizes({310, 1050});
    splitter->setStretchFactor(1, 1);
    setCentralWidget(central);
    connect(tabs_, &QTabWidget::tabCloseRequested, this,
            [this](int i) { delete tabs_->widget(i); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        pages_->setCurrentIndex(i < 0 ? 0 : 1);
        if (i >= 0) {
            status_->setText(selectedClass());
            recordHistory();
        }
    });
    connect(filter_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    auto activate = [this](const QModelIndex &index) {
        const auto id = index.data(Qt::UserRole + 1).toString();
        if (!id.isEmpty())
            navigateTo(id);
    };
    connect(tree_, &QTreeView::clicked, this, activate);
    connect(tree_, &QTreeView::activated, this, activate);
    connect(tree_, &QTreeView::customContextMenuRequested, this, [this](const QPoint &pos) {
        const auto index = tree_->indexAt(pos);
        const auto id = index.data(Qt::UserRole + 1).toString();
        if (id.isEmpty())
            return;
        QMenu menu;
        menu.addAction(tr("打开声明"), this, [this, id] { navigateTo(id); });
        menu.addAction(tr("查找引用"), this, [this, id] { showReferences(id); });
        menu.addAction(tr("重命名…"), this, [this, id] { renameSymbol(id); });
        menu.exec(tree_->viewport()->mapToGlobal(pos));
    });
    logDock_ = new QDockWidget(tr("引擎日志"), this);
    logDock_->setObjectName("engineLogs");
    logs_ = new QPlainTextEdit;
    logs_->setReadOnly(true);
    logs_->setMaximumBlockCount(250);
    logDock_->setWidget(logs_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    logDock_->hide();
    viewMenu->addAction(logDock_->toggleViewAction());
    resultDock_ = new QDockWidget(tr("搜索 / 引用"), this);
    resultDock_->setObjectName("searchResults");
    results_ = new QTableWidget(0, 3);
    results_->setHorizontalHeaderLabels({tr("类 / 方法"), tr("位置"), tr("内容 / 引用目标")});
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_->horizontalHeader()->setStretchLastSection(true);
    results_->setColumnWidth(0, 350);
    resultDock_->setWidget(results_);
    addDockWidget(Qt::BottomDockWidgetArea, resultDock_);
    resultDock_->hide();
    viewMenu->addAction(resultDock_->toggleViewAction());
    connect(results_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto item = results_->item(row, 0);
        if (item)
            navigateTo(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toInt());
    });
    status_ = new QLabel(tr("就绪"));
    statusBar()->addWidget(status_, 1);
    progress_ = new QProgressBar;
    progress_->setMaximumWidth(140);
    progress_->setMaximumHeight(12);
    progress_->setTextVisible(false);
    statusBar()->addPermanentWidget(progress_);
    mcp_ = new McpServer(this, this);
    connect(&backend_, &Backend::indexed, this, [this](const QStringList &classes) {
        populate(classes);
        if (!pendingProject_.isEmpty()) {
            QString error;
            if (!backend_.project()->loadAliases(pendingProject_, &error))
                status_->setText(error);
            pendingProject_.clear();
        }
    });
    connect(&backend_, &Backend::sourceReady, this, &MainWindow::showSource);
    connect(&backend_, &Backend::busyChanged, this, &MainWindow::updateBusy);
    connect(&backend_, &Backend::preparationChanged, this, &MainWindow::updateBusy);
    connect(&backend_, &Backend::log, logs_, &QPlainTextEdit::appendPlainText);
    connect(&backend_, &Backend::failed, this, [this](const QString &text) {
        logs_->appendPlainText(text);
        logDock_->show();
        status_->setText(tr("操作失败，详情见日志"));
    });
    connect(&backend_, &Backend::projectSourcesReady, this,
            [this] { status_->setText(tr("项目源码已就绪，可以全文搜索。")); });
    connect(&backend_, &Backend::searchFinished, this,
            [this](const QJsonArray &hits, bool truncated) {
                results_->setRowCount(0);
                for (const auto &v : hits) {
                    const auto hit = v.toObject();
                    int row = results_->rowCount();
                    results_->insertRow(row);
                    auto item = new QTableWidgetItem(hit.value("class").toString());
                    item->setData(Qt::UserRole, Project::classId(item->text()));
                    item->setData(Qt::UserRole + 1, hit.value("line").toInt());
                    results_->setItem(row, 0, item);
                    results_->setItem(
                        row, 1, new QTableWidgetItem(QString::number(hit.value("line").toInt())));
                    results_->setItem(row, 2, new QTableWidgetItem(hit.value("text").toString()));
                }
                resultDock_->setWindowTitle(
                    tr("项目搜索：%1 条结果%2")
                        .arg(hits.size())
                        .arg(truncated ? tr("（达到 1000 条上限）") : QString()));
                resultDock_->show();
                status_->setText(tr("搜索完成，双击结果跳转。"));
            });
    connect(backend_.project(), &Project::renamed, this, &MainWindow::refreshAliases);
    connect(&backend_, &Backend::exported, this, [this](const QString &path) {
        // Export aliases with the source so edits are reviewable and recoverable.
        QString error;
        backend_.project()->save(QDir(path).filePath("project.garlic.json"), &error);
        status_->setText(tr("导出完成：%1").arg(path));
        QMessageBox::information(this, tr("导出完成"), path);
    });
    applySettings(backend_.settings());
    updateBusy();
}

ClassView *MainWindow::view() const { return qobject_cast<ClassView *>(tabs_->currentWidget()); }
CodeEditor *MainWindow::editor() const { return view() ? view()->editor() : nullptr; }
QString MainWindow::selectedClass() const { return view() ? view()->name() : QString(); }
QString MainWindow::mcpEndpoint() const { return mcp_->endpoint(); }
void MainWindow::chooseFile() {
    if (backend_.busy())
        return;
    const auto path = QFileDialog::getOpenFileName(
        this, tr("打开字节码"), QSettings().value("lastDirectory").toString(),
        tr("字节码 (*.apk *.xapk *.apks *.dex *.jar *.war *.class)"));
    if (!path.isEmpty())
        openPath(path);
}
void MainWindow::openPath(const QString &path) {
    if (backend_.busy())
        return;
    if (!QFileInfo(path).isFile()) {
        status_->setText(tr("文件不存在：%1").arg(path));
        return;
    }
    while (tabs_->count())
        delete tabs_->widget(0);
    if (searchDialog_) {
        delete searchDialog_;
        searchDialog_ = nullptr;
    }
    ++treeGeneration_;
    model_->clear();
    filter_->clear();
    results_->setRowCount(0);
    logs_->clear();
    history_.clear();
    historyIndex_ = -1;
    pendingId_.clear();
    pendingLine_ = 0;
    fileLabel_->setText(QFileInfo(path).fileName());
    fileLabel_->setToolTip(QFileInfo(path).absoluteFilePath());
    QSettings().setValue("lastDirectory", QFileInfo(path).absolutePath());
    status_->setText(tr("正在读取类与符号索引…"));
    backend_.open(path);
}
void MainWindow::populate(const QStringList &classes) {
    const int generation = ++treeGeneration_;
    model_->clear();
    struct State {
        QStringList names;
        int offset = 0;
        QHash<QString, QStandardItem *> packages, items;
    };
    auto state = std::make_shared<State>();
    state->names = classes;
    auto tick = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = tick;
    *tick = [this, state, generation, weak] {
        if (generation != treeGeneration_)
            return;
        QElapsedTimer time;
        time.start();
        while (state->offset < state->names.size() && time.elapsed() < 8) {
            const auto name = state->names[state->offset++];
            const QString pkg = name.contains('/')
                                    ? QString(name.section('/', 0, -2)).replace('/', '.')
                                    : tr("默认包");
            if (!state->packages.contains(pkg)) {
                auto p = new QStandardItem(NodeIcons::icon("package"), pkg);
                p->setData(pkg, Qt::UserRole + 2);
                model_->appendRow(p);
                state->packages[pkg] = p;
            }
            auto info = backend_.project()->info(name);
            auto item = new QStandardItem(
                NodeIcons::icon(info.value("kind").toString(), info.value("flags").toInt()),
                backend_.project()->displayName(name));
            item->setData(Project::classId(name), Qt::UserRole + 1);
            item->setData(QString(name).replace('/', '.') + " " + item->text(), Qt::UserRole + 2);
            item->setToolTip(name + " · " + info.value("kind").toString());
            state->items[name] = item;
            const QString parent = name.left(name.lastIndexOf('$'));
            if (info.value("inner").toBool() && state->items.contains(parent))
                state->items[parent]->appendRow(item);
            else
                state->packages[pkg]->appendRow(item);
            if (!info.value("methods").toArray().isEmpty() ||
                !info.value("fields").toArray().isEmpty()) {
                auto placeholder = new QStandardItem(tr("展开加载成员…"));
                placeholder->setData(true, Qt::UserRole + 3);
                item->appendRow(placeholder);
            }
        }
        if (state->offset < state->names.size()) {
            if (auto next = weak.lock())
                QTimer::singleShot(0, this, [next] { (*next)(); });
            return;
        }
        if (state->packages.size() <= 12)
            tree_->expandToDepth(0);
        countLabel_->setText(
            tr("%1 个包 / %2 个类与接口  ").arg(state->packages.size()).arg(state->names.size()));
        status_->setText(tr("目录就绪，展开类加载成员。"));
    };
    (*tick)();
}
void MainWindow::populateMembers(const QModelIndex &index) {
    auto item = model_->itemFromIndex(proxy_->mapToSource(index));
    if (!item)
        return;
    int placeholder = -1;
    for (int i = 0; i < item->rowCount(); i++)
        if (item->child(i)->data(Qt::UserRole + 3).toBool()) {
            placeholder = i;
            break;
        }
    if (placeholder < 0)
        return;
    item->removeRow(placeholder);
    const auto name = Project::classOf(item->data(Qt::UserRole + 1).toString());
    auto info = backend_.project()->info(name);
    for (const auto &kind : {QString("fields"), QString("methods")})
        for (const auto &v : info.value(kind).toArray()) {
            auto m = v.toObject();
            const auto id = m.value("id").toString();
            auto child = new QStandardItem(
                NodeIcons::icon(kind == "methods" ? "method" : "field", m.value("flags").toInt(),
                                m.value("name").toString() == "<init>"),
                backend_.project()->symbolName(id) + m.value("descriptor").toString());
            child->setData(id, Qt::UserRole + 1);
            child->setData(name + " " + child->text(), Qt::UserRole + 2);
            child->setToolTip(id);
            item->appendRow(child);
        }
}
void MainWindow::openClass(const QString &name, bool smali) {
    const auto n = Project::normalize(name);
    if (backend_.project()->info(n).isEmpty()) {
        status_->setText(tr("当前项目未包含：%1").arg(n));
        return;
    }
    for (int i = 0; i < tabs_->count(); i++) {
        auto current = qobject_cast<ClassView *>(tabs_->widget(i));
        if (current->name() == n) {
            tabs_->setCurrentIndex(i);
            current->selectMode(smali);
            loadCurrent();
            return;
        }
    }
    if (tabs_->count() >= backend_.settings().maxTabs)
        delete tabs_->widget(0);
    auto page = new ClassView(n, backend_.supportsSmali(), backend_.settings());
    connect(page, &ClassView::modeChanged, this, [this, page] {
        if (page == view())
            loadCurrent();
    });
    for (int i = 0; i < 2; i++) {
        auto code = page->editor(i);
        connect(code, &CodeEditor::navigateRequested, this,
                [this, code] { navigateTo(code->symbolAtCursor()); });
        connect(code, &CodeEditor::referencesRequested, this,
                [this, code] { showReferences(code->symbolAtCursor()); });
        connect(code, &CodeEditor::renameRequested, this,
                [this, code] { renameSymbol(code->symbolAtCursor()); });
    }
    const auto info = backend_.project()->info(n);
    int index = tabs_->addTab(
        page, NodeIcons::icon(info.value("kind").toString(), info.value("flags").toInt()),
        backend_.project()->displayName(n));
    tabs_->setTabToolTip(index, n);
    tabs_->setCurrentIndex(index);
    page->selectMode(smali);
    loadCurrent();
}
void MainWindow::loadCurrent() {
    auto page = view();
    if (!page)
        return;
    if (page->loaded(page->smali())) {
        if (!pendingId_.isEmpty()) {
            page->editor()->goToSymbol(pendingId_);
            pendingId_.clear();
        }
        if (pendingLine_) {
            page->editor()->goToLine(pendingLine_);
            pendingLine_ = 0;
        }
        return;
    }
    if (backend_.busy()) {
        status_->setText(tr("等待当前引擎请求完成…"));
        return;
    }
    if (page->property(page->smali() ? "loadingSmali" : "loadingJava").toBool())
        return;
    backend_.request(page->name(), page->smali());
}
void MainWindow::showSource(const QString &name, bool smali, const QString &path) {
    if (QFileInfo(path).size() > qint64(backend_.settings().sourceMiB) * 1024 * 1024) {
        status_->setText(tr("源码超过查看器大小限制，可在设置中调整或导出。"));
        return;
    }
    for (int i = 0; i < tabs_->count(); i++) {
        auto page = qobject_cast<ClassView *>(tabs_->widget(i));
        if (page->name() != name)
            continue;
        const auto key = smali ? "loadingSmali" : "loadingJava";
        if (page->property(key).toBool())
            return;
        page->setProperty(key, true);
        auto snapshot = backend_.project()->snapshot();
        auto watcher = new QFutureWatcher<SourceDocument>(this);
        QPointer<ClassView> target(page);
        connect(watcher, &QFutureWatcher<SourceDocument>::finished, this,
                [this, watcher, target, smali, key, name] {
                    const auto raw = watcher->result();
                    watcher->deleteLater();
                    if (!target)
                        return;
                    target->setProperty(key, false);
                    target->setRawDocument(smali, raw);
                    target->setSource(smali, present(raw, smali));
                    if (target == view())
                        status_->setText(
                            tr("%1 · %2 行").arg(name).arg(target->editor(smali)->blockCount()));
                    loadCurrent();
                });
        watcher->setFuture(QtConcurrent::run([snapshot, name, smali, path] {
            return snapshot->document(name, smali, path, false);
        }));

        return;
    }
    loadCurrent();
}
void MainWindow::find(bool backwards) {
    auto code = editor();
    if (!code || find_->text().isEmpty())
        return;
    const auto original = code->textCursor();
    auto flags = backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags();
    if (!code->find(find_->text(), flags)) {
        code->moveCursor(backwards ? QTextCursor::End : QTextCursor::Start);
        if (!code->find(find_->text(), flags)) {
            code->setTextCursor(original);
            status_->setText(tr("未找到：%1").arg(find_->text()));
        }
    }
}
void MainWindow::updateBusy() {
    const bool busy = backend_.busy();
    openAction_->setEnabled(!busy);
    engineAction_->setEnabled(!busy && !backend_.preparing());
    settingsAction_->setEnabled(!busy && !backend_.preparing());
    exportAction_->setEnabled(!busy && !backend_.input().isEmpty());
    stopAction_->setEnabled(busy || backend_.preparing());
    tree_->setEnabled(!busy);
    progress_->setRange(0, 0);
    progress_->setVisible(busy || backend_.preparing());
}
void MainWindow::recordHistory() {
    if (restoringHistory_ || !view())
        return;
    QPair<QString, int> at{Project::classId(selectedClass()),
                           editor() ? editor()->textCursor().blockNumber() + 1 : 1};
    if (historyIndex_ >= 0 && history_[historyIndex_].first == at.first)
        return;
    while (history_.size() > historyIndex_ + 1)
        history_.removeLast();
    history_.append(at);
    if (history_.size() > 100)
        history_.removeFirst();
    historyIndex_ = history_.size() - 1;
}
void MainWindow::navigateTo(const QString &id, int line) {
    if (id.isEmpty()) {
        status_->setText(tr("此位置没有可定位的符号。"));
        return;
    }
    pendingId_ = backend_.project()->canonicalId(id);
    pendingLine_ = line;
    openClass(Project::classOf(pendingId_));
}
void MainWindow::showReferences(const QString &id) {
    if (id.isEmpty())
        return;
    auto snapshot = backend_.project()->snapshot();
    const auto input = backend_.input();
    auto watcher = new QFutureWatcher<QJsonArray>(this);
    status_->setText(tr("正在查询引用…"));
    connect(watcher, &QFutureWatcher<QJsonArray>::finished, this, [this, watcher, id, input] {
        const auto refs = watcher->result();
        watcher->deleteLater();
        if (input != backend_.input())
            return;
        results_->setRowCount(0);
        for (const auto &v : refs) {
            if (results_->rowCount() >= 2000)
                break;
            auto r = v.toObject();
            int row = results_->rowCount();
            results_->insertRow(row);
            auto item = new QTableWidgetItem(r.value("from").toString());
            item->setData(Qt::UserRole, r.value("from"));
            results_->setItem(row, 0, item);
            results_->setItem(
                row, 1,
                new QTableWidgetItem(r.value("offset").toInt() < 0
                                         ? r.value("kind").toString()
                                         : tr("字节码 +%1").arg(r.value("offset").toInt())));
            results_->setItem(row, 2, new QTableWidgetItem(r.value("target").toString()));
        }
        resultDock_->setWindowTitle(
            tr("引用：%1（%2）").arg(backend_.project()->symbolName(id)).arg(refs.size()));
        resultDock_->show();
        if (refs.size() > 2000)
            status_->setText(
                tr("共 %1 处引用，界面显示前 2000 处；MCP 支持分页读取。").arg(refs.size()));
    });
    watcher->setFuture(QtConcurrent::run([snapshot, id] { return snapshot->xrefs(id); }));
}
void MainWindow::renameSymbol(const QString &id) {
    if (id.isEmpty())
        return;
    bool ok = false;
    const auto name =
        QInputDialog::getText(this, tr("重命名符号"), id + tr("\n新的项目名称（保留原始字节码）："),
                              QLineEdit::Normal, backend_.project()->symbolName(id), &ok);
    if (ok) {
        const auto error = backend_.project()->rename(id, name);
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("无法重命名"), error);
        else
            status_->setText(tr("已更新项目别名。保存项目可保留，下次打开时恢复。"));
    }
}
void MainWindow::refreshAliases() {
    populate(backend_.project()->classes());
    for (int i = 0; i < tabs_->count(); i++) {
        auto page = qobject_cast<ClassView *>(tabs_->widget(i));
        tabs_->setTabText(i, backend_.project()->displayName(page->name()));
        for (int mode = 0; mode < 2; mode++)
            if (page->loaded(mode)) {
                page->setSource(mode, present(page->rawDocument(mode), mode));
            }
    }
}
void MainWindow::saveProject() {
    if (backend_.input().isEmpty())
        return;
    const auto path = QFileDialog::getSaveFileName(
        this, tr("保存项目"), QFileInfo(backend_.input()).completeBaseName() + ".garlic.json",
        tr("Garlic 项目 (*.garlic.json)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!backend_.project()->save(path, &error))
        QMessageBox::warning(this, tr("保存失败"), error);
    else
        status_->setText(tr("项目已保存：%1").arg(path));
}
void MainWindow::openProject() {
    if (backend_.busy())
        return;
    auto path = QFileDialog::getOpenFileName(this, tr("打开项目"), {},
                                             tr("Garlic 项目 (*.garlic.json *.json)"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    auto j = QJsonDocument::fromJson(file.readAll()).object();
    if (j.value("version").toInt() != 1)
        return;
    pendingProject_ = path;
    openPath(j.value("input").toString());
}
void MainWindow::searchDialog() {
    if (backend_.project()->classes().isEmpty())
        return;
    if (!searchDialog_)
        searchDialog_ = new SearchDialog(this);
    searchDialog_->show();
    searchDialog_->raise();
    searchDialog_->activateWindow();
}
void MainWindow::exportAll() {
    auto parent = QFileDialog::getExistingDirectory(this, tr("导出到新目录"));
    if (parent.isEmpty())
        return;
    const auto folder = QFileInfo(backend_.input()).completeBaseName() + "-sources-" +
                        QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
    backend_.exportSources(QDir(parent).filePath(folder), view() && view()->smali());
}
void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (!backend_.busy() && event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty() &&
        event->mimeData()->urls().first().isLocalFile())
        event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->urls().isEmpty())
        openPath(event->mimeData()->urls().first().toLocalFile());
}
void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings().setValue("geometry", saveGeometry());
    backend_.cancel();
    event->accept();
}

SourceDocument MainWindow::present(const SourceDocument &raw, bool smali) const {
    auto doc = backend_.project()->applyAliases(raw, smali);
    if (smali)
        return doc;
    // Blank presentation-only annotations/notices without moving source spans.
    int offset = 0;
    for (const auto &line : doc.text.split('\n')) {
        bool hide =
            !backend_.settings().showMetadata && (line.trimmed().startsWith("@Metadata(") ||
                                                  line.trimmed().startsWith("@kotlin.Metadata("));
        if (!backend_.settings().showNotice && (line.startsWith(" *") || line == "/*" ||
                                                line == " */" || line.startsWith("// class:")))
            hide = true;
        if (hide)
            for (int j = 0; j < line.size(); j++)
                doc.text[offset + j] = ' ';
        offset += line.size() + 1;
    }
    return doc;
}
