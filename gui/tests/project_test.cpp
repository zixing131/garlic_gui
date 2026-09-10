#include "backend.h"
#include <QFile>
#include <QJsonDocument>
#include <QtTest>
class ProjectTest : public QObject {
    Q_OBJECT
  private slots:
    void applicationInheritance() {
        Project p;
        p.addClass({{"name","demo/Base"}, {"flags",1025}, {"refs",QJsonArray{QJsonObject{{"kind","extends"},{"target","Landroid/app/Application;"}}}}});
        p.addClass({{"name","demo/App"}, {"flags",1}, {"refs",QJsonArray{QJsonObject{{"kind","extends"},{"target","Ldemo/Base;"}}}}});
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
        const QString field = "Ldemo/Main;->name:Ljava/lang/String;";
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
