#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

template <class KeyType, class SearchClass, size_t pgm_error,
          size_t flush_threshold>
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
      active_dpgm_ = std::make_unique<DynamicPGMType>();
    });

    worker_thread_ = std::thread(&HybridPGMLIPP::FlushWorkerLoop, this);
    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    uint64_t value = 0;
    if (base_lipp_.find(lookup_key, value)) {
      return value;
    }

    auto published = LoadPublishedState();
    if (published != nullptr) {
      if (published->shadow_has_range &&
          InRange(lookup_key, published->shadow_min_key,
                  published->shadow_max_key) &&
          published->shadow != nullptr &&
          published->shadow->find(lookup_key, value)) {
        return value;
      }

      if (published->flush_has_range &&
          InRange(lookup_key, published->flush_min_key,
                  published->flush_max_key) &&
          published->flush != nullptr) {
        auto it = published->flush->find(lookup_key);
        if (it != published->flush->end()) {
          return it->value();
        }
      }
    }

    if (active_has_range_ &&
        InRange(lookup_key, active_min_key_, active_max_key_) &&
        active_dpgm_ != nullptr) {
      auto it = active_dpgm_->find(lookup_key);
      if (it != active_dpgm_->end()) {
        return it->value();
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

    auto published = LoadPublishedState();
    if (published != nullptr) {
      if (published->shadow_has_range &&
          RangesOverlap(lower_key, upper_key, published->shadow_min_key,
                        published->shadow_max_key) &&
          published->shadow != nullptr) {
        auto it = published->shadow->lower_bound(lower_key);
        while (it != published->shadow->end() && it->comp.data.key <= upper_key) {
          result += it->comp.data.value;
          ++it;
        }
      }

      if (published->flush_has_range &&
          RangesOverlap(lower_key, upper_key, published->flush_min_key,
                        published->flush_max_key) &&
          published->flush != nullptr) {
        auto it = published->flush->lower_bound(lower_key);
        while (it != published->flush->end() && it->key() <= upper_key) {
          result += it->value();
          ++it;
        }
      }
    }

    if (active_has_range_ && RangesOverlap(lower_key, upper_key, active_min_key_,
                                           active_max_key_) &&
        active_dpgm_ != nullptr) {
      auto it = active_dpgm_->lower_bound(lower_key);
      while (it != active_dpgm_->end() && it->key() <= upper_key) {
        result += it->value();
        ++it;
      }
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    std::unique_lock<std::mutex> lock(control_mutex_);
    active_dpgm_->insert(data.key, data.value);
    ++active_size_;
    UpdateRange(active_has_range_, active_min_key_, active_max_key_, data.key);

    if (!flush_in_progress_ && active_size_ >= FlushThreshold()) {
      PublishFrozenFlushLocked();
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    auto published = LoadPublishedState();

    std::lock_guard<std::mutex> lock(control_mutex_);
    size_t total = base_lipp_.index_size();
    total += active_dpgm_ != nullptr ? active_dpgm_->size_in_bytes() : 0;
    if (published != nullptr) {
      total += published->shadow != nullptr ? published->shadow->index_size() : 0;
      total += published->flush != nullptr ? published->flush->size_in_bytes() : 0;
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
    return {"async_snapshot_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-f" +
                std::to_string(flush_threshold)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using LippType = LIPP<KeyType, uint64_t>;

  struct PublishedState {
    std::shared_ptr<const LippType> shadow;
    std::shared_ptr<const DynamicPGMType> flush;

    bool shadow_has_range{false};
    bool flush_has_range{false};

    KeyType shadow_min_key{};
    KeyType shadow_max_key{};
    KeyType flush_min_key{};
    KeyType flush_max_key{};
  };

  void ResetState() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    base_lipp_.bulk_load(nullptr, 0);
    active_dpgm_.reset();
    active_size_ = 0;
    active_has_range_ = false;
    flush_in_progress_ = false;
    shutdown_ = false;
    worker_flush_.reset();
    StorePublishedState(std::make_shared<PublishedState>());
  }

  void ShutdownWorker() {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      shutdown_ = true;
      flush_requested_ = false;
    }
    worker_cv_.notify_all();
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }

  size_t FlushThreshold() const {
    return std::max<size_t>(32, flush_threshold);
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

  std::shared_ptr<const PublishedState> LoadPublishedState() const {
    return std::atomic_load_explicit(&published_state_, std::memory_order_acquire);
  }

  void StorePublishedState(std::shared_ptr<const PublishedState> next_state) {
    std::atomic_store_explicit(&published_state_, std::move(next_state),
                               std::memory_order_release);
  }

  void PublishFrozenFlushLocked() {
    auto frozen_flush =
        std::shared_ptr<const DynamicPGMType>(active_dpgm_.release());

    auto current = LoadPublishedState();
    auto next_state = std::make_shared<PublishedState>();
    if (current != nullptr) {
      next_state->shadow = current->shadow;
      next_state->shadow_has_range = current->shadow_has_range;
      next_state->shadow_min_key = current->shadow_min_key;
      next_state->shadow_max_key = current->shadow_max_key;
    }
    next_state->flush = frozen_flush;
    next_state->flush_has_range = active_has_range_;
    next_state->flush_min_key = active_min_key_;
    next_state->flush_max_key = active_max_key_;

    active_dpgm_ = std::make_unique<DynamicPGMType>();
    active_size_ = 0;
    active_has_range_ = false;

    worker_flush_ = frozen_flush;
    flush_in_progress_ = true;
    StorePublishedState(next_state);

    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      flush_requested_ = true;
    }
    worker_cv_.notify_one();
  }

  void CollectLippPairs(const LippType& lipp,
                        std::vector<std::pair<KeyType, uint64_t>>& out) const {
    auto it = lipp.lower_bound(std::numeric_limits<KeyType>::lowest());
    while (it != lipp.end()) {
      out.emplace_back(it->comp.data.key, it->comp.data.value);
      ++it;
    }
  }

  void CollectDynamicPairsSorted(
      const DynamicPGMType& dpgm,
      std::vector<std::pair<KeyType, uint64_t>>& out) const {
    for (auto it = dpgm.begin(); it != dpgm.end(); ++it) {
      out.emplace_back(it->key(), it->value());
    }
  }

  std::shared_ptr<const LippType> BuildMergedShadow(
      const std::shared_ptr<const LippType>& shadow_snapshot,
      const std::shared_ptr<const DynamicPGMType>& flush_snapshot,
      bool& out_has_range, KeyType& out_min_key, KeyType& out_max_key) const {
    std::vector<std::pair<KeyType, uint64_t>> shadow_pairs;
    std::vector<std::pair<KeyType, uint64_t>> flush_pairs;
    std::vector<std::pair<KeyType, uint64_t>> merged_pairs;

    if (shadow_snapshot != nullptr) {
      CollectLippPairs(*shadow_snapshot, shadow_pairs);
    }
    if (flush_snapshot != nullptr) {
      CollectDynamicPairsSorted(*flush_snapshot, flush_pairs);
    }

    auto by_key = [](const auto& lhs, const auto& rhs) {
      return lhs.first < rhs.first;
    };
    std::sort(shadow_pairs.begin(), shadow_pairs.end(), by_key);
    std::sort(flush_pairs.begin(), flush_pairs.end(), by_key);

    merged_pairs.reserve(shadow_pairs.size() + flush_pairs.size());
    std::merge(shadow_pairs.begin(), shadow_pairs.end(), flush_pairs.begin(),
               flush_pairs.end(), std::back_inserter(merged_pairs), by_key);

    merged_pairs.erase(
        std::unique(merged_pairs.begin(), merged_pairs.end(),
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.first == rhs.first;
                    }),
        merged_pairs.end());

    out_has_range = !merged_pairs.empty();
    if (!merged_pairs.empty()) {
      out_min_key = merged_pairs.front().first;
      out_max_key = merged_pairs.back().first;
    }

    auto next_shadow = std::make_shared<LippType>();
    if (!merged_pairs.empty()) {
      next_shadow->bulk_load(merged_pairs.data(), merged_pairs.size());
    }
    return next_shadow;
  }

  void FlushWorkerLoop() {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(worker_mutex_);
        worker_cv_.wait(lock, [&] { return flush_requested_ || shutdown_; });
        if (shutdown_) {
          return;
        }
        flush_requested_ = false;
      }

      std::shared_ptr<const DynamicPGMType> flush_snapshot;
      std::shared_ptr<const LippType> shadow_snapshot;
      {
        std::lock_guard<std::mutex> lock(control_mutex_);
        flush_snapshot = worker_flush_;
        auto current = LoadPublishedState();
        shadow_snapshot = current != nullptr ? current->shadow : nullptr;
      }

      if (flush_snapshot == nullptr) {
        std::lock_guard<std::mutex> lock(control_mutex_);
        flush_in_progress_ = false;
        continue;
      }

      bool shadow_has_range = false;
      KeyType shadow_min_key{};
      KeyType shadow_max_key{};
      auto next_shadow = BuildMergedShadow(shadow_snapshot, flush_snapshot,
                                           shadow_has_range, shadow_min_key,
                                           shadow_max_key);

      std::lock_guard<std::mutex> lock(control_mutex_);
      auto next_state = std::make_shared<PublishedState>();
      next_state->shadow = next_shadow;
      next_state->shadow_has_range = shadow_has_range;
      next_state->shadow_min_key = shadow_min_key;
      next_state->shadow_max_key = shadow_max_key;
      next_state->flush.reset();
      next_state->flush_has_range = false;

      worker_flush_.reset();
      flush_in_progress_ = false;
      StorePublishedState(next_state);
    }
  }

  mutable LippType base_lipp_;

  mutable std::shared_ptr<const PublishedState> published_state_;

  mutable std::mutex control_mutex_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::thread worker_thread_;

  std::unique_ptr<DynamicPGMType> active_dpgm_;
  std::shared_ptr<const DynamicPGMType> worker_flush_;

  size_t active_size_{0};
  bool active_has_range_{false};
  KeyType active_min_key_{};
  KeyType active_max_key_{};

  bool flush_in_progress_{false};
  bool flush_requested_{false};
  bool shutdown_{false};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
