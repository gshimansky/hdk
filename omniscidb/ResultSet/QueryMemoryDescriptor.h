/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file    QueryMemoryDescriptor.h
 * @author  Alex Suhan <alex@mapd.com>
 * @brief   Descriptor for the result set buffer layout.
 *
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 */

#pragma once

#include "ColSlotContext.h"
#include "CountDistinctDescriptor.h"
#include "ResultType.h"

#include <boost/optional.hpp>
#include "Logger/Logger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "Shared/Config.h"
#include "Shared/DeviceType.h"
#include "Shared/SqlTypesLayout.h"
#include "Shared/TargetInfo.h"

namespace Data_Namespace {
class DataMgr;
}

class BufferProvider;
class QueryExecutionContext;
class RowSetMemoryOwner;
struct InputTableInfo;
class TResultSetBufferDescriptor;
struct ColRangeInfo;

class StreamingTopNOOM : public std::runtime_error {
 public:
  StreamingTopNOOM(const size_t heap_size_bytes)
      : std::runtime_error("Unable to use streaming top N due to required heap size of " +
                           std::to_string(heap_size_bytes) +
                           " bytes exceeding maximum slab size.") {}
};

struct KeylessInfo {
  const bool keyless;
  const int32_t target_index;
};

class QueryMemoryDescriptor {
 public:
  QueryMemoryDescriptor();

  // constructor for init call
  QueryMemoryDescriptor(Data_Namespace::DataMgr* data_mgr,
                        ConfigPtr config,
                        const std::vector<InputTableInfo>& query_infos,
                        const bool approx_quantile,
                        const bool allow_multifrag,
                        const bool keyless_hash,
                        const bool interleaved_bins_on_gpu,
                        const int32_t idx_target_as_key,
                        const ColRangeInfo& col_range_info,
                        const ColSlotContext& col_slot_context,
                        const std::vector<int8_t>& group_col_widths,
                        const int8_t group_col_compact_width,
                        const std::vector<int64_t>& target_groupby_indices,
                        const size_t entry_count,
                        const CountDistinctDescriptors count_distinct_descriptors,
                        const bool sort_on_gpu_hint,
                        const bool output_columnar,
                        const bool must_use_baseline_sort,
                        const bool use_streaming_top_n);

  QueryMemoryDescriptor(Data_Namespace::DataMgr* data_mgr,
                        ConfigPtr config,
                        const size_t entry_count,
                        const QueryDescriptionType query_desc_type,
                        const bool is_table_function);

  QueryMemoryDescriptor(const QueryDescriptionType query_desc_type,
                        const int64_t min_val,
                        const int64_t max_val,
                        const bool has_nulls,
                        const std::vector<int8_t>& group_col_widths);

  bool operator==(const QueryMemoryDescriptor& other) const;

  static bool many_entries(const int64_t max_val,
                           const int64_t min_val,
                           const int64_t bucket) {
    return max_val - min_val > 10000 * std::max(bucket, int64_t(1));
  }

  static bool countDescriptorsLogicallyEmpty(
      const CountDistinctDescriptors& count_distinct_descriptors) {
    return std::all_of(count_distinct_descriptors.begin(),
                       count_distinct_descriptors.end(),
                       [](const CountDistinctDescriptor& desc) {
                         return desc.impl_type_ == CountDistinctImplType::Invalid;
                       });
  }

  bool countDistinctDescriptorsLogicallyEmpty() const {
    return countDescriptorsLogicallyEmpty(count_distinct_descriptors_);
  }

  // Getters and Setters
  QueryDescriptionType getQueryDescriptionType() const { return query_desc_type_; }
  void setQueryDescriptionType(const QueryDescriptionType val) { query_desc_type_ = val; }
  bool isSingleColumnGroupByWithPerfectHash() const {
    return getQueryDescriptionType() == QueryDescriptionType::GroupByPerfectHash &&
           getGroupbyColCount() == 1;
  }

  bool hasKeylessHash() const { return keyless_hash_; }
  void setHasKeylessHash(const bool val) { keyless_hash_ = val; }

  bool hasInterleavedBinsOnGpu() const { return interleaved_bins_on_gpu_; }
  void setHasInterleavedBinsOnGpu(const bool val) { interleaved_bins_on_gpu_ = val; }

