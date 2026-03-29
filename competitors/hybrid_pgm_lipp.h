#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "../util.h"
#include "base.h"
#include "lipp.h"
#include "dynamic_pgm_index.h"

template <class KeyType, class SearchClass, size_t pgm_error, size_t flush_threshold_items>
class HybridPGMLipp : public Competitor<KeyType, SearchClass> {
 public:
  HybridPGMLipp(const std::vector<int>& params) : buffer_threshold_override_(0) {
    if (!params.empty() && params[0] > 0) {
      buffer_threshold_override_ = static_cast<size_t>(params[0]);
    }
  }

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    buffer_item_count_ = 0;
    flush_count_ = 0;
    buffer_threshold_ = ResolveBufferThreshold();
    std::vector<KeyValue<KeyType>> sorted_data = data;
    if (!std::is_sorted(sorted_data.begin(), sorted_data.end(),
                        [](const auto& lhs, const auto& rhs) { return lhs.key < rhs.key; })) {
      std::sort(sorted_data.begin(), sorted_data.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.key < rhs.key; });
    }

    const uint64_t build_time_ns = lipp_.Build(sorted_data, num_threads);
    ResetBuffer();
    return build_time_ns;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    const size_t buffer_result = buffer_.EqualityLookup(lookup_key, thread_id);
    if (buffer_result != util::OVERFLOW) {
      return buffer_result;
    }
    return lipp_.EqualityLookup(lookup_key, thread_id);
  }

  uint64_t RangeQuery(const KeyType& lower_key, const KeyType& upper_key, uint32_t thread_id) const {
    return lipp_.RangeQuery(lower_key, upper_key, thread_id) +
           buffer_.RangeQuery(lower_key, upper_key, thread_id);
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    buffer_.Insert(data, thread_id);
    ++buffer_item_count_;
    if (buffer_item_count_ >= buffer_threshold_) {
      FlushBufferToLipp();
    }
  }

  std::string name() const { return "HybridPGMLIPP"; }

  std::size_t size() const { return lipp_.size() + buffer_.size(); }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    (void)ops_filename;
    return unique && !multithread;
  }

  std::vector<std::string> variants() const {
    return {
        SearchClass::name(),
        "eps" + std::to_string(pgm_error) + "_flush" + std::to_string(buffer_threshold_),
    };
  }

 private:
  using BufferIndex =
      DynamicPGMIndex<KeyType, uint64_t, SearchClass, PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  size_t ResolveBufferThreshold() const {
    if (buffer_threshold_override_ > 0) {
      return buffer_threshold_override_;
    }
    return std::max<size_t>(1, flush_threshold_items);
  }

  void ResetBuffer() {
    buffer_ = BufferIndex();
    buffer_item_count_ = 0;
  }

  void FlushBufferToLipp() {
    if (buffer_item_count_ == 0 || buffer_.empty()) {
      ResetBuffer();
      return;
    }

    for (auto it = buffer_.begin(); it != buffer_.end(); ++it) {
      lipp_.Insert({it->key(), it->value()}, 0);
    }
    ++flush_count_;
    ResetBuffer();
    buffer_threshold_ = ResolveBufferThreshold();
  }

  Lipp<KeyType> lipp_;
  BufferIndex buffer_;
  size_t buffer_item_count_ = 0;
  size_t flush_count_ = 0;
  size_t buffer_threshold_ = 1;
  size_t buffer_threshold_override_;
};
