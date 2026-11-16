#include "merger.h"

namespace lsm {

namespace {

class MemTableIteratorAdapter : public Iterator {
 public:
  explicit MemTableIteratorAdapter(const MemTable* mem) : it_(mem) {}
  bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void Seek(std::string_view key) override { it_.Seek(key); }
  void Next() override { it_.Next(); }
  std::string_view key() const override { return it_.entry().key; }
  SequenceNumber seq() const override { return it_.entry().seq; }
  ValueType type() const override { return it_.entry().type; }
  std::string_view value() const override { return it_.entry().value; }
  Status status() const override { return Status::OK(); }

 private:
  MemTable::Iterator it_;
};

// Picks the smallest current entry among the children with a linear scan.
// With the handful of sources an LSM tree has (a few memtables and tables)
// this is as fast as a heap and simpler.
class MergingIterator : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return current_ != nullptr; }
  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    FindSmallest();
  }
  void Seek(std::string_view key) override {
    for (auto& c : children_) c->Seek(key);
    FindSmallest();
  }
  void Next() override {
    current_->Next();
    FindSmallest();
  }
  std::string_view key() const override { return current_->key(); }
  SequenceNumber seq() const override { return current_->seq(); }
  ValueType type() const override { return current_->type(); }
  std::string_view value() const override { return current_->value(); }
  Status status() const override {
    for (const auto& c : children_) {
      Status s = c->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  void FindSmallest() {
    current_ = nullptr;
    for (auto& c : children_) {
      if (!c->status().ok()) {  // stop on the first error rather than skip data
        current_ = nullptr;
        return;
      }
      if (!c->Valid()) continue;
      if (current_ == nullptr ||
          CompareInternal(c->key(), c->seq(), current_->key(), current_->seq()) < 0) {
        current_ = c.get();
      }
    }
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
};

class DBIterator : public Iterator {
 public:
  DBIterator(std::unique_ptr<Iterator> merged, SequenceNumber snapshot,
             std::shared_ptr<const void> keep_alive)
      : keep_alive_(std::move(keep_alive)), it_(std::move(merged)), snapshot_(snapshot) {}

  bool Valid() const override { return valid_; }
  void SeekToFirst() override {
    it_->SeekToFirst();
    has_skip_ = false;
    FindNextVisible();
  }
  void Seek(std::string_view key) override {
    it_->Seek(key);
    has_skip_ = false;
    FindNextVisible();
  }
  void Next() override {
    // it_ sits on the version we returned; skip_key_ makes the loop pass over
    // that key's older versions.
    it_->Next();
    FindNextVisible();
  }
  std::string_view key() const override { return skip_key_; }
  SequenceNumber seq() const override { return it_->seq(); }
  ValueType type() const override { return ValueType::kValue; }
  std::string_view value() const override { return it_->value(); }
  Status status() const override { return it_->status(); }

 private:
  void FindNextVisible() {
    valid_ = false;
    while (it_->Valid()) {
      if (it_->seq() > snapshot_) {  // written after this iterator was created
        it_->Next();
        continue;
      }
      if (has_skip_ && it_->key() == std::string_view(skip_key_)) {  // older version
        it_->Next();
        continue;
      }
      // Newest visible version of a new key.
      skip_key_.assign(it_->key());
      has_skip_ = true;
      if (it_->type() == ValueType::kDeletion) {
        it_->Next();
        continue;
      }
      valid_ = true;
      return;
    }
  }

  // Declared first so it is destroyed last: it_ points into what it keeps alive.
  std::shared_ptr<const void> keep_alive_;
  std::unique_ptr<Iterator> it_;
  SequenceNumber snapshot_;
  std::string skip_key_;
  bool has_skip_ = false;
  bool valid_ = false;
};

}  // namespace

std::unique_ptr<Iterator> NewMemTableIterator(const MemTable* mem) {
  return std::make_unique<MemTableIteratorAdapter>(mem);
}

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> merged, SequenceNumber snapshot,
                                        std::shared_ptr<const void> keep_alive) {
  return std::make_unique<DBIterator>(std::move(merged), snapshot, std::move(keep_alive));
}

}  // namespace lsm
