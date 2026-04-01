#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
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

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    ResetState();

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& item : data) {
      loading_data.emplace_back(item.key, item.value);
    }

    return util::timing([&] {
      base_lipp_.bulk_load(loading_data.data(), loading_data.size());
      root_model_a_ = base_lipp_.root_model_a();
      root_model_b_ = base_lipp_.root_model_b();
      root_num_items_ = std::max<size_t>(1, base_lipp_.root_num_items());

      active_.assign(num_buckets, DynamicPGMType());
      active_sizes_.assign(num_buckets, 0);
      overlays_.clear();
      overlays_.resize(num_buckets);
      overlay_sizes_.assign(num_buckets, 0);

      active_has_range_.assign(num_buckets, false);
      overlay_has_range_.assign(num_buckets, false);
      active_min_.assign(num_buckets, KeyType{});
      active_max_.assign(num_buckets, KeyType{});
      overlay_min_.assign(num_buckets, KeyType{});
      overlay_max_.assign(num_buckets, KeyType{});
    });
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    uint64_t value = 0;
    if (base_lipp_.find(lookup_key, value)) {
      return value;
    }

    const size_t bucket = BucketForKey(lookup_key);
    if (overlay_has_range_[bucket] && InRange(lookup_key, overlay_min_[bucket],
                                              overlay_max_[bucket])) {
      if (overlays_[bucket] != nullptr &&
          overlays_[bucket]->find(lookup_key, value)) {
        return value;
      }
    }

    if (active_has_range_[bucket] && InRange(lookup_key, active_min_[bucket],
                                             active_max_[bucket])) {
      auto it = active_[bucket].find(lookup_key);
      if (it != active_[bucket].end()) {
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

    size_t lower_bucket = BucketForKey(lower_key);
    size_t upper_bucket = BucketForKey(upper_key);
    if (lower_bucket > upper_bucket) {
      std::swap(lower_bucket, upper_bucket);
    }

    for (size_t bucket = lower_bucket; bucket <= upper_bucket; ++bucket) {
      if (overlay_has_range_[bucket] &&
          RangesOverlap(lower_key, upper_key, overlay_min_[bucket],
                        overlay_max_[bucket]) &&
          overlays_[bucket] != nullptr) {
        auto it = overlays_[bucket]->lower_bound(lower_key);
        while (it != overlays_[bucket]->end() && it->comp.data.key <= upper_key) {
          result += it->comp.data.value;
          ++it;
        }
      }

      if (active_has_range_[bucket] &&
          RangesOverlap(lower_key, upper_key, active_min_[bucket],
                        active_max_[bucket])) {
        auto it = active_[bucket].lower_bound(lower_key);
        while (it != active_[bucket].end() && it->key() <= upper_key) {
          result += it->value();
          ++it;
        }
      }
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    const size_t bucket = BucketForKey(data.key);
    active_[bucket].insert(data.key, data.value);
    ++active_sizes_[bucket];
    UpdateActiveRange(bucket, data.key);

    if (active_sizes_[bucket] >= ActiveThreshold()) {
      FlushBucket(bucket);
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    size_t total = base_lipp_.index_size();
    for (size_t bucket = 0; bucket < num_buckets; ++bucket) {
      total += active_[bucket].size_in_bytes();
      if (overlays_[bucket] != nullptr) {
        total += overlays_[bucket]->index_size();
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
    return {"bucket_local_shadow_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-b" +
                std::to_string(num_buckets) + "-a" +
                std::to_string(active_threshold)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using LippType = LIPP<KeyType, uint64_t>;

  void ResetState() {
    active_.clear();
    active_sizes_.clear();
    overlays_.clear();
    overlay_sizes_.clear();
    active_has_range_.clear();
    overlay_has_range_.clear();
    active_min_.clear();
    active_max_.clear();
    overlay_min_.clear();
    overlay_max_.clear();
    root_model_a_ = 0;
    root_model_b_ = 0;
    root_num_items_ = 1;
  }

  size_t ActiveThreshold() const {
    return std::max<size_t>(32, active_threshold);
  }

  bool InRange(const KeyType& key, const KeyType& lower,
               const KeyType& upper) const {
    return key >= lower && key <= upper;
  }

  bool RangesOverlap(const KeyType& lower_a, const KeyType& upper_a,
                     const KeyType& lower_b, const KeyType& upper_b) const {
    return !(upper_a < lower_b || upper_b < lower_a);
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

  void UpdateActiveRange(size_t bucket, const KeyType& key) {
    if (!active_has_range_[bucket]) {
      active_has_range_[bucket] = true;
      active_min_[bucket] = key;
      active_max_[bucket] = key;
      return;
    }
    if (key < active_min_[bucket]) {
      active_min_[bucket] = key;
    }
    if (key > active_max_[bucket]) {
      active_max_[bucket] = key;
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

  void FlushBucket(size_t bucket) {
    if (active_sizes_[bucket] == 0) {
      return;
    }

    std::vector<std::pair<KeyType, uint64_t>> merged;
    merged.reserve(active_sizes_[bucket] + overlay_sizes_[bucket]);
    if (overlays_[bucket] != nullptr) {
      CollectLippPairs(*overlays_[bucket], merged);
    }
    active_[bucket].for_each([&](const KeyType& key, const uint64_t value) {
      merged.emplace_back(key, value);
    });
    std::sort(merged.begin(), merged.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    auto overlay = std::make_unique<LippType>();
    overlay->bulk_load(merged.data(), merged.size());
    overlays_[bucket] = std::move(overlay);
    overlay_sizes_[bucket] = merged.size();

    overlay_has_range_[bucket] = !merged.empty();
    if (!merged.empty()) {
      overlay_min_[bucket] = merged.front().first;
      overlay_max_[bucket] = merged.back().first;
    }

    active_[bucket] = DynamicPGMType();
    active_sizes_[bucket] = 0;
    active_has_range_[bucket] = false;
  }

  mutable LippType base_lipp_;
  mutable std::vector<DynamicPGMType> active_;
  mutable std::vector<size_t> active_sizes_;
  mutable std::vector<std::unique_ptr<LippType>> overlays_;
  mutable std::vector<size_t> overlay_sizes_;

  mutable std::vector<bool> active_has_range_;
  mutable std::vector<bool> overlay_has_range_;
  mutable std::vector<KeyType> active_min_;
  mutable std::vector<KeyType> active_max_;
  mutable std::vector<KeyType> overlay_min_;
  mutable std::vector<KeyType> overlay_max_;

  long double root_model_a_{0};
  long double root_model_b_{0};
  size_t root_num_items_{1};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
