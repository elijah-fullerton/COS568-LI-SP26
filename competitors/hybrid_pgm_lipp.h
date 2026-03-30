#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../util.h"
#include "base.h"
#include "lipp/src/core/lipp.h"
#include "pgm_index_dynamic.hpp"

template <class KeyType, class SearchClass, size_t pgm_error,
          size_t trigger_bps, size_t max_frozen_keys, size_t flush_budget>
class HybridPGMLIPP : public Competitor<KeyType, SearchClass> {
 public:
  HybridPGMLIPP(const std::vector<int>& params) {}

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    ResetState();
    base_key_count_ = data.size();

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& item : data) {
      loading_data.emplace_back(item.key, item.value);
    }

    return util::timing([&] {
      active_pgm_ = DynamicPGMType();
      frozen_pgm_ = DynamicPGMType();
      lipp_.bulk_load(loading_data.data(), loading_data.size());
    });
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    ++lookup_count_;
    MaybeFreezeActive();
    AdvanceFlush(CurrentFlushBudget());

    if (InTrackedRange(lookup_key, active_has_range_, active_min_key_,
                       active_max_key_)) {
      auto active_it = active_pgm_.find(lookup_key);
      if (active_it != active_pgm_.end()) {
        return active_it->value();
      }
    }

    if (frozen_present_ &&
        InTrackedRange(lookup_key, frozen_has_range_, frozen_min_key_,
                       frozen_max_key_)) {
      auto frozen_it = frozen_pgm_.find(lookup_key);
      if (frozen_it != frozen_pgm_.end()) {
        return frozen_it->value();
      }
    }

    uint64_t value;
    if (lipp_.find(lookup_key, value)) {
      return value;
    }

    return util::OVERFLOW;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key,
                      uint32_t thread_id) const {
    ++lookup_count_;
    MaybeFreezeActive();
    AdvanceFlush(CurrentFlushBudget());

    uint64_t result = 0;

    if (RangesOverlap(lower_key, upper_key, active_has_range_, active_min_key_,
                      active_max_key_)) {
      auto active_it = active_pgm_.lower_bound(lower_key);
      while (active_it != active_pgm_.end() && active_it->key() <= upper_key) {
        result += active_it->value();
        ++active_it;
      }
    }

    if (frozen_present_ &&
        RangesOverlap(lower_key, upper_key, frozen_has_range_, frozen_min_key_,
                      frozen_max_key_)) {
      auto frozen_it = frozen_pgm_.lower_bound(lower_key);
      while (frozen_it != frozen_pgm_.end() && frozen_it->key() <= upper_key) {
        result += frozen_it->value();
        ++frozen_it;
      }
    }

    auto lipp_it = lipp_.lower_bound(lower_key);
    while (lipp_it != lipp_.end() && lipp_it->comp.data.key <= upper_key) {
      result += lipp_it->comp.data.value;
      ++lipp_it;
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    ++insert_count_;
    active_pgm_.insert(data.key, data.value);
    ++active_count_;
    UpdateRange(data.key, active_has_range_, active_min_key_, active_max_key_);

    MaybeFreezeActive();

    size_t budget = CurrentFlushBudget();
    if (active_count_ > max_frozen_keys && budget < flush_budget * 4) {
      budget = flush_budget * 4;
    }
    AdvanceFlush(budget);
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    return lipp_.index_size() + active_pgm_.size_in_bytes() +
           (frozen_present_ ? frozen_pgm_.size_in_bytes() : 0);
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
    return {"strategy", SearchClass::name() + "-e" + std::to_string(pgm_error) +
                            "-t" + std::to_string(trigger_bps) + "-m" +
                            std::to_string(max_frozen_keys) + "-b" +
                            std::to_string(flush_budget)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using KVPair = std::pair<KeyType, uint64_t>;

  static constexpr size_t kMinTriggerKeys = 4096;

  static bool InTrackedRange(const KeyType& key, bool has_range,
                             const KeyType& min_key,
                             const KeyType& max_key) {
    return has_range && key >= min_key && key <= max_key;
  }

  static bool RangesOverlap(const KeyType& lower_key, const KeyType& upper_key,
                            bool has_range, const KeyType& min_key,
                            const KeyType& max_key) {
    return has_range && !(upper_key < min_key || lower_key > max_key);
  }

  static void UpdateRange(const KeyType& key, bool& has_range, KeyType& min_key,
                          KeyType& max_key) {
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

  bool LookupHeavy() const {
    const size_t total_ops = insert_count_ + lookup_count_;
    if (total_ops < 16384 || insert_count_ * 10 > total_ops * 3) {
      return false;
    }
    return lookup_count_ * 10 >= total_ops * 7;
  }

  bool InsertHeavy() const {
    const size_t total_ops = insert_count_ + lookup_count_;
    if (total_ops < 16384 || lookup_count_ * 10 > total_ops * 3) {
      return false;
    }
    return insert_count_ * 10 >= total_ops * 7;
  }

  size_t BaseTriggerThreshold() const {
    size_t threshold =
        std::max<size_t>(kMinTriggerKeys, (base_key_count_ * trigger_bps) / 10000);
    return std::min(threshold, max_frozen_keys);
  }

  size_t EffectiveTriggerThreshold() const {
    const size_t base_threshold = BaseTriggerThreshold();
    if (LookupHeavy()) {
      return std::max<size_t>(kMinTriggerKeys, base_threshold / 2);
    }
    if (InsertHeavy()) {
      return std::min(max_frozen_keys, base_threshold * 2);
    }
    return base_threshold;
  }

  size_t CurrentFlushBudget() const {
    size_t budget = flush_budget;
    if (LookupHeavy()) {
      budget *= 4;
    } else if (InsertHeavy()) {
      budget = std::max<size_t>(1, budget / 2);
    }
    return budget;
  }

  void ResetState() {
    active_count_ = 0;
    frozen_count_ = 0;
    insert_count_ = 0;
    lookup_count_ = 0;
    active_has_range_ = false;
    frozen_has_range_ = false;
    frozen_present_ = false;
    frozen_flush_offset_ = 0;
    frozen_items_.clear();
  }

  void MaybeFreezeActive() const {
    if (frozen_present_ || active_count_ < EffectiveTriggerThreshold()) {
      return;
    }

    frozen_pgm_ = std::move(active_pgm_);
    active_pgm_ = DynamicPGMType();

    frozen_present_ = true;
    frozen_count_ = active_count_;
    frozen_has_range_ = active_has_range_;
    frozen_min_key_ = active_min_key_;
    frozen_max_key_ = active_max_key_;

    active_count_ = 0;
    active_has_range_ = false;

    frozen_items_.clear();
    frozen_items_.reserve(frozen_count_);
    frozen_pgm_.for_each([&](const KeyType& key, const uint64_t value) {
      frozen_items_.emplace_back(key, value);
    });
    std::sort(frozen_items_.begin(), frozen_items_.end(),
              [](const KVPair& lhs, const KVPair& rhs) {
                return lhs.first < rhs.first;
              });
    frozen_flush_offset_ = 0;
  }

  void AdvanceFlush(size_t budget) const {
    if (!frozen_present_ || budget == 0) {
      return;
    }

    const size_t flush_limit =
        std::min(frozen_items_.size(), frozen_flush_offset_ + budget);
    while (frozen_flush_offset_ < flush_limit) {
      const auto& kv = frozen_items_[frozen_flush_offset_++];
      lipp_.insert(kv.first, kv.second);
    }

    if (frozen_flush_offset_ == frozen_items_.size()) {
      frozen_present_ = false;
      frozen_count_ = 0;
      frozen_has_range_ = false;
      frozen_flush_offset_ = 0;
      frozen_items_.clear();
      frozen_pgm_ = DynamicPGMType();
    }
  }

  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable DynamicPGMType active_pgm_;
  mutable DynamicPGMType frozen_pgm_;

  size_t base_key_count_{0};

  mutable size_t active_count_{0};
  mutable size_t frozen_count_{0};
  mutable size_t insert_count_{0};
  mutable size_t lookup_count_{0};

  mutable bool active_has_range_{false};
  mutable bool frozen_has_range_{false};
  mutable bool frozen_present_{false};

  mutable KeyType active_min_key_{};
  mutable KeyType active_max_key_{};
  mutable KeyType frozen_min_key_{};
  mutable KeyType frozen_max_key_{};

  mutable std::vector<KVPair> frozen_items_;
  mutable size_t frozen_flush_offset_{0};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
