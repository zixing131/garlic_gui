#include "mainwindow.h"
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
        window.close();
    }
};
QTEST_MAIN(InteractionTest)
#include "interaction_test.moc"
