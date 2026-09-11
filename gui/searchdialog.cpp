#include "searchdialog.h"
#include "mainwindow.h"
#include "nodeicons.h"
#include <QtWidgets>
namespace {
class ResultsModel : public QAbstractTableModel {
  public:
    using QAbstractTableModel::QAbstractTableModel;
    QJsonArray hits;
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : hits.size(); }
    int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : 2; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
            return section == 0 ? QStringLiteral("节点 / 位置") : QStringLiteral("匹配内容");
        return {};
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid())
            return {};
        const auto hit = hits[index.row()].toObject();
        if (role == Qt::DecorationRole && index.column() == 0)
            return NodeIcons::icon(hit.value("icon_kind").toString(hit.value("kind").toString()),
                                   hit.value("flags").toInt(), hit.value("constructor").toBool());
        if (role == Qt::UserRole)
            return hit;
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
            if (index.column())
                return hit.value("text").toString();
            return hit.value("node").toString() +
                   (hit.value("line").toInt() > 0 ? QString(" : %1").arg(hit.value("line").toInt())
                                                  : QString());
        }
        return {};
    }
    void clear() {
        beginResetModel();
        hits = {};
        endResetModel();
    }
    void append(const QJsonArray &batch) {
        if (batch.isEmpty())
            return;
        beginInsertRows({}, hits.size(), hits.size() + batch.size() - 1);
        for (const auto &hit : batch)
            hits.append(hit);
        endInsertRows();
    }
};
class MatchDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QRegularExpression expression;
    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        QString text = opt.text;
        auto style = opt.widget ? opt.widget->style() : QApplication::style();
        const auto textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        opt.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);
        QTextLayout layout(text, option.font);
        QList<QTextLayout::FormatRange> formats;
        if (expression.isValid() && !expression.pattern().isEmpty()) {
            auto matches = expression.globalMatch(text);
            int count = 0;
            while (matches.hasNext() && count++ < 100) {
                auto match = matches.next();
                QTextLayout::FormatRange range;
                range.start = match.capturedStart();
                range.length = match.capturedLength();
                range.format.setBackground(QColor("#ffe373"));
                range.format.setForeground(QColor("#152536"));
                formats << range;
            }
        }
        layout.setFormats(formats);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid())
            line.setLineWidth(100000);
        layout.endLayout();
        p->save();
        p->setClipRect(textRect);
        p->setPen(option.palette.color(
            option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        layout.draw(p, QPointF(textRect.left() + 3,
                               option.rect.top() +
                                   (option.rect.height() - option.fontMetrics.height()) / 2));
        p->restore();
    }
};
} // namespace
SearchDialog::SearchDialog(MainWindow *window) : QDialog(window), window_(window) {
    setObjectName("projectSearch");
    setWindowTitle(tr("项目搜索"));
    resize(1160, 700);
    setMinimumSize(640, 480);
    setModal(false);
    auto root = new QVBoxLayout(this);
    auto top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("搜索文本：")));
    query_ = new QLineEdit;
    query_->setObjectName("projectQuery");
    query_->setClearButtonEnabled(true);
    query_->setPlaceholderText(tr("输入类名、方法名、字段或代码…"));
    top->addWidget(query_, 1);
    sensitive_ = new QCheckBox(tr("区分大小写"));
    regex_ = new QCheckBox(tr("正则"));
    automatic_ = new QCheckBox(tr("自动搜索"));
    automatic_->setChecked(true);
    top->addWidget(sensitive_);
    top->addWidget(regex_);
    top->addWidget(automatic_);
    root->addLayout(top);
    auto filters = new QHBoxLayout;
    filters_ = filters;
    auto group = new QGroupBox(tr("在以下位置搜索"));
    auto locations = new QHBoxLayout(group);
    auto check = [&](const QString &label, bool checked) {
        auto box = new QCheckBox(label);
        box->setChecked(checked);
        locations->addWidget(box);
        return box;
    };
    classes_ = check(tr("类名"), true);
    methods_ = check(tr("方法名"), true);
    fields_ = check(tr("字段名"), true);
    code_ = check(tr("代码"), true);
    comments_ = check(tr("注释"), false);
    resources_ = check(tr("资源（含 ARSC）"), false);
    resources_->setObjectName("searchResources");
    filters->addWidget(group);
    auto packages = new QGroupBox(tr("限制 package"));
    auto packageLayout = new QVBoxLayout(packages);
    package_ = new QLineEdit;
    package_->setObjectName("searchPackage");
    package_->setClearButtonEnabled(true);
    package_->setPlaceholderText(tr("例如 androidx.activity；留空搜索整个项目"));
    packageLayout->addWidget(package_);
    filters->addWidget(packages, 1);
    root->addLayout(filters);
    table_ = new QTableView;
    table_->setObjectName("searchResults");
    model_ = new ResultsModel(table_);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setWordWrap(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(28);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->setItemDelegate(new MatchDelegate(table_));
    root->addWidget(table_, 1);
    auto progress = new QHBoxLayout;
    auto all = new QPushButton(tr("加载所有（最多 10000）")),
         more = new QPushButton(tr("加载更多")), stop = new QPushButton(tr("停止"));
    progress->addWidget(all);
    progress->addWidget(more);
    progress->addWidget(stop);
    status_ = new QLabel(tr("名称搜索直接使用索引；代码结果会随后台生成逐批出现。"));
    status_->setWordWrap(true);
    progress->addWidget(status_, 1);
    root->addLayout(progress);
    auto footer = new QHBoxLayout;
    keep_ = new QCheckBox(tr("跳转后保持窗口"));
    keep_->setChecked(true);
    footer->addWidget(keep_);
    footer->addStretch();
    auto copy = new QPushButton(tr("复制结果")), search = new QPushButton(tr("搜索")),
         go = new QPushButton(tr("转到")), close = new QPushButton(tr("关闭"));
    footer->addWidget(copy);
    footer->addWidget(search);
    footer->addWidget(go);
    footer->addWidget(close);
    root->addLayout(footer);
    const QList<QPair<QString, QCheckBox *>> options = {
        {"resources", resources_}, {"classes", classes_}, {"methods", methods_},   {"fields", fields_},
        {"code", code_},       {"comments", comments_}, {"regex", regex_},
        {"case", sensitive_},  {"auto", automatic_},    {"keep", keep_}};
    for (const auto &option : options) {
        const QString key = "search/" + option.first;
        option.second->setChecked(QSettings().value(key, option.second->isChecked()).toBool());
        connect(option.second, &QCheckBox::toggled, this,
                [key](bool v) { QSettings().setValue(key, v); });
    }
    package_->setText(QSettings().value("search/package").toString());
    connect(package_, &QLineEdit::textChanged, this,
            [](const QString &v) { QSettings().setValue("search/package", v); });
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(350);
    auto changed = [this] {
        debounce_->stop();
        window_->backend()->cancelSearch(request_);
        if (automatic_->isChecked())
            debounce_->start();
    };
    connect(window_->backend()->project(), &Project::renamed, this, changed);
    connect(query_, &QLineEdit::textChanged, this, changed);
    connect(package_, &QLineEdit::textChanged, this, changed);
    for (auto box : {resources_, classes_, methods_, fields_, code_, comments_, regex_, sensitive_, automatic_})
        connect(box, &QCheckBox::toggled, this, changed);
    connect(debounce_, &QTimer::timeout, this, [this] { startSearch(); });
    connect(query_, &QLineEdit::returnPressed, this, [this] { startSearch(); });
    connect(search, &QPushButton::clicked, this, [this] { startSearch(); });
    connect(more, &QPushButton::clicked, this, [this] { startSearch(qMin(limit_ + 100, 10000)); });
    connect(all, &QPushButton::clicked, this, [this] { startSearch(10000); });
    connect(stop, &QPushButton::clicked, this, [this] {
        debounce_->stop();
        window_->backend()->cancelSearch(request_);
        status_->setText(tr("已停止搜索，保留现有结果。"));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(go, &QPushButton::clicked, this, &SearchDialog::navigate);
    connect(table_, &QTableView::doubleClicked, this, [this] { navigate(); });
    connect(copy, &QPushButton::clicked, this, [this] {
        QString text;
        for (const auto &v : static_cast<ResultsModel *>(model_)->hits) {
            auto h = v.toObject();
            text += h.value("node").toString() + ":" + QString::number(h.value("line").toInt()) +
                    '\t' + h.value("text").toString() + '\n';
        }
        QApplication::clipboard()->setText(text);
    });
    connect(window_->backend(), &Backend::searchBatch, this,
            [this](int id, const QJsonArray &hits) {
                if (id != request_)
                    return;
                static_cast<ResultsModel *>(model_)->append(hits);
                status_->setText(tr("已找到 %1 条，正在搜索…").arg(model_->rowCount()));
            });
    connect(window_->backend(), &Backend::searchProgress, this,
            [this](int id, int scanned, int total) {
                if (id == request_)
                    status_->setText(tr("已找到 %1 条 · 已搜索 %2 / %3 个源码文件")
                                         .arg(model_->rowCount())
                                         .arg(scanned)
                                         .arg(total));
            });
    connect(window_->backend(), &Backend::searchCompleted, this,
            [this](int id, const SearchResult &r) {
                if (id != request_)
                    return;
                if (!r.error.isEmpty()) {
                    status_->setText(tr("搜索错误：%1").arg(r.error));
                    return;
                }
                status_->setToolTip(tr("复用 %1 个源码缓存；索引排除 %2 个不匹配文件").arg(r.cachedFiles).arg(r.indexRejected));
                status_->setText(tr("%1 条结果%2 · %3 个文件 · %4 未生成 / %5 超出大小限制")
                                     .arg(model_->rowCount())
                                     .arg(r.canceled    ? tr("（已停止）")
                                          : r.truncated ? tr("，可加载更多")
                                                        : tr("，搜索完成"))
                                     .arg(r.scanned)
                                     .arg(r.missing)
                                     .arg(r.skipped));
            });
}
void SearchDialog::startSearch(int limit) {
    debounce_->stop();
    limit_ = limit;
    SearchOptions o;
    o.query = query_->text();
    o.package = package_->text();
    o.classes = classes_->isChecked();
    o.methods = methods_->isChecked();
    o.fields = fields_->isChecked();
    o.code = code_->isChecked();
    o.comments = comments_->isChecked();
    o.resources = resources_->isChecked();
    o.regex = regex_->isChecked();
    o.caseSensitive = sensitive_->isChecked();
    o.limit = limit;
    static_cast<ResultsModel *>(model_)->clear();
    static_cast<MatchDelegate *>(table_->itemDelegate())->expression = searchExpression(o);
    request_ = window_->backend()->search(o);
    setWindowTitle(tr("项目搜索：%1").arg(o.query));
    status_->setText(o.query.isEmpty() ? tr("请输入搜索文本。") : tr("搜索中…"));
}
void SearchDialog::navigate() {
    auto index = table_->currentIndex();
    if (!index.isValid())
        return;
    auto hit = index.data(Qt::UserRole).toJsonObject();
    const auto id = hit.value("id").toString();
    if (hit.value("kind") == "resource") {
        window_->openResource(hit.value("path").toString(), hit.value("entry").toString(),
                              hit.value("generated").toString(), hit.value("line").toInt());
    } else window_->navigateTo(id.isEmpty() ? Project::classId(hit.value("class").toString()) : id,
                        hit.value("line").toInt());
    if (!keep_->isChecked())
        hide();
}
void SearchDialog::closeEvent(QCloseEvent *event) {
    debounce_->stop();
    window_->backend()->cancelSearch(request_);
    QDialog::closeEvent(event);
}

void SearchDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (filters_)
        filters_->setDirection(width() < 900 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

void SearchDialog::setPackage(const QString &name) {
    package_->setText(QString(name).replace('/', '.'));
    query_->setFocus();
    query_->selectAll();
    startSearch();
}
