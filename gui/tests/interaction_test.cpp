#include "mainwindow.h"
#include "hookcode.h"
#include "elfsummary.h"
#include "nativeanalysisdialog.h"
#include "referencesdialog.h"
#include "resources.h"
#include "searchdialog.h"
#include <QtTest>
#include <QtWidgets>
class InteractionTest : public QObject {
    Q_OBJECT
  private slots:
    void clickSelectAndReferencePosition() {
        CodeEditor editor(false);
        const QString text = "int X = 0;\nX += X;\n";
        editor.setSource({text, {{4, 5, "local:X", true}, {11, 12, "local:X", false}, {16, 17, "local:X", false}}});
        editor.resize(500, 300); editor.show();
        QVERIFY(editor.goToSymbol("local:X", 2));
        QCOMPARE(editor.textCursor().selectionStart(), 11);
        QCOMPARE(editor.textCursor().selectedText(), QString("X"));
        QTextCursor cursor(editor.document()); cursor.setPosition(4);
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, {}, editor.cursorRect(cursor).center());
        QCOMPARE(editor.textCursor().selectedText(), QString("X"));
        QVERIFY(editor.extraSelections().size() >= 4);
    }
    void exactResultNavigation() {
        CodeEditor editor(false);
        const QString text = "@Override\nvoid run() {\n  X += X;\n  X += X;\n}\n";
        const int first = text.indexOf("X"), second = text.indexOf("X", first + 1);
        editor.setSource({text, {{first, first + 1, "local:X", false},
                                {second, second + 1, "local:X", false}}});
        QVERIFY(editor.goToHit({{"symbol", "local:X"}, {"occurrence", 1}}));
        QCOMPARE(editor.textCursor().selectionStart(), second);
        QVERIFY(editor.goToHit({{"sourceLine", "  X += X;"}, {"lineOccurrence", 1},
                               {"column", 7}, {"length", 1}}));
        QCOMPARE(editor.textCursor().selectionStart(), text.lastIndexOf("X"));
        QCOMPARE(editor.textCursor().selectedText(), QString("X"));
        QVERIFY(!editor.goToHit({{"sourceLine", "stale result"}, {"column", 0}, {"length", 5}}));
        editor.setSource({"Foo.call();\r\n", {{4, 8, "LFoo;->call()V", false}}});
        QVERIFY(!editor.goToSymbol("LFoo;", 1));
        QVERIFY(editor.goToHit({{"sourceLine", "Foo.call();\r"}, {"column", 4}, {"length", 4}}));
        QCOMPARE(editor.textCursor().selectedText(), QString("call"));
        editor.setSource({"void a(){ X; } void b(){ X; }",
                          {{5, 6, "LC;->a()V", true}, {10, 11, "local:X", false},
                           {19, 20, "LC;->b()V", true}, {24, 25, "local:X", false}}});
        QVERIFY(editor.goToHit({{"symbol", "local:X"}, {"scope", "LC;->b()V"}}));
        QCOMPARE(editor.textCursor().selectionStart(), 24);
    }
    void ghidraDecompile() {
        const auto home = qEnvironmentVariable("GARLIC_TEST_GHIDRA_HOME");
        const auto input = qEnvironmentVariable("GARLIC_TEST_NATIVE_ELF");
        if (home.isEmpty() || input.isEmpty()) QSKIP("Set Ghidra home and an ELF fixture for real headless decompilation");
        QSettings().setValue("native/ghidraHome", home);
        QSettings().setValue("native/backend", 1);
        NativeAnalysisDialog dialog(qEnvironmentVariable("GARLIC_TEST_ENGINE"), input);
        dialog.show();
        auto tabs = dialog.findChild<QTabWidget *>();
        auto pseudo = [&] { for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i).contains("伪代码")) return i; return -1; };
        auto failed = [&] { for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i) == "错误") return true; return false; };
        QTRY_VERIFY_WITH_TIMEOUT(pseudo() >= 0 || failed(), 180000);
        if (failed()) {
            for (auto editor : dialog.findChildren<QPlainTextEdit *>()) qWarning().noquote() << editor->toPlainText();
        }
        QVERIFY(pseudo() >= 0);
        tabs->setCurrentIndex(pseudo());
        auto editor = qobject_cast<QPlainTextEdit *>(tabs->currentWidget());
        QVERIFY(editor);
        QTRY_VERIFY_WITH_TIMEOUT(editor->toPlainText().contains("return"), 10000);
        QSettings().remove("native/backend");
        QSettings().remove("native/ghidraHome");
    }
    void elfHeaders() {
        QTemporaryDir dir;
        QFile file(dir.filePath("中文.elf"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray header(64, '\0');
        header.replace(0, 4, QByteArray::fromHex("7f454c46"));
        header[4] = 2; header[5] = 1; header[18] = char(183); header[24] = 0x40;
        file.write(header); file.close();
        auto text = ElfSummary::read(file.fileName());
        QVERIFY(text.contains("AArch64")); QVERIFY(text.contains("0x40"));
        header[5] = 2; header[18] = 0; header[19] = 62; header[24] = 0; header[31] = 0x40;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(header); file.close();
        text = ElfSummary::read(file.fileName());
        QVERIFY(text.contains("x86-64")); QVERIFY(text.contains("0x40"));
        header[40] = char(0xff); // Out-of-file table, never allocate from this offset.
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(header); file.close();
        QVERIFY(ElfSummary::read(file.fileName()).contains("节表损坏"));
    }
    void hookTemplates() {
        const QString id = "Ldemo/Outer$Inner;->call(I[Ljava/lang/String;[[I)Ljava/lang/Object;";
        const auto frida = HookCode::generate(id, false);
        QVERIFY(frida.contains("Java.use(\"demo.Outer$Inner\")"));
        QVERIFY(frida.contains(".overload(\"int\", \"[Ljava.lang.String;\", \"[[I\")"));
        QVERIFY(frida.contains("method.call(this, arg0, arg1, arg2)"));
        const auto xposed = HookCode::generate(id, true);
        QVERIFY(xposed.contains("findAndHookMethod(\"demo.Outer$Inner\", classLoader, \"call\", int.class"));
        QVERIFY(HookCode::generate("Ldemo/Test;-><init>()V", false).contains("[\"$init\"].overload()"));
        QVERIFY(HookCode::generate("Ldemo/Test;-><init>()V", true).contains("classLoader, new XC_MethodHook"));
        QVERIFY(HookCode::generate("Ldemo/Test;-><clinit>()V", true).isEmpty());
        QVERIFY(HookCode::generate("Ldemo/Test;->bad([)V", false).isEmpty());
    }
    void initTestCase() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("InteractionTest");
        QSettings().clear();
    }
    void resourceQualifiers() {
        QByteArray config(60, 0);
        QCOMPARE(Resources::configurationName(config), QString());
        config[24] = char(0x80);
        QCOMPARE(Resources::configurationName(config), QString("-ldrtl"));
        config.fill(0);
        config.replace(4, 2, "sr");
        config.replace(32, 4, "Latn");
        QCOMPARE(Resources::configurationName(config), QString("-b+sr+Latn"));
        config.fill(0);
        config.replace(4, 2, "en");
        config.replace(6, 2, "AU");
        QCOMPARE(Resources::configurationName(config), QString("-en-rAU"));
        config.fill(0);
        config[8] = 2;
        config[26] = char(0x58);
        config[27] = 2;
        config[25] = 0x20;
        QCOMPARE(Resources::configurationName(config), QString("-sw600dp-land-night"));
        config.fill(0);
        config[4] = char(0xad);
        config[5] = char(0x05); // packed fil
        QCOMPARE(Resources::configurationName(config), QString("-fil"));
        config.fill(0);
        config[44] = 2;
        config[45] = 0x0a;
        QCOMPARE(Resources::configurationName(config), QString("-round-highdr-widecg"));
    }
    void largeResourceTable() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_LARGE_APK");
        if (path.isEmpty() || !path.endsWith(".apk"))
            QSKIP("Set a real APK for resource directory validation");
        QString error;
        QMap<QString, QString> files;
        auto data = Resources::read(path, "resources.arsc", 128LL * 1024 * 1024, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        Resources::describeTable(data, &files);
        QVERIFY(!files.isEmpty());
        for (auto it = files.begin(); it != files.end(); ++it) {
            QVERIFY(it.key().startsWith("res/values"));
            QVERIFY(!it.key().contains("config-"));
        }
        qInfo() << "Resource files:" << files.size() << "directories:" << files.keys().mid(0, 8);
    }
    void launcherAliases() {
        const QString manifest =
            "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" "
            "package=\"demo\">"
            "        <application><activity android:name=\".NotLauncher\"><intent-filter><action "
            "android:name=\"android.intent.action.MAIN\"/></intent-filter>"
            "        <intent-filter><category "
            "android:name=\"android.intent.category.LAUNCHER\"/></intent-filter></activity>"
            "        <activity-alias android:name=\".Alias\" "
            "android:targetActivity=\".Main\"><intent-filter>"
            "        <action android:name=\"android.intent.action.MAIN\"/><category "
            "android:name=\"android.intent.category.LAUNCHER\"/>"
            "        </intent-filter></activity-alias><activity android:name=\"Disabled\" "
            "android:enabled=\"false\"><intent-filter>"
            "        <action android:name=\"android.intent.action.MAIN\"/><category "
            "android:name=\"android.intent.category.LAUNCHER\"/>"
            "        </intent-filter></activity></application></manifest>";
        QCOMPARE(Resources::launcherActivities(manifest), QStringList{"demo.Main"});
        QVERIFY(Resources::launcherActivities(manifest.left(40)).isEmpty());
    }
    void searchIndexCache() {
        QTemporaryDir dir;
        QDir().mkpath(dir.path() + "/demo");
        const auto path = dir.path() + "/demo/Main.java";
        auto write = [&](const QByteArray &text) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(text);
        };
        write("class Main {\n void greet() {} // commentOnly\n}\n");
        auto project = std::make_shared<Project>();
        project->addClass({{"name", "demo/Main"}});
        auto index = std::make_shared<SearchIndex>();
        auto generating = std::make_shared<std::atomic_bool>(false);
        auto events = std::make_shared<SearchEvents>();
        SearchOptions options;
        options.query = "greet";
        auto run = [&](bool cached) {
            return searchProject(project, options, dir.path(), false, generating,
                                 std::make_shared<SearchControl>(), events, 1,
                                 cached ? index : nullptr);
        };
        auto cold = run(true);
        QCOMPARE(cold.hits.size(), 1);
        QCOMPARE(cold.cachedFiles, 0);
        auto warm = run(true);
        QCOMPARE(warm.hits, cold.hits);
        QCOMPARE(warm.cachedFiles, 1);
        options.query = "totallyMissing";
        auto rejected = run(true);
        QCOMPARE(rejected.indexRejected, 1);
        QVERIFY(rejected.hits.isEmpty());
        options.query = "commentOnly";
        QVERIFY(run(true).hits.isEmpty());
        options.code = false;
        options.comments = true;
        QCOMPARE(run(true).hits.size(), 1);
        QCOMPARE(index->filters.size(), 1); // Scope switches reuse the same superset filter.
        options.code = true;
        options.comments = false;
        options.regex = true;
        options.query = "gr.*t";
        QCOMPARE(run(true).hits, run(false).hits);
        options.regex = false;
        options.query = "updated";
        write("class Main { void updated() {} }\n");
        auto changed = run(true);
        QCOMPARE(changed.hits.size(), 1);
        QCOMPARE(changed.cachedFiles, 0);
        write("abc/**/def\n");
        options.query = "abc    def";
        QCOMPARE(run(true).hits.size(), 1); // Masked spaces must not cause false rejection.
    }
    void referencesAndInputsTree() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        window.showReferences("Ldemo/Main;->greet(I)Ljava/lang/String;");
        auto dialog = window.findChild<ReferencesDialog *>();
        QVERIFY(dialog);
        auto table = dialog->findChild<QTableView *>("referenceResults");
        QVERIFY(table);
        QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() > 0, 15000);
        QVERIFY2(table->model()->index(0, 1).data().toString().contains("greet"),
                 qPrintable(table->model()->index(0, 1).data().toString()));
        QTRY_VERIFY_WITH_TIMEOUT(table->model()->index(0, 0).data(Qt::UserRole + 2).toInt() > 0, 15000);
        QVERIFY(!table->model()->index(0, 1).data(Qt::UserRole + 3).toJsonArray().isEmpty());
        auto tree = window.findChild<QTreeView *>();
        QVERIFY(tree);
        auto root = tree->model()->index(0, 0);
        QCOMPARE(tree->model()->index(0, 0, root).data().toString(), QString("输入"));
        QCOMPARE(tree->model()->index(1, 0, root).data().toString(), QString("源代码"));
        QCOMPARE(tree->model()->index(2, 0, root).data().toString(), QString("资源文件"));
        auto info = Resources::inspect(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QVERIFY(!info.value("entries").toArray().isEmpty());
        const auto zipPath = qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.zip";
        QVERIFY(!Resources::inspect(zipPath).value("entries").toArray().isEmpty());
        QString zipError;
        QCOMPARE(Resources::read(zipPath, "assets/readme.txt", 1024, &zipError),
                 QByteArray("ZIP resource preview"));
        QVERIFY(zipError.isEmpty());
        QTemporaryDir unicodeArchive;
        const auto unicodePath = unicodeArchive.path() + "/中文 resources.jar";
        QVERIFY(
            QFile::copy(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar", unicodePath));
        QVERIFY(!Resources::inspect(unicodePath).value("entries").toArray().isEmpty());
        QString error;
        auto bytes = Resources::read(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar",
                                     "demo/Main.class", 1, &error);
        QVERIFY(bytes.isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(Resources::decodeXml(QByteArray::fromHex("0300")).contains("失败"));
        QTRY_VERIFY_WITH_TIMEOUT(!tree->model()
                                      ->match(root, Qt::UserRole + 6, "layout/main.xml", 1,
                                              Qt::MatchExactly | Qt::MatchRecursive)
                                      .isEmpty(),
                                 5000);
        auto resource = tree->model()
                            ->match(root, Qt::UserRole + 6, "layout/main.xml", 1,
                                    Qt::MatchExactly | Qt::MatchRecursive)
                            .first();
        QVERIFY(QMetaObject::invokeMethod(tree, "activated", Q_ARG(QModelIndex, resource)));
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("中文 resource preview"),
            5000);
        auto settings = window.backend()->settings();
        settings.theme = "light";
        window.applySettings(settings);
        QVERIFY(window.editor()->styleSheet().contains("#ffffff"));
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty()) {
            dialog->grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/references.png");
            window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/resource.png");
        }
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("class Main"), 15000);
        auto tabs = window.findChild<QTabWidget *>("sourceTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2);
        bool closedOthers = false;
        QTimer::singleShot(50, &window, [&] {
            auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (auto action : menu->actions())
                if (action->text() == "关闭其他") {
                    action->trigger();
                    closedOthers = true;
                    break;
                }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(tabs->tabBar(), "customContextMenuRequested",
                                          Q_ARG(QPoint, tabs->tabBar()->tabRect(1).center())));
        QVERIFY(closedOthers);
        QCOMPARE(tabs->count(), 1);
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/Main.class");
        QVERIFY(window.findChildren<ReferencesDialog *>().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!window.backend()->busy(), 15000);
    }
    void preferencesThemeAndSearch() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        auto settings = window.backend()->settings();
        settings.theme = "light";
        window.applySettings(settings);
        QVERIFY(window.editor()->styleSheet().contains("#ffffff"));
        QVERIFY(window.editor()->document()->property("lightTheme").toBool());
        auto block = window.editor()->document()->findBlockByNumber(0);
        QVERIFY(block.isValid());
        bool checked = false;
        QTimer::singleShot(50, &window, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto nav = dialog->findChild<QListWidget *>("preferencesNavigation");
            checked = nav && nav->count() == 7 && nav->item(0)->text() == "反编译" &&
                dialog->findChild<QLineEdit *>("pythonPath") && dialog->findChild<QLineEdit *>("nodePath");
            if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty())
                dialog->grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") +
                                    "/preferences.png");
            if (nav && !qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty()) {
                nav->setCurrentRow(1);
                QTimer::singleShot(150, dialog, [dialog] {
                    dialog->grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/cache.png");
                    dialog->reject();
                });
            } else
                dialog->reject();
        });
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "设置…") {
                action->trigger();
                break;
            }
        QVERIFY(checked);
        SearchDialog search(&window);
        auto resources = search.findChild<QCheckBox *>("searchResources");
        QVERIFY(resources && !resources->isChecked());
        resources->setChecked(true);
        { SearchDialog restored(&window); QVERIFY(restored.findChild<QCheckBox *>("searchResources")->isChecked()); }
        resources->setChecked(false);
        search.show();
        auto query = search.findChild<QLineEdit *>("projectQuery");
        auto table = search.findChild<QTableView *>("searchResults");
        QVERIFY(query && table);
        query->setText("greet");
        search.startSearch();
        QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() >= 2, 15000);
        auto package = search.findChild<QLineEdit *>("searchPackage");
        package->setText("does.not.exist");
        search.startSearch();
        QTest::qWait(200);
        QCOMPARE(table->model()->rowCount(), 0);
        package->clear();
        query->setText("literal greet should not change");
        search.startSearch();
        QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() > 0, 30000);
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty()) {
            search.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/search.png");
            window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/light-editor.png");
        }
        auto hit = table->model()->index(0, 0).data(Qt::UserRole).toJsonObject();
        QCOMPARE(hit.value("class").toString(), QString("demo/Use"));
        search.close();
        window.close();
    }
    void scopesRegexAndCancellation() {
        Backend backend;
        backend.setEngine(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        backend.open(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 15000);
        QSignalSpy completed(&backend, &Backend::searchCompleted);
        SearchOptions options;
        options.code = false;
        options.methods = true;
        options.query = "^greet$";
        options.regex = true;
        backend.search(options);
        QTRY_COMPARE(completed.count(), 1);
        auto result = qvariant_cast<SearchResult>(completed.last()[1]);
        QCOMPARE(result.hits.size(), 3);
        options.methods = false;
        options.fields = true;
        options.query = "name";
        options.regex = false;
        backend.search(options);
        QTRY_COMPARE(completed.count(), 2);
        result = qvariant_cast<SearchResult>(completed.last()[1]);
        QCOMPARE(result.hits.size(), 1);
        options.package = "not.demo";
        backend.search(options);
        QTRY_COMPARE(completed.count(), 3);
        QVERIFY(qvariant_cast<SearchResult>(completed.last()[1]).hits.isEmpty());
        options.package.clear();
        options.regex = true;
        options.query = "[";
        backend.search(options);
        QTRY_COMPARE(completed.count(), 4);
        QVERIFY(!qvariant_cast<SearchResult>(completed.last()[1]).error.isEmpty());
        options.query = "greet";
        options.code = true;
        int request = backend.search(options);
        backend.cancelSearch(request);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 5, 15000);
        QVERIFY(qvariant_cast<SearchResult>(completed.last()[1]).canceled);
    }
    void realApkResources() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_REAL_APK");
        if (path.isEmpty())
            QSKIP("Set GARLIC_TEST_REAL_APK for real resource validation");
        auto info = Resources::inspect(path);
        QVERIFY2(!info.value("package").toString().isEmpty(),
                 qPrintable(info.value("manifest").toString().left(300)));
        QXmlStreamReader xml(info.value("manifest").toString());
        while (!xml.atEnd())
            xml.readNext();
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        const auto signature = Resources::signature(path);
        QVERIFY2(signature.contains("证书 MD5") && signature.contains("RSA 指数: 65537") &&
                     signature.contains("v1 覆盖范围"),
                 qPrintable(signature.left(2500)));
        QString error;
        QMap<QString, QString> files;
        const auto table = Resources::describeTable(
            Resources::read(path, "resources.arsc", 32 * 1024 * 1024, &error), &files);
        QVERIFY(!files.isEmpty());
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QXmlStreamReader decoded(it.value());
            while (!decoded.atEnd())
                decoded.readNext();
            QVERIFY(it.key().startsWith("res/"));
            QVERIFY(!it.key().contains("config-"));
            QVERIFY2(!decoded.hasError(), qPrintable(it.key() + ": " + decoded.errorString()));
        }
        qInfo() << "Decoded resource files:" << files.size();
        qInfo() << "Launcher activities:" << info.value("main_activities");
        QVERIFY2(!table.contains("解析失败"), qPrintable(table.left(500)));
        QVERIFY(table.contains("string/"));
    }
    void realReferenceLatency() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_REAL_APK");
        if (path.isEmpty()) QSKIP("Set GARLIC_TEST_REAL_APK for large reference latency");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = window.backend()->settings();
        settings.background = false;
        window.backend()->configure(settings);
        window.show();
        window.openPath(path);
        QTRY_VERIFY_WITH_TIMEOUT(!window.backend()->busy() &&
            !window.backend()->project()->classes().isEmpty(), 120000);
        const QString target = "Landroidx/activity/OnBackPressedCallback;";
        QElapsedTimer elapsed;
        elapsed.start();
        window.showReferences(target);
        auto dialog = window.findChild<ReferencesDialog *>();
        QVERIFY(dialog);
        auto table = dialog->findChild<QTableView *>("referenceResults");
        QTRY_VERIFY_WITH_TIMEOUT(table->model()->rowCount() > 0, 10000);
        const auto firstMs = elapsed.elapsed();
        elapsed.restart();
        QVERIFY(!window.backend()->project()->xrefs(target).isEmpty());
        const auto warmMs = elapsed.elapsed();
        qInfo() << "First displayed reference / warm lookup ms:" << firstMs << warmMs;
        QVERIFY2(firstMs < 1000, qPrintable(QString("First references took %1 ms").arg(firstMs)));
        QVERIFY(warmMs < 1000);
        dialog->close();
    }
    void realApkResponsiveness() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_REAL_APK");
        if (path.isEmpty())
            QSKIP("Set GARLIC_TEST_REAL_APK to run the local large-project responsiveness check");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        QElapsedTimer clock;
        clock.start();
        qint64 last = 0, maxGap = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, &window, [&] {
            auto now = clock.elapsed();
            maxGap = qMax(maxGap, now - last);
            last = now;
        });
        heartbeat.start();
        window.openPath(path);
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 60000);
        qInfo() << "Application candidates:"
                << window.backend()->project()->applicationCandidates();
        QElapsedTimer refsTimer;
        refsTimer.start();
        window.showReferences("Landroidx/activity/OnBackPressedCallback;");
        auto refsDialog = window.findChild<ReferencesDialog *>();
        QVERIFY(refsDialog);
        auto refsTable = refsDialog->findChild<QTableView *>("referenceResults");
        QTRY_VERIFY_WITH_TIMEOUT(refsTable->model()->rowCount() > 0, 3000);
        qInfo() << "Reference first results (ms):" << refsTimer.elapsed();
        refsTimer.restart();
        QVERIFY(!window.backend()
                     ->project()
                     ->xrefs("Landroidx/activity/OnBackPressedCallback;")
                     .isEmpty());
        qInfo() << "Warm reference lookup (ms):" << refsTimer.elapsed();
        refsDialog->close();
        window.findChild<QAction *>("mainActivity")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() &&
                                     window.editor()->toPlainText().contains("class MainActivity"),
                                 15000);
        window.findChild<QAction *>("syncEditor")->trigger();
        QCOMPARE(window.findChild<QTreeView *>("classTree")
                     ->currentIndex()
                     .data(Qt::UserRole + 1)
                     .toString(),
                 QString("Lcom/nobi/mmsd/offline/MainActivity;"));
        window.openClass("androidx/activity/ComponentActivity$$ExternalSyntheticLambda0");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() &&
                                     window.editor()->toPlainText().contains("implements Runnable"),
                                 30000);
        QVERIFY(window.editor()->toPlainText().contains("classes"));
        window.openClass("androidx/activity/OnBackPressedCallback");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("@Metadata"), 30000);
        QSignalSpy completed(window.backend(), &Backend::searchCompleted);
        SearchOptions options;
        options.query = "OnBackPressed";
        options.package = "androidx.activity";
        options.limit = 200;
        window.backend()->search(options);
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 90000);
        auto result = qvariant_cast<SearchResult>(completed.last()[1]);
        QVERIFY(!result.hits.isEmpty());
        qInfo() << "Classes:" << window.backend()->project()->classes().size()
                << "Search hits:" << result.hits.size() << "Max UI heartbeat gap (ms):" << maxGap;
        QVERIFY2(maxGap < 350, qPrintable(QString("UI event-loop gap was %1 ms").arg(maxGap)));
        heartbeat.stop();
        const auto snapshot = window.backend()->project()->snapshot();
        const auto index = std::make_shared<SearchIndex>();
        const auto events = std::make_shared<SearchEvents>();
        const auto control = std::make_shared<SearchControl>();
        const auto generating = std::make_shared<std::atomic_bool>(false);
        QElapsedTimer searchClock;
        searchClock.start();
        auto cold =
            searchProject(snapshot, options, window.backend()->workspacePath() + "/all-java", false,
                          generating, control, events, 1, index);
        const auto coldMs = searchClock.elapsed();
        searchClock.restart();
        auto warm =
            searchProject(snapshot, options, window.backend()->workspacePath() + "/all-java", false,
                          generating, control, events, 2, index);
        QCOMPARE(warm.hits, cold.hits);
        QVERIFY(warm.cachedFiles > 0);
        qInfo() << "Search cold/warm (ms):" << coldMs << searchClock.elapsed()
                << "cached files:" << warm.cachedFiles << "index rejected:" << warm.indexRejected;

        auto resourceTree = window.findChild<QTreeView *>("classTree");
        auto resourceTables = resourceTree->model()->match(resourceTree->model()->index(0, 0),
                                                           Qt::UserRole + 4, "resource-table", 1,
                                                           Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY(!resourceTables.isEmpty());
        auto tableIndex = resourceTables.first();
        resourceTree->expand(tableIndex.parent());
        resourceTree->expand(tableIndex);
        QModelIndexList decoded;
        QTRY_VERIFY_WITH_TIMEOUT(!(decoded = resourceTree->model()->match(
                                       tableIndex, Qt::UserRole + 4, "decoded-resource", 1,
                                       Qt::MatchExactly | Qt::MatchRecursive))
                                      .isEmpty(),
                                 15000);
        QVERIFY(QMetaObject::invokeMethod(resourceTree, "activated",
                                          Q_ARG(QModelIndex, decoded.first())));
        auto sourceTabs = window.findChild<QTabWidget *>("sourceTabs");
        auto preview = sourceTabs->currentWidget()->findChild<CodeEditor *>();
        QVERIFY(preview);
        QTRY_VERIFY_WITH_TIMEOUT(preview->toPlainText().contains("<resources>"), 5000);
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty()) {
            resourceTree->scrollTo(decoded.first());
            window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/resource-table.png");
        }
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty()) {
            auto settings = window.backend()->settings();
            settings.theme = "light";
            settings.showMemory = true;
            window.applySettings(settings);
            auto tree = window.findChild<QTreeView *>();
            for (const QString kind : {QString("summary"), QString("signature")}) {
                auto nodes = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 4,
                                                  kind, 1, Qt::MatchExactly | Qt::MatchRecursive);
                QVERIFY(!nodes.isEmpty());
                QVERIFY(QMetaObject::invokeMethod(tree, "activated",
                                                  Q_ARG(QModelIndex, nodes.first())));
                auto tabs = window.findChild<QTabWidget *>("sourceTabs");
                auto report = tabs->currentWidget()->findChild<QTextBrowser *>("overviewReport");
                QVERIFY(report);
                QTRY_VERIFY_WITH_TIMEOUT(!report->toPlainText().contains("正在读取"), 15000);
                window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/" + kind +
                                   ".png");
            }
        }
        window.close();
    }
};
QTEST_MAIN(InteractionTest)
#include "interaction_test.moc"
