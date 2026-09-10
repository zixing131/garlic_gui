#include "mainwindow.h"
#include "callgraphdialog.h"
#include "mcpserver.h"
#include "memoryusage.h"
#include "nativeanalysisdialog.h"
#include "nodeicons.h"
#include "referencesdialog.h"
#include "resources.h"
#include "searchdialog.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtWidgets>
#include <algorithm>

MainWindow::MainWindow(const QString &engine, QWidget *parent)
    : QMainWindow(parent), backend_(this) {
    setWindowTitle("Garlic — 代码浏览器");
    setMinimumSize(640, 440);
    resize(1360, 860);
    setAcceptDrops(true);
    QSettings prefs;
    flatPackages_ = prefs.value("view/flatPackages", true).toBool();
    restoreGeometry(prefs.value("geometry").toByteArray());
    if (!engine.isEmpty())
        backend_.setEngine(QFileInfo(engine).absoluteFilePath());
    auto file = menuBar()->addMenu(tr("文件"));
    openAction_ =
        file->addAction(tr("打开文件…"), QKeySequence::Open, this, &MainWindow::chooseFile);
    recentMenu_ = file->addMenu(tr("最近打开"));
    refreshRecent();
    file->addAction(tr("添加文件…"), this, [this] {
        auto paths = QFileDialog::getOpenFileNames(
            this, tr("添加输入文件"), {},
            tr("可分析文件 (*.apk *.dex *.jar *.war *.zip *.class *.xapk *.apks *.so *.dylib)"));
        if (!paths.isEmpty())
            openPaths(backend_.inputs() + paths);
    });
    file->addAction(tr("打开项目…"), this, &MainWindow::openProject);
    file->addAction(tr("保存项目…"), QKeySequence::Save, this, &MainWindow::saveProject);
    exportAction_ = file->addAction(tr("导出源码…"), QKeySequence("Ctrl+Shift+E"), this,
                                    &MainWindow::exportAll);
    settingsAction_ =
        file->addAction(tr("设置…"), QKeySequence::Preferences, this, &MainWindow::settingsDialog);
    file->addSeparator();
    file->addAction(tr("退出"), QKeySequence::Quit, this, &QWidget::close);
    auto edit = menuBar()->addMenu(tr("导航"));
    edit->addAction(tr("跳转到声明"), QKeySequence(Qt::Key_F12), this, [this] {
        if (editor())
            navigateTo(editor()->symbolAtCursor());
    });
    edit->addAction(tr("查找引用"), QKeySequence("X"), this, [this] {
        if (tree_->hasFocus())
            showReferences(tree_->currentIndex().data(Qt::UserRole + 1).toString());
        else if (editor())
            showReferences(editor()->symbolAtCursor());
    });
    auto callGraphAction =
        edit->addAction(NodeIcons::icon("methodReference"), tr("查看函数调用图"));
    callGraphAction->setShortcut(QKeySequence("G"));
    connect(callGraphAction, &QAction::triggered, this, [this] {
        const auto id = tree_->hasFocus()
                            ? tree_->currentIndex().data(Qt::UserRole + 1).toString()
                            : editor() ? editor()->symbolAtCursor() : QString();
        showCallGraph(id);
    });
    edit->addAction(tr("重命名"), QKeySequence("N"), this, [this] {
        if (tree_->hasFocus())
            renameSymbol(tree_->currentIndex().data(Qt::UserRole + 1).toString());
        else if (editor())
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
    auto showFindAction = viewMenu->addAction(tr("查找当前代码"), QKeySequence::Find, this,
                                               &MainWindow::showFindBar);
    showFindAction->setObjectName("showCodeFind");
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
    auto syncAction = viewMenu->addAction(tr("与编辑器同步"), this, &MainWindow::syncEditor);
    syncAction->setObjectName("syncEditor");
    auto flatAction = viewMenu->addAction(tr("展开显示代码包"));
    flatAction->setObjectName("flatPackages");
    flatAction->setCheckable(true);
    flatAction->setChecked(flatPackages_);
    connect(flatAction, &QAction::toggled, this, [this](bool flat) {
        flatPackages_ = flat;
        QSettings().setValue("view/flatPackages", flat);
        if (!backend_.project()->classes().isEmpty())
            populate(backend_.project()->classes());
    });
    auto toolbar = addToolBar("Main");
    toolbar->setObjectName("mainToolbar");
    toolbar->setMovable(false);
    toolbar->addAction(openAction_);
    toolbar->addAction(exportAction_);
    auto projectSearchAction =
        toolbar->addAction(tr("项目搜索"), this, &MainWindow::searchDialog);
    toolbar->addAction(callGraphAction);
    toolbar->addAction(back);
    toolbar->addAction(forward);
    toolbar->addSeparator();
    stopAction_ = toolbar->addAction(tr("停止"), &backend_, &Backend::cancel);
    toolbar->addAction(settingsAction_);
    auto applicationAction =
        edit->addAction(tr("前往 Application"), this, &MainWindow::goApplication);
    auto manifestAction =
        edit->addAction(tr("前往 AndroidManifest.xml"), this, &MainWindow::goManifest);
    toolbar->addAction(applicationAction);
    toolbar->addAction(manifestAction);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(20, 20));
    auto mainActivity = edit->addAction(tr("前往主 Activity"), this, &MainWindow::goMainActivity);
    mainActivity->setObjectName("mainActivity");
    toolbar->insertAction(applicationAction, mainActivity);
    toolbar->insertAction(back, syncAction);
    toolbar->insertAction(back, flatAction);
    openAction_->setObjectName("openFile");
    exportAction_->setObjectName("exportSources");
    projectSearchAction->setObjectName("projectSearch");
    callGraphAction->setObjectName("callGraph");
    back->setObjectName("navigateBack");
    forward->setObjectName("navigateForward");
    stopAction_->setObjectName("stopTask");
    settingsAction_->setObjectName("settings");
    applicationAction->setObjectName("goApplication");
    manifestAction->setObjectName("goManifest");
    const auto bindToolIcon = [](QAction *action, const QString &iconName) {
        action->setIcon(NodeIcons::icon(iconName));
        action->setToolTip(action->text());
    };
    bindToolIcon(openAction_, "toolopenDisk");
    bindToolIcon(exportAction_, "toolexport");
    bindToolIcon(projectSearchAction, "toolfind");
    bindToolIcon(callGraphAction, "methodReference");
    bindToolIcon(syncAction, "toolsync");
    bindToolIcon(flatAction, "toolpackages");
    bindToolIcon(back, "toolleft");
    bindToolIcon(forward, "toolright");
    bindToolIcon(stopAction_, "toolclose");
    bindToolIcon(settingsAction_, "toolsettings");
    bindToolIcon(mainActivity, "toolmainActivity");
    bindToolIcon(applicationAction, "toolapplication");
    bindToolIcon(manifestAction, "toolandroidManifest");
    auto central = new QWidget;
    auto layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    auto fileRow = new QHBoxLayout;
    fileLabel_ = new QLabel(tr("打开 APK / DEX / JAR / CLASS"));
    fileLabel_->setTextFormat(Qt::PlainText);
    fileLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    fileLabel_->setContentsMargins(8, 0, 0, 0);
    countLabel_ = new QLabel;
    countLabel_->setObjectName("muted");
    fileRow->addWidget(fileLabel_, 1);
    fileRow->addWidget(countLabel_);
    layout->addLayout(fileRow);
    auto splitter = new QSplitter;
    splitter->setObjectName("mainSplitter");
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);
    auto left = new QWidget;
    left->setMinimumWidth(160);
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
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex &i) {
        if (!filtering_)
            expandedNodes_.insert(proxy_->mapToSource(i));
    });
    connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex &i) {
        if (!filtering_)
            expandedNodes_.remove(proxy_->mapToSource(i));
    });
    leftLayout->addWidget(tree_, 1);
    splitter->addWidget(left);
    auto right = new QWidget;
    right->setMinimumWidth(300);
    auto rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 4, 6, 0);
    findBar_ = new QWidget;
    findBar_->setObjectName("codeFindBar");
    auto search = new QHBoxLayout(findBar_);
    search->setContentsMargins(8, 2, 0, 2);
    search->setSpacing(5);
    find_ = new QLineEdit;
    find_->setObjectName("codeFind");
    find_->setPlaceholderText(tr("查找当前代码…"));
    find_->setClearButtonEnabled(true);
    search->addWidget(find_, 1);
    findCount_ = new QLabel(tr("0 个结果"));
    findCount_->setObjectName("codeFindCount");
    findCount_->setMinimumWidth(64);
    findCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    search->addWidget(findCount_);
    auto findButton = [search](const QString &text, const QString &tip, const QString &name,
                               bool checkable = false) {
        auto button = new QToolButton;
        button->setText(text);
        button->setToolTip(tip);
        button->setObjectName(name);
        button->setCheckable(checkable);
        search->addWidget(button);
        return button;
    };
    findCase_ = findButton("Cc", tr("区分大小写"), "findCase", true);
    findWord_ = findButton("W", tr("全字匹配"), "findWord", true);
    findRegex_ = findButton(".*", tr("正则表达式"), "findRegex", true);
    auto previous = findButton("↑", tr("上一个匹配"), "findPrevious");
    auto next = findButton("↓", tr("下一个匹配"), "findNext");
    auto closeFind = findButton("×", tr("关闭查找栏（Esc）"), "closeCodeFind");
    findCase_->setChecked(prefs.value("find/caseSensitive", false).toBool());
    findWord_->setChecked(prefs.value("find/wholeWords", false).toBool());
    findRegex_->setChecked(prefs.value("find/regex", false).toBool());
    find_->setText(prefs.value("find/query").toString());
    findBar_->setVisible(prefs.value("view/findVisible", false).toBool());
    rightLayout->addWidget(findBar_);
    auto findTimer = new QTimer(findBar_);
    findTimer->setSingleShot(true);
    findTimer->setInterval(100);
    connect(findTimer, &QTimer::timeout, this, &MainWindow::refreshFindHighlights);
    connect(find_, &QLineEdit::textChanged, this, [findTimer](const QString &text) {
        QSettings().setValue("find/query", text);
        findTimer->start();
    });
    for (const auto &option : {findCase_, findWord_, findRegex_})
        connect(option, &QToolButton::toggled, this, [this, option] {
            const QString key = option == findCase_   ? "caseSensitive"
                                : option == findWord_ ? "wholeWords"
                                                      : "regex";
            QSettings().setValue("find/" + key, option->isChecked());
            refreshFindHighlights();
        });
    connect(previous, &QToolButton::clicked, this, [this] { find(true); });
    connect(next, &QToolButton::clicked, this, [this] { find(false); });
    connect(closeFind, &QToolButton::clicked, this, &MainWindow::hideFindBar);
    connect(find_, &QLineEdit::returnPressed, this, [this] { find(false); });
    auto closeFindShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), findBar_);
    connect(closeFindShortcut, &QShortcut::activated, this, &MainWindow::hideFindBar);
    pages_ = new QStackedWidget;
    auto welcome = new QWidget;
    auto welcomeLayout = new QVBoxLayout(welcome);
    welcomeLayout->addStretch();
    auto title = new QLabel(tr("选择一个类，开始阅读。"));
    title->setObjectName("welcomeTitle");
    title->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(title);
    auto desc = new QLabel(tr("Java / Smali 底部切换 · F12 / 双击跳转 · X 引用 · N 重命名"));
    desc->setObjectName("shortcutHint");
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
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    setCentralWidget(central);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint &p) {
        int i = tabs_->tabBar()->tabAt(p);
        if (i < 0)
            return;
        QMenu menu;
        const QList<QPair<QString, QString>> actions = {{tr("关闭"), "one"},
                                                        {tr("关闭其他"), "others"},
                                                        {tr("关闭左侧"), "left"},
                                                        {tr("关闭右侧"), "right"},
                                                        {tr("关闭全部"), "all"}};
        for (const auto &a : actions)
            menu.addAction(a.first, this, [this, i, a] { closeTabs(i, a.second); });
        menu.exec(tabs_->tabBar()->mapToGlobal(p));
    });
    for (auto action : edit->actions())
        if (action->text() == tr("查找引用") || action->text() == tr("重命名")) {
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            tree_->addAction(action);
            tabs_->addAction(action);
        }
    connect(tabs_, &QTabWidget::tabCloseRequested, this,
            [this](int i) { delete tabs_->widget(i); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        pages_->setCurrentIndex(i < 0 ? 0 : 1);
        if (i >= 0) {
            status_->setText(selectedClass());
            recordHistory();
            QTimer::singleShot(0, this, &MainWindow::loadCurrent);
        }
        refreshFindHighlights();
    });
    auto filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(180);
    connect(filter_, &QLineEdit::textChanged, this, [filterTimer] { filterTimer->start(); });
    connect(filterTimer, &QTimer::timeout, this, [this] { filterTree(filter_->text()); });
    auto activate = [this](const QModelIndex &index) {
        const auto kind = index.data(Qt::UserRole + 4).toString(),
                   path = index.data(Qt::UserRole + 5).toString();
        if (kind == "decoded-resource") {
            openResource(path, index.data(Qt::UserRole + 6).toString(),
                         index.data(Qt::UserRole + 7).toString());
            return;
        }
        if (kind == "resource" || kind == "resource-table") {
            const auto entry = index.data(Qt::UserRole + 6).toString();
            if (entry.endsWith(".so", Qt::CaseInsensitive) ||
                entry.endsWith(".dylib", Qt::CaseInsensitive))
                showNativeAnalysis(path, entry);
            else
                openResource(path, entry);
            return;
        }
        if (kind == "summary" || kind == "input" || kind == "signature") {
            showOverview(path, kind == "signature");
            return;
        }
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
        if (id.contains("->") && id.contains('(')) {
            auto graph = menu.addAction(NodeIcons::icon("methodReference"),
                                        tr("查看函数调用图  G"), this,
                                        [this, id] { showCallGraph(id); });
            graph->setShortcutVisibleInContextMenu(false);
        }
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
    memoryLabel_ = new QLabel;
    memoryLabel_->setObjectName("memoryUsage");
    statusBar()->addPermanentWidget(memoryLabel_);
    auto memoryTimer = new QTimer(this);
    connect(memoryTimer, &QTimer::timeout, this, [this] {
        if (!backend_.settings().showMemory)
            return;
        auto s = MemoryUsage::read(backend_.workerPids());
        peakMemory_ = qMax(peakMemory_, s.peak);
        memoryLabel_->setText(tr("内存 %1 MiB · 系统可用 %2 GiB · 峰值 %3 MiB")
                                  .arg(s.current / 1048576., 0, 'f', 0)
                                  .arg(s.available / 1073741824., 0, 'f', 1)
                                  .arg(peakMemory_ / 1048576., 0, 'f', 0));
        memoryLabel_->setToolTip(
            tr("当前占用含 GUI 与正在运行的 garlic 引擎；峰值为进程峰值与定时采样峰值的最大值。"));
    });
    memoryTimer->start(1000);
    status_ = new QLabel(tr("就绪"));
    statusBar()->addWidget(status_, 1);
    progress_ = new QProgressBar;
    progress_->setMaximumWidth(140);
    progress_->setMaximumHeight(12);
    progress_->setTextVisible(false);
    statusBar()->addPermanentWidget(progress_);
    for (auto action : findChildren<QAction *>()) {
        if (!action->shortcut().isEmpty())
            action->setProperty("defaultShortcut", action->shortcut().toString());
    }
    qApp->installEventFilter(this);
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
    connect(&backend_, &Backend::cacheCleared, this, [this] {
        for (int i = 0; i < tabs_->count(); ++i)
            if (auto page = qobject_cast<ClassView *>(tabs_->widget(i)))
                page->invalidate();
        status_->setText(tr("正在清理源码缓存…"));
    });
    connect(&backend_, &Backend::busyChanged, this, &MainWindow::updateBusy);
    connect(&backend_, &Backend::preparationChanged, this, &MainWindow::updateBusy);
    connect(&backend_, &Backend::log, logs_, &QPlainTextEdit::appendPlainText);
    connect(&backend_, &Backend::failed, this, [this](const QString &text) {
        logs_->appendPlainText(text);
        logDock_->show();
        status_->setText(tr("操作失败，详情见日志"));
        if (view() && !view()->loaded(view()->smali()))
            editor()->setPlainText(text);
    });
    connect(&backend_, &Backend::projectSourcesReady, this,
            [this] { status_->setText(tr("项目源码已就绪，可以全文搜索。")); });
    connect(backend_.project(), &Project::renamed, this, &MainWindow::refreshAliases);
    connect(&backend_, &Backend::exported, this, [this](const QString &path) {
        // Export aliases with the source so edits are reviewable and recoverable.
        QString error;
        backend_.project()->save(QDir(path).filePath("project.garlic.json"), &error);
        status_->setText(tr("导出完成：%1").arg(path));
        QMessageBox::information(this, tr("导出完成"), path);
    });
    applySettings(backend_.settings());
    restoreState(QSettings().value("windowState").toByteArray());
    updateBusy();
}

