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
#if defined(AUTORESEARCH_SCREEN_SAFE)
      // The screen build is a measurability canary. Keep exactly one deferred
      // DynamicPGM overlay so we do not spend startup time assigning owners or
      // fragmenting inserts across many tiny local buffers before the first
      // RESULT line.
      buffers_.assign(1, DynamicPGMType());
      buffer_sizes_.assign(1, 0);
      owner_region_sizes_.assign(1, data.size());
#else
      if (ShouldUseSingleDeferredOverlay(data.size())) {
        // The current strategy candidate is "restore full-run measurability
        // first". When the owner span is effectively global, skip owner
        // assignment entirely so the runtime matches the intended sweep point
        // instead of paying startup and routing costs for a near-global split.
        buffers_.assign(1, DynamicPGMType());
        buffer_sizes_.assign(1, 0);
        owner_region_sizes_.assign(1, data.size());
      } else {
        const int owner_count = std::max(
            1, lipp_.assign_buffer_owners(static_cast<int>(EffectiveOwnerMaxSize())));
        buffers_.assign(owner_count, DynamicPGMType());
        buffer_sizes_.assign(owner_count, 0);
        owner_region_sizes_.assign(owner_count, 0);
        for (int owner_id = 0; owner_id < owner_count; ++owner_id) {
          owner_region_sizes_[owner_id] =
              static_cast<size_t>(lipp_.buffer_owner_size(owner_id));
        }
      }
#endif
    });
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    ++lookup_count_;

#if defined(AUTORESEARCH_SCREEN_SAFE)
    uint64_t lipp_value = 0;
    if (ShouldProbeScreenBufferFirst()) {
      const size_t buffered_value = LookupInBuffer(0, lookup_key);
      if (buffered_value != util::OVERFLOW) {
        return buffered_value;
      }
      if (lipp_.find(lookup_key, lipp_value)) {
        return lipp_value;
      }
    } else {
      if (lipp_.find(lookup_key, lipp_value)) {
        return lipp_value;
      }
      const size_t buffered_value = LookupInBuffer(0, lookup_key);
      if (buffered_value != util::OVERFLOW) {
        return buffered_value;
      }
    }
    return util::OVERFLOW;
