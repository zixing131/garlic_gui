#include "backend.h"
#include <QDirIterator>
#include <QFile>
#include <QtTest>

class BackendTest : public QObject {
    Q_OBJECT
    QString fixtures_ = qEnvironmentVariable("GARLIC_TEST_FIXTURES");
    QString engine_ = qEnvironmentVariable("GARLIC_TEST_ENGINE");
  private slots:
    void multipleInputs() {
        Backend backend;
        backend.setEngine(engine_);
        const QStringList inputs{fixtures_ + "/Main.class", fixtures_ + "/demo.jar"};
        QSignalSpy indexed(&backend, &Backend::indexed), source(&backend, &Backend::sourceReady),
            errors(&backend, &Backend::failed);
        backend.openPaths(inputs);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 15000);
        QCOMPARE(indexed.count(), 1);
        QVERIFY(errors.isEmpty());
        QCOMPARE(backend.inputs(), inputs);
        QCOMPARE(backend.classInput("demo/Main"), inputs.last());
        QVERIFY(backend.project()->classes().size() >= 8);
        backend.request("demo/Use", false);
        QTRY_COMPARE_WITH_TIMEOUT(source.count(), 1, 15000);
        QTemporaryDir save;
        QString error;
        QVERIFY(backend.project()->save(save.path() + "/project.json", &error));
        QVERIFY2(backend.project()->loadAliases(save.path() + "/project.json", &error),
                 qPrintable(error));
        backend.clearCache();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 5000);
        QVERIFY(backend.cachedPath("demo/Use", false).isEmpty());
        if (QFileInfo::exists(fixtures_ + "/classes.dex")) {
            backend.openPaths({inputs.last(), fixtures_ + "/classes.dex"});
            QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 15000);
            QVERIFY(backend.supportsSmali("demo/Main"));
            backend.request("demo/Main", true);
            QTRY_COMPARE_WITH_TIMEOUT(source.count(), 2, 15000);
        }
    }
    void memoryCache() {
        Backend backend;
        backend.setEngine(engine_);
        auto settings = backend.settings();
        settings.cacheMode = "memory";
        backend.configure(settings);
        QSignalSpy source(&backend, &Backend::sourceReady), errors(&backend, &Backend::failed);
        backend.open(fixtures_ + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 15000);
        backend.request("demo/Main", false);
        QTRY_COMPARE_WITH_TIMEOUT(source.count(), 1, 15000);
        QVERIFY(errors.isEmpty());
        auto path = source.first()[2].toString();
        QVERIFY(path.startsWith("memory:"));
        QVERIFY(backend.project()->document("demo/Main", false, path).text.contains("greet"));
        QVERIFY(backend.cacheStats().value("source_bytes").toDouble() > 0);
        QDirIterator files(backend.workspacePath(), {"*.java"}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(!files.hasNext());
        backend.request("demo/Main", false);
        QCOMPARE(source.count(), 2);
        backend.clearCache();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 5000);
        QVERIFY(backend.cachedPath("demo/Main", false).isEmpty());
    }
    void safeNames() {
        QVERIFY(Backend::safeClassName("demo/Main$Inner"));
        QVERIFY(Backend::safeClassName("测试/示例"));
        for (const QString &name :
             {"../evil", "/absolute", "a//b", "a/../b", "C:/file", "a\\b", "", "a\nb"})
            QVERIFY(!Backend::safeClassName(name));
    }
    void realFiles_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<bool>("smali");
        QTest::newRow("jar") << "demo.jar" << false;
        QTest::newRow("zip") << "demo.zip" << false;
        QTest::newRow("class") << "Main.class" << false;
        if (QFileInfo::exists(fixtures_ + "/classes.dex")) {
            QTest::newRow("dex") << "classes.dex" << true;
            QTest::newRow("apk-unicode-space") << "示例 app.apk" << true;
        }
    }
    void realFiles() {
        QFETCH(QString, file);
        QFETCH(bool, smali);
        Backend backend;
        backend.setEngine(engine_);
        QSignalSpy indexed(&backend, &Backend::indexed), source(&backend, &Backend::sourceReady);
        QSignalSpy errors(&backend, &Backend::failed), exported(&backend, &Backend::exported);
        backend.open(fixtures_ + "/" + file);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 15000);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        QCOMPARE(indexed.count(), 1);
        const auto names = indexed.first().first().toStringList();
        QVERIFY(names.contains("demo/Main"));
        QCOMPARE(names.contains("demo/Main$Details"), file != "Main.class");
        backend.request("demo/Main", false);
        QTRY_COMPARE_WITH_TIMEOUT(source.count(), 1, 15000);
        QFile code(source.first().at(2).toString());
        QVERIFY(code.open(QIODevice::ReadOnly));
        const auto text = code.readAll();
        QVERIFY(text.contains("class Main"));
        QVERIFY(text.contains("greet"));
        if (file != "Main.class")
            QVERIFY(text.contains("Details"));
        const auto sourcePath = code.fileName();
        const QDir sourceDir(QFileInfo(sourcePath).absolutePath());
        QVERIFY(!QFileInfo::exists(sourceDir.filePath("Extra.java")));
        backend.request("demo/Main", false); // Disk cache hit, no process launch.
        QCOMPARE(source.count(), 2);
        QVERIFY(!backend.busy());
        QCOMPARE(source.at(1).at(2).toString(), sourcePath);
        if (smali) {
            backend.request("demo/Main", true);
            QTRY_COMPARE_WITH_TIMEOUT(source.count(), 3, 15000);
            QFile assembly(source.last().at(2).toString());
            QVERIFY(assembly.open(QIODevice::ReadOnly));
            const auto smaliText = assembly.readAll();
            QVERIFY(smaliText.contains("Ldemo/Main;"));
            QVERIFY(smaliText.contains(".method"));
            QVERIFY(smaliText.contains("greet(I)Ljava/lang/String;"));
        }
        QTemporaryDir output;
        backend.exportSources(output.path() + "/export", false);
        QTRY_VERIFY_WITH_TIMEOUT(!exported.isEmpty() || !errors.isEmpty(), 15000);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        QCOMPARE(exported.count(), 1);
        QVERIFY(
            QFileInfo::exists(output.path() + (file == "Main.class" ? "/export/source.java"
                                                                    : "/export/demo/Main.java")));
        if (file != "Main.class")
            QVERIFY(QFileInfo::exists(output.path() + "/export/demo/Extra.java"));
        backend.request("demo/DoesNotExist", false);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 15000);
        QVERIFY(!errors.isEmpty());
        backend.request("demo/Main", false);
        QVERIFY(!backend.busy()); // A failed request must not poison cached classes.
    }
    void invalidAndMissingEngine() {
        Backend backend;
        backend.setEngine(engine_);
        QSignalSpy errors(&backend, &Backend::failed);
        backend.open(fixtures_ + "/invalid.apk");
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 5000);
        QVERIFY(!backend.busy());
        backend.setEngine("/does/not/exist/garlic");
        backend.open(fixtures_ + "/demo.jar");
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 5000);
        QVERIFY(!backend.busy());
        backend.setEngine(engine_);
        QSignalSpy indexed(&backend, &Backend::indexed);
        backend.open(fixtures_ + "/demo.jar");
        QTRY_COMPARE_WITH_TIMEOUT(indexed.count(), 1, 15000);
    }
    void zipWithoutClasses() {
        Backend backend;
        backend.setEngine(engine_);
        QSignalSpy errors(&backend, &Backend::failed), indexed(&backend, &Backend::indexed);
        backend.open(fixtures_ + "/empty.zip");
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 5000);
        QCOMPARE(indexed.count(), 0);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.first().first().toString(), QString("无类被加载，没有什么可以反编译。"));
    }
    void largeAndTruncatedIndex() {
        Backend backend;
        backend.setEngine(qEnvironmentVariable("GARLIC_TEST_FAKE_ENGINE"));
        QTemporaryDir files;
        for (const auto &name : {"large-index.jar", "truncated-index.jar"}) {
            QFile file(files.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("test");
        }
        QSignalSpy indexed(&backend, &Backend::indexed), errors(&backend, &Backend::failed);
        backend.open(files.filePath("large-index.jar"));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 30000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.last()[0].toString()));
        QCOMPARE(indexed.size(), 1);
        QCOMPARE(backend.project()->classes().size(), 257);
        QVERIFY(QFileInfo(backend.workspacePath() + "/classes.jsonl").size() > 256LL * 1048576);
        backend.open(files.filePath("truncated-index.jar"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 5000);
        QVERIFY(errors.last()[0].toString().contains("2"));
        QCOMPARE(indexed.size(), 1);
    }
    void realLargeApk() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_LARGE_APK");
        if (path.isEmpty()) QSKIP("Set GARLIC_TEST_LARGE_APK to validate a large real index");
        Backend backend;
        backend.setEngine(engine_);
        QSignalSpy indexed(&backend, &Backend::indexed), errors(&backend, &Backend::failed);
        QElapsedTimer elapsed;
        elapsed.start();
        backend.open(path);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 120000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.last()[0].toString()));
        QCOMPARE(indexed.size(), 1);
        QVERIFY(!backend.project()->classes().isEmpty());
        qInfo() << "Large APK classes:" << backend.project()->classes().size()
                << "index MiB:" << QFileInfo(backend.workspacePath() + "/classes.jsonl").size()/1048576
                << "load ms:" << elapsed.elapsed();
    }
    void realLargeSearch() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_SEARCH_APK");
        if (path.isEmpty()) QSKIP("Set GARLIC_TEST_SEARCH_APK to benchmark full source search");
        Backend backend;
        backend.setEngine(engine_);
        auto settings = backend.settings();
        settings.background = false;
        backend.configure(settings);
        QSignalSpy completed(&backend, &Backend::searchCompleted), errors(&backend, &Backend::failed);
        backend.open(path);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 120000);
        SearchOptions options;
        options.classes = options.methods = options.fields = false;
        options.code = options.comments = true;
        options.query = "__garlic_search_missing_a__";
        QElapsedTimer elapsed;
        elapsed.start();
        backend.search(options);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 300000);
        const auto coldMs = elapsed.restart();
        options.query = "__garlic_search_missing_b__";
        backend.search(options);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 2, 30000);
        const auto warmMs = elapsed.elapsed();
        const auto warm = qvariant_cast<SearchResult>(completed.last()[1]);
        elapsed.restart();
        options.query = "public";
        options.limit = 1000;
        backend.search(options);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 3, 30000);
        const auto commonMs = elapsed.elapsed();
        elapsed.restart();
        options.query = "Main";
        options.code = options.comments = false;
        options.classes = options.methods = options.fields = true;
        backend.search(options);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 4, 30000);
        const auto symbolMs = elapsed.elapsed();
        QVERIFY(errors.isEmpty());
        QVERIFY(warm.indexRejected > 0);
        QVERIFY2(warmMs < 1000, qPrintable(QString("Warm search took %1 ms").arg(warmMs)));
        QVERIFY2(commonMs < 1000,
                 qPrintable(QString("Common-term search took %1 ms").arg(commonMs)));
        QVERIFY2(symbolMs < 1000, qPrintable(QString("Symbol search took %1 ms").arg(symbolMs)));
        qInfo() << "Full source search cold/warm/common/symbol ms:" << coldMs << warmMs
                << commonMs << symbolMs
                << "classes:" << backend.project()->classes().size()
                << "Bloom rejects:" << warm.indexRejected;
    }
    void cancelCrashAndBadIndex() {
        Backend backend;
        backend.setEngine(qEnvironmentVariable("GARLIC_TEST_FAKE_ENGINE"));
        QTemporaryDir files;
        for (const QString &name : {"slow.jar", "crash.jar", "bad.jar"}) {
            QFile file(files.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("test");
        }
        QSignalSpy errors(&backend, &Backend::failed);
        backend.open(files.filePath("slow.jar"));
        QVERIFY(backend.busy());
        QTest::qWait(100);
        backend.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 5000);
        QVERIFY(errors.isEmpty());
        backend.open(files.filePath("crash.jar"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 5000);
        QVERIFY(!backend.busy());
        backend.open(files.filePath("bad.jar"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 5000);
        QVERIFY(!backend.busy());
    }
};
QTEST_GUILESS_MAIN(BackendTest)
#include "backend_test.moc"
