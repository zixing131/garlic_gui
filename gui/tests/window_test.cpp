#include "classview.h"
#include "mainwindow.h"
#include <QtTest>
#include <QtWidgets>

class WindowTest : public QObject {
    Q_OBJECT
  private slots:
    void browseSwitchFindRenameAndReopen() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("WindowTest");
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
        QVERIFY(tree && filter && tabs && find);
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        filter->setText("Main");
        QVERIFY(tree->model()->rowCount() > 0);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        QCOMPARE(tabs->count(), 1);
        auto code = window.editor();
        find->setText("greet");
        QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
        find->setText("not_present_123");
        QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
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
        QVERIFY(window.backend()->project()->rename(id, "welcome").isEmpty());
        QVERIFY(window.editor()->toPlainText().contains("welcome(int"));
        QVERIFY(window.editor()->toPlainText().contains("greet(String"));
        window.backend()->project()->undoRename();
        QVERIFY(window.editor()->toPlainText().contains("greet(int"));
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
