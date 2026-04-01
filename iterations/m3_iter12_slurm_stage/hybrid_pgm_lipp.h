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

    uint64_t build_time = util::timing([&] {
      base_lipp_.bulk_load(loading_data.data(), loading_data.size());
      root_model_a_ = base_lipp_.root_model_a();
      root_model_b_ = base_lipp_.root_model_b();
      root_num_items_ = std::max<size_t>(1, base_lipp_.root_num_items());

      buckets_.clear();
      buckets_.resize(num_buckets);
      for (auto& bucket : buckets_) {
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

    const size_t bucket_id = BucketForKey(lookup_key);
    const auto& bucket = buckets_[bucket_id];
    auto published = LoadPublished(bucket);

    if (published != nullptr) {
      if (published->shadow_has_range &&
          InRange(lookup_key, published->shadow_min, published->shadow_max) &&
          published->shadow != nullptr &&
          published->shadow->find(lookup_key, value)) {
        return value;
      }

      if (published->flush_has_range &&
          InRange(lookup_key, published->flush_min, published->flush_max) &&
          published->flush != nullptr) {
        auto it = published->flush->find(lookup_key);
        if (it != published->flush->end()) {
          return it->value();
        }
      }
    }

    {
      std::lock_guard<std::mutex> bucket_lock(bucket.mutex);
      if (bucket.active_has_range &&
          InRange(lookup_key, bucket.active_min, bucket.active_max)) {
        auto it = bucket.active->find(lookup_key);
        if (it != bucket.active->end()) {
          return it->value();
        }
      }
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
      const auto& bucket = buckets_[bucket_id];
      auto published = LoadPublished(bucket);

      if (published != nullptr && published->shadow_has_range &&
          RangesOverlap(lower_key, upper_key, published->shadow_min,
                        published->shadow_max) &&
          published->shadow != nullptr) {
        auto it = published->shadow->lower_bound(lower_key);
        while (it != published->shadow->end() && it->comp.data.key <= upper_key) {
          result += it->comp.data.value;
          ++it;
        }
      }

      if (published != nullptr && published->flush_has_range &&
          RangesOverlap(lower_key, upper_key, published->flush_min,
                        published->flush_max) &&
          published->flush != nullptr) {
        auto it = published->flush->lower_bound(lower_key);
        while (it != published->flush->end() && it->key() <= upper_key) {
          result += it->value();
          ++it;
        }
      }

      {
        std::lock_guard<std::mutex> bucket_lock(bucket.mutex);
        if (bucket.active_has_range &&
            RangesOverlap(lower_key, upper_key, bucket.active_min,
                          bucket.active_max)) {
          auto it = bucket.active->lower_bound(lower_key);
          while (it != bucket.active->end() && it->key() <= upper_key) {
            result += it->value();
            ++it;
          }
        }
      }
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    const size_t bucket_id = BucketForKey(data.key);
    BucketState& bucket = buckets_[bucket_id];
    std::lock_guard<std::mutex> bucket_lock(bucket.mutex);

    bucket.active->insert(data.key, data.value);
    ++bucket.active_size;
    UpdateRange(bucket.active_has_range, bucket.active_min, bucket.active_max,
                data.key);

    if (bucket.flush_in_progress || bucket.active_size < ActiveThreshold()) {
      return;
    }

    HandoffBucket(bucket_id, bucket);
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    size_t total = base_lipp_.index_size();
    for (const auto& bucket : buckets_) {
      total += bucket.active != nullptr ? bucket.active->size_in_bytes() : 0;
      auto published = LoadPublished(bucket);
      if (published != nullptr) {
        total += published->flush != nullptr ? published->flush->size_in_bytes() : 0;
        total += published->shadow != nullptr ? published->shadow->index_size() : 0;
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
    return {"async_bucket_shadow_lipp",
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
    std::shared_ptr<const DynamicPGMType> flush;
    std::shared_ptr<const LippType> shadow;

    bool flush_has_range{false};
    bool shadow_has_range{false};

    KeyType flush_min{};
    KeyType flush_max{};
    KeyType shadow_min{};
    KeyType shadow_max{};
  };

  struct BucketState {
    mutable std::mutex mutex;
    std::unique_ptr<DynamicPGMType> active;
    size_t active_size{0};
    bool active_has_range{false};
    KeyType active_min{};
    KeyType active_max{};

    std::shared_ptr<const DynamicPGMType> worker_flush;
    bool flush_in_progress{false};
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

  size_t ActiveThreshold() const {
    return std::max<size_t>(32, active_threshold);
  }

  std::shared_ptr<const PublishedBucketState> LoadPublished(
      const BucketState& bucket) const {
    return std::atomic_load_explicit(&bucket.published, std::memory_order_acquire);
  }

  void StorePublished(
      BucketState& bucket,
      std::shared_ptr<const PublishedBucketState> next_state) {
    std::atomic_store_explicit(&bucket.published, std::move(next_state),
                               std::memory_order_release);
  }

  bool InRange(const KeyType& key, const KeyType& lower,
               const KeyType& upper) const {
    return key >= lower && key <= upper;
  }

  bool RangesOverlap(const KeyType& lower_a, const KeyType& upper_a,
                     const KeyType& lower_b, const KeyType& upper_b) const {
    return !(upper_a < lower_b || upper_b < lower_a);
  }

  void UpdateRange(bool& has_range, KeyType& min_key, KeyType& max_key,
                   const KeyType& key) {
    if (!has_range) {
      has_range = true;
      min_key = key;
      max_key = key;
      return;
    }
    if (key < min_key) {
      min_key = key;
    }
    if (key > max_key) {
      max_key = key;
    }
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

  void HandoffBucket(size_t bucket_id, BucketState& bucket) {
    auto frozen_flush = std::shared_ptr<const DynamicPGMType>(bucket.active.release());
    auto current = LoadPublished(bucket);

    auto next_state = std::make_shared<PublishedBucketState>();
    if (current != nullptr) {
      next_state->shadow = current->shadow;
      next_state->shadow_has_range = current->shadow_has_range;
      next_state->shadow_min = current->shadow_min;
      next_state->shadow_max = current->shadow_max;
    }
    next_state->flush = frozen_flush;
    next_state->flush_has_range = bucket.active_has_range;
    next_state->flush_min = bucket.active_min;
    next_state->flush_max = bucket.active_max;

    bucket.worker_flush = frozen_flush;
    bucket.active = std::make_unique<DynamicPGMType>();
    bucket.active_size = 0;
    bucket.active_has_range = false;
    bucket.flush_in_progress = true;

    StorePublished(bucket, next_state);

    {
      std::lock_guard<std::mutex> queue_lock(queue_mutex_);
      if (!bucket.queued) {
        bucket.queued = true;
        work_queue_.push_back(bucket_id);
      }
    }
    queue_cv_.notify_one();
  }

  void CollectLippPairs(const LippType& lipp,
                        std::vector<std::pair<KeyType, uint64_t>>& out) const {
    auto it = lipp.lower_bound(std::numeric_limits<KeyType>::lowest());
    while (it != lipp.end()) {
      out.emplace_back(it->comp.data.key, it->comp.data.value);
      ++it;
    }
  }

  void CollectDynamicPairs(const DynamicPGMType& dpgm,
                           std::vector<std::pair<KeyType, uint64_t>>& out) const {
    dpgm.for_each([&](const KeyType& key, const uint64_t value) {
      out.emplace_back(key, value);
    });
  }

  std::shared_ptr<const LippType> BuildShadowForBucket(
      const std::shared_ptr<const LippType>& shadow_snapshot,
      const std::shared_ptr<const DynamicPGMType>& flush_snapshot,
      bool& shadow_has_range, KeyType& shadow_min, KeyType& shadow_max) const {
    std::vector<std::pair<KeyType, uint64_t>> merged;
    if (shadow_snapshot != nullptr) {
      CollectLippPairs(*shadow_snapshot, merged);
    }
    if (flush_snapshot != nullptr) {
      CollectDynamicPairs(*flush_snapshot, merged);
    }

    std::sort(merged.begin(), merged.end(),
              [](const auto& lhs, const auto& rhs) {
                return lhs.first < rhs.first;
              });
    merged.erase(
        std::unique(merged.begin(), merged.end(),
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.first == rhs.first;
                    }),
        merged.end());

    shadow_has_range = !merged.empty();
    if (!merged.empty()) {
      shadow_min = merged.front().first;
      shadow_max = merged.back().first;
    }

    auto next_shadow = std::make_shared<LippType>();
    if (!merged.empty()) {
      next_shadow->bulk_load(merged.data(), merged.size());
    }
    return next_shadow;
  }

  void WorkerLoop() {
    while (true) {
      size_t bucket_id = 0;
      {
        std::unique_lock<std::mutex> queue_lock(queue_mutex_);
        queue_cv_.wait(queue_lock, [&] {
          return shutdown_ || !work_queue_.empty();
        });
        if (shutdown_ && work_queue_.empty()) {
          return;
        }
        bucket_id = work_queue_.front();
        work_queue_.pop_front();
      }

      BucketState& bucket = buckets_[bucket_id];
      std::shared_ptr<const DynamicPGMType> flush_snapshot;
      std::shared_ptr<const LippType> shadow_snapshot;
      {
        std::lock_guard<std::mutex> bucket_lock(bucket.mutex);
        auto current = LoadPublished(bucket);
        flush_snapshot = bucket.worker_flush;
        shadow_snapshot = current != nullptr ? current->shadow : nullptr;
      }

      bool shadow_has_range = false;
      KeyType shadow_min{};
      KeyType shadow_max{};
      auto next_shadow = BuildShadowForBucket(shadow_snapshot, flush_snapshot,
                                             shadow_has_range, shadow_min,
                                             shadow_max);

      auto next_state = std::make_shared<PublishedBucketState>();
      next_state->shadow = next_shadow;
      next_state->shadow_has_range = shadow_has_range;
      next_state->shadow_min = shadow_min;
      next_state->shadow_max = shadow_max;

      {
        std::lock_guard<std::mutex> bucket_lock(bucket.mutex);
        bucket.worker_flush.reset();
        bucket.flush_in_progress = false;
        bucket.queued = false;
        StorePublished(bucket, next_state);
      }
    }
  }

  mutable LippType base_lipp_;
  mutable std::deque<BucketState> buckets_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::thread worker_thread_;
  std::deque<size_t> work_queue_;
  bool shutdown_{true};

  long double root_model_a_{0};
  long double root_model_b_{0};
  size_t root_num_items_{1};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