#else
    if (HasSingleGlobalBuffer()) {
      uint64_t lipp_value = 0;
      if (ShouldProbeSingleBufferFirst()) {
        const size_t buffered_value = LookupInBuffer(0, lookup_key);
        if (buffered_value != util::OVERFLOW) {
          return buffered_value;
        }
        if (lipp_.find(lookup_key, lipp_value)) {
          return lipp_value;
        }
      } else {
        if (lipp_.find(lookup_key, lipp_value)) {
          return lipp_value;
        }
        const size_t buffered_value = LookupInBuffer(0, lookup_key);
        if (buffered_value != util::OVERFLOW) {
          return buffered_value;
        }
      }
      return util::OVERFLOW;
    }

    if (PreferBufferFirst()) {
      const size_t owner_id =
          static_cast<size_t>(lipp_.locate_buffer_owner(lookup_key));
      if (ShouldProbeBufferFirst(owner_id)) {
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
    }

    uint64_t lipp_value = 0;
    int owner_id = 0;
    if (lipp_.find_with_buffer_owner(lookup_key, lipp_value, owner_id)) {
      return lipp_value;
    }
    return LookupInBuffer(static_cast<size_t>(owner_id), lookup_key);
#endif
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
#if defined(AUTORESEARCH_SCREEN_SAFE)
    constexpr size_t owner_id = 0;
#else
    const size_t owner_id =
        HasSingleGlobalBuffer()
            ? 0
            : static_cast<size_t>(lipp_.locate_buffer_owner(data.key));
#endif
    buffers_[owner_id].insert(data.key, data.value);
    ++buffer_sizes_[owner_id];

    if (ShouldFlushOwner(owner_id)) {
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
#if defined(AUTORESEARCH_SCREEN_SAFE)
    return {"screen_chunked_overlay",
            SearchClass::name() + "-e" + std::to_string(pgm_error) +
                "-global-f" + std::to_string(ScreenFlushThreshold())};
#else
    return {"owner_buffered_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-s" +
                std::to_string(EffectiveOwnerMaxSize()) + "-f" +
                std::to_string(local_flush_threshold)};
#endif
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

  bool HasSingleGlobalBuffer() const {
    return buffers_.size() == 1;
  }

  bool ShouldUseSingleDeferredOverlay(size_t data_size) const {
    if (data_size == 0) {
      return true;
    }

    const size_t effective_owner_span = EffectiveOwnerMaxSize();
    return effective_owner_span >= data_size / 2 ||
           local_flush_threshold >= data_size / 4;
  }

  size_t ScreenFlushThreshold() const {
    // Keep the screen canary bounded so it emits a RESULT line instead of
    // letting a single deferred DynamicPGM grow until verification dominates.
    return 4096;
  }

  bool ShouldProbeScreenBufferFirst() const {
    if (buffer_sizes_.empty() || buffer_sizes_[0] == 0) {
      return false;
    }
    if (PreferBufferFirst()) {
      return true;
    }
    return buffer_sizes_[0] >= ScreenFlushThreshold() / 4;
  }

  bool ShouldProbeSingleBufferFirst() const {
    if (!HasSingleGlobalBuffer() || buffer_sizes_[0] == 0) {
      return false;
    }
    if (PreferBufferFirst()) {
      return true;
    }
    const size_t threshold = EffectiveFlushThreshold(0);
    return buffer_sizes_[0] >= std::max<size_t>(8, threshold / 4);
  }

  size_t EffectiveOwnerMaxSize() const {
    // Owners that span only a couple of local flush batches churn on both
    // lookup misses and flushes. Keep each owner large enough to absorb at
    // least several threshold-sized bursts before we split it off.
    const size_t min_owner_span =
        std::max<size_t>(256, local_flush_threshold * 16);
    return std::max<size_t>(owner_max_size, min_owner_span);
  }

  bool ShouldProbeBufferFirst(size_t owner_id) const {
    if (owner_id >= buffer_sizes_.size()) {
      return false;
    }

    const size_t buffered = buffer_sizes_[owner_id];
    if (buffered == 0) {
      return false;
    }

    const size_t threshold = EffectiveFlushThreshold(owner_id);
    const size_t region =
        owner_id < owner_region_sizes_.size() ? owner_region_sizes_[owner_id] : 0;

    // Buffer-first only pays off once the owner has accumulated enough pending
    // inserts that a hit is plausible; otherwise avoid the extra DynamicPGM miss.
    const size_t occupancy_trigger = std::max<size_t>(8, threshold / 4);
    if (buffered >= occupancy_trigger) {
      return true;
    }

    return region > 0 && buffered * 8 >= region;
  }

  size_t EffectiveFlushThreshold(size_t owner_id) const {
    const size_t region =
        owner_id < owner_region_sizes_.size() ? owner_region_sizes_[owner_id] : 0;
    size_t threshold = local_flush_threshold;
    size_t min_threshold = std::max<size_t>(local_flush_threshold, 64);
    size_t max_threshold = std::max<size_t>(min_threshold, 128);

    if (region > 0) {
      // Tiny owner regions were timing out from disruption-heavy flushes.
      // Keep a stronger absolute floor so we batch several inserts before
      // mutating LIPP, then scale up mildly for larger owners.
      const size_t region_floor = std::max<size_t>(64, region / 8);
      const size_t region_cap = std::max<size_t>(region_floor, region / 2);
      min_threshold = std::max(min_threshold, region_floor);
      max_threshold =
          std::max(min_threshold,
                   std::min<size_t>(std::max<size_t>(local_flush_threshold, 256),
                                    region_cap));
      threshold = std::min(max_threshold,
                           std::max(min_threshold, local_flush_threshold));
    }

    if (insert_count_ > lookup_count_ * 4) {
      return std::min(max_threshold, std::max<size_t>(threshold, threshold * 2));
    }

    if (lookup_count_ > insert_count_ * 4) {
      return threshold;
    }

    return threshold;
  }

  bool ShouldFlushOwner(size_t owner_id) const {
    if (owner_id >= buffer_sizes_.size()) {
      return false;
    }

    const size_t buffered = buffer_sizes_[owner_id];
#if defined(AUTORESEARCH_SCREEN_SAFE)
    return buffered >= ScreenFlushThreshold();
#else
    const size_t threshold = EffectiveFlushThreshold(owner_id);
    if (buffered < threshold) {
      return false;
    }

    if (!PreferBufferFirst()) {
      return true;
    }

    // In insert-heavy phases, defer the first flush a bit longer so a hot owner
    // can absorb a larger burst before we pay the LIPP mutation cost.
    size_t deferred_threshold =
        threshold + std::max<size_t>(32, threshold / 2);
    const size_t region =
        owner_id < owner_region_sizes_.size() ? owner_region_sizes_[owner_id] : 0;
    if (region > 0) {
      deferred_threshold =
          std::min(deferred_threshold, std::max(threshold, region));
    }
    return buffered >= deferred_threshold;
#endif
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
#if defined(AUTORESEARCH_SCREEN_SAFE)
    owner_region_sizes_[owner_id] = 0;
#else
    owner_region_sizes_[owner_id] =
        static_cast<size_t>(lipp_.buffer_owner_size(static_cast<int>(owner_id)));
#endif
  }

  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable std::vector<DynamicPGMType> buffers_;
  mutable std::vector<size_t> buffer_sizes_;
  mutable std::vector<size_t> owner_region_sizes_;

  mutable size_t insert_count_{0};
  mutable size_t lookup_count_{0};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
