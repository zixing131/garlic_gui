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
    void blockIntegrity() {
        auto p = project("input");
        QByteArray bytes; QBuffer out(&bytes); QVERIFY(out.open(QIODevice::WriteOnly));
        QVERIFY(p->writeIndexSnapshot(&out)); out.close();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        auto read = [&](const QByteArray &data) {
            QBuffer in; in.setData(data); in.open(QIODevice::ReadOnly);
            return Project::readIndexSnapshot(&in, cancel);
        };
        QVERIFY(read(bytes));
        // Every independently parsed block must be verified, even when the
        // outer fixed header remains intact and the file length is unchanged.
        for (qsizetype offset : {qsizetype(4096), bytes.size() / 2 + 2048, bytes.size() - 1}) {
            auto corrupt = bytes; corrupt[offset] = char(corrupt.at(offset) ^ 1); QVERIFY(!read(corrupt));
        }
        auto truncated = bytes; truncated.chop(1); QVERIFY(!read(truncated));
        auto appended = bytes; appended.append('x'); QVERIFY(!read(appended));
        auto badHeader = bytes; badHeader[24] = char(0xff); QVERIFY(!read(badHeader));
        cancel->store(true); QVERIFY(!read(bytes));
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
    void sourceQuotaAndCancellation() {
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = temp.path() + "/cache";
        const auto input = temp.path() + "/input", engine = temp.path() + "/engine", source = temp.path() + "/code.java", jsonl = temp.path() + "/classes.jsonl";
        write(input, "input"); write(engine, "engine"); write(source, QByteArray(100000, 'x')); write(jsonl, "{}\n");
        auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto ticket = IndexCache::lookup(settings, {input}, engine, cancel).ticket;
        QString error;
        cancel->store(true);
        QVERIFY(!IndexCache::storeSources(settings, ticket, temp.path(), "Caller", false, source, false, cancel, &error));
        cancel->store(false);
        auto tiny = settings; tiny.indexCacheGiB = 0;
        QVERIFY(!IndexCache::storeSources(tiny, ticket, temp.path(), "Caller", false, source, false, cancel, &error));
        QCOMPARE(IndexCache::stats(settings).value("sourceEntries").toInt(), 0);
        QVERIFY2(IndexCache::storeSources(settings, ticket, temp.path(), "Caller", false, source, false, cancel, &error), qPrintable(error));
        QCOMPARE(IndexCache::stats(settings).value("sourceEntries").toInt(), 1);
        // Index and source entries share the same oldest-first storage budget.
        QVERIFY2(IndexCache::store(settings, ticket, project(input), {jsonl}, temp.path(), &error, 16000), qPrintable(error));
        QCOMPARE(IndexCache::stats(settings).value("sourceEntries").toInt(), 0);
        QCOMPARE(IndexCache::stats(settings).value("entries").toInt(), 1);
        QVERIFY(IndexCache::clear(settings));
        QVERIFY(!IndexCache::storeSources(settings, ticket, temp.path(), "Caller", false, source, false, cancel, &error));
        QCOMPARE(IndexCache::stats(settings).value("sourceEntries").toInt(), 0);
    }
    void persistentSources_data() {
        QTest::addColumn<bool>("full"); QTest::addColumn<QString>("fixture");
        QTest::newRow("per-class-dex") << false << QString("cases.dex");
        QTest::newRow("full-packed-dex") << true << QString("cases.dex");
        QTest::newRow("full-directory-jar") << true << QString("demo.jar");
        QTest::newRow("per-class-class") << false << QString("Main.class");
        QTest::newRow("full-class") << true << QString("Main.class");
        QTest::newRow("full-apks") << true << QString("resources.apks");
    }
    void persistentSources() {
        QFETCH(bool, full); QFETCH(QString, fixture);
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = temp.path() + "/cache"; settings.background = false;
        const auto input = qEnvironmentVariable("GARLIC_TEST_FIXTURES") + '/' + fixture;
        const auto engine = qEnvironmentVariable("GARLIC_TEST_ENGINE");
        const auto name = (fixture == "demo.jar" || fixture == "Main.class") ? QString("demo/Main") : QString("demo/cases/Foo");
        SourceDocument expected;
        QString oldWorkspace;
        {
            Backend first; first.setEngine(engine); first.configure(settings); first.open(input);
            QTRY_VERIFY_WITH_TIMEOUT(!first.busy() && first.metadataReady(), 15000);
            if (full) {
                first.prepareSources(); QTRY_VERIFY_WITH_TIMEOUT(first.projectReady(), 15000);
            } else {
                QSignalSpy ready(&first, &Backend::sourceReady); first.request(name, false);
                QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 15000);
                if (first.supportsSmali(name)) { first.request(name, true); QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 15000); }
            }
            QTRY_VERIFY_WITH_TIMEOUT(!first.indexCacheWriting(), 15000);
            QCOMPARE(IndexCache::stats(settings).value("sourceEntries").toInt(), 1);
            expected = first.project()->document(name, false, first.cachedPath(name, false));
            QVERIFY(!expected.text.isEmpty()); QVERIFY(!expected.spans.isEmpty());
            oldWorkspace = first.workspacePath();
        }
        QVERIFY(QThreadPool::globalInstance()->waitForDone(15000));
        QVERIFY(!QFileInfo::exists(oldWorkspace)); // No session temp file can satisfy the following reads.
        Backend second; second.setEngine(engine); second.configure(settings); second.open(input);
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 15000);
        QVERIFY(second.indexCacheHit()); QCOMPARE(second.projectReady(), full);
        QSignalSpy ready(&second, &Backend::sourceReady), preparing(&second, &Backend::preparationChanged);
        second.request(name, false); QCOMPARE(ready.count(), 1); QVERIFY(!second.busy());
        const auto restored = second.project()->document(name, false, second.cachedPath(name, false));
        QCOMPARE(restored.text, expected.text); QCOMPARE(restored.spans.size(), expected.spans.size());
        for (qsizetype i = 0; i < restored.spans.size(); ++i) {
            QCOMPARE(restored.spans[i].id, expected.spans[i].id);
            QCOMPARE(restored.spans[i].start, expected.spans[i].start);
        }
        if (full) {
            second.prepareSources(); QCOMPARE(preparing.count(), 0); QVERIFY(second.workerPids().isEmpty());
            QSignalSpy searched(&second, &Backend::searchCompleted);
            SearchOptions options; options.query = "return"; options.code = true; options.limit = 10;
            second.search(options); QTRY_COMPARE_WITH_TIMEOUT(searched.count(), 1, 15000);
            const auto result = qvariant_cast<SearchResult>(searched.first()[1]);
            QVERIFY(result.error.isEmpty()); QVERIFY(!result.hits.isEmpty()); QVERIFY(second.workerPids().isEmpty());
        } else {
            if (second.supportsSmali(name)) {
                second.request(name, true); QCOMPARE(ready.count(), 2);
                QVERIFY(second.project()->document(name, true, second.cachedPath(name, true)).text.contains(".class"));
            }
            const auto path = second.cachedPath(name, false);
            QVERIFY(QFile::remove(path)); QVERIFY(second.cachedPath(name, false).isEmpty());
        }
        auto changed = settings; changed.unflatten = !settings.unflatten;
        const auto different = IndexCache::lookup(changed, {input}, engine, std::make_shared<std::atomic_bool>(false));
        QVERIFY(different.project); QVERIFY(different.sources.isEmpty());
        second.configure(changed); QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 15000);
        QCOMPARE(IndexCache::stats(settings).value("sourceEntries").toInt(), 1);
        QVERIFY(second.cachedPath(name, false).isEmpty());
        second.configure(settings); QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 15000);
        QCOMPARE(second.projectReady(), full);
        if (full) QVERIFY(!second.cachedPath(name, false).isEmpty());
        second.clearCache(); QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 15000);
        QCOMPARE(IndexCache::stats(settings).value("sourceEntries").toInt(), 0);
        QCOMPARE(IndexCache::stats(settings).value("entries").toInt(), 1);
        QVERIFY(second.cachedPath(name, false).isEmpty());
    }
    void realSourceCache() {
        const auto input = qEnvironmentVariable("GARLIC_TEST_CACHE_APK");
        if (input.isEmpty()) QSKIP("Set GARLIC_TEST_CACHE_APK for persistent source timing");
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = temp.path() + "/cache"; settings.deobfuscate = true;
        const auto engine = qEnvironmentVariable("GARLIC_TEST_ENGINE");
        const QString name = "com/tencent/mm/ui/LauncherUI";
        SourceDocument expected;
        {
            Backend first; first.setEngine(engine); first.configure(settings); first.open(input);
            QTRY_VERIFY_WITH_TIMEOUT(first.metadataReady() && !first.busy(), 120000);
            first.prepareSources(); QTRY_VERIFY_WITH_TIMEOUT(first.projectReady(), 180000);
            QTRY_VERIFY_WITH_TIMEOUT(!first.indexCacheWriting(), 180000);
            expected = first.project()->document(name, false, first.cachedPath(name, false));
            QVERIFY(!expected.text.isEmpty());
            qInfo() << "source cache bytes" << IndexCache::stats(settings).value("bytes");
        }
        QVERIFY(QThreadPool::globalInstance()->waitForDone(30000));
        Backend second; second.setEngine(engine); second.configure(settings);
        QElapsedTimer timer; timer.start(); second.open(input);
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 120000);
        QVERIFY(second.indexCacheHit()); QVERIFY(second.projectReady());
        qInfo() << "index and sources restored ms" << timer.elapsed();
        timer.restart();
        QSignalSpy ready(&second, &Backend::sourceReady); second.request(name, false); QCOMPARE(ready.count(), 1);
        const auto actual = second.project()->document(name, false, second.cachedPath(name, false));
        QCOMPARE(actual.text, expected.text); QCOMPARE(actual.spans.size(), expected.spans.size());
        QVERIFY(second.workerPids().isEmpty());
        qInfo() << "cached Java and mappings read ms" << timer.elapsed() << "spans" << actual.spans.size();
    }
    void realApkCache() {
        const auto input = qEnvironmentVariable("GARLIC_TEST_CACHE_APK");
        if (input.isEmpty()) QSKIP("Set GARLIC_TEST_CACHE_APK for cold/warm timing");
        QTemporaryDir temp; AppSettings settings; settings.indexDirectory = qEnvironmentVariable("GARLIC_TEST_CACHE_DIRECTORY", temp.path() + "/cache");
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
        const auto referenceId = Project::classId("com/tencent/mm/ui/LauncherUI");
        const auto references = backend.project()->xrefs(referenceId);
        const auto aliases = backend.project()->aliasVersion();
        timer.restart(); backend.open(input);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 180000);
        QVERIFY(backend.indexCacheHit()); QVERIFY(backend.metadataReady());
        QCOMPARE(backend.project()->classCount(), count);
        qInfo() << "warm complete ms" << timer.elapsed() << "classes" << count;
        QCOMPARE(backend.project()->xrefs(referenceId), references);
        QCOMPARE(backend.project()->aliasVersion(), aliases);
    }
    void realWarmCache() {
        const auto input = qEnvironmentVariable("GARLIC_TEST_CACHE_APK");
        const auto root = qEnvironmentVariable("GARLIC_TEST_CACHE_DIRECTORY");
        if (input.isEmpty() || root.isEmpty()) QSKIP("Provide an APK and a previously populated benchmark cache directory");
        AppSettings settings; settings.indexDirectory = root; settings.deobfuscate = true; settings.background = false;
        Backend backend; backend.setEngine(qEnvironmentVariable("GARLIC_TEST_ENGINE")); backend.configure(settings);
        QElapsedTimer timer; timer.start(); backend.open(input);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 120000);
        QVERIFY(backend.indexCacheHit()); QVERIFY(backend.metadataReady());
        qInfo() << "fresh process warm complete ms" << timer.elapsed() << "classes" << backend.project()->classCount();
        timer.restart(); backend.open(input);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 120000);
        QVERIFY(backend.indexCacheHit()); QVERIFY(backend.metadataReady());
        qInfo() << "same process warm complete ms" << timer.elapsed();
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
