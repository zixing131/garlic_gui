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
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
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
            if (dialog)
                dialog->reject();
        });
        QTest::keyClick(editor, Qt::Key_N);
        QTRY_VERIFY(renameOpened);
        auto tree = window.findChild<QTreeView *>("classTree");
        auto filter = window.findChild<QLineEdit *>("classFilter");
        auto matches =
            tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1, "Ldemo/Main;", 1,
                                 Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY(!matches.isEmpty());
        auto idx = matches.first();
        tree->expand(idx.parent());
        tree->expand(idx);
        tree->setCurrentIndex(idx);
        filter->setText("no_matching_class_123");
        QTest::qWait(220);
        filter->clear();
        QTest::qWait(220);
        matches = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1, "Ldemo/Main;",
                                       1, Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY(!matches.isEmpty());
        QVERIFY(tree->isExpanded(matches.first()));
    }
    void clearCacheAndResize() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        const auto source = window.backend()->cachedPath("demo/Main", false);
        QVERIFY(QFileInfo::exists(source));
        window.backend()->clearCache();
        QTRY_VERIFY_WITH_TIMEOUT(!window.backend()->busy(), 5000);
        QVERIFY(!QFileInfo::exists(source));
        QVERIFY(!window.editor()->toPlainText().contains("greet"));
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains("greet"), 15000);
        auto splitter = window.findChild<QSplitter *>("mainSplitter");
        QVERIFY(splitter);
        window.resize(1200, 760);
        QTest::qWait(50);
        const auto wide = splitter->sizes();
        window.resize(700, 500);
        QTest::qWait(50);
        QVERIFY2(window.width() <= 700,
                 qPrintable(QString::number(window.minimumSizeHint().width())));
        const auto narrow = splitter->sizes();
        QVERIFY(narrow[0] < wide[0]);
        QVERIFY(narrow[1] < wide[1]);
        QVERIFY(narrow[1] >= 300);
    }
    void treeModesSyncAndSideButtons() {
        QSettings().setValue("view/flatPackages", true);
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        window.openPath(qEnvironmentVariable("GARLIC_TEST_FIXTURES") + "/demo.jar");
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        auto tree = window.findChild<QTreeView *>("classTree");
        auto flat = window.findChild<QAction *>("flatPackages");
        QVERIFY(flat && flat->isChecked());
        auto matches = [&](const QString &id) {
            return tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1, id, 1,
                                        Qt::MatchExactly | Qt::MatchRecursive);
        };
        QTRY_VERIFY(!matches("Ldemo/deep/nested/Leaf;").isEmpty());
        QCOMPARE(matches("Ldemo/deep/nested/Leaf;").first().parent().data().toString(),
                 QString("demo.deep.nested"));
        flat->setChecked(false);
        QTRY_VERIFY(!matches("Ldemo/deep/nested/Leaf;").isEmpty());
        auto leaf = matches("Ldemo/deep/nested/Leaf;").first();
        QCOMPARE(leaf.parent().data().toString(), QString("nested"));
        QCOMPARE(leaf.parent().parent().data().toString(), QString("deep"));
        QCOMPARE(QSettings().value("view/flatPackages").toBool(), false);
        window.openClass("demo/deep/nested/Leaf");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("return 7"), 15000);
        window.findChild<QLineEdit *>("classFilter")->setText("hidden");
        window.findChild<QAction *>("syncEditor")->trigger();
        QCOMPARE(tree->currentIndex().data(Qt::UserRole + 1).toString(),
                 QString("Ldemo/deep/nested/Leaf;"));
        QVERIFY(tree->isExpanded(tree->currentIndex().parent()));
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty()) {
            window.resize(1100, 720);
            QTest::qWait(100);
            window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/tree-modes.png");
        }
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(window.editor()->toPlainText().contains("greet"), 15000);
        QTest::mouseClick(window.editor()->viewport(), Qt::BackButton);
        QTRY_VERIFY(window.editor()->toPlainText().contains("return 7"));
        QTest::mouseClick(window.editor()->viewport(), Qt::ForwardButton);
        QTRY_VERIFY(window.editor()->toPlainText().contains("greet"));
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
