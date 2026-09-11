#include "backend.h"
#include "indexcache.h"
#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QtTest>
class IndexCacheTest : public QObject {
    Q_OBJECT
    static void write(const QString &path, const QByteArray &bytes) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); }
    static std::shared_ptr<Project> project(const QString &input) {
        auto p = std::make_shared<Project>(); p->reset(input);
        p->addClass({{"name", "Caller"}, {"kind", "class"}, {"refs", QJsonArray{
            QJsonObject{{"from", "LCaller;"}, {"target", "LTarget;"}, {"offset", 12}}}}});
        p->addClass({{"name", "Target"}, {"kind", "class"}});
        p->xrefs({}); p->symbols(); p->aliasVersion(); return p;
    }
  private slots:
    void defaults() {
        AppSettings s; QCOMPARE(s.indexCacheGiB, 20);
        s.indexDirectory = "/example/custom"; s.indexCacheGiB = 31;
        auto restored = AppSettings::fromJson(s.toJson());
        QCOMPARE(restored.indexDirectory, s.indexDirectory); QCOMPARE(restored.indexCacheGiB, 31);
    }
    void snapshotAndInvalidation() {
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = temp.path() + "/cache";
        const auto input = temp.path() + "/input", engine = temp.path() + "/engine", jsonl = temp.path() + "/classes.jsonl";
        write(input, "input"); write(engine, "engine"); write(jsonl, "{\"name\":\"Caller\"}\n");
        auto canceled = std::make_shared<std::atomic_bool>(false);
        auto cold = IndexCache::lookup(settings, {input}, engine, canceled);
        QVERIFY(!cold.project); QVERIFY(!cold.ticket.key.isEmpty());
        auto p = project(input); QString error;
        QVERIFY2(IndexCache::store(settings, cold.ticket, p, {jsonl}, temp.path(), &error), qPrintable(error));
        auto warm = IndexCache::lookup(settings, {input}, engine, canceled);
        QVERIFY(warm.project); QCOMPARE(warm.project->classCount(), 2);
        QCOMPARE(warm.project->xrefs("LTarget;"), p->xrefs("LTarget;"));
        QCOMPARE(warm.project->symbolInfo("LCaller;"), p->symbolInfo("LCaller;"));
        QVERIFY(QFileInfo::exists(warm.path + "/metadata-0.jsonl"));
        canceled->store(true); QVERIFY(!IndexCache::lookup(settings, {input}, engine, canceled).project); canceled->store(false);
        QVERIFY(QFile::remove(warm.path + "/metadata-0.jsonl"));
        QVERIFY(!IndexCache::lookup(settings, {input}, engine, canceled).project);
        write(warm.path + "/metadata-0.jsonl", "{\"name\":\"Caller\"}\n");
        auto different = settings; different.deobfuscate = true;
        QVERIFY(!IndexCache::lookup(different, {input}, engine, canceled).project);
        different = settings; different.indexDirectory = temp.path() + "/other";
        QVERIFY(!IndexCache::lookup(different, {input}, engine, canceled).project);
        write(input, "changed"); QVERIFY(!IndexCache::lookup(settings, {input}, engine, canceled).project);
        write(input, "input"); write(warm.path + "/snapshot.bin", "corrupt");
        QVERIFY(!IndexCache::lookup(settings, {input}, engine, canceled).project);
        write(warm.path + "/manifest.json", "corrupt");
        QVERIFY(IndexCache::clear(settings));
        QVERIFY(!QFileInfo::exists(warm.path));
        QVERIFY(!IndexCache::store(settings, cold.ticket, p, {jsonl}, temp.path(), &error)); // stale writer after clear
        QCOMPARE(IndexCache::stats(settings).value("entries").toInt(), 0);
    }
    void oldestFirstQuota() {
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = temp.path() + "/cache";
        auto canceled = std::make_shared<std::atomic_bool>(false);
        const auto engine = temp.path() + "/engine", jsonl = temp.path() + "/classes.jsonl";
        write(engine, "engine"); write(jsonl, "{}\n");
        QStringList inputs; QList<IndexCache::Lookup> keys;
        qint64 budget = 1024 * 1024;
        for (int i = 0; i < 3; ++i) {
            const auto input = temp.path() + QString("/input%1").arg(i); inputs.append(input); write(input, "data");
            keys.append(IndexCache::lookup(settings, {input}, engine, canceled));
            QString error;
            QVERIFY2(IndexCache::store(settings, keys.last().ticket, project(input), {jsonl}, temp.path(), &error, budget), qPrintable(error));
            if (i == 0) budget = qint64(IndexCache::stats(settings).value("bytes").toDouble()) * 2 + 128;
            QTest::qWait(2);
        }
        QCOMPARE(IndexCache::stats(settings).value("entries").toInt(), 2);
        QVERIFY(!IndexCache::lookup(settings, {inputs[0]}, engine, canceled).project);
        QVERIFY(IndexCache::lookup(settings, {inputs[1]}, engine, canceled).project);
        QVERIFY(IndexCache::lookup(settings, {inputs[2]}, engine, canceled).project);
        write(settings.indexDirectory + "/keep.txt", "unrelated");
        QDir().mkpath(settings.indexDirectory + "/.garlic-stage-orphan");
        write(settings.indexDirectory + "/.garlic-stage-orphan/.garlic-owner", "garlic-index-cache");
        write(settings.indexDirectory + "/.garlic-stage-orphan/incomplete.bin", "partial");
        QVERIFY(IndexCache::clear(settings)); QVERIFY(QFileInfo::exists(settings.indexDirectory + "/keep.txt"));
        QVERIFY(!QFileInfo::exists(settings.indexDirectory + "/.garlic-stage-orphan"));
    }
    void realApkCache() {
        const auto input = qEnvironmentVariable("GARLIC_TEST_CACHE_APK");
        if (input.isEmpty()) QSKIP("Set GARLIC_TEST_CACHE_APK for cold/warm timing");
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = temp.path() + "/cache";
        settings.background = false; settings.deobfuscate = true;
        Backend backend; backend.setEngine(qEnvironmentVariable("GARLIC_TEST_ENGINE")); backend.configure(settings);
        connect(&backend, &Backend::log, &backend, [](const QString &text) { if (!text.startsWith("[")) qInfo().noquote() << text; });
        QElapsedTimer timer; timer.start();
        backend.open(input);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 120000);
        qInfo() << "cold directory ms" << timer.elapsed();
        backend.prepareMetadata();
        QTRY_VERIFY_WITH_TIMEOUT(backend.metadataReady(), 180000);
        qInfo() << "cold complete ms" << timer.elapsed();
        const auto count = backend.project()->classCount();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.indexCacheWriting(), 180000);
        QCOMPARE(IndexCache::stats(settings).value("entries").toInt(), 1);
        qInfo() << "saved ms" << timer.elapsed() << "bytes" << IndexCache::stats(settings).value("bytes");
        timer.restart(); backend.open(input);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 180000);
        QVERIFY(backend.indexCacheHit()); QVERIFY(backend.metadataReady());
        QCOMPARE(backend.project()->classCount(), count);
        qInfo() << "warm complete ms" << timer.elapsed() << "classes" << count;
    }
    void backendReopenAndRebuild_data() {
        QTest::addColumn<QStringList>("names");
        QTest::newRow("dex") << QStringList{"cases.dex"};
        QTest::newRow("multiple-inputs") << QStringList{"cases.dex", "demo.jar"};
        QTest::newRow("apks") << QStringList{"resources.apks"};
    }
    void backendReopenAndRebuild() {
        QFETCH(QStringList, names);
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = temp.path() + "/cache"; settings.background = false;
        QStringList files;
        for (const auto &name : names) files.append(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + '/' + name);
        Backend first; first.setEngine(qEnvironmentVariable("GARLIC_TEST_ENGINE")); first.configure(settings);
        first.openPaths(files);
        QTRY_VERIFY_WITH_TIMEOUT(!first.busy() && first.metadataReady(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(IndexCache::stats(settings).value("entries").toInt(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!first.indexCacheWriting(), 15000);
        Backend second; second.setEngine(first.engine()); second.configure(settings); second.openPaths(files);
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 15000);
        QVERIFY(second.indexCacheHit()); QCOMPARE(second.project()->classCount(), first.project()->classCount());
        QSignalSpy source(&second, &Backend::sourceReady);
        second.request("demo/cases/Foo", false);
        QTRY_COMPARE_WITH_TIMEOUT(source.count(), 1, 15000);
        QVERIFY(second.project()->document("demo/cases/Foo", false, second.cachedPath("demo/cases/Foo", false)).text.contains("return 11;"));
        second.rebuildIndex();
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy() && !second.indexCacheWriting(), 15000);
        QVERIFY(!second.indexCacheHit()); QCOMPARE(second.project()->classCount(), first.project()->classCount());
    }
};
QTEST_GUILESS_MAIN(IndexCacheTest)
#include "indexcache_test.moc"
