#include "referencesdialog.h"
#include "mainwindow.h"
#include "nodeicons.h"
#include <QtConcurrent>
#include <QtWidgets>
#include <algorithm>
namespace {
// Build line and declaration lookup once per source, not once per bytecode reference.
struct ReferenceDocument {
    SourceDocument doc;
    QVector<int> lines{0};
    QVector<SourceSpan> members, matches;
    QHash<QString, int> declarations;
    explicit ReferenceDocument(SourceDocument source = {}, const QString &id = {}) : doc(std::move(source)) {
        for (int i = 0; i < doc.text.size(); ++i)
            if (doc.text[i] == '\n') lines.append(i + 1);
        for (const auto &span : doc.spans) {
            if (span.declaration) {
                if (!declarations.contains(span.id)) declarations.insert(span.id, span.start);
                if (span.id.contains("->") && !span.id.contains("@local:")) members.append(span);
            } else if (span.id == id || (!id.contains("->") && !span.id.contains("@local:") &&
                       Project::classId(Project::classOf(span.id)) == id)) matches.append(span);
        }
        const auto order = [](const SourceSpan &a, const SourceSpan &b) { return a.start < b.start; };
        std::sort(members.begin(), members.end(), order);
        std::sort(matches.begin(), matches.end(), order);
    }
    int start(const QString &from) const { return declarations.value(from, -1); }
    int end(const QString &from) const {
        const int at = start(from);
        if (at < 0 || !from.contains("->")) return doc.text.size();
        const auto next = std::upper_bound(members.cbegin(), members.cend(), at,
            [](int p, const SourceSpan &s) { return p < s.start; });
        return next == members.cend() ? doc.text.size() : next->start;
    }
    int line(int at) const {
        return int(std::upper_bound(lines.cbegin(), lines.cend(), at) - lines.cbegin());
    }
    QString text(int at) const {
        const int row = line(at) - 1;
        if (row < 0) return {};
        const int a = lines[row], b = row + 1 < lines.size() ? lines[row + 1] - 1 : doc.text.size();
        return doc.text.mid(a, qMin(1200, b - a));
    }
    QJsonArray highlights(int at) const {
        QJsonArray result;
        const int row = line(at) - 1;
        if (row < 0) return result;
        const int start = lines[row], end = row + 1 < lines.size() ? lines[row + 1] : doc.text.size();
        auto match = std::lower_bound(matches.cbegin(), matches.cend(), start,
            [](const SourceSpan &span, int p) { return span.start < p; });
        for (; match != matches.cend() && match->start < end; ++match)
            result.append(QJsonObject{{"start", match->start - start}, {"end", match->end - start}});
        return result;
    }
    QString container(int at, const QString &fallback) const {
        auto decl = std::lower_bound(members.cbegin(), members.cend(), at,
            [](const SourceSpan &s, int p) { return s.start < p; });
        return decl == members.cbegin() ? fallback : (--decl)->id;
    }
};
class SnippetDelegate : public QStyledItemDelegate {
  public:
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
            const auto match = matches.next();
            const auto t = match.captured();
            bool referenced = false;
            for (const auto &v : index.data(Qt::UserRole + 3).toJsonArray()) {
                const auto range = v.toObject();
                if (match.capturedStart() >= range.value("start").toInt() &&
                    match.capturedEnd() <= range.value("end").toInt()) { referenced = true; break; }
            }
            QString esc = t.toHtmlEscaped();
            if (referenced)
                esc.replace(t.toHtmlEscaped(),
                            "<span style='background:#ffe373;color:#152536'>" +
                                t.toHtmlEscaped() + "</span>");
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
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    auto delegate = new SnippetDelegate(table);
    table->setItemDelegate(delegate);
    root->addWidget(table, 1);
    auto count = new QLabel(tr("正在查询引用索引…"));
    root->addWidget(count);
    auto bottom = new QHBoxLayout;
    auto keep = new QCheckBox(tr("保持窗口"));
    keep->setChecked(QSettings().value("references/keep", false).toBool());
    connect(keep, &QCheckBox::toggled, this,
            [](bool v) { QSettings().setValue("references/keep", v); });
    bottom->addWidget(keep);
    auto preview = new QPushButton(tr("打开选中来源 / 加载源码"));
    bottom->addWidget(preview);
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
    auto navigate = [window, table, keep, this, id] {
        auto index = table->currentIndex();
        if (!index.isValid())
            return;
        auto first = index.siblingAtColumn(0);
        window->navigateTo(first.data(Qt::UserRole + 1).toString(),
                           first.data(Qt::UserRole + 2).toInt(), id);
        if (!keep->isChecked())
            this->close();
    };
    connect(go, &QPushButton::clicked, this, navigate);
    connect(preview, &QPushButton::clicked, this, [window, table] {
        auto i = table->currentIndex().siblingAtColumn(0);
        if (i.isValid())
            window->navigateTo(i.data(Qt::UserRole + 1).toString());
    });
    connect(table, &QTableView::doubleClicked, this,
            [navigate](const QModelIndex &) { navigate(); });
    connect(copy, &QPushButton::clicked, this, [model] {
        QString text;
        for (int i = 0; i < model->rowCount(); i++)
            text += model->item(i, 0)->text() + "\t" + model->item(i, 1)->text() + "\n";
        QApplication::clipboard()->setText(text);
    });
    auto previewTimer = new QTimer(this);
    previewTimer->setInterval(200);
    auto attempted = std::make_shared<QSet<QString>>();
    connect(previewTimer, &QTimer::timeout, this, [window, table, model, canceled, attempted] {
        if (*canceled || window->backend()->busy() || !table->isVisible()) return;
        int first = table->rowAt(0);
        if (first < 0) first = 0;
        int last = table->rowAt(table->viewport()->height() - 1);
        if (last < 0) last = model->rowCount() - 1;
        for (int row = first; row <= last; ++row) {
            if (model->item(row)->data(Qt::UserRole + 2).toInt() > 0) continue;
            const auto owner = window->backend()->project()->owner(
                Project::classOf(model->item(row)->data(Qt::UserRole + 1).toString()));
            if (attempted->contains(owner)) continue;
            attempted->insert(owner);
            window->backend()->request(owner, false);
            break;
        }
    });
    previewTimer->start();
    connect(window->backend(), &Backend::sourceReady, this,
            [this, window, model, id, count](const QString &name, bool smali, const QString &path) {
                if (smali)
                    return;
                auto project = window->backend()->project()->snapshot();
                bool relevant = false;
                for (int row = 0; row < model->rowCount(); ++row)
                    if (project->owner(Project::classOf(model->item(row)->data(Qt::UserRole + 1).toString())) == project->owner(name)) {
                        relevant = true;
                        break;
                    }
                if (!relevant) return;
                auto task = new QFutureWatcher<ReferenceDocument>(this);
                connect(task, &QFutureWatcher<ReferenceDocument>::finished, this,
                        [task, model, project, name, id, count] {
                            auto doc = task->result();
                            task->deleteLater();
                            QSet<QString> shown;
                            for (int row = model->rowCount() - 1; row >= 0; --row) {
                                auto node = model->item(row);
                                const auto from = node->data(Qt::UserRole + 1).toString();
                                if (project->owner(Project::classOf(from)) != project->owner(name)) continue;
                                const int start = doc.start(from), end = doc.end(from);
                                if (start < 0 && from.contains("->")) continue;
                                auto match = std::lower_bound(doc.matches.cbegin(), doc.matches.cend(), start,
                                    [](const SourceSpan &span, int p) { return span.start < p; });
                                bool matched = false, replaced = false;
                                for (; match != doc.matches.cend() && match->start < end; ++match) {
                                    matched = true;
                                    const auto key = from + ':' + QString::number(doc.line(match->start));
                                    if (shown.contains(key)) continue;
                                    shown.insert(key);
                                    auto target = replaced ? node->clone() : node;
                                    auto snippet = replaced ? new QStandardItem : model->item(row, 1);
                                    snippet->setText(doc.text(match->start));
                                    snippet->setData(doc.highlights(match->start), Qt::UserRole + 3);
                                    target->setData(doc.line(match->start), Qt::UserRole + 2);
                                    if (replaced) model->appendRow({target, snippet});
                                    replaced = true;
                                }
                                if (matched && !replaced) model->removeRow(row);
                            }
                            count->setText(QObject::tr("%1 处引用").arg(model->rowCount()));
                        });
                task->setFuture(QtConcurrent::run(
                    [project, name, path, id] { return ReferenceDocument(project->document(name, false, path), id); }));
            });
    auto project = window->backend()->project()->snapshot();
    auto cached = window->backend()->cachedSources();
    const QString allSources = window->backend()->workspacePath() + "/all-java/";
    auto settings = window->backend()->settings();
    const bool allReady = window->backend()->projectReady();
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
                    auto snippet = new QStandardItem(row.value("text").toString());
                    snippet->setData(row.value("highlights").toArray(), Qt::UserRole + 3);
                    model->appendRow({node, snippet});
                }
                count->setText(QObject::tr("已找到 %1 处引用…").arg(model->rowCount()));
            });
    connect(task, &QFutureWatcher<QJsonArray>::finished, this,
            [task, model, count, stop, canceled] {
                count->setText(QObject::tr("%1 处引用%2")
                                   .arg(model->rowCount())
                                   .arg(*canceled ? QObject::tr("（已停止）") : QString()));
                stop->setEnabled(false);
                task->deleteLater();
            });
    task->setFuture(QtConcurrent::run([project, id, cached, allSources, settings, allReady,
                                       canceled](QPromise<QJsonArray> &promise) {
        QJsonArray rows;
        auto refs = project->xrefs(id);
        if (id.contains("@local:"))
            refs.append(QJsonObject{{"from", id.left(id.indexOf("@local:"))}, {"target", id}});
        // Group by owner: one cached document read per class, without spawning processes.
        QList<QPair<QString, QJsonObject>> ordered;
        for (const auto &v : refs) {
            const auto ref = v.toObject();
            ordered.append({project->owner(Project::classOf(ref.value("from").toString())), ref});
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });
        QString loadedOwner;
        ReferenceDocument doc;
        QSet<QString> resolvedMethods;
        QSet<QString> seen;
        for (const auto &v : ordered) {
            if (rows.size() >= 64) {
                promise.addResult(rows);
                rows = {};
            }
            if (*canceled)
                break;
            const auto &ref = v.second;
            auto from = ref.value("from").toString(), name = Project::classOf(from),
                 owner = project->owner(name);
            if (resolvedMethods.contains(from)) continue;
            if (loadedOwner != owner) {
                loadedOwner = owner;
                QString path = cached.value(name + ":java", cached.value(owner + ":java"));
                if (path.isEmpty() && (allReady || QFileInfo::exists(Project::sourcePath(allSources, owner, ".map.json"))))
                    path = Project::sourcePath(allSources, owner, ".java");
                if (!path.isEmpty() && QFileInfo(path).size() <= qint64(settings.sourceMiB) * 1048576)
                    doc = ReferenceDocument(project->document(name, false, path), id);
                else
                    doc = ReferenceDocument();
            }
            const int start = doc.start(from),
                      end = start < 0 && from.contains("->") ? 0 : doc.end(from);
            bool found = false;
            auto match = std::lower_bound(doc.matches.cbegin(), doc.matches.cend(), start,
                [](const SourceSpan &span, int pos) { return span.start < pos; });
            for (; match != doc.matches.cend() && match->start < end; ++match) {
                const auto &span = *match;
                int line = doc.line(span.start);
                QString key = name + ":" + QString::number(line);
                found = true;
                if (seen.contains(key)) continue;
                seen.insert(key);
                rows.append(QJsonObject{{"from", doc.container(span.start, from)},
                                        {"line", line}, {"text", doc.text(span.start)}, {"highlights", doc.highlights(span.start)}});
            }
            if (found) resolvedMethods.insert(from);
            if (!found) {
                QString key = from + ":" + ref.value("kind").toString();
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                QString text;
                int line = 0;
                if (start >= 0) {
                    line = doc.line(start);
                    text = doc.text(start);
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
