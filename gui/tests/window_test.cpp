#include "classview.h"
#include "mainwindow.h"
#include "referencesdialog.h"
#include <QtTest>
#include <QtWidgets>

class WindowTest : public QObject {
    Q_OBJECT
  private slots:
    void shortcutsAndFilterState() {
        QCoreApplication::setOrganizationName("GarlicTests");
        QCoreApplication::setApplicationName("WindowTest");
        QSettings().clear();
        QSettings().setValue("shortcuts/查找引用", "Ctrl+Shift+U");
        QSettings().setValue("shortcuts/重命名", "F2");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.activateWindow();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(!window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        window.navigateTo("Ldemo/Main;->greet(I)Ljava/lang/String;");
        auto editor = window.editor();
        editor->setFocus();
        QTest::keyClick(editor, Qt::Key_X);
        QTRY_VERIFY(window.findChild<ReferencesDialog *>());
        window.findChild<ReferencesDialog *>()->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.activateWindow();
        editor->setFocus();
        bool renameOpened = false;
        QTimer::singleShot(100, &window, [&] {
            auto dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            renameOpened = dialog != nullptr;
            if (dialog) dialog->reject();
        });
        QTest::keyClick(editor, Qt::Key_N);
        QTRY_VERIFY(renameOpened);
        auto tree = window.findChild<QTreeView *>("classTree");
        auto filter = window.findChild<QLineEdit *>("classFilter");
        auto matches = tree->model()->match(tree->model()->index(0,0), Qt::UserRole + 1, "Ldemo/Main;", 1, Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY(!matches.isEmpty());
        auto idx = matches.first();
        tree->expand(idx.parent()); tree->expand(idx);
        tree->setCurrentIndex(idx);
        filter->setText("no_matching_class_123");
        filter->clear();
        matches = tree->model()->match(tree->model()->index(0,0), Qt::UserRole + 1, "Ldemo/Main;", 1, Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY(!matches.isEmpty());
        QVERIFY(tree->isExpanded(matches.first()));
    }
    void crlfSymbolPositions() {
        CodeEditor editor(false);
        const QString text = "// 中文\r\npublic class Example {\r\n    void greet() {}\r\n}\r\n";
        const int start = text.indexOf("greet");
        const QString id = "LExample;->greet()V";
        editor.setSource({text, {{start, start + 5, id, true}}});
        QVERIFY(editor.goToSymbol(id));
        QCOMPARE(editor.textCursor().selectedText(), QString("greet"));
        QCOMPARE(editor.symbolAtCursor(), id);
        QVERIFY(!editor.toPlainText().contains('\r'));
    }
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
