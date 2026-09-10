#include "callgraphdialog.h"
#include "nodeicons.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtWidgets>
#include <algorithm>
#include <functional>

namespace {
class CallGraphView final : public QGraphicsView {
  public:
    explicit CallGraphView(QGraphicsScene *scene, QWidget *parent = nullptr)
        : QGraphicsView(scene, parent) {
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorUnderMouse);
        setToolTip(QObject::tr("滚动鼠标滚轮缩放；按住鼠标左键拖动画布平移。"));
    }

  protected:
    void wheelEvent(QWheelEvent *event) override {
        const int delta = event->angleDelta().y();
        if (!delta) {
            QGraphicsView::wheelEvent(event);
            return;
        }
        const qreal current = transform().m11();
        const qreal factor = delta > 0 ? 1.16 : 1.0 / 1.16;
        const qreal next = current * factor;
        if (next >= 0.12 && next <= 5.0)
            scale(factor, factor);
        event->accept();
    }
};

class GraphNode : public QGraphicsRectItem {
  public:
    GraphNode(const QString &id, const QString &label, const QColor &color,
              std::function<void(const QString &)> open)
        : id_(id), open_(std::move(open)) {
        auto text = new QGraphicsTextItem(label, this);
        text->setDefaultTextColor(QColor("#17283a"));
        text->setTextWidth(230);
        const auto bounds = text->boundingRect();
        setRect(0, 0, qMax(150., bounds.width() + 18), qMax(38., bounds.height() + 12));
        text->setPos(9, 5);
        setBrush(color);
        setPen(QPen(color.darker(130), 1.2));
        setToolTip(id + "\n" + QObject::tr("双击跳转到声明"));
        setFlag(QGraphicsItem::ItemIsSelectable);
    }

  protected:
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
        if (open_)
            open_(id_);
        event->accept();
    }

  private:
    QString id_;
    std::function<void(const QString &)> open_;
};

QString labelFor(const std::shared_ptr<Project> &project, const QString &id) {
    const auto owner = Project::classOf(id).replace('/', '.');
    return owner + "\n" + project->symbolName(id);
}
} // namespace

CallGraphDialog::CallGraphDialog(const std::shared_ptr<Project> &project, const QString &method,
                                 QWidget *parent)
    : QDialog(parent), project_(project), method_(project->canonicalId(method)) {
    setObjectName("callGraphDialog");
    setWindowTitle(tr("函数调用图：%1").arg(project_->symbolName(method_)));
    resize(1120, 720);
    setMinimumSize(700, 480);
    auto root = new QVBoxLayout(this);
    auto controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("方向：")));
    direction_ = new QComboBox;
    direction_->addItem(tr("调用者与被调用者"), "both");
    direction_->addItem(tr("仅调用者"), "callers");
    direction_->addItem(tr("仅被调用者"), "callees");
    controls->addWidget(direction_);
    controls->addWidget(new QLabel(tr("深度：")));
    depth_ = new QSpinBox;
    depth_->setRange(1, 5);
    depth_->setValue(QSettings().value("callGraph/depth", 2).toInt());
    controls->addWidget(depth_);
    auto refresh = new QPushButton(tr("刷新"));
    controls->addWidget(refresh);
    status_ = new QLabel;
    controls->addWidget(status_, 1);
    root->addLayout(controls);
    scene_ = new QGraphicsScene(this);
    view_ = new CallGraphView(scene_);
    view_->setObjectName("callGraphView");
    view_->setRenderHint(QPainter::Antialiasing);
    root->addWidget(view_, 1);
    auto close = new QPushButton(tr("关闭"));
    auto footer = new QHBoxLayout;
    footer->addStretch();
    footer->addWidget(close);
    root->addLayout(footer);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(refresh, &QPushButton::clicked, this, &CallGraphDialog::rebuild);
    connect(depth_, qOverload<int>(&QSpinBox::valueChanged), this, [](int value) {
        QSettings().setValue("callGraph/depth", value);
    });
    rebuild();
}

