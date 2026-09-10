#include "mainwindow.h"
#include "codeeditor.h"
#include <QtWidgets>

MainWindow::MainWindow(const QString &engine, QWidget *parent) : QMainWindow(parent), backend_(this) {
    setWindowTitle("Garlic — 代码浏览器");
    setMinimumSize(850, 580); resize(1280, 820); setAcceptDrops(true);
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    const auto savedEngine = settings.value("engine").toString();
    if (!savedEngine.isEmpty() && QFileInfo(savedEngine).isExecutable()) backend_.setEngine(savedEngine);
    if (!engine.isEmpty()) backend_.setEngine(QFileInfo(engine).absoluteFilePath());

    auto fileMenu = menuBar()->addMenu(tr("文件"));
    openAction_ = fileMenu->addAction(tr("打开文件…"), QKeySequence::Open, this, &MainWindow::chooseFile);
    exportAction_ = fileMenu->addAction(tr("导出全部源码…"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::exportAll);
    engineAction_ = fileMenu->addAction(tr("选择引擎…"), this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("选择本项目编译的 garlic 可执行文件"), QFileInfo(backend_.engine()).absolutePath());
        if (!path.isEmpty()) {
            backend_.setEngine(path); QSettings().setValue("engine", path);
            status_->setText(tr("引擎：%1").arg(path));
        }
    });
    fileMenu->addSeparator();
    fileMenu->addAction(tr("退出"), QKeySequence::Quit, this, &QWidget::close);
    auto viewMenu = menuBar()->addMenu(tr("视图"));
    viewMenu->addAction(tr("查找当前代码"), QKeySequence::Find, this, [this] { find_->setFocus(); find_->selectAll(); });
    viewMenu->addAction(tr("查找类"), QKeySequence("Ctrl+L"), this, [this] { filter_->setFocus(); filter_->selectAll(); });
    viewMenu->addAction(tr("关闭标签"), QKeySequence::Close, this, [this] { if (tabs_->count()) delete tabs_->currentWidget(); });
    viewMenu->addAction(tr("放大代码"), QKeySequence::ZoomIn, this, [this] { if (editor()) editor()->zoomIn(); });
    viewMenu->addAction(tr("缩小代码"), QKeySequence::ZoomOut, this, [this] { if (editor()) editor()->zoomOut(); });
    auto help = menuBar()->addMenu(tr("帮助"));
    help->addAction(tr("关于 Garlic GUI"), this, [this] {
        QMessageBox::about(this, "Garlic GUI", tr("Garlic GUI 0.1 · C++ / Qt 6\n\n按需反编译 Java / Smali，源码缓存在临时目录。\n类树展示顶层类，内部类随外部类显示。\n每次未缓存的请求会重新解析输入文件。\n查找支持类名和当前文档；尚未提供全项目语义索引。\n\nGarlic: Apache-2.0 · Qt: LGPL-3.0 / GPL / commercial"));
    });

    auto toolbar = addToolBar("Main"); toolbar->setMovable(false); toolbar->setIconSize({18,18});
    auto brand = new QLabel("  GARLIC  "); brand->setObjectName("brand"); toolbar->addWidget(brand);
    toolbar->addSeparator(); toolbar->addAction(openAction_); toolbar->addAction(exportAction_);
    toolbar->addSeparator();
    stopAction_ = toolbar->addAction(tr("停止"), &backend_, &Backend::cancel);
    auto spacer = new QWidget; spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); toolbar->addWidget(spacer);
    auto hint = new QLabel(tr("按需反编译  ·  本地运行  ")); hint->setObjectName("muted"); toolbar->addWidget(hint);

    auto central = new QWidget; auto layout = new QVBoxLayout(central); layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
    auto fileBar = new QWidget; fileBar->setObjectName("fileBar"); auto fileLayout = new QHBoxLayout(fileBar);
    fileLabel_ = new QLabel(tr("尚未打开文件")); fileLabel_->setTextFormat(Qt::PlainText); fileLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    countLabel_ = new QLabel; countLabel_->setObjectName("muted");
    fileLayout->addWidget(fileLabel_,1); fileLayout->addWidget(countLabel_); layout->addWidget(fileBar);
    auto splitter = new QSplitter; layout->addWidget(splitter,1);
    auto sidebar = new QWidget; auto side = new QVBoxLayout(sidebar); side->setContentsMargins(14,16,10,10);
    auto title = new QLabel(tr("源码目录")); title->setObjectName("sectionTitle"); side->addWidget(title);
    filter_ = new QLineEdit; filter_->setObjectName("classFilter"); filter_->setPlaceholderText(tr("过滤类名或包名…")); filter_->setClearButtonEnabled(true); side->addWidget(filter_);
    tree_ = new QTreeView; tree_->setObjectName("classTree"); tree_->setHeaderHidden(true); tree_->setUniformRowHeights(true); tree_->setAnimated(false); tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    model_ = new QStandardItemModel(this); proxy_ = new QSortFilterProxyModel(this); proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive); proxy_->setRecursiveFilteringEnabled(true); proxy_->setFilterRole(Qt::UserRole + 2); tree_->setModel(proxy_);
    side->addWidget(tree_,1); auto treeHint = new QLabel(tr("单击类以打开  ·  内部类包含于 Java 源码")); treeHint->setWordWrap(true); treeHint->setObjectName("muted"); side->addWidget(treeHint);
    splitter->addWidget(sidebar);
    auto content = new QWidget; auto contentLayout = new QVBoxLayout(content); contentLayout->setContentsMargins(0,10,10,0);
    auto searchRow = new QHBoxLayout;
    language_ = new QComboBox; language_->setObjectName("sourceLanguage"); language_->addItems({"Java", "Smali"}); language_->setMinimumWidth(100);
    searchRow->addWidget(language_); searchRow->addSpacing(12);
    find_ = new QLineEdit; find_->setObjectName("codeFind"); find_->setPlaceholderText(tr("查找当前代码…")); find_->setClearButtonEnabled(true); searchRow->addWidget(find_,1);
    auto previous = new QPushButton(tr("上一个")); auto next = new QPushButton(tr("下一个")); searchRow->addWidget(previous); searchRow->addWidget(next); contentLayout->addLayout(searchRow);
    connect(previous, &QPushButton::clicked, this, [this] { find(true); });
    connect(next, &QPushButton::clicked, this, [this] { find(false); });
    connect(find_, &QLineEdit::returnPressed, this, [this] { find(false); });
    pages_ = new QStackedWidget;
    auto welcome = new QWidget; auto welcomeLayout = new QVBoxLayout(welcome); welcomeLayout->addStretch();
    auto welcomeTitle = new QLabel(tr("从一个类开始，读懂整个应用。")); welcomeTitle->setObjectName("welcomeTitle"); welcomeTitle->setAlignment(Qt::AlignCenter);
    auto welcomeText = new QLabel(tr("打开 APK、DEX、JAR 或 CLASS 文件\n先浏览目录，再按需生成 Java 与 Smali 源码。")); welcomeText->setAlignment(Qt::AlignCenter); welcomeText->setObjectName("muted");
    auto welcomeButton = new QPushButton(tr("打开文件")); welcomeButton->setObjectName("primary"); welcomeButton->setFixedWidth(160);
    welcomeLayout->addWidget(welcomeTitle); welcomeLayout->addSpacing(16); welcomeLayout->addWidget(welcomeText); welcomeLayout->addSpacing(24); welcomeLayout->addWidget(welcomeButton,0,Qt::AlignHCenter); welcomeLayout->addStretch();
    connect(welcomeButton, &QPushButton::clicked, this, &MainWindow::chooseFile);
    tabs_ = new QTabWidget; tabs_->setObjectName("sourceTabs"); tabs_->setDocumentMode(true); tabs_->setTabsClosable(true); tabs_->setMovable(true);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) { delete tabs_->widget(index); if (!tabs_->count()) pages_->setCurrentIndex(0); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index < 0) { pages_->setCurrentIndex(0); return; }
        auto tab = tabs_->widget(index); selectedName_ = tab->property("className").toString();
        QSignalBlocker block(language_); language_->setCurrentIndex(tab->property("smali").toBool() ? 1 : 0);
        status_->setText(QString(selectedName_).replace('/', '.') + tr(" · 已缓存"));
    });
    pages_->addWidget(welcome); pages_->addWidget(tabs_); contentLayout->addWidget(pages_,1); splitter->addWidget(content); splitter->setSizes({290,990}); splitter->setStretchFactor(1,1); setCentralWidget(central);

    auto dock = new QDockWidget(tr("引擎日志"), this); dock->setObjectName("engineLogs");
    logs_ = new QPlainTextEdit; logs_->setReadOnly(true); logs_->setMaximumBlockCount(250); dock->setWidget(logs_); addDockWidget(Qt::BottomDockWidgetArea, dock); dock->hide(); viewMenu->addAction(dock->toggleViewAction());
    status_ = new QLabel(tr("就绪 · 拖入文件即可开始")); statusBar()->addWidget(status_,1);
    progress_ = new QProgressBar; progress_->setMaximumWidth(140); progress_->setMaximumHeight(12); progress_->setTextVisible(false); statusBar()->addPermanentWidget(progress_);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString &text) { proxy_->setFilterFixedString(text); });
    connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const auto name = index.data(Qt::UserRole + 1).toString();
        if (!name.isEmpty()) { selectedName_ = name; openSelected(); }
    });
    connect(tree_, &QTreeView::activated, this, [this](const QModelIndex &index) {
        const auto name = index.data(Qt::UserRole + 1).toString();
        if (!name.isEmpty()) { selectedName_ = name; openSelected(); }
    });
    connect(language_, &QComboBox::currentIndexChanged, this, [this] { openSelected(); });
    connect(&backend_, &Backend::indexed, this, &MainWindow::populate);
    connect(&backend_, &Backend::sourceReady, this, &MainWindow::showSource);
    connect(&backend_, &Backend::busyChanged, this, &MainWindow::updateBusy);
    connect(&backend_, &Backend::log, logs_, &QPlainTextEdit::appendPlainText);
    connect(&backend_, &Backend::failed, this, [this, dock](const QString &message) {
        status_->setText(tr("操作失败，详情见引擎日志")); logs_->appendPlainText(message); dock->show();
    });
    connect(&backend_, &Backend::exported, this, [this](const QString &path) {
        status_->setText(tr("导出完成：%1").arg(path));
        QMessageBox::information(this, tr("导出完成"), tr("源码已保存到：\n%1").arg(path));
    });
    updateBusy(false);
}

