#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../util.h"
#include "base.h"
#include "lipp/src/core/lipp.h"
#include "pgm_index_dynamic.hpp"

template <class KeyType, class SearchClass, size_t pgm_error, size_t num_buckets,
          size_t active_threshold>
class HybridPGMLIPP : public Competitor<KeyType, SearchClass> {
 public:
  HybridPGMLIPP(const std::vector<int>& params) {}

  ~HybridPGMLIPP() { ShutdownWorker(); }

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    ShutdownWorker();
    ResetState();

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& item : data) {
      loading_data.emplace_back(item.key, item.value);
    }

    const uint64_t build_time = util::timing([&] {
      base_lipp_.bulk_load(loading_data.data(), loading_data.size());
      root_model_a_ = base_lipp_.root_model_a();
      root_model_b_ = base_lipp_.root_model_b();
      root_num_items_ = std::max<size_t>(1, base_lipp_.root_num_items());

      buckets_.clear();
      buckets_.reserve(num_buckets);
      for (size_t bucket_id = 0; bucket_id < num_buckets; ++bucket_id) {
        buckets_.push_back(std::make_unique<BucketState>());
        auto& bucket = *buckets_.back();
        bucket.active = std::make_unique<DynamicPGMType>();
        std::atomic_store_explicit(
            &bucket.published,
            std::shared_ptr<const PublishedBucketState>(
                std::make_shared<PublishedBucketState>()),
            std::memory_order_release);
      }
    });

    worker_thread_ = std::thread(&HybridPGMLIPP::WorkerLoop, this);
    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    uint64_t value = 0;
    if (base_lipp_.find(lookup_key, value)) {
      return value;
    }

    const BucketState& bucket = *buckets_[BucketForKey(lookup_key)];

    {
      std::lock_guard<std::mutex> lock(bucket.mutex);
      if (bucket.active_has_range &&
          InRange(lookup_key, bucket.active_min, bucket.active_max)) {
        auto it = bucket.active->find(lookup_key);
        if (it != bucket.active->end()) {
          return it->value();
        }
      }

      if (bucket.pending_has_range &&
          InRange(lookup_key, bucket.pending_min, bucket.pending_max) &&
          bucket.pending != nullptr) {
        auto it = bucket.pending->find(lookup_key);
        if (it != bucket.pending->end()) {
          return it->value();
        }
      }
    }

    auto published = LoadPublished(bucket);
    if (published != nullptr && published->has_range &&
        InRange(lookup_key, published->min_key, published->max_key) &&
        published->MaybeContains(lookup_key) && published->overlay != nullptr &&
        published->overlay->find(lookup_key, value)) {
      return value;
    }

    return util::OVERFLOW;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key,
                      uint32_t thread_id) const {
    uint64_t result = 0;

    auto base_it = base_lipp_.lower_bound(lower_key);
    while (base_it != base_lipp_.end() && base_it->comp.data.key <= upper_key) {
      result += base_it->comp.data.value;
      ++base_it;
    }

    size_t lower_bucket = BucketForKey(lower_key);
    size_t upper_bucket = BucketForKey(upper_key);
    if (lower_bucket > upper_bucket) {
      std::swap(lower_bucket, upper_bucket);
    }

    for (size_t bucket_id = lower_bucket; bucket_id <= upper_bucket; ++bucket_id) {
      const BucketState& bucket = *buckets_[bucket_id];
      {
        std::lock_guard<std::mutex> lock(bucket.mutex);
        if (bucket.active_has_range &&
            RangesOverlap(lower_key, upper_key, bucket.active_min,
                          bucket.active_max)) {
          auto it = bucket.active->lower_bound(lower_key);
          while (it != bucket.active->end() && it->key() <= upper_key) {
            result += it->value();
            ++it;
          }
        }

        if (bucket.pending_has_range &&
            RangesOverlap(lower_key, upper_key, bucket.pending_min,
                          bucket.pending_max) &&
            bucket.pending != nullptr) {
          auto it = bucket.pending->lower_bound(lower_key);
          while (it != bucket.pending->end() && it->key() <= upper_key) {
            result += it->value();
            ++it;
          }
        }
      }

      auto published = LoadPublished(bucket);
      if (published != nullptr && published->has_range &&
          RangesOverlap(lower_key, upper_key, published->min_key,
                        published->max_key) &&
          published->overlay != nullptr) {
        auto it = published->overlay->lower_bound(lower_key);
        while (it != published->overlay->end() && it->comp.data.key <= upper_key) {
          result += it->comp.data.value;
          ++it;
        }
      }
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    const size_t bucket_id = BucketForKey(data.key);
    BucketState& bucket = *buckets_[bucket_id];

    std::lock_guard<std::mutex> lock(bucket.mutex);
    bucket.active->insert(data.key, data.value);
    ++bucket.active_size;
    UpdateRange(bucket.active_has_range, bucket.active_min, bucket.active_max,
                data.key);

    if (bucket.pending == nullptr && bucket.active_size >= active_threshold) {
      HandoffBucket(bucket_id, bucket);
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    size_t total = base_lipp_.index_size();
    for (const auto& bucket_ptr : buckets_) {
      const auto& bucket = *bucket_ptr;
      {
        std::lock_guard<std::mutex> lock(bucket.mutex);
        total += bucket.active != nullptr ? bucket.active->size_in_bytes() : 0;
        total += bucket.pending != nullptr ? bucket.pending->size_in_bytes() : 0;
      }

      auto published = LoadPublished(bucket);
      if (published != nullptr) {
        total += published->overlay != nullptr ? published->overlay->index_size() : 0;
        total += published->items != nullptr
                     ? published->items->size() *
                           sizeof(typename PublishedBucketState::ValueType)
                     : 0;
        total += published->bloom_words.size() * sizeof(uint64_t);
      }
    }
    return total;
  }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    const bool supported_dataset =
        ops_filename.find("books_100M") != std::string::npos ||
        ops_filename.find("fb_100M") != std::string::npos ||
        ops_filename.find("osmc_100M") != std::string::npos;
    return unique && !multithread && insert &&
           ops_filename.find("mix") != std::string::npos && supported_dataset;
  }

  std::vector<std::string> variants() const {
    return {"async_overlay_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-b" +
                std::to_string(num_buckets) + "-a" +
                std::to_string(active_threshold)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using LippType = LIPP<KeyType, uint64_t>;

  struct PublishedBucketState {
    using ValueType = std::pair<KeyType, uint64_t>;

    std::shared_ptr<const std::vector<ValueType>> items;
    std::shared_ptr<const LippType> overlay;
    std::vector<uint64_t> bloom_words;
    size_t bloom_mask{0};
    bool has_range{false};
    KeyType min_key{};
    KeyType max_key{};

    bool MaybeContains(const KeyType& key) const {
      if (bloom_words.empty()) {
        return false;
      }
      const uint64_t hash1 = HashKey(key);
      const uint64_t hash2 = RotateLeft(hash1 * 0x9E3779B97F4A7C15ULL, 17);
      const size_t bit1 = static_cast<size_t>(hash1) & bloom_mask;
      const size_t bit2 = static_cast<size_t>(hash2) & bloom_mask;
      return TestBit(bit1) && TestBit(bit2);
    }

    static uint64_t HashKey(const KeyType& key) {
      uint64_t x = static_cast<uint64_t>(key) + 0x9E3779B97F4A7C15ULL;
      x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
      x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
      return x ^ (x >> 31);
    }

    static uint64_t RotateLeft(uint64_t value, int shift) {
      return (value << shift) | (value >> (64 - shift));
    }

    bool TestBit(size_t bit) const {
      const size_t word = bit >> 6;
      const uint64_t mask = uint64_t{1} << (bit & 63);
      return (bloom_words[word] & mask) != 0;
    }
  };

  struct BucketState {
    mutable std::mutex mutex;
    std::unique_ptr<DynamicPGMType> active;
    size_t active_size{0};
    bool active_has_range{false};
    KeyType active_min{};
    KeyType active_max{};

    std::shared_ptr<const DynamicPGMType> pending;
    bool pending_has_range{false};
    KeyType pending_min{};
    KeyType pending_max{};
    bool queued{false};

    mutable std::shared_ptr<const PublishedBucketState> published;
  };

  void ResetState() {
    {
      std::lock_guard<std::mutex> queue_lock(queue_mutex_);
      shutdown_ = false;
      work_queue_.clear();
    }
    buckets_.clear();
    root_model_a_ = 0;
    root_model_b_ = 0;
    root_num_items_ = 1;
  }

  void ShutdownWorker() {
    {
      std::lock_guard<std::mutex> queue_lock(queue_mutex_);
      shutdown_ = true;
    }
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }

  void HandoffBucket(size_t bucket_id, BucketState& bucket) {
    bucket.pending = std::make_shared<DynamicPGMType>(std::move(*bucket.active));
    bucket.pending_has_range = bucket.active_has_range;
    bucket.pending_min = bucket.active_min;
    bucket.pending_max = bucket.active_max;

    bucket.active = std::make_unique<DynamicPGMType>();
    bucket.active_size = 0;
    bucket.active_has_range = false;

    if (!bucket.queued) {
      bucket.queued = true;
      {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        work_queue_.push_back(bucket_id);
      }
      queue_cv_.notify_one();
    }
  }

  std::shared_ptr<const PublishedBucketState> LoadPublished(
      const BucketState& bucket) const {
    return std::atomic_load_explicit(&bucket.published,
                                     std::memory_order_acquire);
  }

  void WorkerLoop() {
    while (true) {
      size_t bucket_id = 0;
      {
        std::unique_lock<std::mutex> queue_lock(queue_mutex_);
        queue_cv_.wait(queue_lock,
                       [&] { return shutdown_ || !work_queue_.empty(); });
        if (shutdown_ && work_queue_.empty()) {
          return;
        }
        bucket_id = work_queue_.front();
        work_queue_.pop_front();
      }

      ProcessBucket(bucket_id);
    }
  }

  void ProcessBucket(size_t bucket_id) {
    std::shared_ptr<const DynamicPGMType> pending;
    std::shared_ptr<const PublishedBucketState> published;
    {
      std::lock_guard<std::mutex> lock(buckets_[bucket_id]->mutex);
      pending = buckets_[bucket_id]->pending;
      buckets_[bucket_id]->queued = false;
      published = LoadPublished(*buckets_[bucket_id]);
    }

    if (pending == nullptr) {
      return;
    }

    using ValueType = typename PublishedBucketState::ValueType;
    std::vector<ValueType> batch;
    batch.reserve(std::max<size_t>(active_threshold, 64));
    pending->for_each([&](const KeyType& key, const uint64_t value) {
      batch.emplace_back(key, value);
    });
    std::sort(batch.begin(), batch.end(),
              [](const ValueType& lhs, const ValueType& rhs) {
                return lhs.first < rhs.first;
              });

    std::vector<ValueType> merged;
    const size_t prior_size =
        published != nullptr && published->items != nullptr ? published->items->size()
                                                            : 0;
    merged.reserve(prior_size + batch.size());
    if (published != nullptr && published->items != nullptr) {
      std::merge(published->items->begin(), published->items->end(), batch.begin(),
                 batch.end(), std::back_inserter(merged),
                 [](const ValueType& lhs, const ValueType& rhs) {
                   return lhs.first < rhs.first;
                 });
    } else {
      merged = std::move(batch);
    }

    auto next_state = std::make_shared<PublishedBucketState>();
    if (!merged.empty()) {
      auto items =
          std::make_shared<const std::vector<ValueType>>(std::move(merged));
      auto overlay = std::make_shared<LippType>();
      overlay->bulk_load(items->data(), static_cast<int>(items->size()));

      next_state->items = std::move(items);
      next_state->overlay = std::move(overlay);
      next_state->has_range = true;
      next_state->min_key = next_state->items->front().first;
      next_state->max_key = next_state->items->back().first;
      BuildBloom(*next_state->items, next_state.get());
    }

    BucketState& bucket = *buckets_[bucket_id];
    bool need_requeue = false;
    {
      std::lock_guard<std::mutex> lock(bucket.mutex);
      if (bucket.pending == pending) {
        bucket.pending.reset();
        bucket.pending_has_range = false;
      }
      std::atomic_store_explicit(&bucket.published,
                                 std::shared_ptr<const PublishedBucketState>(
                                     std::move(next_state)),
                                 std::memory_order_release);
      if (bucket.pending == nullptr && bucket.active_size >= active_threshold) {
        HandoffBucket(bucket_id, bucket);
      } else if (bucket.pending != nullptr && !bucket.queued) {
        bucket.queued = true;
        need_requeue = true;
      }
    }

    if (need_requeue) {
      {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        work_queue_.push_back(bucket_id);
      }
      queue_cv_.notify_one();
    }
  }

  static void BuildBloom(
      const std::vector<typename PublishedBucketState::ValueType>& items,
      PublishedBucketState* state) {
    size_t bit_count = 64;
    const size_t target_bits = std::max<size_t>(64, items.size() * 8);
    while (bit_count < target_bits) {
      bit_count <<= 1;
    }
    state->bloom_words.assign(bit_count / 64, 0);
    state->bloom_mask = bit_count - 1;

    for (const auto& item : items) {
      const uint64_t hash1 = PublishedBucketState::HashKey(item.first);
      const uint64_t hash2 =
          PublishedBucketState::RotateLeft(hash1 * 0x9E3779B97F4A7C15ULL, 17);
      SetBloomBit(state, static_cast<size_t>(hash1) & state->bloom_mask);
      SetBloomBit(state, static_cast<size_t>(hash2) & state->bloom_mask);
    }
  }

  static void SetBloomBit(PublishedBucketState* state, size_t bit) {
    const size_t word = bit >> 6;
    state->bloom_words[word] |= uint64_t{1} << (bit & 63);
  }

  static bool InRange(const KeyType& key, const KeyType& min_key,
                      const KeyType& max_key) {
    return key >= min_key && key <= max_key;
  }

  static bool RangesOverlap(const KeyType& lower_a, const KeyType& upper_a,
                            const KeyType& lower_b, const KeyType& upper_b) {
    return lower_a <= upper_b && lower_b <= upper_a;
  }

  static void UpdateRange(bool& has_range, KeyType& min_key, KeyType& max_key,
                          const KeyType& key) {
    if (!has_range) {
      has_range = true;
      min_key = key;
      max_key = key;
      return;
    }
    min_key = std::min(min_key, key);
    max_key = std::max(max_key, key);
  }

  size_t PredictRootSlot(const KeyType& key) const {
    long double v = root_model_a_ * static_cast<long double>(key) + root_model_b_;
    if (v < 0) {
      return 0;
    }
    const long double max_pos = static_cast<long double>(root_num_items_ - 1);
    if (v > max_pos) {
      return root_num_items_ - 1;
    }
    return static_cast<size_t>(v);
  }

  size_t BucketForKey(const KeyType& key) const {
    const size_t slot = PredictRootSlot(key);
    const size_t bucket = (slot * num_buckets) / root_num_items_;
    return std::min(bucket, num_buckets - 1);
  }

  mutable LippType base_lipp_;
  mutable std::vector<std::unique_ptr<BucketState>> buckets_;

  mutable std::mutex queue_mutex_;
  mutable std::condition_variable queue_cv_;
  mutable std::deque<size_t> work_queue_;
  mutable bool shutdown_{false};
  mutable std::thread worker_thread_;

  long double root_model_a_{0};
  long double root_model_b_{0};
  size_t root_num_items_{1};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