void CallGraphDialog::rebuild() {
    const int request = ++generation_;
    const int maxDepth = depth_->value();
    const auto direction = direction_->currentData().toString();
    const auto project = project_;
    const auto root = method_;
    status_->setText(tr("正在建立调用关系…"));
    auto watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this,
            [this, watcher, request] {
                const auto graph = watcher->result();
                watcher->deleteLater();
                if (request == generation_)
                    render(graph);
            });
    watcher->setFuture(QtConcurrent::run([project, root, direction, maxDepth] {
        QJsonArray nodes, edges;
        QHash<QString, int> levels;
        QSet<QString> expanded;
        levels.insert(root, 0);
        QStringList queue{root};
        while (!queue.isEmpty() && levels.size() < 150) {
            const auto current = queue.takeFirst();
            const int level = levels.value(current);
            if (expanded.contains(current) || qAbs(level) >= maxDepth)
                continue;
            expanded.insert(current);
            auto add = [&](const QString &other, int nextLevel, bool caller) {
                if (!other.contains("->") || levels.size() >= 150)
                    return;
                const auto id = project->canonicalId(other);
                edges.append(QJsonObject{{"from", caller ? id : current},
                                         {"to", caller ? current : id}});
                if (!levels.contains(id)) {
                    levels.insert(id, nextLevel);
                    queue.append(id);
                }
            };
            if (direction != "callees")
                for (const auto &value : project->xrefs(current))
                    add(value.toObject().value("from").toString(), level - 1, true);
            if (direction != "callers")
                for (const auto &value : project->callees(current))
                    add(value.toObject().value("target").toString(), level + 1, false);
        }
        for (auto it = levels.cbegin(); it != levels.cend(); ++it)
            nodes.append(QJsonObject{{"id", it.key()}, {"level", it.value()}});
        return QJsonObject{{"nodes", nodes}, {"edges", edges}, {"limited", levels.size() >= 150}};
    }));
}

void CallGraphDialog::render(const QJsonObject &graph) {
    scene_->clear();
    QHash<int, QStringList> layers;
    for (const auto &value : graph.value("nodes").toArray()) {
        const auto node = value.toObject();
        layers[node.value("level").toInt()].append(node.value("id").toString());
    }
    QHash<QString, GraphNode *> items;
    const auto levels = layers.keys();
    for (int level : levels) {
        auto ids = layers.value(level);
        std::sort(ids.begin(), ids.end());
        for (int row = 0; row < ids.size(); ++row) {
            const auto id = ids[row];
            const QColor color = level == 0    ? QColor("#9ee5c5")
                                 : level < 0 ? QColor("#b9dcff")
                                             : QColor("#ffe2a8");
            auto item = new GraphNode(id, labelFor(project_, id), color,
                                      [this](const QString &target) {
                                          emit navigationRequested(target);
                                      });
            item->setPos(level * 310., row * 76. - (ids.size() - 1) * 38.);
            scene_->addItem(item);
            items.insert(id, item);
        }
    }
    QPen edgePen(QColor("#7890a3"), 1.3);
    for (const auto &value : graph.value("edges").toArray()) {
        const auto edge = value.toObject();
        auto from = items.value(edge.value("from").toString());
        auto to = items.value(edge.value("to").toString());
        if (!from || !to)
            continue;
        const auto a = from->sceneBoundingRect().center();
        const auto b = to->sceneBoundingRect().center();
        auto line = scene_->addLine(QLineF(a, b), edgePen);
        line->setZValue(-1);
    }
    status_->setText(tr("%1 个函数 · %2 条调用关系%3")
                         .arg(items.size())
                         .arg(graph.value("edges").toArray().size())
                         .arg(graph.value("limited").toBool() ? tr("（已限制为 150 个节点）")
                                                               : QString()));
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-40, -40, 40, 40));
    view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
}
