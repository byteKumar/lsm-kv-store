#pragma once

// A sorted skiplist that allows one writer and any number of concurrent
// readers without a lock.
//
// Rules for callers:
//   * Insert() calls must be serialized by the caller (one writer at a time).
//   * Readers (Contains, Iterator) need no lock and may run during an Insert.
//   * Nodes are never removed until the whole list is destroyed, so a reader
//     can never hold a pointer to freed memory.
//
// Why this is safe without a lock: a new node is fully built (key and all its
// forward pointers) before it is linked in. Linking is a single atomic store
// with release ordering, and readers load pointers with acquire ordering, so a
// reader that sees the new node also sees its finished contents.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <random>

namespace lsm {

template <typename Key, typename Compare>
class SkipList {
  struct Node;

 public:
  static constexpr int kMaxHeight = 12;
  static constexpr unsigned kBranching = 4;  // each level holds ~1/4 of the level below

  explicit SkipList(Compare cmp, uint32_t seed = 0xdeadbeef)
      : cmp_(cmp), head_(new Node(Key{}, kMaxHeight)), rng_(seed) {}

  ~SkipList() {
    Node* n = head_;
    while (n != nullptr) {
      Node* next = n->Next(0);
      delete n;
      n = next;
    }
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts key. The list must not already contain an equal key.
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || !Equal(key, x->key));
    (void)x;

    int height = RandomHeight();
    int max_height = max_height_.load(std::memory_order_relaxed);
    if (height > max_height) {
      for (int i = max_height; i < height; i++) prev[i] = head_;
      // A reader that sees the new height before the node is linked just
      // finds head_->next == nullptr at the new levels and drops down a level.
      max_height_.store(height, std::memory_order_relaxed);
    }

    Node* node = new Node(key, height);
    for (int i = 0; i < height; i++) {
      // The node is not visible yet, so relaxed stores are enough here.
      node->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
      // Publishing store: readers that load this pointer see a complete node.
      prev[i]->SetNext(i, node);
    }
  }

  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && Equal(key, x->key);
  }

  // Forward iterator. Valid while the list is alive.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    // Positions at the first entry >= target.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  struct Node {
    Node(const Key& k, int height) : key(k), next_(new std::atomic<Node*>[height]) {
      for (int i = 0; i < height; i++) next_[i].store(nullptr, std::memory_order_relaxed);
    }
    Node* Next(int level) const { return next_[level].load(std::memory_order_acquire); }
    void SetNext(int level, Node* n) { next_[level].store(n, std::memory_order_release); }
    Node* NoBarrierNext(int level) const { return next_[level].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int level, Node* n) { next_[level].store(n, std::memory_order_relaxed); }

    const Key key;
    std::unique_ptr<std::atomic<Node*>[]> next_;
  };

  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && (rng_() % kBranching) == 0) height++;
    return height;
  }

  bool Equal(const Key& a, const Key& b) const { return cmp_(a, b) == 0; }

  // Returns the first node >= key, or nullptr. If prev is non-null, fills
  // prev[level] with the last node < key at every level (used by Insert).
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = max_height_.load(std::memory_order_relaxed) - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next != nullptr && cmp_(next->key, key) < 0) {
        x = next;  // keep moving right on this level
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        level--;  // drop down a level
      }
    }
  }

  Compare const cmp_;
  Node* const head_;
  std::atomic<int> max_height_{1};
  std::minstd_rand rng_;  // only touched by the writer
};

}  // namespace lsm
