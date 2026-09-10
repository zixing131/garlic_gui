#include "referencesdialog.h"
#include "mainwindow.h"
#include "nodeicons.h"
#include <QtConcurrent>
#include <QtWidgets>
namespace {
class SnippetDelegate : public QStyledItemDelegate {
  public:
    QString token;
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (index.column() == 0) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        QString text = opt.text;
        opt.text.clear();
        auto style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);
        QString html;
        QRegularExpression re("[A-Za-z_$][A-Za-z0-9_$]*|[^A-Za-z_$]+");
        auto matches = re.globalMatch(text);
        const QSet<QString> keywords = {"public", "private", "protected", "static",
                                        "final",  "class",   "new",       "return",
                                        "void",   "if",      "else",      "interface"};
        while (matches.hasNext()) {
            auto t = matches.next().captured();
            QString esc = t.toHtmlEscaped();
            if (!token.isEmpty() && t.contains(token))
                esc.replace(token.toHtmlEscaped(),
                            "<span style='background:#ffe373;color:#152536'>" +
                                token.toHtmlEscaped() + "</span>");
            if (keywords.contains(t))
                esc = "<b style='color:#397eca'>" + esc + "</b>";
            html += esc;
        }
        QTextDocument doc;
        doc.setDefaultFont(option.font);
        doc.setDocumentMargin(0);
        doc.setHtml("<span style='white-space:pre;color:" +
                    option.palette
                        .color(option.state & QStyle::State_Selected ? QPalette::HighlightedText
                                                                     : QPalette::Text)
                        .name() +
                    "'>" + html + "</span>");
        p->save();
        p->setClipRect(option.rect);
        p->translate(option.rect.left() + 4, option.rect.top() + 3);
        doc.drawContents(p);
        p->restore();
    }
};
} // namespace
ReferencesDialog::ReferencesDialog(MainWindow *window, const QString &id) : QDialog(window) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("referencesDialog");
    setWindowTitle(tr("查找引用：%1").arg(window->backend()->project()->symbolName(id)));
    resize(1200, 700);
    auto root = new QVBoxLayout(this);
    auto label =
        new QLabel(tr("查找用例：%1")
                       .arg(Project::classOf(id).replace('/', '.') +
                            (id.contains("->") ? "." + window->backend()->project()->symbolName(id)
                                               : QString())));
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(label);
    auto table = new QTableView;
    table->setObjectName("referenceResults");
    auto model = new QStandardItemModel(0, 2, table);
    model->setHorizontalHeaderLabels({tr("节点"), tr("代码 / 引用位置")});
    table->setModel(model);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(28);
    table->setColumnWidth(0, 560);
    table->horizontalHeader()->setStretchLastSection(true);
    auto delegate = new SnippetDelegate(table);
    delegate->token = window->backend()->project()->symbolName(id).section('$', -1);
    table->setItemDelegate(delegate);
    root->addWidget(table, 1);
    auto count = new QLabel(tr("正在查询引用并加载代码…"));
    root->addWidget(count);
    auto bottom = new QHBoxLayout;
    auto keep = new QCheckBox(tr("保持窗口"));
    bottom->addWidget(keep);
    bottom->addStretch();
    auto copy = new QPushButton(tr("复制全部")), go = new QPushButton(tr("转到")),
         stop = new QPushButton(tr("停止")), close = new QPushButton(tr("关闭"));
    bottom->addWidget(copy);
    bottom->addWidget(stop);
    bottom->addWidget(go);
    bottom->addWidget(close);
    root->addLayout(bottom);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    connect(this, &QObject::destroyed, [canceled] { *canceled = true; });
    connect(stop, &QPushButton::clicked, this, [canceled, stop] {
        *canceled = true;
        stop->setEnabled(false);
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto navigate = [window, table, keep, this] {
        auto index = table->currentIndex();
        if (!index.isValid())
            return;
        auto first = index.siblingAtColumn(0);
        window->navigateTo(first.data(Qt::UserRole + 1).toString(),
                           first.data(Qt::UserRole + 2).toInt());
        if (!keep->isChecked())
            this->close();
    };
    connect(go, &QPushButton::clicked, this, navigate);
    connect(table, &QTableView::doubleClicked, this,
            [navigate](const QModelIndex &) { navigate(); });
    connect(copy, &QPushButton::clicked, this, [model] {
        QString text;
        for (int i = 0; i < model->rowCount(); i++)
            text += model->item(i, 0)->text() + "\t" + model->item(i, 1)->text() + "\n";
        QApplication::clipboard()->setText(text);
    });
    auto project = window->backend()->project()->snapshot();
    QHash<QString, QString> cached;
    for (const auto &name : project->classes()) {
        auto p = window->backend()->cachedPath(name, false);
        if (!p.isEmpty())
            cached[name] = p;
    }
    auto engine = window->backend()->engine();
    auto settings = window->backend()->settings();
    auto temp = std::make_shared<QTemporaryDir>();
    auto task = new QFutureWatcher<QJsonArray>(this);
    connect(task, &QFutureWatcher<QJsonArray>::resultReadyAt, this,
            [task, model, count, project](int index) {
                auto rows = task->resultAt(index);
                for (const auto &v : rows) {
                    auto row = v.toObject();
                    auto symbol = row.value("from").toString();
                    auto info = project->info(Project::classOf(symbol));
                    QString kind = info.value("kind").toString();
                    int flags = info.value("flags").toInt();
                    for (const auto &collection : {"methods", "fields"})
                        for (const auto &v : info.value(collection).toArray()) {
                            auto m = v.toObject();
                            if (m.value("id").toString() == symbol) {
                                kind = QString(collection) == "methods" ? "method" : "field";
                                flags = m.value("flags").toInt();
                            }
                        }
                    auto node = new QStandardItem(
                        NodeIcons::icon(kind, flags, symbol.contains("-><init>")),
                        project->displayName(Project::classOf(symbol)).replace('/', '.') +
                            (symbol.contains("->")
                                 ? "." + project->symbolName(symbol) +
                                       (symbol.contains('(') ? symbol.mid(symbol.indexOf('('))
                                                             : QString())
                                 : QString()));
                    node->setData(symbol, Qt::UserRole + 1);
                    node->setData(row.value("line").toInt(), Qt::UserRole + 2);
                    model->appendRow({node, new QStandardItem(row.value("text").toString())});
                }
                count->setText(QObject::tr("已找到 %1 处引用，正在加载…").arg(model->rowCount()));
            });
    connect(task, &QFutureWatcher<QJsonArray>::finished, this,
            [task, model, count, stop, canceled] {
                count->setText(QObject::tr("%1 处引用%2")
                                   .arg(model->rowCount())
                                   .arg(*canceled ? QObject::tr("（已停止）") : QString()));
                stop->setEnabled(false);
                task->deleteLater();
            });
    task->setFuture(QtConcurrent::run(
        [project, id, cached, engine, settings, temp, canceled](QPromise<QJsonArray> &promise) {
            QJsonArray rows;
            auto refs = project->xrefs(id);
            QHash<QString, SourceDocument> docs;
            QSet<QString> seen;
            for (const auto &v : refs) {
                if (!rows.isEmpty()) {
                    promise.addResult(rows);
                    rows = {};
                }
                if (*canceled)
                    break;
                auto ref = v.toObject();
                auto from = ref.value("from").toString(), name = Project::classOf(from),
                     owner = project->owner(name);
                if (!docs.contains(owner)) {
                    docs.clear();
                    QString path = cached.value(name);
                    QString input = project->info(name).value("input").toString(project->input());
                    if (path.isEmpty()) {
                        QString dir = temp->path() + "/source";
                        QDir(dir).removeRecursively();
                        QDir().mkpath(dir);
                        QProcess proc;
                        auto env = QProcessEnvironment::systemEnvironment();
                        env.insert("GARLIC_SOURCE_MAP_DIR", dir);
                        proc.setProcessEnvironment(env);
                        bool single = QFileInfo(input).suffix() == "class";
                        path = dir + "/" + (single ? "source" : owner) + ".java";
                        if (single)
                            proc.setStandardOutputFile(path);
                        proc.start(engine, {input, "-c", owner, "-o", dir, "-t", "1"});
                        QElapsedTimer timeout;
                        timeout.start();
                        while (!proc.waitForFinished(50)) {
                            proc.readAllStandardError();
                            if (!single)
                                proc.readAllStandardOutput();
                            if (*canceled || timeout.elapsed() > 30000) {
                                proc.kill();
                                proc.waitForFinished(1000);
                                break;
                            }
                        }
                    }
                    if (QFileInfo(path).size() <= qint64(settings.sourceMiB) * 1048576)
                        docs[owner] = project->document(name, false, path);
                    else
                        docs[owner] = {};
                }
                const auto &doc = docs[owner];
                int start = -1, end = doc.text.size();
                for (const auto &span : doc.spans)
                    if (span.id == from && span.declaration) {
                        start = span.start;
                        break;
                    }
                if (start >= 0 && from.contains("->"))
                    for (const auto &span : doc.spans)
                        if (span.declaration && span.id.contains("->") && span.start > start)
                            end = qMin(end, span.start);
                bool found = false;
                for (const auto &span : doc.spans)
                    if (span.id == id && !span.declaration &&
                        (start < 0 || (span.start >= start && span.start < end))) {
                        int line = doc.text.left(span.start).count('\n') + 1;
                        QString key = name + ":" + QString::number(line);
                        if (seen.contains(key)) {
                            found = true;
                            continue;
                        }
                        seen.insert(key);
                        int a = doc.text.lastIndexOf('\n', span.start) + 1,
                            b = doc.text.indexOf('\n', span.start);
                        if (b < 0)
                            b = doc.text.size();
                        QString container = from;
                        int closest = -1;
                        for (const auto &decl : doc.spans)
                            if (decl.declaration && decl.id.contains("->") &&
                                decl.start < span.start && decl.start > closest) {
                                closest = decl.start;
                                container = decl.id;
                            }
                        rows.append(QJsonObject{{"from", container},
                                                {"line", line},
                                                {"text", doc.text.mid(a, qMin(1200, b - a))}});
                        found = true;
                    }
                if (!found) {
                    QString key = from + ":" + ref.value("kind").toString();
                    if (seen.contains(key))
                        continue;
                    seen.insert(key);
                    QString text;
                    int line = 0;
                    if (start >= 0) {
                        line = doc.text.left(start).count('\n') + 1;
                        int a = doc.text.lastIndexOf('\n', start) + 1,
                            b = doc.text.indexOf('\n', start);
                        text = doc.text.mid(a, b < 0 ? 1200 : qMin(1200, b - a));
                    }
                    if (text.isEmpty())
                        text = ref.value("kind").toString() +
                               QString(" · bytecode +%1 · ").arg(ref.value("offset").toInt()) + id;
                    rows.append(QJsonObject{{"from", from}, {"line", line}, {"text", text}});
                }
            }
            if (!rows.isEmpty())
                promise.addResult(rows);
        }));
}
