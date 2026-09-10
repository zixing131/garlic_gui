#include "classview.h"
#include "mainwindow.h"
#include "nodeicons.h"
#include "referencesdialog.h"
#include <QtTest>
#include <QtWidgets>

class WindowTest : public QObject {
    Q_OBJECT
  private slots:
    void toolbarIconsMatchActions() {
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        const QHash<QString, QString> expected{
            {"openFile", "toolopenDisk"},       {"exportSources", "toolexport"},
            {"projectSearch", "toolfind"},      {"callGraph", "methodReference"},
            {"syncEditor", "toolsync"},         {"flatPackages", "toolpackages"},
            {"navigateBack", "toolleft"},       {"navigateForward", "toolright"},
            {"stopTask", "toolclose"},          {"settings", "toolsettings"},
            {"mainActivity", "toolmainActivity"}, {"goApplication", "toolapplication"},
            {"goManifest", "toolandroidManifest"}};
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            auto action = window.findChild<QAction *>(it.key());
            QVERIFY2(action, qPrintable(it.key()));
            QCOMPARE(action->icon().cacheKey(), NodeIcons::icon(it.value()).cacheKey());
            QCOMPARE(action->toolTip(), action->text());
        }
    }
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
        QVERIFY(window.editor()->toPlainText().contains("@Override"));
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
        window.showCallGraph("Ldemo/Main;->greet(I)Ljava/lang/String;");
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QGraphicsView *>("callGraphView"), 5000);
        auto graph = window.findChild<QGraphicsView *>("callGraphView");
        QTRY_VERIFY_WITH_TIMEOUT(!graph->scene()->items().isEmpty(), 5000);
        const auto beforeZoom = graph->transform().m11();
        for (int i = 0; i < 8; ++i) {
            QWheelEvent zoom(graph->viewport()->rect().center(),
                             graph->mapToGlobal(graph->viewport()->rect().center()), QPoint(),
                             QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate,
                             false);
            QCoreApplication::sendEvent(graph->viewport(), &zoom);
        }
        QVERIFY(graph->transform().m11() > beforeZoom);
        auto horizontal = graph->horizontalScrollBar();
        QVERIFY(horizontal->maximum() > horizontal->minimum());
        horizontal->setValue((horizontal->minimum() + horizontal->maximum()) / 2);
        const int beforePan = horizontal->value();
        const QPoint center = graph->viewport()->rect().center();
        QTest::mousePress(graph->viewport(), Qt::LeftButton, {}, center);
        QTest::mouseMove(graph->viewport(), center - QPoint(80, 0), 20);
        QTest::mouseRelease(graph->viewport(), Qt::LeftButton, {}, center - QPoint(80, 0));
        QVERIFY(horizontal->value() > beforePan);
        window.findChild<QGraphicsView *>("callGraphView")->window()->close();
        auto tree = window.findChild<QTreeView *>("classTree");
        auto filter = window.findChild<QLineEdit *>("classFilter");
        QModelIndexList matches;
        QTRY_VERIFY_WITH_TIMEOUT(
            !(matches = tree->model()->match(tree->model()->index(0, 0), Qt::UserRole + 1,
                                             "Ldemo/Main;", 1,
                                             Qt::MatchExactly | Qt::MatchRecursive))
                 .isEmpty(),
            5000);
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
    void largeProjectOpenTime() {
        const auto path = qEnvironmentVariable("GARLIC_TEST_LARGE_APK");
        if (path.isEmpty())
            QSKIP("Set GARLIC_TEST_LARGE_APK to measure full tree readiness");
        MainWindow window(qEnvironmentVariable("GARLIC_TEST_ENGINE"));
        window.show();
        QElapsedTimer elapsed;
        elapsed.start();
        window.openPath(path);
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 120000);
        const auto indexedMs = elapsed.elapsed();
        const auto count = QString::number(window.backend()->project()->classes().size());
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("muted")->text().contains(count),
                                 30000);
        qInfo() << "Index ready ms:" << indexedMs << "Full directory ready ms:" << elapsed.elapsed()
                << "classes:" << count;
        if (!qEnvironmentVariable("GARLIC_SCREENSHOTS").isEmpty())
            window.grab().save(qEnvironmentVariable("GARLIC_SCREENSHOTS") + "/large-project.png");
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
    void localVariableDoubleClick() {
        CodeEditor editor(false);
        const QString text = "void sample() { int answer = 7; return answer + 1; }";
        editor.setSource({text, {}});
        editor.resize(640, 160);
        editor.show();
        const int use = text.lastIndexOf("answer");
        auto cursor = editor.textCursor();
        cursor.setPosition(use);
        editor.setTextCursor(cursor);
        QTest::mouseDClick(editor.viewport(), Qt::LeftButton, {}, editor.cursorRect(cursor).center());
        QCOMPARE(editor.textCursor().selectedText(), QString("answer"));
        QCOMPARE(editor.textCursor().selectionStart(), text.indexOf("answer"));
    }
    void codeEditorCallGraphContextMenu() {
        CodeEditor editor(false);
        const QString text = "class Example { void run() {} }";
        const int start = text.indexOf("run");
        editor.setSource({text, {{start, start + 3, "LExample;->run()V", true}}});
        editor.resize(640, 180);
        editor.show();
        auto cursor = editor.textCursor();
        cursor.setPosition(start + 1);
        editor.setTextCursor(cursor);
        QSignalSpy requested(&editor, &CodeEditor::callGraphRequested);
        bool found = false;
        QTimer::singleShot(50, &editor, [&] {
            auto menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (auto action : menu->actions())
                if (action->text().contains("查看函数调用图") && action->text().contains('G')) {
                    found = action->isEnabled();
                    action->trigger();
                    break;
                }
            menu->close();
        });
        const QPoint position = editor.cursorRect(cursor).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, position,
                                editor.viewport()->mapToGlobal(position));
        QCoreApplication::sendEvent(editor.viewport(), &event);
        QVERIFY(found);
        QCOMPARE(requested.count(), 1);
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
        auto findBar = window.findChild<QWidget *>("codeFindBar");
        auto showFind = window.findChild<QAction *>("showCodeFind");
        QVERIFY(tree && filter && tabs && find && findBar && showFind);
        QVERIFY(!findBar->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(
            !window.backend()->busy() && !window.backend()->project()->classes().isEmpty(), 15000);
        filter->setText("Main");
        QVERIFY(tree->model()->rowCount() > 0);
        window.openClass("demo/Main");
        QTRY_VERIFY_WITH_TIMEOUT(
            window.editor() && window.editor()->toPlainText().contains("greet"), 15000);
        QCOMPARE(tabs->count(), 1);
        auto code = window.editor();
        showFind->trigger();
        QVERIFY(findBar->isVisible());
        find->setText("greet");
        QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
        QTRY_VERIFY(code->extraSelections().size() > 1);
        find->setText("not_present_123");
        QTest::keyClick(find, Qt::Key_Return);
        QCOMPARE(code->textCursor().selectedText(), QString("greet"));
        QTest::keyClick(find, Qt::Key_Escape);
        QVERIFY(!findBar->isVisible());
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
