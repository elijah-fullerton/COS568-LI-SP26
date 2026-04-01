#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
      root_model_a_ = base_lipp_.root_model_a();
      root_model_b_ = base_lipp_.root_model_b();
      root_num_items_ = std::max<size_t>(1, base_lipp_.root_num_items());
    });

    worker_thread_ = std::thread(&HybridPGMLIPP::FlushWorkerLoop, this);
    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    uint64_t value = 0;

    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    if (base_lipp_.find(lookup_key, value)) {
      return value;
    }

    if (shadow_has_range_ &&
        InRange(lookup_key, shadow_min_key_, shadow_max_key_) &&
        shadow_lipp_ != nullptr && shadow_lipp_->find(lookup_key, value)) {
      return value;
    }

    if (flush_has_range_ && InRange(lookup_key, flush_min_key_, flush_max_key_) &&
        flush_dpgm_ != nullptr) {
      auto it = flush_dpgm_->find(lookup_key);
      if (it != flush_dpgm_->end()) {
        return it->value();
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

    std::shared_lock<std::shared_mutex> lock(state_mutex_);

    auto base_it = base_lipp_.lower_bound(lower_key);
    while (base_it != base_lipp_.end() && base_it->comp.data.key <= upper_key) {
      result += base_it->comp.data.value;
      ++base_it;
    }

    if (shadow_has_range_ && RangesOverlap(lower_key, upper_key, shadow_min_key_,
                                           shadow_max_key_) &&
        shadow_lipp_ != nullptr) {
      auto it = shadow_lipp_->lower_bound(lower_key);
      while (it != shadow_lipp_->end() && it->comp.data.key <= upper_key) {
        result += it->comp.data.value;
        ++it;
      }
    }

    if (flush_has_range_ && RangesOverlap(lower_key, upper_key, flush_min_key_,
                                          flush_max_key_) &&
        flush_dpgm_ != nullptr) {
      auto it = flush_dpgm_->lower_bound(lower_key);
      while (it != flush_dpgm_->end() && it->key() <= upper_key) {
        result += it->value();
        ++it;
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
    bool request_flush = false;

    {
      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      active_dpgm_->insert(data.key, data.value);
      ++active_size_;
      UpdateRange(active_has_range_, active_min_key_, active_max_key_, data.key);

      if (!flush_in_progress_ && active_size_ >= FlushThreshold()) {
        flush_dpgm_ =
            std::make_shared<DynamicPGMType>(std::move(*active_dpgm_));
        flush_size_ = active_size_;
        flush_has_range_ = active_has_range_;
        flush_min_key_ = active_min_key_;
        flush_max_key_ = active_max_key_;

        active_dpgm_ = std::make_unique<DynamicPGMType>();
        active_size_ = 0;
        active_has_range_ = false;

        flush_in_progress_ = true;
        flush_requested_ = true;
        request_flush = true;
      }
    }

    if (request_flush) {
      worker_cv_.notify_one();
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    size_t total = base_lipp_.index_size();
    total += active_dpgm_ != nullptr ? active_dpgm_->size_in_bytes() : 0;
    total += flush_dpgm_ != nullptr ? flush_dpgm_->size_in_bytes() : 0;
    total += shadow_lipp_ != nullptr ? shadow_lipp_->index_size() : 0;
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
    return {"async_shadow_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-f" +
                std::to_string(flush_threshold)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using LippType = LIPP<KeyType, uint64_t>;

  void ResetState() {
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    base_lipp_.bulk_load(nullptr, 0);
    shadow_lipp_.reset();
    flush_dpgm_.reset();
    active_dpgm_.reset();
    active_size_ = 0;
    flush_size_ = 0;
    active_has_range_ = false;
    flush_has_range_ = false;
    shadow_has_range_ = false;
    flush_in_progress_ = false;
    flush_requested_ = false;
    shutdown_ = false;
    root_model_a_ = 0;
    root_model_b_ = 0;
    root_num_items_ = 1;
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

      std::shared_ptr<DynamicPGMType> flush_snapshot;
      std::shared_ptr<LippType> shadow_snapshot;
      {
        std::shared_lock<std::shared_mutex> lock(state_mutex_);
        flush_snapshot = flush_dpgm_;
        shadow_snapshot = shadow_lipp_;
      }

      if (flush_snapshot == nullptr) {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        flush_in_progress_ = false;
        continue;
      }

      std::vector<std::pair<KeyType, uint64_t>> merged;
      merged.reserve(flush_size_);
      if (shadow_snapshot != nullptr) {
        CollectLippPairs(*shadow_snapshot, merged);
      }
      CollectDynamicPairs(*flush_snapshot, merged);
      std::sort(merged.begin(), merged.end(),
                [](const auto& lhs, const auto& rhs) {
                  return lhs.first < rhs.first;
                });

      auto next_shadow = std::make_shared<LippType>();
      if (!merged.empty()) {
        next_shadow->bulk_load(merged.data(), merged.size());
      }

      std::unique_lock<std::shared_mutex> lock(state_mutex_);
      shadow_lipp_ = next_shadow;
      shadow_has_range_ = !merged.empty();
      if (!merged.empty()) {
        shadow_min_key_ = merged.front().first;
        shadow_max_key_ = merged.back().first;
      }

      flush_dpgm_.reset();
      flush_size_ = 0;
      flush_has_range_ = false;
      flush_in_progress_ = false;
    }
  }

  mutable LippType base_lipp_;

  mutable std::shared_mutex state_mutex_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::thread worker_thread_;

  std::unique_ptr<DynamicPGMType> active_dpgm_;
  std::shared_ptr<DynamicPGMType> flush_dpgm_;
  std::shared_ptr<LippType> shadow_lipp_;

  size_t active_size_{0};
  size_t flush_size_{0};

  bool active_has_range_{false};
  bool flush_has_range_{false};
  bool shadow_has_range_{false};

  KeyType active_min_key_{};
  KeyType active_max_key_{};
  KeyType flush_min_key_{};
  KeyType flush_max_key_{};
  KeyType shadow_min_key_{};
  KeyType shadow_max_key_{};

  bool flush_in_progress_{false};
  bool flush_requested_{false};
  bool shutdown_{false};

  long double root_model_a_{0};
  long double root_model_b_{0};
  size_t root_num_items_{1};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
