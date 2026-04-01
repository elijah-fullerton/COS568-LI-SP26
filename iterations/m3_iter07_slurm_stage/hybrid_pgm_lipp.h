#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../util.h"
#include "PGM-index/include/pgm_index_dynamic.hpp"
#include "base.h"
#include "lipp/src/core/lipp.h"

template <class KeyType, class SearchClass, size_t pgm_error,
          size_t owner_max_size, size_t stress_bps>
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
      lipp_.bulk_load(loading_data.data(), loading_data.size());
      lipp_.freeze_existing_nodes();
      const size_t owner_count = static_cast<size_t>(
          std::max(1, lipp_.assign_buffer_owners(static_cast<int>(owner_max_size))));

      spills_.assign(owner_count, DynamicPGMType());
      spill_sizes_.assign(owner_count, 0);
      owner_region_sizes_.assign(owner_count, 0);
      owner_direct_inserts_.assign(owner_count, 0);
      owner_expand_counts_.assign(owner_count, 0);
      owner_spill_mode_.assign(owner_count, false);

      for (size_t owner_id = 0; owner_id < owner_count; ++owner_id) {
        owner_region_sizes_[owner_id] =
            static_cast<size_t>(lipp_.buffer_owner_size(static_cast<int>(owner_id)));
      }
    });
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    ++lookup_count_;

    if (PreferSpillFirst()) {
      const size_t owner_id =
          static_cast<size_t>(lipp_.locate_buffer_owner(lookup_key));
      const size_t spill_value = LookupInSpill(owner_id, lookup_key);
      if (spill_value != util::OVERFLOW) {
        return spill_value;
      }

      uint64_t lipp_value = 0;
      if (lipp_.find(lookup_key, lipp_value)) {
        return lipp_value;
      }
      return util::OVERFLOW;
    }

    uint64_t lipp_value = 0;
    int owner_id = 0;
    if (lipp_.find_with_buffer_owner(lookup_key, lipp_value, owner_id)) {
      return lipp_value;
    }

    if (owner_id < 0 || !owner_spill_mode_[static_cast<size_t>(owner_id)]) {
      return util::OVERFLOW;
    }
    return LookupInSpill(static_cast<size_t>(owner_id), lookup_key);
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key,
                      uint32_t thread_id) const {
    ++lookup_count_;

    uint64_t result = 0;
    for (size_t owner_id = 0; owner_id < spills_.size(); ++owner_id) {
      if (!owner_spill_mode_[owner_id] || spill_sizes_[owner_id] == 0) {
        continue;
      }
      auto it = spills_[owner_id].lower_bound(lower_key);
      while (it != spills_[owner_id].end() && it->key() <= upper_key) {
        result += it->value();
        ++it;
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
    const size_t owner_id = static_cast<size_t>(lipp_.locate_buffer_owner(data.key));

    if (owner_spill_mode_[owner_id]) {
      spills_[owner_id].insert(data.key, data.value);
      ++spill_sizes_[owner_id];
      if (spill_sizes_[owner_id] >= SpillFlushThreshold(owner_id)) {
        FlushSpill(owner_id);
      }
      return;
    }

    lipp_.insert(data.key, data.value);
    ++owner_region_sizes_[owner_id];
    ++owner_direct_inserts_[owner_id];

    if (owner_direct_inserts_[owner_id] >= ExpandThreshold(owner_id)) {
      ExpandOwner(owner_id);
      return;
    }

    if (ShouldEnableSpill(owner_id)) {
      owner_spill_mode_[owner_id] = true;
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    size_t total = lipp_.index_size();
    for (const auto& spill : spills_) {
      total += spill.size_in_bytes();
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
    return {"selective_spill_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-s" +
                std::to_string(owner_max_size) + "-t" +
                std::to_string(stress_bps)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using PairType = std::pair<KeyType, uint64_t>;

  void ResetState() {
    insert_count_ = 0;
    lookup_count_ = 0;
    spills_.clear();
    spill_sizes_.clear();
    owner_region_sizes_.clear();
    owner_direct_inserts_.clear();
    owner_expand_counts_.clear();
    owner_spill_mode_.clear();
  }

  bool PreferSpillFirst() const {
    return insert_count_ > lookup_count_ * 3;
  }

  size_t LookupInSpill(size_t owner_id, const KeyType& lookup_key) const {
    if (owner_id >= spills_.size() || spill_sizes_[owner_id] == 0) {
      return util::OVERFLOW;
    }
    auto it = spills_[owner_id].find(lookup_key);
    if (it != spills_[owner_id].end()) {
      return it->value();
    }
    return util::OVERFLOW;
  }

  size_t ExpandThreshold(size_t owner_id) const {
    const size_t region = owner_region_sizes_[owner_id];
    size_t threshold = std::max<size_t>(256, region / 4);
    if (lookup_count_ > insert_count_ * 4) {
      threshold = std::max<size_t>(128, region / 8);
    }
    return threshold;
  }

  bool ShouldEnableSpill(size_t owner_id) const {
    const size_t region = owner_region_sizes_[owner_id];
    const size_t direct = owner_direct_inserts_[owner_id];
    if (lookup_count_ > insert_count_ * 4) {
      return false;
    }
    if (direct < std::max<size_t>(64, (region * stress_bps) / 10000)) {
      return false;
    }
    return owner_expand_counts_[owner_id] > 0 || direct > region / 6;
  }

  size_t SpillFlushThreshold(size_t owner_id) const {
    const size_t region = owner_region_sizes_[owner_id];
    size_t threshold = std::max<size_t>(256, region / 8);
    if (insert_count_ > lookup_count_ * 4) {
      threshold = std::max<size_t>(512, region / 4);
    }
    return threshold;
  }

  double OwnerSlack(size_t owner_id) const {
    double slack = 0.08;
    if (lookup_count_ > insert_count_ * 4) {
      slack = 0.15;
    }
    slack += std::min<double>(0.12, 0.04 * owner_expand_counts_[owner_id]);
    return std::min(0.25, slack);
  }

  void ExpandOwner(size_t owner_id) {
    static const std::vector<PairType> empty;
    lipp_.rebuild_buffer_owner(static_cast<int>(owner_id), empty.data(), 0,
                               OwnerSlack(owner_id));
    owner_region_sizes_[owner_id] = static_cast<size_t>(
        lipp_.buffer_owner_size(static_cast<int>(owner_id)));
    owner_direct_inserts_[owner_id] = 0;
    ++owner_expand_counts_[owner_id];
  }

  void FlushSpill(size_t owner_id) {
    std::vector<PairType> staged;
    staged.reserve(spill_sizes_[owner_id]);
    spills_[owner_id].for_each([&](const KeyType& key, const uint64_t value) {
      staged.emplace_back(key, value);
    });
    std::sort(staged.begin(), staged.end(),
              [](const PairType& lhs, const PairType& rhs) {
                return lhs.first < rhs.first;
              });

    lipp_.rebuild_buffer_owner(static_cast<int>(owner_id), staged.data(),
                               static_cast<int>(staged.size()),
                               OwnerSlack(owner_id));

    spills_[owner_id] = DynamicPGMType();
    spill_sizes_[owner_id] = 0;
    owner_region_sizes_[owner_id] = static_cast<size_t>(
        lipp_.buffer_owner_size(static_cast<int>(owner_id)));
    owner_direct_inserts_[owner_id] = 0;
    owner_spill_mode_[owner_id] = false;
    ++owner_expand_counts_[owner_id];
  }

  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable std::vector<DynamicPGMType> spills_;
  mutable std::vector<size_t> spill_sizes_;
  mutable std::vector<size_t> owner_region_sizes_;
  mutable std::vector<size_t> owner_direct_inserts_;
  mutable std::vector<size_t> owner_expand_counts_;
  mutable std::vector<bool> owner_spill_mode_;

  mutable size_t insert_count_{0};
  mutable size_t lookup_count_{0};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
