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
          size_t owner_max_size, size_t local_flush_threshold>
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
      const int owner_count =
          std::max(1, lipp_.assign_buffer_owners(static_cast<int>(owner_max_size)));
      buffers_.assign(owner_count, DynamicPGMType());
      buffer_sizes_.assign(owner_count, 0);
      owner_region_sizes_.assign(owner_count, 0);
      for (int owner_id = 0; owner_id < owner_count; ++owner_id) {
        owner_region_sizes_[owner_id] =
            static_cast<size_t>(lipp_.buffer_owner_size(owner_id));
      }
    });
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    ++lookup_count_;

    if (PreferBufferFirst()) {
      const size_t owner_id =
          static_cast<size_t>(lipp_.locate_buffer_owner(lookup_key));
      const size_t buffered_value = LookupInBuffer(owner_id, lookup_key);
      if (buffered_value != util::OVERFLOW) {
        return buffered_value;
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
    return LookupInBuffer(static_cast<size_t>(owner_id), lookup_key);
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key,
                      uint32_t thread_id) const {
    ++lookup_count_;

    uint64_t result = 0;
    for (size_t owner_id = 0; owner_id < buffers_.size(); ++owner_id) {
      if (buffer_sizes_[owner_id] == 0) {
        continue;
      }
      auto it = buffers_[owner_id].lower_bound(lower_key);
      while (it != buffers_[owner_id].end() && it->key() <= upper_key) {
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
    buffers_[owner_id].insert(data.key, data.value);
    ++buffer_sizes_[owner_id];

    if (buffer_sizes_[owner_id] >= EffectiveFlushThreshold(owner_id)) {
      FlushOwner(owner_id);
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const {
    size_t total = lipp_.index_size();
    for (const auto& buffer : buffers_) {
      total += buffer.size_in_bytes();
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
    return {"owner_buffered_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-s" +
                std::to_string(owner_max_size) + "-f" +
                std::to_string(local_flush_threshold)};
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  void ResetState() {
    insert_count_ = 0;
    lookup_count_ = 0;
    buffers_.clear();
    buffer_sizes_.clear();
    owner_region_sizes_.clear();
  }

  bool PreferBufferFirst() const {
    return insert_count_ > lookup_count_ * 4;
  }

  size_t EffectiveFlushThreshold(size_t owner_id) const {
    const size_t region =
        owner_id < owner_region_sizes_.size() ? owner_region_sizes_[owner_id] : 0;
    size_t threshold =
        std::max<size_t>(16, std::min<size_t>(local_flush_threshold, region / 8));
    if (threshold == 16 && local_flush_threshold > 16 && region == 0) {
      threshold = local_flush_threshold;
    }
    if (lookup_count_ > insert_count_ * 4) {
      return std::max<size_t>(8, threshold / 2);
    }
    if (insert_count_ > lookup_count_ * 4) {
      return std::max<size_t>(32, threshold * 2);
    }
    return threshold;
  }

  size_t LookupInBuffer(size_t owner_id, const KeyType& lookup_key) const {
    if (owner_id >= buffers_.size() || buffer_sizes_[owner_id] == 0) {
      return util::OVERFLOW;
    }
    auto it = buffers_[owner_id].find(lookup_key);
    if (it != buffers_[owner_id].end()) {
      return it->value();
    }
    return util::OVERFLOW;
  }

  void FlushOwner(size_t owner_id) {
    if (owner_id >= buffers_.size() || buffer_sizes_[owner_id] == 0) {
      return;
    }

    buffers_[owner_id].for_each([&](const KeyType& key, const uint64_t value) {
      lipp_.insert(key, value);
    });
    buffer_sizes_[owner_id] = 0;
    buffers_[owner_id] = DynamicPGMType();
    owner_region_sizes_[owner_id] =
        static_cast<size_t>(lipp_.buffer_owner_size(static_cast<int>(owner_id)));
  }

  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable std::vector<DynamicPGMType> buffers_;
  mutable std::vector<size_t> buffer_sizes_;
  mutable std::vector<size_t> owner_region_sizes_;

  mutable size_t insert_count_{0};
  mutable size_t lookup_count_{0};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
