#pragma once
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <array>

// Keep independently restored tables in their original shards. Copying a
// Project remains implicitly shared, and mutation only detaches one shard.
class ProjectMap {
public:
    static constexpr int ShardCount = 4;
    using Map = QHash<QString, QJsonObject>;
    static int shardFor(const QString &key) { return int(qHash(key, size_t(0)) % ShardCount); }
    class const_iterator {
    public:
        const_iterator(const ProjectMap *owner, int shard) : owner_(owner), shard_(shard) {
            if (shard_ < ShardCount) it_ = owner_->maps_[shard_].cbegin();
            skipEmpty();
        }
        const_iterator &operator++() { ++it_; skipEmpty(); return *this; }
        bool operator!=(const const_iterator &other) const {
            return owner_ != other.owner_ || shard_ != other.shard_ || (shard_ < ShardCount && it_ != other.it_);
        }
        const QString &key() const { return it_.key(); }
        const QJsonObject &value() const { return it_.value(); }
    private:
        void skipEmpty() {
            while (shard_ < ShardCount && it_ == owner_->maps_[shard_].cend()) {
                if (++shard_ < ShardCount) it_ = owner_->maps_[shard_].cbegin();
            }
        }
        const ProjectMap *owner_;
        int shard_;
        Map::const_iterator it_;
    };
    const_iterator cbegin() const { return {this, 0}; }
    const_iterator cend() const { return {this, ShardCount}; }
    bool contains(const QString &key) const { return maps_[shardFor(key)].contains(key); }
    QJsonObject value(const QString &key) const { return maps_[shardFor(key)].value(key); }
    void insert(const QString &key, const QJsonObject &value) { maps_[shardFor(key)].insert(key, value); }
    void remove(const QString &key) { maps_[shardFor(key)].remove(key); }
    void clear() { for (auto &map : maps_) map.clear(); }
    qsizetype size() const { qsizetype n = 0; for (const auto &map : maps_) n += map.size(); return n; }
    QStringList keys() const {
        QStringList result; result.reserve(size());
        for (const auto &map : maps_) for (auto it = map.cbegin(); it != map.cend(); ++it) result.append(it.key());
        return result;
    }
    const Map &shard(int n) const { return maps_[n]; }
    void adopt(int n, Map &&map) { maps_[n] = std::move(map); }
private:
    std::array<Map, ShardCount> maps_;
};
