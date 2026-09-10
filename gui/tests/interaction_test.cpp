#include "mainwindow.h"
#include "referencesdialog.h"
#include "resources.h"
#include "searchdialog.h"
#include <QtTest>
#include <QtWidgets>
class InteractionTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("InteractionTest");
        QSettings().clear();
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
        auto tree = window.findChild<QTreeView *>();
        QVERIFY(tree);
        auto root = tree->model()->index(0, 0);
        QCOMPARE(tree->model()->index(0, 0, root).data().toString(), QString("输入"));
        QCOMPARE(tree->model()->index(1, 0, root).data().toString(), QString("源代码"));
        QCOMPARE(tree->model()->index(2, 0, root).data().toString(), QString("资源文件"));
        auto info = Resources::inspect(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QVERIFY(!info.value("entries").toArray().isEmpty());
        QTemporaryDir unicodeArchive;
        const auto unicodePath = unicodeArchive.path()+"/中文 resources.jar";
        QVERIFY(QFile::copy(qEnvironmentVariable("GARLIC_TEST_FIXTURES")+"/demo.jar", unicodePath));
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
            checked = nav && nav->count() == 6 && nav->item(0)->text() == "反编译";
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
        QVERIFY2(signature.contains("证书 MD5") && signature.contains("RSA 指数: 65537") && signature.contains("v1 覆盖范围"), qPrintable(signature.left(2500)));
        QString error;
        const auto table = Resources::describeTable(
            Resources::read(path, "resources.arsc", 32 * 1024 * 1024, &error));
        QVERIFY2(!table.contains("解析失败"), qPrintable(table.left(500)));
        QVERIFY(table.contains("string/"));
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
        qInfo() << "Application candidates:" << window.backend()->project()->applicationCandidates();
        QElapsedTimer refsTimer;
        refsTimer.start();
        window.showReferences("Landroidx/activity/OnBackPressedCallback;");
        auto refsDialog = window.findChild<ReferencesDialog *>();
        QVERIFY(refsDialog);
        auto refsTable = refsDialog->findChild<QTableView *>("referenceResults");
        QTRY_VERIFY_WITH_TIMEOUT(refsTable->model()->rowCount() > 0, 3000);
        qInfo() << "Reference first results (ms):" << refsTimer.elapsed();
        refsTimer.restart();
        QVERIFY(!window.backend()->project()->xrefs("Landroidx/activity/OnBackPressedCallback;").isEmpty());
        qInfo() << "Warm reference lookup (ms):" << refsTimer.elapsed();
        refsDialog->close();
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
                QVERIFY(QMetaObject::invokeMethod(tree, "activated", Q_ARG(QModelIndex, nodes.first())));
                auto tabs = window.findChild<QTabWidget *>("sourceTabs");
                auto report = tabs->currentWidget()->findChild<QTextBrowser *>("overviewReport");
                QVERIFY(report);
                QTRY_VERIFY_WITH_TIMEOUT(!report->toPlainText().contains("正在读取"), 15000);
                window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/" + kind + ".png");
            }
        }
        window.close();
    }
};
QTEST_MAIN(InteractionTest)
#include "interaction_test.moc"
