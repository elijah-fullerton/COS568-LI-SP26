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
    staged_inserts_ = 0;
    base_key_count_ = data.size();
    flush_threshold_ =
        std::max<size_t>(1, (base_key_count_ * flush_bps) / 10000);

    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& item : data) {
      loading_data.emplace_back(item.key, item.value);
    }

    uint64_t build_time = util::timing([&] {
      pgm_ = decltype(pgm_)();
      lipp_.bulk_load(loading_data.data(), loading_data.size());
    });

    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    auto it = pgm_.find(lookup_key);
    if (it != pgm_.end()) {
      return it->value();
    }

    uint64_t value;
    if (lipp_.find(lookup_key, value)) {
      return value;
    }

    return util::OVERFLOW;
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key,
                      uint32_t thread_id) const {
    uint64_t result = 0;

    auto pgm_it = pgm_.lower_bound(lower_key);
    while (pgm_it != pgm_.end() && pgm_it->key() <= upper_key) {
      result += pgm_it->value();
      ++pgm_it;
    }

    auto lipp_it = lipp_.lower_bound(lower_key);
    while (lipp_it != lipp_.end() && lipp_it->comp.data.key <= upper_key) {
      result += lipp_it->comp.data.value;
      ++lipp_it;
    }

    return result;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    pgm_.insert(data.key, data.value);
    ++staged_inserts_;
    if (staged_inserts_ >= flush_threshold_) {
      FlushIntoLipp();
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const { return lipp_.index_size() + pgm_.size_in_bytes(); }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    return unique && !multithread && insert &&
           ops_filename.find("mix") != std::string::npos;
  }

  std::vector<std::string> variants() const {
    return {"flush_bps", std::to_string(flush_bps)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  void FlushIntoLipp() {
    pgm_.for_each([&](const KeyType& key, const uint64_t value) {
      lipp_.insert(key, value);
    });
    pgm_ = DynamicPGMType();
    staged_inserts_ = 0;
  }

  LIPP<KeyType, uint64_t> lipp_;
  DynamicPGMType pgm_;
  size_t base_key_count_{0};
  size_t flush_threshold_{1};
  size_t staged_inserts_{0};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
