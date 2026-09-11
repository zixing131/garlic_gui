#include "backend.h"
#include <QFile>
#include <QJsonDocument>
#include <QtTest>
#include <algorithm>
class ProjectTest : public QObject {
    Q_OBJECT
  private slots:
    void referenceDedupLargeGroup() {
        Project project;
        QJsonArray rows;
        for (int i = 0; i < 24; ++i) {
            const QJsonObject ref{{"from", "LCaller;"}, {"target", "LTarget;"}, {"offset", i}};
            rows.append(ref); rows.append(ref);
        }
        rows.append(rows.first());
        project.addClass({{"name", "Caller"}, {"refs", rows}});
        QCOMPARE(project.xrefs("LTarget;").size(), 24);
        QJsonArray compact;
        for (const auto &v : rows) compact.append(QJsonArray{0, v.toObject().value("offset")});
        project.addClass({{"name", "Caller"}, {"ref_targets", QJsonArray{"LTarget;"}},
            {"refs", QJsonObject{{"LCaller;", compact}}}});
        QCOMPARE(project.xrefs("LTarget;").size(), 24);
    }
    void referenceIndexMutationAndAliases() {
        Project project;
        auto entry = [](const QString &target) {
            return QJsonObject{{"name", "Caller"}, {"refs", QJsonArray{QJsonObject{
                {"from", "LCaller;"}, {"target", target}, {"offset", 0}}}}};
        };
        project.addClass(entry("LFirst;"));
        auto beforeBuild = project.snapshot();
        project.addClass(entry("LSecond;"));
        QCOMPARE(beforeBuild->xrefs("LFirst;").size(), 1);
        QVERIFY(project.xrefs("LFirst;").isEmpty());
        QCOMPARE(project.xrefs("LSecond;").size(), 1);
        auto afterBuild = project.snapshot();
        project.addClass(entry("LThird;"));
        QCOMPARE(afterBuild->xrefs("LSecond;").size(), 1);
        QVERIFY(project.xrefs("LSecond;").isEmpty());
        QCOMPARE(project.xrefs("LThird;").size(), 1);
        for (const auto &name : QStringList{"Valid_123", "正常名称", "a", "9invalid", QString("no") + QChar(0x200b) + "isy"})
            project.addClass({{"name", name}, {"kind", "class"}});
        project.deobfuscateNames();
        QVERIFY(!project.aliasMap().contains("LValid_123;"));
        QVERIFY(!project.aliasMap().contains("L正常名称;"));
        QVERIFY(project.aliasMap().contains("La;"));
        QVERIFY(project.aliasMap().contains("L9invalid;"));
        QVERIFY(project.aliasMap().contains(QString("Lno") + QChar(0x200b) + "isy;"));
    }
    void filterOnlyWarmup() {
        auto project = std::make_shared<Project>();
        project->addClass({{"name", "Warm"}});
        QTemporaryDir dir;
        QFile file(dir.path() + "/Warm.java"); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("class Warm { String value = \"needle\"; /* commentToken */ }\n"); file.close();
        auto index = std::make_shared<SearchIndex>();
        auto run = [&](SearchOptions options) {
            return searchProject(project, options, dir.path(), false,
                std::make_shared<std::atomic_bool>(false), std::make_shared<SearchControl>(),
                std::make_shared<SearchEvents>(), 1, index);
        };
        SearchOptions options; options.query = "needle"; options.indexOnly = true;
        auto warm = run(options); QVERIFY(warm.hits.isEmpty());
        QCOMPARE(index->filters.size(), 1); QCOMPARE(index->documents.size(), 0);
        options.indexOnly = false;
        QVERIFY(!run(options).hits.isEmpty());
        options.code = false; options.comments = true; options.query = "commentToken";
        QVERIFY(!run(options).hits.isEmpty());
        options.query = "absentUniqueToken"; QVERIFY(run(options).hits.isEmpty());
    }
    void multiAndLambdaLocals() {
        Project project;
        const QString method = "LUse;->run()V";
        project.addClass({{"name", "Use"}, {"methods", QJsonArray{QJsonObject{{"id", method}, {"name", "run"}}}}});
        const QString text = "class Use { void run() { int first = call(1, 2), X = 3; X++; consume(X); use(Y -> Y + X); } }";
        QTemporaryDir dir;
        QFile file(dir.path() + "/Use.java"); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text.toUtf8()); file.close();
        QJsonArray records{QJsonObject{{"id", method}, {"token", "run"}, {"start", text.indexOf("run")}, {"end", text.indexOf("run") + 3}, {"declaration", true}}};
        QFile map(dir.path() + "/Use.map.json"); QVERIFY(map.open(QIODevice::WriteOnly));
        map.write(QJsonDocument(records).toJson()); map.close();
        const auto doc = project.document("Use", false, file.fileName());
        int xUses = 0, yUses = 0;
        for (const auto &span : doc.spans) if (!span.declaration) {
            if (span.id.endsWith(":X")) ++xUses;
            if (span.id.endsWith(":Y")) ++yUses;
        }
        QCOMPARE(xUses, 3); QCOMPARE(yUses, 1);
    }
    void sameNameCallAndLocal() {
        Project project;
        const QString method = "LUse;->run()V", target = "Lother/a;->a(Ljava/lang/Object;)V";
        project.addClass({{"name", "Use"}, {"methods", QJsonArray{QJsonObject{{"id", method}, {"name", "run"}}}}});
        project.addClass({{"name", "a"}});
        const QString text = "class Use { void run() { Object a = null; a.a(a); a[0]; } }";
        QTemporaryDir dir;
        QFile file(dir.path() + "/Use.java"); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text.toUtf8()); file.close();
        QJsonArray records{
            QJsonObject{{"id", method}, {"token", "run"}, {"start", text.indexOf("run")}, {"end", text.indexOf("run") + 3}, {"declaration", true}},
            QJsonObject{{"id", target}, {"token", "a"}, {"start", text.indexOf("a.a(a)")}, {"end", text.indexOf("a.a(a)") + 6}}};
        QFile map(dir.path() + "/Use.map.json"); QVERIFY(map.open(QIODevice::WriteOnly));
        map.write(QJsonDocument(records).toJson()); map.close();
        const auto doc = project.document("Use", false, file.fileName());
        const int call = text.indexOf("a.a(a)");
        bool receiver = false, invoked = false, argument = false;
        for (const auto &span : doc.spans) {
            if (span.start == call) receiver = span.id.contains("@local:");
            if (span.start == call + 2) invoked = span.id == target;
            if (span.start == call + 4) argument = span.id.contains("@local:");
            if (span.start == text.indexOf("a[0]")) QVERIFY(span.id.contains("@local:"));
        }
        QVERIFY(receiver && invoked && argument);
    }
    void exactReferenceIdentities() {
        Project project;
        const QString first = "Lone/a;->a()V", other = "Ltwo/a;->a()V", field = "Lone/a;->a:I";
        project.addClass({{"name", "demo/Caller"}, {"refs", QJsonArray{
            QJsonObject{{"from", "Ldemo/Caller;->first()V"}, {"target", first}, {"offset", 1}},
            QJsonObject{{"from", "Ldemo/Caller;->other()V"}, {"target", other}, {"offset", 2}},
            QJsonObject{{"from", "Ldemo/Caller;->field()V"}, {"target", field}, {"offset", 3}}}}});
        QCOMPARE(project.xrefs(first).size(), 1);
        QCOMPARE(project.xrefs(field).size(), 1);
        QCOMPARE(project.xrefs("Lone/a;").size(), 2);
        for (const auto &ref : project.xrefs("Lone/a;"))
            QVERIFY(ref.toObject().value("target").toString() != other);
    }
    void realResourceSearch() {
        const auto input = qEnvironmentVariable("GARLIC_TEST_LARGE_APK");
        if (input.isEmpty()) QSKIP("Set APK to validate decoded resource search");
        auto project = std::make_shared<Project>(); project->setInputs({input});
        SearchOptions options; options.code = false; options.resources = true;
        options.query = "<resources>";
        QElapsedTimer timer; timer.start();
        const auto result = searchProject(project, options, {}, false,
            std::make_shared<std::atomic_bool>(false), std::make_shared<SearchControl>(),
            std::make_shared<SearchEvents>(), 1, std::make_shared<SearchIndex>());
        QVERIFY(std::any_of(result.hits.begin(), result.hits.end(), [](const auto &v) {
            return v.toObject().value("entry").toString() == "resources.arsc" &&
                   v.toObject().value("generated").toString().endsWith(".xml");
        }));
        qInfo() << "Resource scan ms:" << timer.elapsed() << "scanned:" << result.scanned;
    }
    void resourceSearchOptIn() {
        auto project = std::make_shared<Project>();
        project->setInputs({qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.zip"});
        SearchOptions options; options.code = false; options.query = "ZIP resource preview";
        auto index = std::make_shared<SearchIndex>();
        auto run = [&] { return searchProject(project, options, {}, false,
            std::make_shared<std::atomic_bool>(false), std::make_shared<SearchControl>(),
            std::make_shared<SearchEvents>(), 1, index); };
        QVERIFY(run().hits.isEmpty());
        options.resources = true;
        const auto result = run();
        QCOMPARE(result.hits.size(), 1);
        QCOMPARE(result.hits.first().toObject().value("entry").toString(), QString("assets/readme.txt"));
        QVERIFY(run().cachedFiles > 0);
    }
    void sameNameTypeDoesNotCaptureVariable() {
        Project project;
        project.addClass({{"name", "other/a"}});
        project.addClass({{"name", "demo/Use"}});
        QTemporaryDir directory;
        QFile file(directory.path() + "/Use.java");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("package demo; class Use { void run() { int a = 0; a.run(); } }"); file.close();
        const auto doc = project.document("demo/Use", false, file.fileName());
        for (const auto &span : doc.spans) QVERIFY(span.id != "Lother/a;");
    }
    void localVariableAliases() {
        QTemporaryDir directory;
        Project project;
        QFile input(directory.path() + "/locals.class");
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        project.reset(input.fileName());
        const QString first = "LExample;->first(I)I", second = "LExample;->second(I)I";
        project.addClass({{"name", "Example"}, {"methods", QJsonArray{
            QJsonObject{{"id", first}, {"name", "first"}},
            QJsonObject{{"id", second}, {"name", "second"}}}}});
        const QString text = "class Example { int first(int value) { { int temp = value; value += temp; } "
                             "{ int temp = 2; value += temp; } return value; } "
                             "int second(int value) { return value; } }";
        const auto path = directory.path() + "/Example.java";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(text.toUtf8()); file.close();
        QJsonArray records;
        for (const auto &entry : {qMakePair(first, QString("first")), qMakePair(second, QString("second"))}) {
            const int start = text.indexOf(entry.second);
            records.append(QJsonObject{{"id", entry.first}, {"token", entry.second},
                {"start", start}, {"end", start + entry.second.size()}, {"declaration", true}});
        }
        QFile map(directory.path() + "/Example.map.json"); QVERIFY(map.open(QIODevice::WriteOnly));
        map.write(QJsonDocument(records).toJson()); map.close();
        auto raw = project.document("Example", false, path, false);
        QString parameter, temp;
        QSet<QString> locals;
        for (const auto &span : raw.spans) if (span.declaration && span.id.contains("@local:")) {
            locals.insert(span.id);
            if (span.id.startsWith(first) && span.id.endsWith(":value")) parameter = span.id;
            if (temp.isEmpty() && span.id.endsWith(":temp")) temp = span.id;
        }
        QCOMPARE(locals.size(), 4);
        QVERIFY(!parameter.isEmpty()); QVERIFY(!temp.isEmpty());
        QVERIFY(!project.rename(parameter, "temp").isEmpty());
        QVERIFY(project.rename(parameter, "input").isEmpty());
        QVERIFY(!project.rename(temp, "input").isEmpty());
        QVERIFY(project.rename(temp, "scratch").isEmpty());
        const auto renamed = project.applyAliases(raw, false).text;
        QVERIFY(renamed.contains("first(int input)"));
        QVERIFY(renamed.contains("int scratch = input; input += scratch;"));
        QVERIFY(renamed.contains("int temp = 2; input += temp;"));
        QVERIFY(renamed.contains("second(int value) { return value; }"));
        QString error;
        const auto saved = directory.path() + "/project.json";
        QVERIFY(project.save(saved, &error));
        project.undoRename(); project.undoRename();
        QVERIFY2(project.loadAliases(saved, &error), qPrintable(error));
        QCOMPARE(project.applyAliases(raw, false).text, renamed);
    }
    void compactReferenceIndex() {
        const QString from = "Ldemo/Main;->call()V", target = "Ldemo/Base;->run()V";
        const QJsonObject inherited{{"from", "Ldemo/Main;"},
                                    {"target", "Ldemo/Base;"},
                                    {"offset", -1},
                                    {"kind", "extends"}};
        const QJsonObject called{
            {"from", from}, {"target", target}, {"offset", 3}, {"kind", "bytecode"}};
        QJsonObject entry{{"name", "demo/Main"}, {"refs", QJsonArray{inherited, called}}};
        Project legacy, compact;
        legacy.addClass(entry);
        QJsonArray inheritanceRow;
        inheritanceRow.append("Ldemo/Base;");
        inheritanceRow.append(-1);
        inheritanceRow.append("extends");
        QJsonArray callRow;
        callRow.append(target);
        callRow.append(3);
        QJsonArray inheritanceRows;
        inheritanceRows.append(inheritanceRow);
        QJsonArray callRows;
        callRows.append(callRow);
        QJsonObject compactRefs;
        compactRefs.insert("Ldemo/Main;", inheritanceRows);
        compactRefs.insert(from, callRows);
        entry["refs"] = compactRefs;
        compact.addClass(entry);
        QCOMPARE(compact.xrefs(target), legacy.xrefs(target));
        QCOMPARE(compact.xrefs("Ldemo/Base;").size(), legacy.xrefs("Ldemo/Base;").size());
        QCOMPARE(compact.callees(from), legacy.callees(from));
        QCOMPARE(compact.callees(from).size(), 1);
        Project dictionary;
        entry["ref_targets"] = QJsonArray{"Ldemo/Base;", target};
        // A single nested QJsonArray can select the copy constructor on Qt 6.8,
        // flattening the row. Append explicitly so this fixture matches wire JSON.
        QJsonArray dictionaryInheritance;
        dictionaryInheritance.append(QJsonArray{0, -1, "extends"});
        entry["refs"] = QJsonObject{{"Ldemo/Main;", dictionaryInheritance},
                                    {from, QJsonArray{QJsonArray{1, 3}, QJsonArray{1, 3}, QJsonArray{99, 4}}}};
        QCOMPARE(entry["refs"].toObject()["Ldemo/Main;"].toArray().size(), 1);
        QVERIFY(entry["refs"].toObject()["Ldemo/Main;"].toArray().first().isArray());
        dictionary.addClass(entry);
        QCOMPARE(dictionary.xrefs(target), compact.xrefs(target));
        QCOMPARE(dictionary.xrefs("Ldemo/Base;"), compact.xrefs("Ldemo/Base;"));
        QCOMPARE(dictionary.callees(from), compact.callees(from));
    }
    void aliasLookupSnapshots() {
        Project project;
        project.addClass({{"name", "demo/Original"}, {"kind", "class"}});
        const auto id = Project::classId("demo/Original");
        QVERIFY(project.rename(id, "Alpha").isEmpty());
        QCOMPARE(project.resolve("Alpha", "demo/Use"), id);
        const auto version = project.aliasVersion();
        auto snapshot = project.snapshot();
        QVERIFY(project.rename(id, "Beta").isEmpty());
        QVERIFY(project.resolve("Alpha", "demo/Use").isEmpty());
        QCOMPARE(project.resolve("Beta", "demo/Use"), id);
        QVERIFY(project.aliasVersion() != version);
        QCOMPARE(snapshot->aliasVersion(), version);
        QCOMPARE(snapshot->resolve("Alpha", "demo/Use"), id);
        project.undoRename();
        QCOMPARE(project.aliasVersion(), version);
        QCOMPARE(project.resolve("Alpha", "demo/Use"), id);
        QVERIFY(project.resolve("Beta", "demo/Use").isEmpty());
    }
    void applicationInheritance() {
        Project p;
        p.addClass({{"name", "demo/Base"},
                    {"flags", 1025},
                    {"refs", QJsonArray{QJsonObject{{"kind", "extends"},
                                                    {"target", "Landroid/app/Application;"}}}}});
        p.addClass(
            {{"name", "demo/App"},
             {"flags", 1},
             {"refs", QJsonArray{QJsonObject{{"kind", "extends"}, {"target", "Ldemo/Base;"}}}}});
        QCOMPARE(p.applicationCandidates(), QStringList{"demo/App"});
    }

    void semanticAndEncoding_data() {
        QTest::addColumn<QString>("input");
        QTest::newRow("jar") << "demo.jar";
        if (QFileInfo::exists(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/classes.dex"))
            QTest::newRow("dex") << "classes.dex";
    }
    void semanticAndEncoding() {
        QFETCH(QString, input);
        Backend b;
        b.setEngine(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        auto settings = b.settings();
        settings.background = false;
        settings.escapeUnicode = false;
        settings.excluded.clear();
        b.configure(settings);
        QSignalSpy errors(&b, &Backend::failed), sources(&b, &Backend::sourceReady),
            ready(&b, &Backend::projectSourcesReady), hits(&b, &Backend::searchFinished);
        b.open(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/" + input);
        QTRY_VERIFY_WITH_TIMEOUT(!b.busy(), 15000);
        QVERIFY(errors.isEmpty());
        auto project = b.project();
        QCOMPARE(project->info("demo/Contract").value("kind").toString(), QString("interface"));
        QCOMPARE(project->info("demo/Kind").value("kind").toString(), QString("enum"));
        QCOMPARE(project->info("demo/Marker").value("kind").toString(), QString("annotation"));
        QCOMPARE(project->owner("demo/Main$Details"), QString("demo/Main"));
        const QString id = "Ldemo/Main;->greet(I)Ljava/lang/String;";
        QVERIFY(!project->xrefs(id).isEmpty());
        QCOMPARE(project->overrideOf(id), QString("Ldemo/Contract;->greet(I)Ljava/lang/String;"));
        QCOMPARE(project->overrideAnnotation(id),
                 QString("@Override // demo.Contract.greet"));
        auto searchPresentation = [&](QString query, bool code, bool comments) {
            SearchOptions options;
            options.query = query;
            options.code = code;
            options.comments = comments;
            options.limit = 20;
            return searchProject(project->snapshot(), options, QString(), false,
                                 std::make_shared<std::atomic_bool>(false),
                                 std::make_shared<SearchControl>(),
                                 std::make_shared<SearchEvents>(), 1);
        };
        const auto annotationHits = searchPresentation("@Override", true, false).hits;
        QVERIFY(std::any_of(annotationHits.cbegin(), annotationHits.cend(), [&id](const auto &hit) {
            return hit.toObject().value("id").toString() == id;
        }));
        const auto commentHits = searchPresentation("demo.Contract.greet", false, true).hits;
        QVERIFY(std::any_of(commentHits.cbegin(), commentHits.cend(), [&id](const auto &hit) {
            return hit.toObject().value("id").toString() == id;
        }));
        b.request("demo/Main", false);
        QTRY_COMPARE_WITH_TIMEOUT(sources.count(), 1, 15000);
        auto doc = project->document("demo/Main", false, b.cachedPath("demo/Main", false));
        QVERIFY(doc.text.contains(QString::fromUtf8("中文")));
        QVERIFY(!doc.text.contains(QChar::ReplacementCharacter));
        QVERIFY(doc.text.contains("\\u0000"));
        QVERIFY(doc.text.contains("\\u0001"));
        QVERIFY(doc.text.contains("\\ud83d\\ude00"));
        QVERIFY(!doc.text.contains("\\0A"));
        QVERIFY2(project->methodSource(doc, id).contains("greet(int"),
                 qPrintable(QJsonDocument(project->members("demo/Main", true)).toJson()));
        const QString field = "Ldemo/Main;->name:Ljava/lang/String;";
        QVERIFY(!project->xrefs(field).isEmpty());
        QVERIFY(std::any_of(doc.spans.cbegin(), doc.spans.cend(), [&field](const SourceSpan &span) {
            return span.id == field && !span.declaration;
        }));
        QVERIFY(project->rename(id, "welcome").isEmpty());
        doc = project->document("demo/Main", false, b.cachedPath("demo/Main", false));
        QVERIFY(doc.text.contains("welcome(int"));
        QVERIFY(doc.text.contains("greet(String"));
        QVERIFY(doc.text.contains("greet: "));
        b.request("demo/Use", false);
        QTRY_COMPARE_WITH_TIMEOUT(sources.count(), 2, 15000);
        auto use = project->document("demo/Use", false, b.cachedPath("demo/Use", false));
        QVERIFY2(use.text.contains(".welcome(2)"), qPrintable(use.text));
        QVERIFY(use.text.contains(".greet(\"literal greet should not change\")"));
        QVERIFY(project->rename(field, "personName").isEmpty());
        doc = project->document("demo/Main", false, b.cachedPath("demo/Main", false));
        QVERIFY(doc.text.contains("personName;"));
        QVERIFY2(!doc.text.contains("this.name"), qPrintable(doc.text));
        QTemporaryDir save;
        QString error;
        QVERIFY(project->save(save.filePath("project.json"), &error));
        project->undoRename();
        QVERIFY(project->loadAliases(save.filePath("project.json"), &error));
        QCOMPARE(project->alias(field), QString("personName"));
        QVERIFY(!project->rename(id, "class").isEmpty());
        b.search("literal greet should not change");
        QTRY_COMPARE_WITH_TIMEOUT(hits.count(), 1, 30000);
        QVERIFY(!hits.first().first().toJsonArray().isEmpty());
        QVERIFY(ready.count() == 1);
        QVERIFY(errors.isEmpty());
        b.search("welcome\\s*\\(2", true);
        QTRY_COMPARE_WITH_TIMEOUT(hits.count(), 2, 10000);
        QVERIFY(!hits.last().first().toJsonArray().isEmpty());
        QVERIFY(project->rename(Project::classId("demo/Main"), "Person").isEmpty());
        QSignalSpy exported(&b, &Backend::exported);
        b.exportSources(save.filePath("java"), false);
        QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 30000);
        QFile java(save.filePath("java/demo/Person.java"));
        QVERIFY(java.open(QIODevice::ReadOnly));
        const auto renamedJava = QString::fromUtf8(java.readAll());
        QVERIFY(renamedJava.contains("class Person"));
        QVERIFY(renamedJava.contains("Person(String"));
        QVERIFY(renamedJava.contains("welcome(int"));
        QVERIFY(renamedJava.contains("personName;"));
        QVERIFY(!QFileInfo::exists(save.filePath("java/demo/Main.java")));
        if (input.endsWith("dex")) {
            b.exportSources(save.filePath("smali"), true);
            QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 2, 30000);
            QFile smali(save.filePath("smali/demo/Person.smali"));
            QVERIFY(smali.open(QIODevice::ReadOnly));
            const auto renamedSmali = QString::fromUtf8(smali.readAll());
            QVERIFY(renamedSmali.contains("Ldemo/Person;"));
            QVERIFY(renamedSmali.contains("<init>(Ljava/lang/String;)V"));
            QVERIFY(renamedSmali.contains("welcome(I)Ljava/lang/String;"));
        }
        QVERIFY(errors.isEmpty());
        b.clearCache();
        QVERIFY(!b.projectReady());
    }
};
QTEST_GUILESS_MAIN(ProjectTest)
#include "project_test.moc"