void MainWindow::chooseFile() {
    if (backend_.busy()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("打开字节码文件"), QSettings().value("lastDirectory").toString(), tr("字节码文件 (*.apk *.xapk *.apks *.dex *.jar *.war *.class)"));
    if (!path.isEmpty()) openPath(path);
}

void MainWindow::openPath(const QString &path) {
    if (backend_.busy()) return;
    if (!QFileInfo(path).isFile()) { status_->setText(tr("文件不存在：%1").arg(path)); return; }
    while (tabs_->count()) delete tabs_->widget(0);
    model_->clear(); filter_->clear(); selectedName_.clear(); logs_->clear(); countLabel_->clear();
    language_->setCurrentIndex(0); pages_->setCurrentIndex(0);
    fileLabel_->setText(QFileInfo(path).fileName()); fileLabel_->setToolTip(QFileInfo(path).absoluteFilePath());
    QSettings().setValue("lastDirectory", QFileInfo(path).absolutePath());
    status_->setText(tr("正在建立类目录…")); backend_.open(path);
}

void MainWindow::populate(const QStringList &classes) {
    QHash<QString, QStandardItem *> packages;
    for (const QString &name : classes) {
        const int slash = name.lastIndexOf('/');
        const QString package = slash < 0 ? tr("默认包") : QString(name.left(slash)).replace('/', '.');
        auto parent = packages.value(package);
        if (!parent) {
            parent = new QStandardItem(style()->standardIcon(QStyle::SP_DirIcon), package);
            parent->setData(package, Qt::UserRole + 2); model_->appendRow(parent); packages.insert(package, parent);
        }
        auto item = new QStandardItem(style()->standardIcon(QStyle::SP_FileIcon), name.mid(slash + 1));
        item->setData(name, Qt::UserRole + 1); item->setData(QString(name).replace('/', '.'), Qt::UserRole + 2); item->setToolTip(name); parent->appendRow(item);
    }
    countLabel_->setText(tr("%1 个包  /  %2 个顶层类").arg(packages.size()).arg(classes.size()));
    status_->setText(tr("目录已就绪 · 选择类以生成源码"));
    if (model_->rowCount() <= 20) tree_->expandAll();
    if (!classes.isEmpty()) {
        const auto index = proxy_->index(0,0); tree_->expand(index);
        const auto first = proxy_->index(0,0,index); tree_->setCurrentIndex(first);
    }
}

