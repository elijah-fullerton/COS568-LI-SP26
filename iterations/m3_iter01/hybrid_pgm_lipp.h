#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../util.h"
#include "base.h"
#include "lipp/src/core/lipp.h"
#include "pgm_index_dynamic.hpp"

template <class KeyType, class SearchClass, size_t pgm_error, size_t flush_bps>
class HybridPGMLIPP : public Competitor<KeyType, SearchClass> {
 public:
  HybridPGMLIPP(const std::vector<int>& params) {}

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    ResetState();
    base_key_count_ = data.size();
    flush_threshold_ =
        std::max<size_t>(1, (base_key_count_ * flush_bps) / 10000);

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& item : data) {
      loading_data.emplace_back(item.key, item.value);
    }

    return util::timing([&] {
      pgm_ = DynamicPGMType();
      lipp_.bulk_load(loading_data.data(), loading_data.size());
    });
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    ++lookup_count_;

    if (PreferLippFirst()) {
      uint64_t value;
      if (lipp_.find(lookup_key, value)) {
        return value;
      }
      return LookupInPgm(lookup_key);
    }

    size_t value = LookupInPgm(lookup_key);
    if (value != util::OVERFLOW) {
      return value;
    }

    uint64_t lipp_value;
    if (lipp_.find(lookup_key, lipp_value)) {
      return lipp_value;
    }

    return util::OVERFLOW;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key,
                      uint32_t thread_id) const {
    ++lookup_count_;

    uint64_t result = 0;

    if (RangesOverlap(lower_key, upper_key, pgm_has_range_, pgm_min_key_,
                      pgm_max_key_)) {
      auto pgm_it = pgm_.lower_bound(lower_key);
      while (pgm_it != pgm_.end() && pgm_it->key() <= upper_key) {
        result += pgm_it->value();
        ++pgm_it;
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
    pgm_.insert(data.key, data.value);
    ++staged_inserts_;
    UpdateRange(data.key);

    if (staged_inserts_ >= CurrentFlushThreshold()) {
      FlushIntoLipp();
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const { return lipp_.index_size() + pgm_.size_in_bytes(); }

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
    return {"flush_bps",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-f" +
                std::to_string(flush_bps)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  static constexpr size_t kMinAdaptiveOps = 16384;
  static constexpr size_t kMinFlushThreshold = 1024;

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

  void ResetState() {
    pgm_ = DynamicPGMType();
    insert_count_ = 0;
    lookup_count_ = 0;
    staged_inserts_ = 0;
    pgm_has_range_ = false;
  }

  void UpdateRange(const KeyType& key) {
    if (!pgm_has_range_) {
      pgm_has_range_ = true;
      pgm_min_key_ = key;
      pgm_max_key_ = key;
      return;
    }
    if (key < pgm_min_key_) {
      pgm_min_key_ = key;
    }
    if (key > pgm_max_key_) {
      pgm_max_key_ = key;
    }
  }

  bool PreferLippFirst() const {
    const size_t total_ops = insert_count_ + lookup_count_;
    if (total_ops < kMinAdaptiveOps) {
      return false;
    }
    return lookup_count_ * 10 >= total_ops * 7;
  }

  size_t CurrentFlushThreshold() const {
    const size_t total_ops = insert_count_ + lookup_count_;
    if (total_ops < kMinAdaptiveOps) {
      return flush_threshold_;
    }
    if (lookup_count_ * 10 >= total_ops * 7) {
      return std::max<size_t>(kMinFlushThreshold, flush_threshold_ / 2);
    }
    if (insert_count_ * 10 >= total_ops * 7) {
      return flush_threshold_ * 2;
    }
    return flush_threshold_;
  }

  size_t LookupInPgm(const KeyType& lookup_key) const {
    if (!InTrackedRange(lookup_key, pgm_has_range_, pgm_min_key_, pgm_max_key_)) {
      return util::OVERFLOW;
    }

    auto it = pgm_.find(lookup_key);
    if (it != pgm_.end()) {
      return it->value();
    }
    return util::OVERFLOW;
  }

  void FlushIntoLipp() {
    pgm_.for_each([&](const KeyType& key, const uint64_t value) {
      lipp_.insert(key, value);
    });
    pgm_ = DynamicPGMType();
    staged_inserts_ = 0;
    pgm_has_range_ = false;
  }

  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable DynamicPGMType pgm_;
  size_t base_key_count_{0};
  size_t flush_threshold_{1};
  mutable size_t insert_count_{0};
  mutable size_t lookup_count_{0};
  size_t staged_inserts_{0};
  bool pgm_has_range_{false};
  KeyType pgm_min_key_{};
  KeyType pgm_max_key_{};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
