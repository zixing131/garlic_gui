#include "classview.h"
#include "scriptdialog.h"
#include "mainwindow.h"
#include "nodeicons.h"
#include "referencesdialog.h"
#include "searchdialog.h"
#include "hexviewer.h"
#include "callgraphdialog.h"
#include "resources.h"
#include "localization.h"
#include <QScopeGuard>
#include <QThreadPool>
#include <QtTest>
#include <QtWidgets>

class WindowTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        // Native Windows QSettings requires an organization before the first write.
        // A per-run namespace also isolates concurrent runs and developer preferences.
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("WindowTest-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    void init() {
        QSettings settings;
        settings.clear();
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }
    void cleanupTestCase() {
        QSettings settings;
        settings.clear();
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }
    void resultNavigationAfterPresentation() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(!window.backend()->busy() &&
            !window.backend()->project()->classes().isEmpty(), 15000);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        ClassView *page = nullptr;
        for (auto candidate : window.findChildren<ClassView *>())
            if (candidate->name() == "demo/Main") page = candidate;
        QVERIFY(page);
        const auto raw = window.backend()->project()->applyAliases(page->rawDocument(false), false);
        const auto lines = raw.text.split('\n');
        int row = -1;
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].contains("return ")) { row = i; break; }
        QVERIFY(row >= 0);
        int occurrence = 0;
        for (int i = 0; i < row; ++i) if (lines[i] == lines[row]) ++occurrence;
        window.navigateTo("Ldemo/Main;", row + 1, {},
            {{"sourceLine", lines[row]}, {"lineOccurrence", occurrence},
             {"column", lines[row].indexOf("return")}, {"length", 6}});
        QCOMPARE(window.editor()->textCursor().selectedText(), QString("return"));

        QTemporaryDir dir;
        QFile resource(dir.filePath("test.txt"));
        QVERIFY(resource.open(QIODevice::WriteOnly));
        resource.write("first needle\nsecond needle\n"); resource.close();
        window.openResource(resource.fileName(), {}, {}, 1,
            {{"sourceLine", "first needle"}, {"column", 6}, {"length", 6}});
        window.openResource(resource.fileName(), {}, {}, 2,
            {{"sourceLine", "second needle"}, {"column", 7}, {"length", 6}});
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->textCursor().selectedText() == "needle", 10000);
        QCOMPARE(window.editor()->textCursor().blockNumber(), 1);
    }
    void appearanceLanguagesAndFonts() {
        const auto oldPreferences = QSettings().value("preferences");
        const auto originalFont = qApp->font();
        const auto oldShortcut = QSettings().value("shortcuts-v3/references");
        const auto restore = qScopeGuard([&] {
            QSettings().setValue("preferences", oldPreferences);
            if (oldShortcut.isValid()) QSettings().setValue("shortcuts-v3/references", oldShortcut);
            else QSettings().remove("shortcuts-v3/references");
            qApp->setFont(originalFont);
        });
        QSettings settings;
        settings.setValue("shortcuts-v3/references", "Ctrl+R");
        settings.remove("preferences");
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QCOMPARE(QSettings().value("shortcuts-v3/references").toString(), QString("Ctrl+R"));
        const auto available = Localization::languages();
        QCOMPARE(available.size(), 7);
        for (const auto &locale : {"en", "zh_TW", "ru", "fr", "ja", "ko"}) {
            Localization translation;
            QVERIFY(translation.loadLanguage(locale));
            QVERIFY(!translation.translate("MainWindow", "语言").isEmpty());
            QVERIFY(translation.translate("MainWindow", "Untranslated text").isEmpty());
            qApp->installTranslator(&translation);
            MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
            QCOMPARE(window.findChild<QAction *>("settings")->text(), translation.translate("MainWindow", "设置…"));
            QCOMPARE(window.findChild<QAction *>("references")->shortcut(), QKeySequence("Ctrl+R"));
            bool checked = false;
            QTimer::singleShot(50, &window, [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                auto language = dialog->findChild<QComboBox *>("interfaceLanguage");
                auto ui = dialog->findChild<QSpinBox *>("uiFontSize");
                auto editor = dialog->findChild<QSpinBox *>("editorFontSize");
                auto mono = dialog->findChild<QSpinBox *>("monoFontSize");
                checked = language && language->count() == 7 && ui && editor && mono;
                if (checked) {
                    language->setCurrentIndex(language->findData(locale));
                    ui->setValue(15); editor->setValue(18); mono->setValue(16);
                    dialog->findChild<QListWidget *>("preferencesNavigation")->setCurrentRow(2);
                    const auto screenshots = qEnvironmentVariable("GARLIC_SCREENSHOTS");
                    if (!screenshots.isEmpty()) dialog->grab().save(screenshots + "/appearance-" + locale + ".png");
                }
                dialog->accept();
            });
            window.findChild<QAction *>("settings")->trigger();
            QVERIFY(checked);
            const auto saved = AppSettings::load();
            QCOMPARE(saved.language, QString(locale));
            QVERIFY(saved.uiFontFamily.isEmpty());
            QVERIFY(saved.editorFontFamily.isEmpty());
            QVERIFY(saved.monoFontFamily.isEmpty());
            QCOMPARE(saved.interfaceFont().pointSize(), 15);
            ClassView view("Test", true, saved);
            QCOMPARE(view.editor(false)->font().pointSize(), 18);
            QCOMPARE(view.editor(true)->font().pointSize(), 16);
            ScriptDialog script(&window);
            QCOMPARE(script.findChild<QPlainTextEdit *>("scriptCode")->font().pointSize(), 18);
            QTemporaryFile file; QVERIFY(file.open());
            HexViewer hex(file.fileName(), 64);
            QCOMPARE(hex.font().pointSize(), 16);
            qApp->removeTranslator(&translation);
        }
        Localization invalid;
        QVERIFY(!invalid.loadLanguage("../en"));
        const auto migrated = AppSettings::fromJson({{"fontSize", 17}});
        QCOMPARE(migrated.monoFontSize, 17);
    }
    void scripts_data() {
        QTest::addColumn<bool>("python"); QTest::newRow("python") << true; QTest::newRow("javascript") << false;
    }
    void scripts() {
        QFETCH(bool, python);
        if (ScriptDialog::interpreter({}, python).isEmpty()) QSKIP("Interpreter not installed");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings(); settings.background = false; settings.mcpEnabled = false; settings.deobfuscate = false;
        window.backend()->configure(settings); window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/resources.apks");
        QTRY_VERIFY_WITH_TIMEOUT(window.backend()->metadataReady(), 15000);
        ScriptDialog dialog(&window, qEnvironmentVariable("GARLIC_TEST_GUI"));
        dialog.findChild<QComboBox *>("scriptLanguage")->setCurrentIndex(python ? 0 : 1);
        auto code = dialog.findChild<QPlainTextEdit *>("scriptCode");
        auto output = dialog.findChild<QPlainTextEdit *>("scriptOutput");
        auto run = dialog.findChild<QPushButton *>("runScript");
        dialog.findChild<QLineEdit *>("scriptArguments")->setText("{\"key\":42}");
        code->setPlainText(python ?
            "from garlic import api\nassert api.get_status()['class_count'] == 2\nassert len(list(api.classes())) == 2\nassert api.get_source_symbols(class_name='demo/cases/Foo')['symbols']\nassert api.read_resource(entry='base.apk!assets/base.txt', encoding='text')['data'] == 'base'\nprint(bytes(x ^ api.arguments['key'] for x in [98,79,70,70,69]).decode())\nprint('SDK_OK')\n"
            : "if ((await garlic.get_status()).class_count !== 2) throw Error('classes');\nconst symbols = await garlic.get_source_symbols({class_name:'demo/cases/Foo'}); if (!symbols.symbols.length) throw Error('symbols');\nconsole.log(Buffer.from([98,79,70,70,69].map(x => x ^ garlic.arguments.key)).toString());\nconsole.log('SDK_OK');\n");
        // Resource entry routing uses the merged metadata, not a guessed nested delimiter.
        if (python) code->setPlainText(code->toPlainText().replace("assert api.read_resource(entry='base.apk!assets/base.txt', encoding='text')['data'] == 'base'", "entry = next(e for e in api.list_resources()['entries'] if e['name'] == 'assets/base.txt')\nassert api.read_resource(path=entry['sourcePath'], entry=entry['sourceEntry'], encoding='text')['data'] == 'base'"));
        run->click();
        QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 20000);
        QVERIFY2(output->toPlainText().contains("SDK_OK"), qPrintable(output->toPlainText()));
        QVERIFY(output->toPlainText().contains("Hello")); QVERIFY(output->toPlainText().contains("exit=0"));
        QVERIFY(!window.backend()->settings().mcpEnabled);
        code->setPlainText(python ? "import time\nprint('WAIT', flush=True)\ntime.sleep(30)" : "console.log('WAIT'); await new Promise(resolve => setTimeout(resolve, 30000));");
        run->click(); QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains("WAIT"), 5000);
        dialog.findChild<QPushButton *>("stopScript")->click();
        QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 3000);
        code->setPlainText(python ? "raise RuntimeError('EXPECTED')" : "throw new Error('EXPECTED');");
        run->click(); QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 5000);
        QVERIFY(output->toPlainText().contains("EXPECTED")); QVERIFY(output->toPlainText().contains("exit=1"));
        settings.scriptTimeout = 1; window.backend()->configure(settings);
        code->setPlainText(python ? "import time\nprint('WAIT', flush=True)\ntime.sleep(30)" : "await new Promise(resolve => setTimeout(resolve, 30000));");
        run->click(); QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 4000);
        QVERIFY(output->toPlainText().contains("超时"));
        if (python) settings.pythonPath = QDir::tempPath() + "/missing-garlic-python";
        else settings.nodePath = QDir::tempPath() + "/missing-garlic-node";
        window.backend()->configure(settings); run->click();
        QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 3000);
        QVERIFY(!output->toPlainText().contains("exit=0"));
        settings.pythonPath.clear(); settings.nodePath.clear(); settings.scriptTimeout = 300; window.backend()->configure(settings);
        code->setPlainText(python ? "print('RECOVERED')" : "console.log('RECOVERED');");
        run->click(); QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 3000);
        QVERIFY(output->toPlainText().contains("RECOVERED")); QVERIFY(output->toPlainText().contains("exit=0"));
    }
    void referenceResultFilter() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto dialog = new ReferencesDialog(&window, "LExample;");
        auto filter = dialog->findChild<QLineEdit *>("referenceFilter");
        auto table = dialog->findChild<QTableView *>("referenceResults");
        auto proxy = qobject_cast<QSortFilterProxyModel *>(table->model());
        auto model = qobject_cast<QStandardItemModel *>(proxy->sourceModel());
        QVERIFY(filter); QVERIFY(model);
        model->appendRow({new QStandardItem("Alpha.call"), new QStandardItem("return value;")});
        model->appendRow({new QStandardItem("Beta.run"), new QStandardItem("decode(secret);")});
        filter->setText("SECRET"); QCOMPARE(proxy->rowCount(), 1);
        QCOMPARE(proxy->index(0, 0).data().toString(), QString("Beta.run"));
        model->appendRow({new QStandardItem("Gamma.run"), new QStandardItem("secret = 1;")});
        QCOMPARE(proxy->rowCount(), 2);
        model->item(1, 1)->setText("plain"); QCOMPARE(proxy->rowCount(), 1);
        filter->setText("Alpha"); QCOMPARE(proxy->rowCount(), 1);
        filter->setText("[literal]"); QCOMPARE(proxy->rowCount(), 0);
        filter->clear(); QCOMPARE(proxy->rowCount(), 3);
        QCOMPARE(dialog->findChild<QLabel *>("referenceCount")->text(), QString("显示 3 / 3 处引用"));
        delete dialog;
    }
    void realLargeLifecycle() {
        const auto input = qEnvironmentVariable("GARLIC_TEST_LIFECYCLE_APK");
        if (input.isEmpty()) QSKIP("Set APK for lifecycle profiling");
        auto window = std::make_unique<MainWindow>(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window->backend()->settings(); settings.background = false;
        settings.indexDirectory = qEnvironmentVariable("GARLIC_TEST_CACHE_DIRECTORY", settings.indexDirectory);
        settings.deobfuscate = qEnvironmentVariableIsSet("GARLIC_TEST_DEOBFUSCATE");
        window->backend()->configure(settings); window->show();
        QElapsedTimer clock; clock.start(); qint64 last = 0, maxGap = 0; QString phase;
        QTimer heartbeat; heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, window.get(), [&] {
            const auto now = clock.elapsed(), gap = now - last; maxGap = qMax(maxGap, gap); last = now;
            if (gap > 200) qInfo() << "UI gap" << gap << "phase" << phase;
        }); heartbeat.start();
        connect(window->backend(), &Backend::loadProgress, window.get(), [&](const QString &stage, int percent) {
            phase = stage + QString::number(percent); if (percent == 100) qInfo() << "Stage" << phase << clock.elapsed();
        });
        window->openPath(input);
        QTRY_VERIFY_WITH_TIMEOUT(!window->backend()->busy() && window->backend()->project()->classCount() > 0, 120000);
        qInfo() << "Directory ready ms" << clock.elapsed();
        QTRY_VERIFY_WITH_TIMEOUT(window->backend()->metadataReady(), 120000);
        qInfo() << "Metadata ready ms" << clock.elapsed();
        QElapsedTimer source; source.start(); window->openClass("com/tencent/mm/ui/LauncherUI");
        QTRY_VERIFY_WITH_TIMEOUT(window->editor() && window->editor()->toPlainText().contains("class LauncherUI"), 60000);
        qInfo() << "Java ready ms" << source.elapsed();
        QTest::qWait(100); heartbeat.stop();
        QElapsedTimer closing; closing.start(); window->close(); window.reset();
        qInfo() << "Window destruction ms" << closing.elapsed();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(30000));
        qInfo() << "Shutdown total ms" << closing.elapsed() << "maximum UI gap" << maxGap;
        QVERIFY2(maxGap < 500, qPrintable(QString::number(maxGap)));
    }
    void largeSourceHighlighting() {
        CodeEditor editor(false);
        editor.setSource({QString("// padding\n").repeated(60000) + "public class Highlighted {}\n", {}});
        const auto block = editor.document()->findBlockByNumber(60000);
        QTRY_VERIFY_WITH_TIMEOUT(!block.layout()->formats().isEmpty(), 10000);
        editor.setTheme(true);
        QTRY_VERIFY_WITH_TIMEOUT(!block.layout()->formats().isEmpty(), 10000);
    }
    void packageSearchAndMemoryDefaults() {
        QVERIFY(AppSettings().showMemory);
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        SearchDialog dialog(&window);
        dialog.setPackage("kotlin/jvm/internal");
        QCOMPARE(dialog.findChild<QLineEdit *>("searchPackage")->text(), QString("kotlin.jvm.internal"));
    }
    void graphSelectionHighlight() {
        Project project;
        const QString caller = "LExample;->caller()V", callee = "LExample;->callee()V";
        project.addClass({{"name", "Example"}, {"methods", QJsonArray{
            QJsonObject{{"id", caller}, {"name", "caller"}}, QJsonObject{{"id", callee}, {"name", "callee"}}}},
            {"refs", QJsonArray{QJsonObject{{"from", caller}, {"target", callee}, {"offset", 0}}}}});
        CallGraphDialog dialog(project.snapshot(), caller); dialog.show();
        auto graph = dialog.findChild<QGraphicsView *>("callGraphView");
        QTRY_VERIFY_WITH_TIMEOUT(graph->scene()->items().size() >= 5, 5000);
        QGraphicsLineItem *edge = nullptr;
        for (auto item : graph->scene()->items())
            if ((edge = dynamic_cast<QGraphicsLineItem *>(item))) break;
        QVERIFY(edge);
        QGraphicsItem *node = nullptr;
        for (auto item : graph->scene()->items())
            if (item->flags().testFlag(QGraphicsItem::ItemIsSelectable) && item->data(0) == edge->data(0)) { node = item; break; }
        QVERIFY(node);
        QTest::mouseClick(graph->viewport(), Qt::LeftButton, {}, graph->mapFromScene(node->sceneBoundingRect().center()));
        QCOMPARE(edge->pen().widthF(), 3.0);
        QTest::mouseClick(graph->viewport(), Qt::LeftButton, {}, QPoint(2, 2));
        QCOMPARE(edge->pen().widthF(), 1.3);
    }
    void mergedSplitResources() {
        QTemporaryDir fixture;
        const auto archive = fixture.path() + "/resources.apks";
        QVERIFY(QFile::copy(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/resources.apks", archive));
        QString extractionError;
        const auto base = Resources::materialize(archive, "base.apk", &extractionError);
        QVERIFY2(!base.isEmpty(), qPrintable(extractionError));
        // Verify persistence after the temporary-file owner has been destroyed.
        QCOMPARE(Resources::read(base, "assets/base.txt", 1024, &extractionError), QByteArray("base"));
        QCOMPARE(Resources::materialize(archive, "base.apk", &extractionError), base);
        const auto canceled = std::make_shared<std::atomic_bool>(true);
        QVERIFY(Resources::materialize(archive, "config.en.apk", &extractionError, canceled).isEmpty());
        const auto inspected = Resources::inspect(archive);
        QCOMPARE(inspected.value("application").toString(), QString("demo.App"));
        QCOMPARE(inspected.value("main_activities").toArray(), QJsonArray{"demo.cases.Foo"});
        const auto entries = inspected.value("entries").toArray();
        QSet<QString> names;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            names.insert(entry.value("name").toString());
            QString error;
            QVERIFY(!Resources::read(entry.value("sourcePath").toString(), entry.value("sourceEntry").toString(), 1024 * 1024, &error).isEmpty());
            QVERIFY(error.isEmpty());
        }
        QVERIFY(names.contains("assets/base.txt"));
        QVERIFY(names.contains("assets/en.txt"));
        QVERIFY(names.contains("resources.arsc"));
        QVERIFY(names.contains("split-data/config.en/resources.arsc"));
        QVERIFY(!names.contains("base.apk"));
    }
    void realResourceXmlNavigation() {
        const auto input = qEnvironmentVariable("GARLIC_TEST_LARGE_APK");
        if (input.isEmpty()) QSKIP("Set APK for resource XML navigation regression");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings(); settings.background = false; settings.deobfuscate = false;
        window.backend()->configure(settings); window.backend()->setProperty("fastOpen", true);
        window.show(); window.openPath(input);
        auto tree = window.findChild<QTreeView *>("classTree");
        auto tables = [&] { return tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 6,
            "resources.arsc", 1, Qt::MatchExactly | Qt::MatchRecursive); };
        QTRY_VERIFY_WITH_TIMEOUT(!tables().isEmpty(), 60000);
        QString error; QMap<QString, QString> files;
        Resources::describeTable(Resources::read(input, "resources.arsc", 128LL * 1048576, &error), &files, false);
        QVERIFY(error.isEmpty()); QVERIFY(!files.isEmpty());
        const auto name = files.firstKey();
        window.openResource(input, "resources.arsc", name, 2);
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("<resources>"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(tree->currentIndex().data(Qt::UserRole + 7).toString(), name, 30000);
        QCOMPARE(window.editor()->textCursor().blockNumber(), 1);
    }
    void mergedManifestNavigation() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings(); settings.background = false; settings.deobfuscate = false;
        window.backend()->configure(settings);
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/resources.apks");
        auto tree = window.findChild<QTreeView *>("classTree");
        auto manifests = [&] { return tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 6,
            "AndroidManifest.xml", 1, Qt::MatchExactly | Qt::MatchRecursive); };
        QTRY_VERIFY_WITH_TIMEOUT(!manifests().isEmpty(), 15000);
        window.findChild<QAction *>("goManifest")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("android.intent.action.MAIN"), 15000);
        QCOMPARE(tree->currentIndex().data(Qt::UserRole + 6).toString(), QString("AndroidManifest.xml"));
        window.findChild<QAction *>("mainActivity")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("return 11;"), 15000);
    }
    void caseSafeExport() {
        Backend backend;
        backend.setEngine(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        backend.open(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/cases.dex");
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy() && backend.project()->classCount() > 0, 15000);
        QTemporaryDir directory;
        const auto output = directory.path() + "/export";
        QSignalSpy finished(&backend, &Backend::exported);
        backend.exportSources(output, false);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
        QVERIFY(QFileInfo::exists(output + "/.garlic-safe-paths"));
        const auto upper = Project::sourcePath(output, "demo/cases/Foo", ".java");
        const auto lower = Project::sourcePath(output, "demo/cases/foo", ".java");
        QVERIFY(upper.toCaseFolded() != lower.toCaseFolded());
        QFile first(upper), second(lower);
        QVERIFY(first.open(QIODevice::ReadOnly)); QVERIFY(second.open(QIODevice::ReadOnly));
        QVERIFY(first.readAll().contains("class Foo")); QVERIFY(second.readAll().contains("class foo"));
    }
    void rawFlattenedSource() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings();
        settings.deobfuscate = settings.unflatten = settings.simplifyControlFlow = false;
        window.applySettings(settings);
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/flattened.dex");
        QTRY_VERIFY_WITH_TIMEOUT(!window.backend()->busy() && window.backend()->project()->classCount() > 0, 15000);
        window.openClass("demo/Flattened");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("compareDispatcher"), 15000);
    }
    void pagedHexLargeFile() {
        QTemporaryFile file; QVERIFY(file.open());
        QVERIFY(file.resize(128LL * 1024 * 1024));
        QVERIFY(file.seek(file.size() - 16)); QCOMPARE(file.write("0123456789abcdef"), qint64(16)); file.flush();
        HexViewer viewer(file.fileName(), 64); viewer.resize(800, 400); viewer.show();
        QTest::qWait(20);
        QVERIFY(viewer.verticalScrollBar()->maximum() > 100000);
        viewer.verticalScrollBar()->setValue(viewer.verticalScrollBar()->maximum());
        QVERIFY(!viewer.grab().isNull());
    }
    void largeProjectClose() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_LARGE_APK");
        if (path.isEmpty()) QSKIP("Set GARLIC_TEST_LARGE_APK for shutdown regression");
        auto window = std::make_unique<MainWindow>(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        bool warming = false;
        connect(window->backend(), &Backend::loadProgress, window.get(), [&](const QString &phase, int percent) {
            if (phase == "构建查询索引" && percent >= 33) warming = true;
        });
        window->show(); window->openPath(path);
        QTRY_VERIFY_WITH_TIMEOUT(window->backend()->project()->classCount() > 0, 30000);
        window->backend()->prepareMetadata();
        if (qEnvironmentVariableIsSet("GARLIC_TEST_CLOSE_DURING_XREFS"))
            QTRY_VERIFY_WITH_TIMEOUT(warming, 120000);
        else QTest::qWait(2000);
        QElapsedTimer timer; timer.start();
        window->close(); window.reset();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(15000));
        qInfo() << "Shutdown including background cleanup ms:" << timer.elapsed();
        QVERIFY(timer.elapsed() < 15000);
    }
    void loadingPercentage() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto bar = window.findChild<QProgressBar *>();
        QVERIFY(bar);
        window.backend()->loadProgress("成员与引用", 42);
        QCOMPARE(bar->minimum(), 0);
        QCOMPARE(bar->maximum(), 100);
        QCOMPARE(bar->value(), 42);
        QVERIFY(bar->text().contains("42%"));
        QVERIFY(bar->isTextVisible());
    }
    void progressiveNavigation() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.backend()->setProperty("fastOpen", true);
        auto settings = window.backend()->settings();
        settings.background = false; settings.deobfuscate = false;
        window.backend()->configure(settings);
        connect(window.backend(), &Backend::indexed, &window, [&] {
            QVERIFY(!window.backend()->metadataReady());
            window.showReferences("Ldemo/cases/foo;->identify()I");
            window.showReferences("Ldemo/cases/foo;->identify()I");
            auto tree = window.findChild<QTreeView *>("classTree");
            auto matches = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1,
                "Ldemo/cases/foo;", 1, Qt::MatchExactly | Qt::MatchRecursive);
            QVERIFY(!matches.isEmpty());
            tree->expand(matches.first());
            window.navigateTo("Ldemo/cases/foo;->identify()I");
        });
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/cases.dex");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("return 22;"), 15000);
        QTRY_COMPARE(window.editor()->textCursor().selectedText(), QString("identify"));
        QVERIFY(window.backend()->metadataReady());
        QTest::qWait(100);
        QVERIFY(window.findChildren<ReferencesDialog *>().isEmpty());
        auto tree = window.findChild<QTreeView *>("classTree");
        auto methods = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1,
            "Ldemo/cases/foo;->identify()I", 1, Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY(!methods.isEmpty());
    }
    void caseSensitiveNavigation() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings();
        settings.background = true;
        settings.deobfuscate = false;
        window.backend()->configure(settings);
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/cases.dex");
        QTRY_VERIFY_WITH_TIMEOUT(window.backend()->projectReady(), 15000);
        window.navigateTo("Ldemo/cases/Foo;->identify()I");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("return 11;"), 15000);
        QTRY_COMPARE(window.editor()->textCursor().selectedText(), QString("identify"));
        window.navigateTo("Ldemo/cases/foo;->identify()I");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains("return 22;"), 15000);
        QTRY_COMPARE(window.editor()->textCursor().selectedText(), QString("identify"));
        window.navigateTo("Ldemo/cases/Foo;->identify()I");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains("return 11;"), 15000);
    }

    void savePreferencesWithoutInput() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto tree = window.findChild<QTreeView *>("classTree");
        QCOMPARE(tree->model()->rowCount(), 0);
        bool saved = false;
        QTimer::singleShot(50, &window, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            saved = true;
            dialog->accept();
        });
        window.findChild<QAction *>("settings")->trigger();
        QVERIFY(saved);
        QCOMPARE(tree->model()->rowCount(), 0);
    }
    void deobfuscationSettingsAndAliases() {
        AppSettings settings;
        settings.deobfuscate = settings.simplifyControlFlow = settings.unflatten = true;
        const auto restored = AppSettings::fromJson(settings.toJson());
        QVERIFY(restored.deobfuscate && restored.simplifyControlFlow && restored.unflatten);
        Project project;
        project.addClass(QJsonObject{{"name", "demo/a"}, {"kind", "class"}});
        project.deobfuscateNames();
        QVERIFY(project.alias("Ldemo/a;").startsWith("Class_"));
        const auto aliases = project.aliases();
        project.deobfuscateNames();
        QCOMPARE(project.aliases(), aliases);
    }
    void toolbarIconsMatchActions() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        const QHash<QString, QString> expected{
            {"openFile", "toolopenDisk"},       {"exportSources", "toolexport"},
            {"projectSearch", "toolfind"},      {"callGraph", "methodReference"},
            {"syncEditor", "toolsync"},         {"flatPackages", "toolpackages"},
            {"navigateBack", "toolleft"},       {"navigateForward", "toolright"},
            {"stopTask", "toolclose"},          {"settings", "toolsettings"},
            {"mainActivity", "toolmainActivity"}, {"goApplication", "toolapplication"},
            {"goManifest", "toolandroidManifest"}};
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            auto action = window.findChild<QAction *>(it.key());
            QVERIFY2(action, qPrintable(it.key()));
            QCOMPARE(action->icon().cacheKey(), NodeIcons::icon(it.value()).cacheKey());
            QCOMPARE(action->toolTip(), action->text());
        }
    }
    void shortcutsAndFilterState() {
        QSettings().clear();
        QSettings().setValue("shortcuts/查找引用", "Ctrl+Shift+U");
        QSettings().setValue("shortcuts/重命名", "F2");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.activateWindow();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        QVERIFY(window.editor()->toPlainText().contains("@Override"));
        window.navigateTo("Ldemo/Main;->greet(I)Ljava/lang/String;");
        auto editor = window.editor();
        editor->setFocus();
        QTest::keyClick(editor, Qt::Key_X);
        QTRY_VERIFY(window.findChild<ReferencesDialog *>());
        window.findChild<ReferencesDialog *>()->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.activateWindow();
        editor->setFocus();
        bool renameOpened = false;
        QTimer::singleShot(100, &window, [&] {
            auto dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            renameOpened = dialog != nullptr;
            if (dialog)
                dialog->reject();
        });
        QTest::keyClick(editor, Qt::Key_N);
        QTRY_VERIFY(renameOpened);
        window.showCallGraph("Ldemo/Main;->greet(I)Ljava/lang/String;");
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QGraphicsView *>("callGraphView"), 5000);
        auto graph = window.findChild<QGraphicsView *>("callGraphView");
        QTRY_VERIFY_WITH_TIMEOUT(!graph->scene()->items().isEmpty(), 5000);
        const auto beforeZoom = graph->transform().m11();
        for (int i = 0; i < 8; ++i) {
            QWheelEvent zoom(graph->viewport()->rect().center(),
                             graph->mapToGlobal(graph->viewport()->rect().center()), QPoint(),
                             QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate,
                             false);
            QCoreApplication::sendEvent(graph->viewport(), &zoom);
        }
        QVERIFY(graph->transform().m11() > beforeZoom);
        auto horizontal = graph->horizontalScrollBar();
        QVERIFY(horizontal->maximum() > horizontal->minimum());
        horizontal->setValue((horizontal->minimum() + horizontal->maximum()) / 2);
        const int beforePan = horizontal->value();
        const QPoint center = graph->viewport()->rect().center();
        QTest::mousePress(graph->viewport(), Qt::LeftButton, {}, center);
        QTest::mouseMove(graph->viewport(), center - QPoint(80, 0), 20);
        QTest::mouseRelease(graph->viewport(), Qt::LeftButton, {}, center - QPoint(80, 0));
        QVERIFY(horizontal->value() > beforePan);
        window.findChild<QGraphicsView *>("callGraphView")->window()->close();
        auto tree = window.findChild<QTreeView *>("classTree");
        auto filter = window.findChild<QLineEdit *>("classFilter");
        QModelIndexList matches;
        QTRY_VERIFY_WITH_TIMEOUT(
            !(matches = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1,
                                             "Ldemo/Main;", 1,
                                             Qt::MatchExactly | Qt::MatchRecursive))
                 .isEmpty(),
            5000);
        auto idx = matches.first();
        tree->expand(idx.parent());
        tree->expand(idx);
        tree->setCurrentIndex(idx);
        filter->setText("no_matching_class_123");
        QTRY_VERIFY_WITH_TIMEOUT(tree->model()->match(tree->model()->index(0, 0),
            Qt::UserRole + 1, "Ldemo/Main;", 1,
            Qt::MatchExactly | Qt::MatchRecursive).isEmpty(), 5000);
        filter->clear();
        QTRY_VERIFY_WITH_TIMEOUT(!(matches = tree->model()->match(tree->model()->index(0, 0),
            Qt::UserRole + 1, "Ldemo/Main;", 1,
            Qt::MatchExactly | Qt::MatchRecursive)).isEmpty(), 5000);
        QVERIFY(tree->isExpanded(matches.first()));
    }
    void clearCacheAndResize() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        const auto source = window.backend()->cachedPath("demo/Main", false);
        QVERIFY(QFileInfo::exists(source));
        window.backend()->clearCache();
        QTRY_VERIFY_WITH_TIMEOUT(!window.backend()->busy(), 5000);
        QVERIFY(!QFileInfo::exists(source));
        QVERIFY(!window.editor()->toPlainText().contains("greet"));
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains("greet"), 15000);
        auto splitter = window.findChild<QSplitter *>("mainSplitter");
        QVERIFY(splitter);
        window.resize(1200, 760);
        QTest::qWait(50);
        const auto wide = splitter->sizes();
        window.resize(700, 500);
        QTest::qWait(50);
        QVERIFY2(window.width() <= 700,
                 qPrintable(QString::number(window.minimumSizeHint().width())));
        const auto narrow = splitter->sizes();
        QVERIFY(narrow[0] < wide[0]);
        QVERIFY(narrow[1] < wide[1]);
        QVERIFY(narrow[1] >= 300);
    }
    void treeModesSyncAndSideButtons() {
        QSettings().setValue("view/flatPackages", true);
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        auto tree = window.findChild<QTreeView *>("classTree");
        auto flat = window.findChild<QAction *>("flatPackages");
        QVERIFY(flat && flat->isChecked());
        auto matches = [&](const QString &id) {
            return tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1, id, 1,
                                        Qt::MatchExactly | Qt::MatchRecursive);
        };
        QTRY_VERIFY(!matches("Ldemo/deep/nested/Leaf;").isEmpty());
        QCOMPARE(matches("Ldemo/deep/nested/Leaf;").first().parent().data().toString(),
                 QString("demo.deep.nested"));
        flat->setChecked(false);
        QTRY_VERIFY(!matches("Ldemo/deep/nested/Leaf;").isEmpty());
        auto leaf = matches("Ldemo/deep/nested/Leaf;").first();
        QVERIFY(window.windowTitle().contains("代码浏览器 v "));
        auto copyMenu = [&](const QModelIndex &index, const QString &label, const QString &expected) {
            tree->scrollTo(index);
            bool triggered = false;
            QTimer::singleShot(50, &window, [&] {
                auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto action : menu->actions()) if (action->text() == label) { action->trigger(); triggered = true; break; }
                menu->close();
            });
            QMetaObject::invokeMethod(tree, "customContextMenuRequested", Q_ARG(QPoint, tree->visualRect(index).center()));
            QVERIFY(triggered);
            QCOMPARE(QApplication::clipboard()->text(), expected);
        };
        copyMenu(leaf.parent(), "复制包名", "demo.deep.nested");
        copyMenu(leaf, "复制类名", "demo.deep.nested.Leaf");
        QCOMPARE(leaf.parent().data().toString(), QString("nested"));
        QCOMPARE(leaf.parent().parent().data().toString(), QString("deep"));
        QCOMPARE(QSettings().value("view/flatPackages").toBool(), false);
        window.openClass("demo/deep/nested/Leaf");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("return 7"), 15000);
        window.findChild<QLineEdit *>("classFilter")->setText("hidden");
        window.findChild<QAction *>("syncEditor")->trigger();
        QCOMPARE(tree->currentIndex().data(Qt::UserRole + 1).toString(),
                 QString("Ldemo/deep/nested/Leaf;"));
        QVERIFY(tree->isExpanded(tree->currentIndex().parent()));
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty()) {
            window.resize(1100, 720);
            QTest::qWait(100);
            window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/tree-modes.png");
        }
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains("greet"), 15000);
        QTest::mouseClick(window.editor()->viewport(), Qt::BackButton);
        QTRY_VERIFY(window.editor()->toPlainText().contains("return 7"));
        QTest::mouseClick(window.editor()->viewport(), Qt::ForwardButton);
        QTRY_VERIFY(window.editor()->toPlainText().contains("greet"));
    }
    void largeProjectOpenTime() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_LARGE_APK");
        if (path.isEmpty())
            QSKIP("Set GARLIC_TEST_LARGE_APK to measure full tree readiness");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        QElapsedTimer elapsed;
        elapsed.start();
        window.openPath(path);
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 120000);
        const auto indexedMs = elapsed.elapsed();
        const auto count = QString::number(window.backend()->project()->classes().size());
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("muted")->text().contains(count),
                                 30000);
        qInfo() << "Index ready ms:" << indexedMs << "Full directory ready ms:" << elapsed.elapsed()
                << "classes:" << count;
        const auto limit = qEnvironmentVariableIntValue("GARLIC_TEST_OPEN_LIMIT_MS");
        if (limit > 0) QVERIFY(elapsed.elapsed() < limit);
        const auto sourceClass = qEnvironmentVariable("GARLIC_TEST_SOURCE_CLASS");
        if (!sourceClass.isEmpty()) {
            QElapsedTimer sourceTime, heartbeat;
            sourceTime.start(); heartbeat.start();
            qint64 maxGap = 0;
            QTimer pulse;
            connect(&pulse, &QTimer::timeout, &window, [&] {
                maxGap = qMax(maxGap, heartbeat.restart());
            });
            pulse.start(10);
            window.openClass(sourceClass);
            QTRY_VERIFY_WITH_TIMEOUT(window.editor() &&
                window.editor()->toPlainText().contains("class " + sourceClass.section('/', -1)), 30000);
            qInfo() << "Java source ready ms:" << sourceTime.elapsed() << "Max UI heartbeat gap ms:" << maxGap;
            const auto sourceLimit = qEnvironmentVariableIntValue("GARLIC_TEST_SOURCE_LIMIT_MS");
            if (sourceLimit > 0) QVERIFY(sourceTime.elapsed() < sourceLimit);
            QVERIFY(maxGap < 1000);
            const auto classBlock = window.editor()->document()->findBlock(
                window.editor()->toPlainText().indexOf("public class "));
            if (classBlock.isValid())
                QTRY_VERIFY_WITH_TIMEOUT(!classBlock.layout()->formats().isEmpty(), 10000);
            if (qEnvironmentVariableIsSet("GARLIC_TEST_WAIT_METADATA")) {
                auto tree = window.findChild<QTreeView *>("classTree");
                const QPersistentModelIndex firstClass(tree->model()->index(0, 0));
                QTRY_VERIFY_WITH_TIMEOUT(window.backend()->metadataReady(), 120000);
                QTest::qWait(200);
                QVERIFY(firstClass.isValid());
                qInfo() << "Through metadata completion max UI heartbeat gap ms:" << maxGap;
                QVERIFY(maxGap < 1000);
                if (qEnvironmentVariableIsSet("GARLIC_TEST_RENAME")) {
                    for (bool local : {false, true}) {
                        QString id;
                        for (const auto &span : window.editor()->spans())
                            if (span.declaration && span.id.contains("->") &&
                                span.id.contains("@local:") == local &&
                                (local || !span.id.contains("-><"))) { id = span.id; break; }
                        QVERIFY(!id.isEmpty());
                        const QString replacement = local ? "renamedLocalBenchmark" : "renamedMethodBenchmark";
                        QElapsedTimer renameTime; renameTime.start();
                        const auto error = window.backend()->project()->rename(id, replacement);
                        QVERIFY2(error.isEmpty(), qPrintable(error));
                        QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains(replacement), 3000);
                        qInfo() << (local ? "Local rename ms:" : "Method rename ms:") << renameTime.elapsed();
                        QVERIFY(renameTime.elapsed() < 1000);
                        QVERIFY(firstClass.isValid());
                    }
                    auto settings = window.backend()->settings(); settings.theme = "light";
                    window.applySettings(settings);
                    QVERIFY(firstClass.isValid());
                }
            }
        }
        if (qEnvironmentVariableIsSet("GARLIC_TEST_WAIT_METADATA")) {
            QTRY_VERIFY_WITH_TIMEOUT(window.backend()->metadataReady(), 120000);
            qInfo() << "Full metadata ready ms:" << elapsed.elapsed();
            QVERIFY(!window.backend()->project()->xrefs(
                "Landroid/util/Log;->d(Ljava/lang/String;Ljava/lang/String;)I").isEmpty());
        }
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty())
            window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/large-project.png");
    }
    void crlfSymbolPositions() {
        CodeEditor editor(false);
        const QString text = "// 中文\r\npublic class Example {\r\n    void greet() {}\r\n}\r\n";
        const int start = text.indexOf("greet");
        const QString id = "LExample;->greet()V";
        editor.setSource({text, {{start, start + 5, id, true}}});
        QVERIFY(editor.goToSymbol(id));
        QCOMPARE(editor.textCursor().selectedText(), QString("greet"));
        QCOMPARE(editor.symbolAtCursor(), id);
        QVERIFY(!editor.toPlainText().contains('\r'));
    }
    void localVariableDoubleClick() {
        CodeEditor editor(false);
        const QString text = "void sample() { int answer = 7; return answer + 1; }";
        editor.setSource({text, {}});
        editor.resize(640, 160);
        editor.show();
        const int use = text.lastIndexOf("answer");
        auto cursor = editor.textCursor();
        cursor.setPosition(use);
        editor.setTextCursor(cursor);
        QTest::mouseDClick(editor.viewport(), Qt::LeftButton, {}, editor.cursorRect(cursor).center());
        QCOMPARE(editor.textCursor().selectedText(), QString("answer"));
        QCOMPARE(editor.textCursor().selectionStart(), text.indexOf("answer"));
    }
    void codeEditorCallGraphContextMenu() {
        CodeEditor editor(false);
        const QString text = "class Example { void run() {} }";
        const int start = text.indexOf("run");
        editor.setSource({text, {{start, start + 3, "LExample;->run()V", true}}});
        editor.resize(640, 180);
        editor.show();
        auto cursor = editor.textCursor();
        cursor.setPosition(start + 1);
        editor.setTextCursor(cursor);
        QSignalSpy requested(&editor, &CodeEditor::callGraphRequested);
        bool found = false;
        QTimer::singleShot(50, &editor, [&] {
            auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (auto action : menu->actions())
                if (action->text().contains("查看函数调用图") && action->text().contains('G')) {
                    found = action->isEnabled();
                    action->trigger();
                    break;
                }
            menu->close();
        });
        const QPoint position = editor.cursorRect(cursor).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position,
                                editor.viewport()->mapToGlobal(position));
        QCoreApplication::sendEvent(editor.viewport(), &event);
        QVERIFY(found);
        QCOMPARE(requested.count(), 1);
    }
    void browseSwitchFindRenameAndReopen() {
        QSettings().clear();
        const QString fixtures = qEnvironmentVariable("GARLIC_TEST_FIXTURES");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        const bool dex = QFileInfo::exists(fixtures + "/classes.dex");
        window.openPath(fixtures + (dex ? "/classes.dex" : "/demo.jar"));
        auto tree = window.findChild<QTreeView *>("classTree");
        auto filter = window.findChild<QLineEdit *>("classFilter");
        auto tabs = window.findChild<QTabWidget *>("sourceTabs");
        auto find = window.findChild<QLineEdit *>("codeFind");
        auto findBar = window.findChild<QWidget *>("codeFindBar");
        auto showFind = window.findChild<QAction *>("showCodeFind");
        QVERIFY(tree && filter && tabs && find && findBar && showFind);
        QVERIFY(!findBar->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        filter->setText("Main");
        QVERIFY(tree->model()->rowCount() > 0);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        QCOMPARE(tabs->count(), 1);
        auto code = window.editor();
        showFind->trigger();
        QVERIFY(findBar->isVisible());
        find->setText("greet");
        QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
        QTRY_VERIFY(code->extraSelections().size() > 1);
        find->setText("not_present_123");
        QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
        QTest::keyClick(find, Qt::Key_Escape);
        QVERIFY(!findBar->isVisible());
        if (dex) {
            auto modes = tabs->currentWidget()->findChild<QTabWidget *>("codeModes");
            QVERIFY(modes);
            QCOMPARE(modes->tabPosition(), QTabWidget::South);
            modes->setCurrentIndex(1);
            QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains(".class"), 15000);
            QCOMPARE(tabs->count(), 1);
            modes->setCurrentIndex(0);
            QVERIFY(window.editor()->toPlainText().contains("public class Main"));
        }
        const QString id = "Ldemo/Main;->greet(I)Ljava/lang/String;";
        window.navigateTo(id);
        QTRY_COMPARE(window.editor()->textCursor().selectedText(), QString("greet"));
        auto classTree = window.findChild<QTreeView *>("classTree");
        const QPersistentModelIndex stableRoot(classTree->model()->index(0, 0));
        classTree->expand(stableRoot);
        QVERIFY(window.backend()->project()->rename(id, "welcome").isEmpty());
        QVERIFY(stableRoot.isValid());
        QVERIFY(classTree->isExpanded(stableRoot));
        auto settings = window.backend()->settings(); settings.theme = "light";
        window.applySettings(settings);
        QVERIFY(stableRoot.isValid());
        QVERIFY(classTree->isExpanded(stableRoot));
        QTRY_VERIFY(window.editor()->toPlainText().contains("welcome(int"));
        QVERIFY(window.editor()->toPlainText().contains("greet(String"));
        window.backend()->project()->undoRename();
        QTRY_VERIFY(window.editor()->toPlainText().contains("greet(int"));
        window.openPath(fixtures + "/Main.class");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        QCOMPARE(tabs->count(), 0);
        QVERIFY(filter->text().isEmpty());
        window.close();
    }
};
QTEST_MAIN(WindowTest)
#include "window_test.moc"