void MainWindow::openSelected() {
    if (selectedName_.isEmpty() || backend_.busy()) return;
    const bool smali = language_->currentIndex() == 1;
    for (int i=0; i<tabs_->count(); ++i) {
        auto tab = tabs_->widget(i);
        if (tab->property("className").toString() == selectedName_ && tab->property("smali").toBool() == smali) {
            tabs_->setCurrentIndex(i); pages_->setCurrentIndex(1); return;
        }
    }
    status_->setText(tr("正在生成 %1…").arg(selectedName_)); backend_.request(selectedName_, smali);
}

void MainWindow::showSource(const QString &name, bool smali, const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { status_->setText(tr("无法读取源码文件")); return; }
    if (file.size() > 8 * 1024 * 1024) {
        status_->setText(tr("源码超过 8 MiB，请导出后使用外部编辑器打开。")); return;
    }
    // Bound document memory; evicted documents remain cached on disk.
    if (tabs_->count() >= 12) delete tabs_->widget(0);
    auto code = new CodeEditor(smali); code->setPlainText(QString::fromUtf8(file.readAll()));
    code->setProperty("className", name); code->setProperty("smali", smali);
    const int index = tabs_->addTab(code, name.section('/',-1) + (smali ? ".smali" : ".java"));
    tabs_->setTabToolTip(index, name); tabs_->setCurrentIndex(index); pages_->setCurrentIndex(1);
    status_->setText(tr("%1 · %2 行 · %3 KiB").arg(name).arg(code->blockCount()).arg(file.size()/1024.0, 0, 'f', 1));
}