ClassView *MainWindow::view() const { return qobject_cast<ClassView *>(tabs_->currentWidget()); }
CodeEditor *MainWindow::editor() const {
    return view()                   ? view()->editor()
           : tabs_->currentWidget() ? tabs_->currentWidget()->findChild<CodeEditor *>()
                                    : nullptr;
}
QString MainWindow::selectedClass() const { return view() ? view()->name() : QString(); }
QString MainWindow::mcpEndpoint() const { return mcp_->endpoint(); }
void MainWindow::chooseFile() {
    if (backend_.busy())
        return;
    const auto paths = QFileDialog::getOpenFileNames(
        this, tr("打开字节码"), QSettings().value("lastDirectory").toString(),
        tr("可分析文件 (*.apk *.xapk *.apks *.dex *.jar *.war *.zip *.class *.so *.dylib)"));
    if (!paths.isEmpty())
        openPaths(paths);
}
void MainWindow::openPath(const QString &path) { openPaths({path}); }
void MainWindow::openPaths(const QStringList &paths) {
    if (paths.isEmpty())
        return;
    const auto path = paths.first();
    if (backend_.busy())
        return;
    for (const auto &input : paths) {
        if (!QFileInfo(input).isFile()) {
            status_->setText(tr("文件不存在：%1").arg(input));
            return;
        }
    }
    QStringList nativeInputs;
    for (const auto &input : paths)
        if (QStringList{"so", "dylib"}.contains(QFileInfo(input).suffix().toLower()))
            nativeInputs.append(input);
    if (!nativeInputs.isEmpty()) {
        for (const auto &input : nativeInputs)
            showNativeAnalysis(input);
        QStringList recent = QSettings().value("recentFiles").toStringList();
        for (const auto &input : nativeInputs) {
            recent.removeAll(input);
            recent.prepend(input);
        }
        while (recent.size() > 20)
            recent.removeLast();
        QSettings().setValue("recentFiles", recent);
        refreshRecent();
        if (nativeInputs.size() == paths.size())
            return;
        QStringList bytecodeInputs = paths;
        for (const auto &input : nativeInputs)
            bytecodeInputs.removeAll(input);
        openPaths(bytecodeInputs);
        return;
    }
    for (auto dialog : findChildren<ReferencesDialog *>())
        delete dialog;
    while (tabs_->count())
        delete tabs_->widget(0);
    if (searchDialog_) {
        delete searchDialog_;
        searchDialog_ = nullptr;
    }
    ++treeGeneration_;
    expandedNodes_.clear();
    model_->clear();
    filter_->clear();
    logs_->clear();
    history_.clear();
    historyIndex_ = -1;
    pendingId_.clear();
    pendingLine_ = 0;
    fileLabel_->setText(QFileInfo(path).fileName());
    fileLabel_->setToolTip(QFileInfo(path).absoluteFilePath());
    QSettings().setValue("lastDirectory", QFileInfo(path).absolutePath());
    status_->setText(tr("正在读取类与符号索引…"));
    resourceInfo_.clear();
    decodedResources_.clear();
    backend_.openPaths(paths);
    QStringList recent = QSettings().value("recentFiles").toStringList();
    for (const auto &p : paths) {
        recent.removeAll(p);
        recent.prepend(p);
    }
    while (recent.size() > 20)
        recent.removeLast();
    QSettings().setValue("recentFiles", recent);
    refreshRecent();
    fileLabel_->setText(
        paths.size() > 1
            ? tr("%1 等 %2 个输入文件").arg(QFileInfo(path).fileName()).arg(paths.size())
            : QFileInfo(path).fileName());
}
void MainWindow::populate(const QStringList &classes) {
    const int generation = ++treeGeneration_;
    expandedNodes_.clear();
    model_->clear();
    projectNodes();
    struct State {
        QStringList names;
        int offset = 0;
        QHash<QString, QStandardItem *> packages, items;
        QSet<QString> leafPackages;
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
            state->leafPackages.insert(pkg);
            if (!state->packages.contains(pkg)) {
                auto parent = sourceRoot_;
                QString full;
                const auto parts = flatPackages_ ? QStringList{pkg} : pkg.split('.');
                for (const auto &part : parts) {
                    full += (full.isEmpty() ? "" : ".") + part;
                    if (!state->packages.contains(full)) {
                        auto p = new QStandardItem(NodeIcons::icon("package"), part);
                        p->setData(full, Qt::UserRole + 2);
                        parent->appendRow(p);
                        state->packages[full] = p;
                    }
                    parent = state->packages[full];
                }
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
            tree_->expand(proxy_->mapFromSource(sourceRoot_->index()));
        countLabel_->setText(tr("%1 个包 / %2 个类与接口  ")
                                 .arg(state->leafPackages.size())
                                 .arg(state->names.size()));
        status_->setText(tr("目录就绪，展开类加载成员。"));
    };
    (*tick)();
}
void MainWindow::populateMembers(const QModelIndex &index) {
    if (index.data(Qt::UserRole + 4) == "resource-table") {
        expandResourceTable(index);
        return;
    }
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
    struct Members {
        QJsonArray entries;
        int cursor = 0;
    };
    auto state = std::make_shared<Members>();
    for (const auto &kind : {QString("fields"), QString("methods")})
        for (const auto &value : info.value(kind).toArray()) {
            auto member = value.toObject();
            member["nodeKind"] = kind == "methods" ? "method" : "field";
            state->entries.append(member);
        }
    const QPersistentModelIndex parent(item->index());
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = step;
    *step = [this, state, parent, name, weak] {
        if (!parent.isValid())
            return;
        auto item = model_->itemFromIndex(parent);
        QElapsedTimer clock;
        clock.start();
        while (state->cursor < state->entries.size() && clock.elapsed() < 6) {
            auto m = state->entries[state->cursor++].toObject();
            const auto id = m.value("id").toString();
            auto child = new QStandardItem(
                NodeIcons::icon(m.value("nodeKind").toString(), m.value("flags").toInt(),
                                m.value("name").toString() == "<init>"),
                backend_.project()->symbolName(id) + m.value("descriptor").toString());
            child->setData(id, Qt::UserRole + 1);
            child->setData(name + " " + child->text(), Qt::UserRole + 2);
            child->setToolTip(id);
            item->appendRow(child);
        }
        if (state->cursor < state->entries.size())
            if (auto next = weak.lock())
                QTimer::singleShot(0, this, [next] { (*next)(); });
    };
    (*step)();
}
void MainWindow::openClass(const QString &name, bool smali) {
    const auto n = Project::normalize(name);
    if (backend_.project()->info(n).isEmpty()) {
        status_->setText(tr("当前项目未包含：%1").arg(n));
        return;
    }
    for (int i = 0; i < tabs_->count(); i++) {
        auto current = qobject_cast<ClassView *>(tabs_->widget(i));
        if (current && current->name() == n) {
            tabs_->setCurrentIndex(i);
            current->selectMode(smali);
            loadCurrent();
            return;
        }
    }
    if (tabs_->count() >= backend_.settings().maxTabs)
        delete tabs_->widget(0);
    auto page = new ClassView(n, backend_.supportsSmali(n), backend_.settings());
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
        connect(code, &CodeEditor::callGraphRequested, this,
                [this, code] { showCallGraph(code->symbolAtCursor()); });
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
        if (!page)
            continue;
        if (page->name() != name)
            continue;
        const auto key = smali ? "loadingSmali" : "loadingJava";
        if (page->property(key).toBool())
            return;
        page->setProperty(key, true);
        auto snapshot = backend_.project()->snapshot();
        auto watcher = new QFutureWatcher<SourceDocument>(this);
        QPointer<ClassView> target(page);
        const int sourceGeneration = page->property("sourceGeneration").toInt();
        connect(watcher, &QFutureWatcher<SourceDocument>::finished, this,
                [this, watcher, target, smali, key, name, sourceGeneration] {
                    const auto raw = watcher->result();
                    watcher->deleteLater();
                    if (!target || target->property("sourceGeneration").toInt() != sourceGeneration)
                        return;
                    target->setProperty(key, false);
                    target->setRawDocument(smali, raw);
                    target->setSource(smali, present(raw, smali));
                    if (target == view())
                        status_->setText(
                            tr("%1 · %2 行").arg(name).arg(target->editor(smali)->blockCount()));
                    if (target == view())
                        refreshFindHighlights();
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
    if (!code->findText(find_->text(), findCase_->isChecked(), findWord_->isChecked(),
                        findRegex_->isChecked(), backwards))
        status_->setText(tr("未找到：%1").arg(find_->text()));
}
void MainWindow::showFindBar() {
    if (!findBar_)
        return;
    findBar_->show();
    QSettings().setValue("view/findVisible", true);
    if (find_->text().isEmpty() && editor() && editor()->textCursor().hasSelection())
        find_->setText(editor()->textCursor().selectedText());
    find_->setFocus();
    find_->selectAll();
    refreshFindHighlights();
}
void MainWindow::hideFindBar() {
    if (!findBar_)
        return;
    findBar_->hide();
    QSettings().setValue("view/findVisible", false);
    if (editor())
        editor()->clearFindHighlights();
}
void MainWindow::refreshFindHighlights() {
    auto code = editor();
    if (!code || !findBar_ || !findBar_->isVisible())
        return;
    const int matches =
        code->setFindHighlights(find_->text(), findCase_->isChecked(), findWord_->isChecked(),
                                findRegex_->isChecked());
    findCount_->setText(matches < 0 ? tr("表达式无效")
                                    : tr("%1 个结果%2")
                                          .arg(matches)
                                          .arg(matches >= 10000 ? "+" : QString()));
}
void MainWindow::updateBusy() {
    const bool busy = backend_.busy();
    openAction_->setEnabled(!busy);
    settingsAction_->setEnabled(!busy && !backend_.preparing());
    exportAction_->setEnabled(!busy && !backend_.input().isEmpty());
    stopAction_->setEnabled(busy || backend_.preparing());
    tree_->setEnabled(true);
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
    auto dialog = new ReferencesDialog(this, id);
    dialog->show();
}
void MainWindow::showCallGraph(const QString &id) {
    const auto method = backend_.project()->canonicalId(id);
    if (!method.contains("->") || !method.contains('(')) {
        status_->setText(tr("请先选择一个方法，再查看函数调用图。"));
        return;
    }
    auto dialog = new CallGraphDialog(backend_.project()->snapshot(), method, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &CallGraphDialog::navigationRequested, this,
            [this](const QString &target) { navigateTo(target); });
    dialog->show();
}
void MainWindow::showNativeAnalysis(const QString &path, const QString &entry) {
    auto dialog = new NativeAnalysisDialog(backend_.engine(), path, entry, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
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
        if (!page)
            continue;
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
    QStringList inputs;
    for (const auto &item : j.value("inputs").toArray())
        inputs << item.toObject().value("path").toString();
    if (inputs.isEmpty())
        inputs << j.value("input").toString();
    openPaths(inputs);
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
    if (!event->mimeData()->urls().isEmpty()) {
        QStringList paths;
        for (const auto &u : event->mimeData()->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
        openPaths(paths);
    }
}
void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings().setValue("geometry", saveGeometry());
    QSettings().setValue("windowState", saveState());
    backend_.cancel();
    event->accept();
}

SourceDocument MainWindow::present(const SourceDocument &raw, bool smali) const {
    auto doc = backend_.project()->applyAliases(raw, smali);
    if (smali)
        return doc;
    // garlic keeps source text close to bytecode and may omit Java's presentation-only
    // @Override annotation. Restore it when the indexed parent/interface declares the same method.
    struct OverrideNote {
        int position;
        QString text;
    };
    QVector<OverrideNote> notes;
    for (const auto &span : doc.spans) {
        if (!span.declaration || !span.id.contains("->"))
            continue;
        const auto annotation = backend_.project()->overrideAnnotation(span.id);
        if (annotation.isEmpty())
            continue;
        const int lineStart = doc.text.lastIndexOf('\n', span.start) + 1;
        const int previousStart = doc.text.lastIndexOf('\n', qMax(0, lineStart - 2)) + 1;
        if (doc.text.mid(previousStart, lineStart - previousStart).contains("@Override"))
            continue;
        const auto indentation =
            QRegularExpression("^\\s*").match(doc.text.mid(lineStart)).captured();
        notes << OverrideNote{lineStart, indentation + annotation + '\n'};
    }
    std::sort(notes.begin(), notes.end(),
              [](const OverrideNote &a, const OverrideNote &b) { return a.position > b.position; });
    for (const auto &note : notes) {
        doc.text.insert(note.position, note.text);
        for (auto &span : doc.spans)
            if (span.start >= note.position) {
                span.start += note.text.size();
                span.end += note.text.size();
            }
    }
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
    if (backend_.settings().showNotice) {
        const auto classComment = QRegularExpression("// class:\\s*(\\S+)").match(doc.text);
        const auto name = classComment.hasMatch() ? classComment.captured(1) : selectedClass();
        auto info = backend_.project()->info(name);
        int comment = doc.text.indexOf("// class:");
        int end = comment < 0 ? -1 : doc.text.indexOf('\n', comment);
        if (!info.isEmpty() && end >= 0) {
            const auto file = QFileInfo(info.value("input").toString()).fileName();
            const auto source = info.value("origin").toString();
            QString origin =
                " · loaded from: " + file + (source == file ? QString() : " / " + source);
            doc.text.insert(end, origin);
            for (auto &span : doc.spans)
                if (span.start >= end) {
                    span.start += origin.size();
                    span.end += origin.size();
                }
        }
    }
    return doc;
}

bool MainWindow::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        auto widget = qobject_cast<QWidget *>(object);
        auto mouse = static_cast<QMouseEvent *>(event);
        if (widget && widget->window() == this &&
            (mouse->button() == Qt::BackButton || mouse->button() == Qt::ForwardButton)) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto action = findChild<QAction *>(
                    mouse->button() == Qt::BackButton ? "navigateBack" : "navigateForward");
                if (action)
                    action->trigger();
            }
            return true;
        }
    }

    auto focused = QApplication::focusWidget();
    const bool codeFocus = qobject_cast<CodeEditor *>(focused) != nullptr;
    if ((focused == tree_ || codeFocus) && event->type() == QEvent::KeyPress) {
        auto key = static_cast<QKeyEvent *>(event);
        for (auto action : findChildren<QAction *>()) {
            if (action->text() != tr("查找引用") && action->text() != tr("重命名"))
                continue;
            if (action->shortcut() == QKeySequence(key->keyCombination())) {
                action->trigger();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(object, event);
}
void MainWindow::filterTree(const QString &text) {
    auto current = QPersistentModelIndex(proxy_->mapToSource(tree_->currentIndex()));
    QList<QPersistentModelIndex> previous;
    for (auto at = tree_->currentIndex(); at.isValid(); at = tree_->indexAbove(at))
        previous.append(proxy_->mapToSource(at));
    filtering_ = true;
    proxy_->setFilterFixedString(text);
    for (const auto &source : expandedNodes_)
        if (source.isValid()) {
            auto mapped = proxy_->mapFromSource(source);
            if (mapped.isValid())
                tree_->expand(mapped);
        }
    auto selected = proxy_->mapFromSource(current);
    if (!selected.isValid()) {
        for (const auto &source : previous) {
            selected = proxy_->mapFromSource(source);
            if (selected.isValid())
                break;
        }
    }
    if (!selected.isValid())
        selected = proxy_->index(0, 0);
    tree_->setCurrentIndex(selected);
    filtering_ = false;
}
