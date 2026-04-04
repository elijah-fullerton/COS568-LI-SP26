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
      InitializeBloomFilters(1);
      InitializeOwnerTelemetry(1);
#else
      if (ShouldUseSingleDeferredOverlay(data.size())) {
        // When the effective owner span is nearly global, skip owner assignment
        // so the runtime matches the intended sweep point instead of paying
        // startup and routing costs for a near-global split.
        buffers_.assign(1, DynamicPGMType());
        buffer_sizes_.assign(1, 0);
        owner_region_sizes_.assign(1, data.size());
        InitializeBloomFilters(1);
        InitializeOwnerTelemetry(1);
      } else {
        const int owner_count = std::max(
            1, lipp_.assign_buffer_owners(static_cast<int>(EffectiveOwnerMaxSize())));
        buffers_.assign(owner_count, DynamicPGMType());
        buffer_sizes_.assign(owner_count, 0);
        owner_region_sizes_.assign(owner_count, 0);
        InitializeBloomFilters(static_cast<size_t>(owner_count));
        InitializeOwnerTelemetry(static_cast<size_t>(owner_count));
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
    RecordLookupEvent(0);
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
      RecordLookupEvent(0);
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
      RecordLookupEvent(owner_id);
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
      RecordLookupEvent(static_cast<size_t>(owner_id));
      return lipp_value;
    }
    RecordLookupEvent(static_cast<size_t>(owner_id));
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
    RecordInsertEvent(owner_id);
    if (ShouldBypassBufferOnInsert(owner_id)) {
      if (buffer_sizes_[owner_id] != 0) {
        FlushOwner(owner_id);
      }
      lipp_.insert(data.key, data.value);
#if defined(AUTORESEARCH_SCREEN_SAFE)
      owner_region_sizes_[owner_id] += 1;
#else
      if (HasSingleGlobalBuffer()) {
        owner_region_sizes_[owner_id] += 1;
      }
#endif
      return;
    }

    buffers_[owner_id].insert(data.key, data.value);
    ++buffer_sizes_[owner_id];
    EnsureBloomFilterCapacity(owner_id);
    if (ShouldMaintainBloomFilter(owner_id)) {
      AddToBloomFilter(owner_id, data.key);
      bloom_filters_[owner_id].inserted_count = buffer_sizes_[owner_id];
    }

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
                "-global-f" + std::to_string(ScreenFlushThreshold()) + "-bf"};
#else
    return {"owner_buffered_lipp",
            SearchClass::name() + "-e" + std::to_string(pgm_error) + "-s" +
                std::to_string(EffectiveOwnerMaxSize()) + "-f" +
                std::to_string(local_flush_threshold) + "-bf"};