  int32_t getTargetIdxForKey() const { return idx_target_as_key_; }
  void setTargetIdxForKey(const int32_t val) { idx_target_as_key_ = val; }

  int8_t groupColWidth(const size_t key_idx) const {
    CHECK_LT(key_idx, group_col_widths_.size());
    return group_col_widths_[key_idx];
  }
  size_t getPrependedGroupColOffInBytes(const size_t group_idx) const;
  size_t getPrependedGroupBufferSizeInBytes() const;

  const auto groupColWidthsBegin() const { return group_col_widths_.begin(); }
  const auto groupColWidthsEnd() const { return group_col_widths_.end(); }
  void clearGroupColWidths() { group_col_widths_.clear(); }

  bool isGroupBy() const { return !group_col_widths_.empty(); }

  void setGroupColCompactWidth(const int8_t val) { group_col_compact_width_ = val; }

  size_t getColCount() const;
  size_t getSlotCount() const;

  const int8_t getPaddedSlotWidthBytes(const size_t slot_idx) const;
  const int8_t getLogicalSlotWidthBytes(const size_t slot_idx) const;

  void setPaddedSlotWidthBytes(const size_t slot_idx, const int8_t bytes);

  const size_t getSlotIndexForSingleSlotCol(const size_t col_idx) const;

  size_t getPaddedColWidthForRange(const size_t offset, const size_t range) const {
    size_t ret = 0;
    for (size_t i = offset; i < offset + range; i++) {
      ret += static_cast<size_t>(getPaddedSlotWidthBytes(i));
    }
    return ret;
  }

  void useConsistentSlotWidthSize(const int8_t slot_width_size);
  size_t getRowWidth() const;

  int8_t updateActualMinByteWidth(const int8_t actual_min_byte_width) const;

  void addColSlotInfo(const std::vector<std::tuple<int8_t, int8_t>>& slots_for_col);

  void clearSlotInfo();

  void alignPaddedSlots();

  int64_t getTargetGroupbyIndex(const size_t target_idx) const {
    CHECK_LT(target_idx, target_groupby_indices_.size());
    return target_groupby_indices_[target_idx];
  }

  void setAllTargetGroupbyIndices(std::vector<int64_t> group_by_indices) {
    target_groupby_indices_ = group_by_indices;
  }

  size_t targetGroupbyIndicesSize() const { return target_groupby_indices_.size(); }
  size_t targetGroupbyNegativeIndicesSize() const {
    return std::count_if(
        target_groupby_indices_.begin(),
        target_groupby_indices_.end(),
        [](const int64_t& target_group_by_index) { return target_group_by_index < 0; });
  }
  void clearTargetGroupbyIndices() { target_groupby_indices_.clear(); }

  size_t getEntryCount() const { return entry_count_; }
  void setEntryCount(const size_t val) { entry_count_ = val; }

  int64_t getMinVal() const { return min_val_; }
  int64_t getMaxVal() const { return max_val_; }
  int64_t getBucket() const { return bucket_; }

  bool hasNulls() const { return has_nulls_; }

  const CountDistinctDescriptor& getCountDistinctDescriptor(const size_t idx) const {
    CHECK_LT(idx, count_distinct_descriptors_.size());
    return count_distinct_descriptors_[idx];
  }
  size_t getCountDistinctDescriptorsSize() const {
    return count_distinct_descriptors_.size();
  }

  bool sortOnGpu() const { return sort_on_gpu_; }

  bool canOutputColumnar() const;
  bool didOutputColumnar() const { return output_columnar_; }
  void setOutputColumnar(const bool val);

  bool useStreamingTopN() const { return use_streaming_top_n_; }

  bool isLogicalSizedColumnsAllowed() const;

  bool mustUseBaselineSort() const { return must_use_baseline_sort_; }

  // Getters derived from state
  size_t getGroupbyColCount() const { return group_col_widths_.size(); }
  size_t getKeyCount() const { return keyless_hash_ ? 0 : getGroupbyColCount(); }
  size_t getBufferColSlotCount() const;

  size_t getBufferSizeBytes(const size_t max_rows,
                            const unsigned thread_count,
                            const ExecutorDeviceType device_type) const;
  size_t getBufferSizeBytes(const ExecutorDeviceType device_type) const;
  size_t getBufferSizeBytes(const ExecutorDeviceType device_type,
                            const size_t override_entry_count) const;

