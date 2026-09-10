#include "mainwindow.h"
#include "codeeditor.h"
#include <QtTest>
#include <QtWidgets>

class WindowTest : public QObject {
    Q_OBJECT
private slots:
    void browseFilterSwitchFindAndReopen() {
        // Isolate test preferences from the user's running GUI.
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
        auto language = window.findChild<QComboBox *>("sourceLanguage");
        auto find = window.findChild<QLineEdit *>("codeFind");
        QVERIFY(tree && filter && tabs && language && find);
        QTRY_VERIFY_WITH_TIMEOUT(tree->model()->rowCount() > 0 && tree->isEnabled(), 15000);
        filter->setText("Main");
        const auto package = tree->model()->index(0, 0);
        tree->expand(package);
        QCOMPARE(tree->model()->rowCount(package), 1);
        const auto main = tree->model()->index(0, 0, package);
        QCOMPARE(main.data().toString(), QString("Main"));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(main).center());
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 15000);
        auto code = dynamic_cast<CodeEditor *>(tabs->currentWidget());
        QVERIFY(code); QVERIFY(code->toPlainText().contains("greet"));
        find->setText("greet"); QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
        find->setText("not_present_123"); QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
        if (dex) {
            language->setCurrentIndex(1);
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 15000);
            QVERIFY(tabs->tabText(tabs->currentIndex()).endsWith(".smali"));
            language->setCurrentIndex(0);
            QCOMPARE(tabs->count(), 2);
            QVERIFY(tabs->tabText(tabs->currentIndex()).endsWith(".java"));
        }
        window.openPath(fixtures + "/Main.class");
        QTRY_VERIFY_WITH_TIMEOUT(tree->isEnabled() && tree->model()->rowCount() > 0, 15000);
        QCOMPARE(tabs->count(), 0);
        QVERIFY(!language->isEnabled());
        QVERIFY(filter->text().isEmpty());
        window.close();
    }
};
QTEST_MAIN(WindowTest)
#include "window_test.moc"