CodeEditor *MainWindow::editor() const { return dynamic_cast<CodeEditor *>(tabs_->currentWidget()); }

void MainWindow::find(bool backwards) {
    auto code = editor(); if (!code || find_->text().isEmpty()) return;
    const auto original = code->textCursor();
    auto flags = backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags();
    if (!code->find(find_->text(), flags)) {
        code->moveCursor(backwards ? QTextCursor::End : QTextCursor::Start);
        if (!code->find(find_->text(), flags)) { code->setTextCursor(original); status_->setText(tr("未找到：%1").arg(find_->text())); }
        else status_->setText(tr("已循环查找"));
    } else status_->setText(tr("找到：%1").arg(find_->text()));
}

void MainWindow::updateBusy(bool busy) {
    openAction_->setEnabled(!busy); engineAction_->setEnabled(!busy); stopAction_->setEnabled(busy);
    exportAction_->setEnabled(!busy && !backend_.input().isEmpty()); tree_->setEnabled(!busy);
    language_->setEnabled(!busy && backend_.supportsSmali());
    progress_->setRange(0, busy ? 0 : 1); progress_->setVisible(busy);
    if (!busy && status_->text().startsWith(tr("正在"))) status_->setText(tr("就绪"));
}

void MainWindow::exportAll() {
    const auto parent = QFileDialog::getExistingDirectory(this, tr("选择导出位置（将在其中创建新目录）"));
    if (parent.isEmpty()) return;
    const auto folder = QFileInfo(backend_.input()).completeBaseName() + (language_->currentIndex() ? "-smali-" : "-sources-") + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
    status_->setText(tr("正在导出全部源码…")); backend_.exportSources(QDir(parent).filePath(folder), language_->currentIndex() == 1);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (!backend_.busy() && event->mimeData()->hasUrls() && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->urls().isEmpty()) openPath(event->mimeData()->urls().first().toLocalFile());
}
void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings().setValue("geometry", saveGeometry()); backend_.cancel(); event->accept();
}