  const ColSlotContext& getColSlotContext() const { return col_slot_context_; }

  // TODO(alex): remove
  bool usesGetGroupValueFast() const;

  bool blocksShareMemory() const;
  bool threadsShareMemory() const;

  bool lazyInitGroups(const ExecutorDeviceType) const;

  bool interleavedBins(const ExecutorDeviceType) const;

  size_t getColOffInBytes(const size_t col_idx) const;
  size_t getColOffInBytesInNextBin(const size_t col_idx) const;
  size_t getNextColOffInBytes(const int8_t* col_ptr,
                              const size_t bin,
                              const size_t col_idx) const;

  // returns the ptr offset of the next column, 64-bit aligned
  size_t getNextColOffInBytesRowOnly(const int8_t* col_ptr, const size_t col_idx) const;
  // returns the ptr offset of the current column, 64-bit aligned
  size_t getColOnlyOffInBytes(const size_t col_idx) const;
  size_t getRowSize() const;
  size_t getColsSize() const;
  size_t getWarpCount() const;

  size_t getCompactByteWidth() const;

  inline size_t getEffectiveKeyWidth() const {
    return group_col_compact_width_ ? group_col_compact_width_ : sizeof(int64_t);
  }

  bool isWarpSyncRequired(const ExecutorDeviceType) const;

  std::string queryDescTypeToString() const;
  std::string toString() const;

  std::string reductionKey() const;

  bool hasVarlenOutput() const { return col_slot_context_.hasVarlenOutput(); }

  // returns a value if the buffer can be a fixed size; otherwise, we will need to use the
  // bump allocator
  std::optional<size_t> varlenOutputBufferElemSize() const;

  // returns the number of bytes needed for all slots preceeding slot_idx. Used to compute
  // the offset into the varlen buffer for each projected target in a given row.
  size_t varlenOutputRowSizeToSlot(const size_t slot_idx) const;

  bool slotIsVarlenOutput(const size_t slot_idx) const {
    return col_slot_context_.slotIsVarlen(slot_idx);
  }

  Data_Namespace::DataMgr* getDataMgr() const;
  BufferProvider* getBufferProvider() const;

 protected:
  void resetGroupColWidths(const std::vector<int8_t>& new_group_col_widths) {
    group_col_widths_ = new_group_col_widths;
  }

  int8_t warpSize() const;
  unsigned gridSize() const;
  unsigned blockSize() const;

 private:
  Data_Namespace::DataMgr* data_mgr_;
  ConfigPtr config_;
  QueryDescriptionType query_desc_type_;
  bool keyless_hash_;
  bool interleaved_bins_on_gpu_;
  int32_t idx_target_as_key_;  // If keyless_hash_ enabled, then represents what target
                               // expression should be used to identify the key (e.g., in
                               // locating empty entries). Currently only valid with
                               // keyless_hash_ and single-column GroupByPerfectHash
  std::vector<int8_t> group_col_widths_;
  int8_t group_col_compact_width_;  // compact width for all group
                                    // cols if able to be consistent
                                    // otherwise 0
  std::vector<int64_t> target_groupby_indices_;
  size_t entry_count_;  // the number of entries in the main buffer
  int64_t min_val_;     // meaningful for OneColKnownRange,
                        // MultiColPerfectHash only
  int64_t max_val_;
  int64_t bucket_;
  bool has_nulls_;
  CountDistinctDescriptors count_distinct_descriptors_;
  bool sort_on_gpu_;
  bool output_columnar_;
  bool must_use_baseline_sort_;
  bool is_table_function_;
  bool use_streaming_top_n_;

  ColSlotContext col_slot_context_;

  size_t getTotalBytesOfColumnarBuffers() const;
  size_t getTotalBytesOfColumnarBuffers(const size_t num_entries_per_column) const;
  size_t getTotalBytesOfColumnarProjections(const size_t projection_count) const;

  friend class ResultSet;
  friend class QueryExecutionContext;
};

inline void set_notnull(TargetInfo& target, const bool not_null) {
  target.skip_null_val = !not_null;
  auto new_type = get_compact_type(target)->withNullable(!not_null);
  set_compact_type(target, new_type);
}

std::vector<TargetInfo> target_exprs_to_infos(
    const std::vector<const hdk::ir::Expr*>& targets,
    const QueryMemoryDescriptor& query_mem_desc,
    bool bigint_count);