#endif
  }

 private:
  using DynamicPGMType =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  struct BufferBloomFilter {
    std::vector<uint64_t> words;
    size_t bit_count{0};
    size_t inserted_count{0};
  };

  struct OwnerTelemetry {
    uint32_t recent_lookups{0};
    uint32_t recent_inserts{0};
    uint32_t overlay_probes{0};
    uint32_t overlay_hits{0};
    uint32_t bloom_positives{0};
    uint32_t bloom_negatives{0};
  };

  void ResetState() {
    insert_count_ = 0;
    lookup_count_ = 0;
    recent_lookup_events_ = 0;
    recent_insert_events_ = 0;
    buffers_.clear();
    buffer_sizes_.clear();
    owner_region_sizes_.clear();
    bloom_filters_.clear();
    owner_telemetry_.clear();
  }

  void InitializeBloomFilters(size_t owner_count) {
    bloom_filters_.assign(owner_count, BufferBloomFilter());
    for (size_t owner_id = 0; owner_id < owner_count; ++owner_id) {
      ResetBloomFilter(owner_id);
    }
  }

  void InitializeOwnerTelemetry(size_t owner_count) {
    owner_telemetry_.assign(owner_count, OwnerTelemetry());
  }

  size_t RoundUpPowerOfTwo(size_t value) const {
    size_t rounded = 1;
    while (rounded < value && rounded < (std::numeric_limits<size_t>::max() >> 1)) {
      rounded <<= 1;
    }
    return rounded;
  }

  size_t DesiredBloomBitCount(size_t buffered_entries) const {
    const size_t min_bits = 1u << 12;
    return std::max(min_bits,
                    RoundUpPowerOfTwo(std::max<size_t>(1, buffered_entries) * 16));
  }

  uint64_t MixKey(uint64_t value) const {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  uint64_t KeyFingerprint(const KeyType& key) const {
    return MixKey(static_cast<uint64_t>(key));
  }

  void SetBloomBit(BufferBloomFilter& filter, uint64_t hash) const {
    const size_t bit_index = static_cast<size_t>(hash & (filter.bit_count - 1));
    filter.words[bit_index >> 6] |= (uint64_t{1} << (bit_index & 63));
  }

  void AddToBloomFilter(size_t owner_id, const KeyType& key) const {
    if (owner_id >= bloom_filters_.size()) {
      return;
    }
    BufferBloomFilter& filter = bloom_filters_[owner_id];
    const uint64_t fingerprint = KeyFingerprint(key);
    SetBloomBit(filter, fingerprint);
    SetBloomBit(filter, MixKey(fingerprint + 0x9e3779b97f4a7c15ULL));
    SetBloomBit(filter, MixKey(fingerprint + 0xbf58476d1ce4e5b9ULL));
    SetBloomBit(filter, MixKey(fingerprint + 0x94d049bb133111ebULL));
  }

  void ResetBloomFilter(size_t owner_id) const {
    if (owner_id >= bloom_filters_.size()) {
      return;
    }
    BufferBloomFilter& filter = bloom_filters_[owner_id];
    filter.bit_count = DesiredBloomBitCount(1);
    filter.words.assign((filter.bit_count + 63) / 64, 0);
    filter.inserted_count = 0;
  }

  void RebuildBloomFilter(size_t owner_id, size_t buffered_entries) const {
    if (owner_id >= bloom_filters_.size()) {
      return;
    }
    BufferBloomFilter& filter = bloom_filters_[owner_id];
    filter.bit_count = DesiredBloomBitCount(buffered_entries);
    filter.words.assign((filter.bit_count + 63) / 64, 0);
    filter.inserted_count = buffered_entries;
    if (owner_id >= buffers_.size() || buffered_entries == 0) {
      return;
    }
    buffers_[owner_id].for_each([&](const KeyType& key, const uint64_t value) {
      (void)value;
      AddToBloomFilter(owner_id, key);
    });
  }

  void EnsureBloomFilterCapacity(size_t owner_id) const {
    if (owner_id >= bloom_filters_.size() || owner_id >= buffer_sizes_.size()) {
      return;
    }
    const size_t buffered_entries = buffer_sizes_[owner_id];
    BufferBloomFilter& filter = bloom_filters_[owner_id];
    if (!ShouldMaintainBloomFilter(owner_id)) {
      filter.inserted_count = 0;
      return;
    }
    if (filter.bit_count == 0) {
      ResetBloomFilter(owner_id);
    }
    const size_t desired_bits = DesiredBloomBitCount(buffered_entries);
    if (desired_bits > filter.bit_count || filter.inserted_count != buffered_entries) {
      RebuildBloomFilter(owner_id, buffered_entries);
      return;
    }
    filter.inserted_count = buffered_entries;
  }

  bool BloomMayContain(size_t owner_id, const KeyType& key) const {
    if (owner_id >= bloom_filters_.size()) {
      return true;
    }
    EnsureBloomFilterCapacity(owner_id);
    if (!ShouldMaintainBloomFilter(owner_id)) {
      return true;
    }
    const BufferBloomFilter& filter = bloom_filters_[owner_id];
    if (filter.bit_count == 0 || filter.inserted_count == 0) {
      return false;
    }

    const uint64_t fingerprint = KeyFingerprint(key);
    const uint64_t hashes[4] = {
        fingerprint,
        MixKey(fingerprint + 0x9e3779b97f4a7c15ULL),
        MixKey(fingerprint + 0xbf58476d1ce4e5b9ULL),
        MixKey(fingerprint + 0x94d049bb133111ebULL),
    };
    for (uint64_t hash : hashes) {
      const size_t bit_index = static_cast<size_t>(hash & (filter.bit_count - 1));
      if ((filter.words[bit_index >> 6] & (uint64_t{1} << (bit_index & 63))) == 0) {
        return false;
      }
    }
    return true;
  }

  void DecayGlobalCountersIfNeeded() const {
    if (recent_lookup_events_ + recent_insert_events_ < 4096) {
      return;
    }
    recent_lookup_events_ >>= 1;
    recent_insert_events_ >>= 1;
  }

  void DecayOwnerTelemetryIfNeeded(size_t owner_id) const {
    if (owner_id >= owner_telemetry_.size()) {
      return;
    }
    OwnerTelemetry& telemetry = owner_telemetry_[owner_id];
    const uint32_t total = telemetry.recent_lookups + telemetry.recent_inserts +
                           telemetry.overlay_probes + telemetry.overlay_hits +
                           telemetry.bloom_positives + telemetry.bloom_negatives;
    if (total < 4096) {
      return;
    }
    telemetry.recent_lookups >>= 1;
    telemetry.recent_inserts >>= 1;
    telemetry.overlay_probes >>= 1;
    telemetry.overlay_hits >>= 1;
    telemetry.bloom_positives >>= 1;
    telemetry.bloom_negatives >>= 1;
  }

  void RecordLookupEvent(size_t owner_id) const {
    ++recent_lookup_events_;
    DecayGlobalCountersIfNeeded();
    if (owner_id >= owner_telemetry_.size()) {
      return;
    }
    ++owner_telemetry_[owner_id].recent_lookups;
    DecayOwnerTelemetryIfNeeded(owner_id);
  }

  void RecordInsertEvent(size_t owner_id) const {
    ++recent_insert_events_;
    DecayGlobalCountersIfNeeded();
    if (owner_id >= owner_telemetry_.size()) {
      return;
    }
    ++owner_telemetry_[owner_id].recent_inserts;
    DecayOwnerTelemetryIfNeeded(owner_id);
  }

  void RecordOverlayProbe(size_t owner_id, bool bloom_positive) const {
    if (owner_id >= owner_telemetry_.size()) {
      return;
    }
    OwnerTelemetry& telemetry = owner_telemetry_[owner_id];
    ++telemetry.overlay_probes;
    if (bloom_positive) {
      ++telemetry.bloom_positives;
    } else {
      ++telemetry.bloom_negatives;
    }
    DecayOwnerTelemetryIfNeeded(owner_id);
  }

  void RecordOverlayHit(size_t owner_id) const {
    if (owner_id >= owner_telemetry_.size()) {
      return;
    }
    ++owner_telemetry_[owner_id].overlay_hits;
    DecayOwnerTelemetryIfNeeded(owner_id);
  }

  bool IsGlobalLookupHeavy() const {
    return recent_lookup_events_ >= 64 &&
           recent_lookup_events_ > std::max<size_t>(32, recent_insert_events_ * 4);
  }

  bool OwnerPrefersBase(size_t owner_id) const {
    if (owner_id >= owner_telemetry_.size()) {
      return false;
    }
    const OwnerTelemetry& telemetry = owner_telemetry_[owner_id];
    const size_t observed_ops = telemetry.recent_lookups + telemetry.recent_inserts;
    if (observed_ops < 64 || !IsGlobalLookupHeavy()) {
      return false;
    }
    const bool owner_lookup_heavy =
        telemetry.recent_lookups > std::max<uint32_t>(32, telemetry.recent_inserts * 4);
    const bool low_overlay_value =
        telemetry.overlay_probes < 16 || telemetry.overlay_hits * 16 < telemetry.overlay_probes;
    const size_t region =
        owner_id < owner_region_sizes_.size() ? owner_region_sizes_[owner_id] : 0;
    const size_t buffered =
        owner_id < buffer_sizes_.size() ? buffer_sizes_[owner_id] : 0;
    const bool sparse_overlay = region == 0 || buffered * 64 < region;
    return owner_lookup_heavy && low_overlay_value && sparse_overlay;
  }

  bool ShouldMaintainBloomFilter(size_t owner_id) const {
    if (owner_id >= buffer_sizes_.size()) {
      return false;
    }
    const size_t buffered = buffer_sizes_[owner_id];
    if (buffered < 32) {
      return false;
    }
    return !OwnerPrefersBase(owner_id) ||
           (owner_id < owner_telemetry_.size() &&
            owner_telemetry_[owner_id].overlay_probes >= 16);
  }

  bool ShouldBypassBufferOnInsert(size_t owner_id) const {
    if (owner_id >= buffer_sizes_.size()) {
      return false;
    }
    if (!OwnerPrefersBase(owner_id)) {
      return false;
    }
    const size_t buffered = buffer_sizes_[owner_id];
    const size_t region =
        owner_id < owner_region_sizes_.size() ? owner_region_sizes_[owner_id] : 0;
    return buffered == 0 || region == 0 || buffered * 64 < region;
  }

  bool PreferBufferFirst() const {
    if (IsGlobalLookupHeavy()) {
      return false;
    }
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
    // Let the benchmark-selected screen variant choose how aggressively the
    // single deferred overlay should drain.
    return std::max<size_t>(64, local_flush_threshold);
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
    if (OwnerPrefersBase(0)) {
      return false;
    }
    if (PreferBufferFirst()) {
      return true;
    }
    const size_t threshold = EffectiveFlushThreshold(0);
    return buffer_sizes_[0] >= std::max<size_t>(8, threshold / 4);
  }

  size_t EffectiveOwnerMaxSize() const {
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

    if (OwnerPrefersBase(owner_id)) {
      const size_t lookup_heavy_trigger = std::max<size_t>(32, threshold);
      return region > 0 && buffered >= lookup_heavy_trigger &&
             buffered * 16 >= region;
    }

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

    if (OwnerPrefersBase(owner_id)) {
      const size_t lookup_heavy_cap = 64;
      threshold = std::min(threshold, lookup_heavy_cap);
      if (region > 0) {
        const size_t region_cap = std::max<size_t>(8, region / 128);
        threshold = std::min(threshold, region_cap);
      }
      return std::max<size_t>(8, threshold);
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
    const size_t region =
        owner_id < owner_region_sizes_.size() ? owner_region_sizes_[owner_id] : 0;
    if (region > 0) {
      const size_t occupancy_threshold =
          OwnerPrefersBase(owner_id) ? std::max<size_t>(8, region / 128)
                                     : std::max<size_t>(64, region / 16);
      if (buffered >= occupancy_threshold) {
        return true;
      }
    }
    if (buffered < threshold) {
      return false;
    }

    if (!PreferBufferFirst()) {
      return true;
    }

    size_t deferred_threshold =
        threshold + std::max<size_t>(32, threshold / 2);
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
    const bool bloom_positive = BloomMayContain(owner_id, lookup_key);
    RecordOverlayProbe(owner_id, bloom_positive);
    if (!bloom_positive) {
      return util::OVERFLOW;
    }
    auto it = buffers_[owner_id].find(lookup_key);
    if (it != buffers_[owner_id].end()) {
      RecordOverlayHit(owner_id);
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
    ResetBloomFilter(owner_id);
#if defined(AUTORESEARCH_SCREEN_SAFE)
    owner_region_sizes_[owner_id] = 0;
#else
    if (owner_id < owner_region_sizes_.size() && owner_region_sizes_[owner_id] == 0) {
      owner_region_sizes_[owner_id] = 1;
    }
#endif
  }

  mutable LIPP<KeyType, uint64_t> lipp_;
  mutable std::vector<DynamicPGMType> buffers_;
  mutable std::vector<size_t> buffer_sizes_;
  mutable std::vector<size_t> owner_region_sizes_;
  mutable std::vector<BufferBloomFilter> bloom_filters_;
  mutable std::vector<OwnerTelemetry> owner_telemetry_;

  mutable size_t insert_count_{0};
  mutable size_t lookup_count_{0};
  mutable size_t recent_lookup_events_{0};
  mutable size_t recent_insert_events_{0};
};

#endif  // TLI_HYBRID_PGM_LIPP_H
