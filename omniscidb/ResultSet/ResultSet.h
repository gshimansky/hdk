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
 * @file    ResultSet.h
 * @author  Alex Suhan <alex@mapd.com>
 * @brief   Basic constructors and methods of the row set interface.
 *
 * Copyright (c) 2014 MapD Technologies, Inc.  All rights reserved.
 */

#ifndef QUERYENGINE_RESULTSET_H
#define QUERYENGINE_RESULTSET_H

#include "BufferProvider/BufferProvider.h"
#include "DataMgr/Chunk/Chunk.h"
#include "IR/CardinalityEstimator.h"
#include "ResultSet/ResultSetBufferAccessors.h"
#include "ResultSet/ResultSetStorage.h"
#include "ResultSet/TargetValue.h"
#include "Shared/quantile.h"

#include <boost/optional/optional_io.hpp>

#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <utility>

/*
 * Stores the underlying buffer and the meta-data for a result set. The buffer
 * format reflects the main requirements for result sets. Not all queries
 * specify a GROUP BY clause, but since it's the most important and challenging
 * case we'll focus on it. Note that the meta-data is stored separately from
 * the buffer and it's not transferred to GPU.
 *
 * 1. It has to be efficient for reduction of partial GROUP BY query results
 *    from multiple devices / cores, the cardinalities can be high. Reduction
 *    currently happens on the host.
 * 2. No conversions should be needed when buffers are transferred from GPU to
 *    host for reduction. This implies the buffer needs to be "flat", with no
 *    pointers to chase since they have no meaning in a different address space.
 * 3. Must be size-efficient.
 *
 * There are several variations of the format of a result set buffer, but the
 * most common is a sequence of entries which represent a row in the result or
 * an empty slot. One entry looks as follows:
 *
 * +-+-+-+-+-+-+-+-+-+-+-+--?--+-+-+-+-+-+-+-+-+-+-+-+-+
 * |key_0| ... |key_N-1| padding |value_0|...|value_N-1|
 * +-+-+-+-+-+-+-+-+-+-+-+--?--+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * (key_0 ... key_N-1) is a multiple component key, unique within the buffer.
 * It stores the tuple specified by the GROUP BY clause. All components have
 * the same width, 4 or 8 bytes. For the 4-byte components, 4-byte padding is
 * added if the number of components is odd. Not all entries in the buffer are
 * valid; an empty entry contains EMPTY_KEY_{64, 32} for 8-byte / 4-byte width,
 * respectively. An empty entry is ignored by subsequent operations on the
 * result set (reduction, iteration, sort etc).
 *
 * value_0 through value_N-1 are 8-byte fields which hold the columns of the
 * result, like aggregates and projected expressions. They're reduced between
 * multiple partial results for identical (key_0 ... key_N-1) tuples.
 *
 * The order of entries is decided by the type of hash used, which depends on
 * the range of the keys. For small enough ranges, a perfect hash is used. When
 * a perfect hash isn't feasible, open addressing (using MurmurHash) with linear
 * probing is used instead, with a 50% fill rate.
 */

struct ReductionCode;

namespace hdk::ir {

class Expr;
class Estimator;
struct OrderEntry;

}  // namespace hdk::ir

class RowSetMemoryOwner;
class StringDictionaryProxy;
class ResultSet;

class ResultSetRowIterator {
 public:
  using value_type = std::vector<TargetValue>;
  using difference_type = std::ptrdiff_t;
  using pointer = std::vector<TargetValue>*;
  using reference = std::vector<TargetValue>&;
  using iterator_category = std::input_iterator_tag;

  bool operator==(const ResultSetRowIterator& other) const {
    return result_set_ == other.result_set_ &&
           crt_row_buff_idx_ == other.crt_row_buff_idx_;
  }
  bool operator!=(const ResultSetRowIterator& other) const { return !(*this == other); }

  inline value_type operator*() const;
  inline ResultSetRowIterator& operator++(void);
  ResultSetRowIterator operator++(int) {
    ResultSetRowIterator iter(*this);
    ++(*this);
    return iter;
  }

  size_t getCurrentRowBufferIndex() const {
    if (crt_row_buff_idx_ == 0) {
      throw std::runtime_error("current row buffer iteration index is undefined");
    }
    return crt_row_buff_idx_ - 1;
  }

 private:
  const ResultSet* result_set_;
  size_t crt_row_buff_idx_;
  size_t global_entry_idx_;
  bool global_entry_idx_valid_;
  size_t fetched_so_far_;
  bool translate_strings_;
  bool decimal_to_double_;

  ResultSetRowIterator(const ResultSet* rs,
                       bool translate_strings,
                       bool decimal_to_double)
      : result_set_(rs)
      , crt_row_buff_idx_(0)
      , global_entry_idx_(0)
      , global_entry_idx_valid_(false)
      , fetched_so_far_(0)
      , translate_strings_(translate_strings)
      , decimal_to_double_(decimal_to_double){};

  ResultSetRowIterator(const ResultSet* rs) : ResultSetRowIterator(rs, false, false){};

  friend class ResultSet;
};

class TSerializedRows;
class ResultSetBuilder;

using AppendedStorage = std::vector<std::shared_ptr<ResultSetStorage>>;
using PermutationIdx = uint32_t;
using Permutation = std::vector<PermutationIdx>;
using PermutationView = VectorView<PermutationIdx>;
using ResultSetPtr = std::shared_ptr<ResultSet>;

class ResultSet {
 public:
  friend ResultSetBuilder;
  // Can use derivatives of the builder class to construct a ResultSet

  ResultSet(const std::vector<TargetInfo>& targets,
            const ExecutorDeviceType device_type,
            const QueryMemoryDescriptor& query_mem_desc,
            const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
            Data_Namespace::DataMgr* data_mgr,
            const unsigned block_size,
            const unsigned grid_size);

  ResultSet(const std::vector<TargetInfo>& targets,
            const std::vector<ColumnLazyFetchInfo>& lazy_fetch_info,
            const std::vector<std::vector<const int8_t*>>& col_buffers,
            const std::vector<std::vector<int64_t>>& frag_offsets,
            const std::vector<int64_t>& consistent_frag_sizes,
            const ExecutorDeviceType device_type,
            const int device_id,
            const QueryMemoryDescriptor& query_mem_desc,
            const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
            Data_Namespace::DataMgr* data_mgr,
            const unsigned block_size,
            const unsigned grid_size);

  ResultSet(const std::shared_ptr<const hdk::ir::Estimator>,
            const ExecutorDeviceType device_type,
            const int device_id,
            Data_Namespace::DataMgr* data_mgr);

  ResultSet(const std::string& explanation);

  ResultSet(int64_t queue_time_ms,
            const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner);

  ~ResultSet();

  std::string toString() const {
    return typeName(this) + "(targets=" + ::toString(targets_) +
           ", query_mem_desc=" + ::toString(query_mem_desc_) + ")";
  }

  std::string getStrScalarVal(const ScalarTargetValue& current_scalar,
                              const hdk::ir::Type* col_type) const;
  std::string contentToString(bool header = true) const;
  std::string summaryToString() const;

  inline ResultSetRowIterator rowIterator(size_t from_logical_index,
                                          bool translate_strings,
                                          bool decimal_to_double) const {
    ResultSetRowIterator rowIterator(this, translate_strings, decimal_to_double);

    // move to first logical position
    ++rowIterator;

    for (size_t index = 0; index < from_logical_index; index++) {
      ++rowIterator;
    }

    return rowIterator;
  }

  inline ResultSetRowIterator rowIterator(bool translate_strings,
                                          bool decimal_to_double) const {
    return rowIterator(0, translate_strings, decimal_to_double);
  }

  ExecutorDeviceType getDeviceType() const;

  const ResultSetStorage* allocateStorage() const;

  const ResultSetStorage* allocateStorage(
      int8_t*,
      const std::vector<int64_t>&,
      std::shared_ptr<VarlenOutputInfo> = nullptr) const;

  const ResultSetStorage* allocateStorage(const std::vector<int64_t>&) const;

  void updateStorageEntryCount(const size_t new_entry_count) {
    CHECK(query_mem_desc_.getQueryDescriptionType() == QueryDescriptionType::Projection);
    query_mem_desc_.setEntryCount(new_entry_count);
    CHECK(storage_);
    storage_->updateEntryCount(new_entry_count);
  }

  std::vector<TargetValue> getNextRow(const bool translate_strings,
                                      const bool decimal_to_double) const;

  size_t getCurrentRowBufferIndex() const;

  std::vector<TargetValue> getRowAt(const size_t index) const;

  // TODO: among multiple rowAt versions only the following two accept actual row
  // index. Other versions accept entry index and should probably be renamed and/or
  // made private.
  std::vector<TargetValue> getRowAt(const size_t row_idx,
                                    const bool translate_strings,
                                    const bool decimal_to_double) const;
  TargetValue getRowAt(const size_t row_idx,
                       const size_t col_idx,
                       const bool translate_strings,
                       const bool decimal_to_double) const;

  // Specialized random access getter for result sets with a single column to
  // avoid the overhead of building a std::vector<TargetValue> result with only
  // one element. Only used by RelAlgTranslator::getInIntegerSetExpr currently.
  OneIntegerColumnRow getOneColRow(const size_t index) const;

  std::vector<TargetValue> getRowAtNoTranslations(
      const size_t index,
      const std::vector<bool>& targets_to_skip = {}) const;

  bool isRowAtEmpty(const size_t index) const;

  void keepFirstN(const size_t n);

  void dropFirstN(const size_t n);

  void append(ResultSet& that);

  const ResultSetStorage* getStorage(size_t idx = 0) const;
  size_t getStorageCount() const;

  size_t colCount() const;

  const hdk::ir::Type* colType(const size_t col_idx) const;

  void setColNames(std::vector<std::string> fields);
  bool hasColNames() const;
  std::string colName(size_t col_idx) const;

  /**
   * @brief Returns the number of valid entries in the result set (i.e that will
   * be returned from the SQL query or inputted into the next query step)
   *
   * Note that this can be less than or equal to the value returned by
   * ResultSet::getEntries(), whether due to a SQL LIMIT/OFFSET applied or because
   * the result set representation is inherently sparse (i.e. baseline hash group by).
   *
   * Internally this function references/sets a cached value (`cached_row_count_`)
   * so that the cost of computing the result is only paid once per result set.
   *
   * If the actual row count is not cached and needs to be computed, in some cases
   * that can be O(1) (i.e. if limits and offsets are present, or for the output
   * of a table function). For projections, we use a binary search, so it is
   * O(log n), otherwise it is O(n) (with n being ResultSet::entryCount()),
   * which will be run in parallel if the entry count >= the default of 20000
   * or if `force_parallel` is set to true
   *
   * Note that we currently do not invalidate the cache if the result set is changed
   * (i.e appended to), so this function should only be called after the result
   * set is finalized.
   *
   * @param force_parallel Forces the row count to be computed in parallel if
   * the row count cannot be otherwise be computed from metadata or via a binary
   * search (otherwise parallel search is automatically used for result sets
   * with `entryCount() >= 20000`)
   *
   */

  size_t rowCount(const bool force_parallel = false) const;

  void invalidateCachedRowCount() const;

  void setCachedRowCount(const size_t row_count) const;

  /**
   * @brief Returns a boolean signifying whether there are valid entries
   * in the result set.
   *
   * Note a result set can be logically empty even if the value returned by
   * `ResultSet::entryCount()` is > 0, whether due to a SQL LIMIT/OFFSET applied or
   * because the result set representation is inherently sparse (i.e. baseline hash group
   * by).
   *
   * Internally this function is just implemented as `ResultSet::rowCount() == 0`, which
   * caches it's value so the row count will only be computed once per finalized result
   * set.
   *
   */

  bool isEmpty() const;

  /**
   * @brief Returns the number of entries the result set is allocated to hold.
   *
   * Note that this can be greater than or equal to the actual number of valid rows
   * in the result set, whether due to a SQL LIMIT/OFFSET applied or because
   * the result set representation is inherently sparse (i.e. baseline hash group by)
   *
   * For getting the number of valid rows in the result set (inclusive
   * of any applied LIMIT and/or OFFSET), use `ResultSet::rowCount().` Or
   * to just test if there are any valid rows, use `ResultSet::entryCount()`,
   * as a return value from `entryCount()` greater than 0 does not neccesarily
   * mean the result set is empty.
   *
   */

  size_t entryCount() const;

  size_t getBufferSizeBytes(const ExecutorDeviceType device_type) const;

  bool definitelyHasNoRows() const;

  const QueryMemoryDescriptor& getQueryMemDesc() const;

  const std::vector<TargetInfo>& getTargetInfos() const;

  const std::vector<int64_t>& getTargetInitVals() const;

  int8_t* getDeviceEstimatorBuffer() const;

  int8_t* getHostEstimatorBuffer() const;

  void syncEstimatorBuffer() const;

  size_t getNDVEstimator() const;

  struct QueryExecutionTimings {
    // all in ms
    int64_t executor_queue_time{0};
    int64_t compilation_queue_time{0};
    int64_t kernel_queue_time{0};
  };

  void setQueueTime(const int64_t queue_time);
  void setKernelQueueTime(const int64_t kernel_queue_time);
  void addCompilationQueueTime(const int64_t compilation_queue_time);

  int64_t getQueueTime() const;

  void moveToBegin() const;

  bool isTruncated() const;

  bool isExplain() const;

  void setValidationOnlyRes();
  bool isValidationOnlyRes() const;

  std::string getExplanation() const {
    if (just_explain_) {
      return explanation_;
    }
    return {};
  }

  int getDeviceId() const;

  void setOuterTableId(int id) { outer_table_id_ = id; }
  int getOuterTableId() const { return outer_table_id_; }

  // Called from the executor because in the new ResultSet we assume the 'padded' field
  // in SlotSize already contains the padding, whereas in the executor it's computed.
  // Once the buffer initialization moves to ResultSet we can remove this method.
  static QueryMemoryDescriptor fixupQueryMemoryDescriptor(const QueryMemoryDescriptor&);

  void fillOneEntry(const std::vector<int64_t>& entry) {
    CHECK(storage_);
    if (storage_->query_mem_desc_.didOutputColumnar()) {
      storage_->fillOneEntryColWise(entry);
    } else {
      storage_->fillOneEntryRowWise(entry);
    }
  }

  void initializeStorage() const;

  void holdChunks(const std::list<std::shared_ptr<Chunk_NS::Chunk>>& chunks) {
    chunks_ = chunks;
  }
  void holdChunkIterators(const std::shared_ptr<std::list<ChunkIter>> chunk_iters) {
    chunk_iters_.push_back(chunk_iters);
  }
  void holdLiterals(std::vector<int8_t>& literal_buff) {
    literal_buffers_.push_back(std::move(literal_buff));
  }

  std::shared_ptr<RowSetMemoryOwner> getRowSetMemOwner() const {
    return row_set_mem_owner_;
  }

  const Permutation& getPermutationBuffer() const;
  const bool isPermutationBufferEmpty() const { return permutation_.empty(); };
  void setPermutationBuffer(Permutation perm) { permutation_ = std::move(perm); }

  void serialize(TSerializedRows& serialized_rows) const;

  size_t getLimit() const;

  size_t getOffset() const;

  void copyColumnIntoBuffer(const size_t column_idx,
                            int8_t* output_buffer,
                            const size_t output_buffer_size) const;

  bool didOutputColumnar() const { return this->query_mem_desc_.didOutputColumnar(); }

  //  Columnar Conversion checker functions
  bool isDirectColumnarConversionPossible() const;
  bool isZeroCopyColumnarConversionPossible(size_t column_idx) const;
  bool isChunkedZeroCopyColumnarConversionPossible(size_t column_idx) const;

  //  Buffer Accessors
  const int8_t* getColumnarBuffer(size_t column_idx) const;

  //  Returns vector of std::pair<chunk buffer ptr, chunk row count>
  std::vector<std::pair<const int8_t*, size_t>> getChunkedColumnarBuffer(
      size_t column_idx) const;

  // For columns with varlen data writes element offsets to the output buffer.
  // It is 0 for the first element and cumulative length of all previous elements
  // for others. The total length is written at the end.
  // Returns the number of values written.
  size_t computeVarLenOffsets(size_t col_idx, int32_t* offsets) const;

  QueryDescriptionType getQueryDescriptionType() const {
    return query_mem_desc_.getQueryDescriptionType();
  }

  const int8_t getPaddedSlotWidthBytes(const size_t slot_idx) const {
    return query_mem_desc_.getPaddedSlotWidthBytes(slot_idx);
  }

  // returns a bitmap of all single-slot targets, as well as its count
  std::tuple<std::vector<bool>, size_t> getSingleSlotTargetBitmap() const;

  std::tuple<std::vector<bool>, size_t> getSupportedSingleSlotTargetBitmap() const;

  std::vector<size_t> getSlotIndicesForTargetIndices() const;

  const std::vector<ColumnLazyFetchInfo>& getLazyFetchInfo() const {
    return lazy_fetch_info_;
  }

  bool areAnyColumnsLazyFetched() const {
    auto is_lazy = [](auto const& info) { return info.is_lazily_fetched; };
    return std::any_of(lazy_fetch_info_.begin(), lazy_fetch_info_.end(), is_lazy);
  }

  size_t getNumColumnsLazyFetched() const {
    auto is_lazy = [](auto const& info) { return info.is_lazily_fetched; };
    return std::count_if(lazy_fetch_info_.begin(), lazy_fetch_info_.end(), is_lazy);
  }

  bool getSeparateVarlenStorageValid() const { return separate_varlen_storage_valid_; }
  void setSeparateVarlenStorageValid(const bool val) {
    separate_varlen_storage_valid_ = val;
  }

  const std::vector<std::string> getStringDictionaryPayloadCopy(const int dict_id) const;

  const std::pair<std::vector<int32_t>, std::vector<std::string>>
  getUniqueStringsForDictEncodedTargetCol(const size_t col_idx) const;

  StringDictionaryProxy* getStringDictionaryProxy(int const dict_id) const;

  template <typename ENTRY_TYPE, QueryDescriptionType QUERY_TYPE, bool COLUMNAR_FORMAT>
  ENTRY_TYPE getEntryAt(const size_t row_idx,
                        const size_t target_idx,
                        const size_t slot_idx) const;

  static double calculateQuantile(quantile::TDigest* const t_digest);

  void translateDictEncodedColumns(std::vector<TargetInfo> const&,
                                   size_t const start_idx);

  unsigned getBlockSize() const { return block_size_; }
  unsigned getGridSize() const { return grid_size_; }

  BufferProvider* getBufferProvider() const;

  struct RowIterationState {
    size_t prev_target_idx_{0};
    size_t cur_target_idx_;
    size_t agg_idx_{0};
    int8_t const* buf_ptr_{nullptr};
    int8_t compact_sz1_;
  };

  using StringId = int32_t;

  class CellCallback;
  void eachCellInColumn(RowIterationState&, CellCallback const&);

  static bool isNull(const hdk::ir::Type* type,
                     const InternalTargetValue& val,
                     const bool float_argument_input);

  struct TargetOffsets {
    const int8_t* ptr1;
    const size_t compact_sz1;
    const int8_t* ptr2;
    const size_t compact_sz2;
  };

  struct StorageLookupResult {
    const ResultSetStorage* storage_ptr;
    const size_t fixedup_entry_idx;
    const size_t storage_idx;
  };

  struct RowWiseTargetAccessor {
    RowWiseTargetAccessor(const ResultSet* result_set)
        : result_set_(result_set)
        , row_bytes_(get_row_bytes(result_set->query_mem_desc_))
        , key_width_(result_set_->query_mem_desc_.getEffectiveKeyWidth())
        , key_bytes_with_padding_(
              align_to_int64(get_key_bytes_rowwise(result_set->query_mem_desc_))) {
      initializeOffsetsForStorage();
    }

    InternalTargetValue getColumnInternal(
        const int8_t* buff,
        const size_t entry_idx,
        const size_t target_logical_idx,
        const StorageLookupResult& storage_lookup_result) const;

    void initializeOffsetsForStorage();

    inline const int8_t* get_rowwise_ptr(const int8_t* buff,
                                         const size_t entry_idx) const {
      return buff + entry_idx * row_bytes_;
    }

    std::vector<std::vector<TargetOffsets>> offsets_for_storage_;

    const ResultSet* result_set_;

    // Row-wise iteration
    const size_t row_bytes_;
    const size_t key_width_;
    const size_t key_bytes_with_padding_;
  };

  struct ColumnWiseTargetAccessor {
    ColumnWiseTargetAccessor(const ResultSet* result_set) : result_set_(result_set) {
      initializeOffsetsForStorage();
    }

    void initializeOffsetsForStorage();

    InternalTargetValue getColumnInternal(
        const int8_t* buff,
        const size_t entry_idx,
        const size_t target_logical_idx,
        const StorageLookupResult& storage_lookup_result) const;

    std::vector<std::vector<TargetOffsets>> offsets_for_storage_;

    const ResultSet* result_set_;
  };

  PermutationView initPermutationBuffer(PermutationView permutation,
                                        PermutationIdx const begin,
                                        PermutationIdx const end) const;

  StorageLookupResult findStorage(const size_t entry_idx) const;

  Data_Namespace::DataMgr* getDataManager() const { return data_mgr_; }

  // only used by serialization
  using SerializedVarlenBufferStorage = std::vector<std::string>;

  const std::vector<SerializedVarlenBufferStorage>& getSerializedVarlenBuffer() const {
    return serialized_varlen_buffer_;
  }

  ResultSetPtr shallowCopy() const;

 private:
  ResultSet(const ResultSet& other);

  void advanceCursorToNextEntry(ResultSetRowIterator& iter) const;

  std::vector<TargetValue> getNextRowImpl(const bool translate_strings,
                                          const bool decimal_to_double) const;

  std::vector<TargetValue> getNextRowUnlocked(const bool translate_strings,
                                              const bool decimal_to_double) const;

  std::vector<TargetValue> getRowAt(const size_t index,
                                    const bool translate_strings,
                                    const bool decimal_to_double,
                                    const bool fixup_count_distinct_pointers,
                                    const std::vector<bool>& targets_to_skip) const;

  // NOTE: just for direct columnarization use at the moment
  template <typename ENTRY_TYPE>
  ENTRY_TYPE getColumnarPerfectHashEntryAt(const size_t row_idx,
                                           const size_t target_idx,
                                           const size_t slot_idx) const;

  template <typename ENTRY_TYPE>
  ENTRY_TYPE getRowWisePerfectHashEntryAt(const size_t row_idx,
                                          const size_t target_idx,
                                          const size_t slot_idx) const;

  template <typename ENTRY_TYPE>
  ENTRY_TYPE getRowWiseBaselineEntryAt(const size_t row_idx,
                                       const size_t target_idx,
                                       const size_t slot_idx) const;

  template <typename ENTRY_TYPE>
  ENTRY_TYPE getColumnarBaselineEntryAt(const size_t row_idx,
                                        const size_t target_idx,
                                        const size_t slot_idx) const;

  size_t binSearchRowCount() const;

  size_t parallelRowCount() const;

  size_t advanceCursorToNextEntry() const;

  void radixSortOnGpu(const Config& config,
                      const std::list<hdk::ir::OrderEntry>& order_entries) const;

  void radixSortOnCpu(const std::list<hdk::ir::OrderEntry>& order_entries) const;

  TargetValue getTargetValueFromBufferRowwise(
      int8_t* rowwise_target_ptr,
      int8_t* keys_ptr,
      const size_t entry_buff_idx,
      const TargetInfo& target_info,
      const size_t target_logical_idx,
      const size_t slot_idx,
      const bool translate_strings,
      const bool decimal_to_double,
      const bool fixup_count_distinct_pointers) const;

  TargetValue getTargetValueFromBufferColwise(const int8_t* col_ptr,
                                              const int8_t* keys_ptr,
                                              const QueryMemoryDescriptor& query_mem_desc,
                                              const size_t local_entry_idx,
                                              const size_t global_entry_idx,
                                              const TargetInfo& target_info,
                                              const size_t target_logical_idx,
                                              const size_t slot_idx,
                                              const bool translate_strings,
                                              const bool decimal_to_double) const;

  TargetValue makeTargetValue(const int8_t* ptr,
                              const int8_t compact_sz,
                              const TargetInfo& target_info,
                              const size_t target_logical_idx,
                              const bool translate_strings,
                              const bool decimal_to_double,
                              const size_t entry_buff_idx) const;

  TargetValue makeVarlenTargetValue(const int8_t* ptr1,
                                    const int8_t compact_sz1,
                                    const int8_t* ptr2,
                                    const int8_t compact_sz2,
                                    const TargetInfo& target_info,
                                    const size_t target_logical_idx,
                                    const bool translate_strings,
                                    const size_t entry_buff_idx) const;

  struct VarlenTargetPtrPair {
    int8_t* ptr1;
    int8_t compact_sz1;
    int8_t* ptr2;
    int8_t compact_sz2;

    VarlenTargetPtrPair()
        : ptr1(nullptr), compact_sz1(0), ptr2(nullptr), compact_sz2(0) {}
  };

  InternalTargetValue getVarlenOrderEntry(const int64_t str_ptr,
                                          const size_t str_len) const;

  int64_t lazyReadInt(const int64_t ival,
                      const size_t target_logical_idx,
                      const StorageLookupResult& storage_lookup_result) const;

  /// Returns (storageIdx, entryIdx) pair, where:
  /// storageIdx : 0 is storage_, storageIdx-1 is index into appended_storage_.
  /// entryIdx   : local index into the storage object.
  std::pair<size_t, size_t> getStorageIndex(const size_t entry_idx) const;

  const std::vector<const int8_t*>& getColumnFrag(const size_t storge_idx,
                                                  const size_t col_logical_idx,
                                                  int64_t& global_idx) const;

  const VarlenOutputInfo* getVarlenOutputInfo(const size_t entry_idx) const;

  size_t rowCountImpl(const bool force_parallel) const;

  void serializeProjection(TSerializedRows& serialized_rows) const;
  void serializeVarlenAggColumn(int8_t* buf,
                                std::vector<std::string>& varlen_bufer) const;

  void serializeCountDistinctColumns(TSerializedRows&) const;

  void unserializeCountDistinctColumns(const TSerializedRows&);

  void fixupCountDistinctPointers();

  using BufferSet = std::set<int64_t>;
  void create_active_buffer_set(BufferSet& count_distinct_active_buffer_set) const;

  int64_t getDistinctBufferRefFromBufferRowwise(int8_t* rowwise_target_ptr,
                                                const TargetInfo& target_info) const;

  const std::vector<TargetInfo> targets_;
  ExecutorDeviceType device_type_;
  std::vector<std::string> fields_;
  const int device_id_;
  QueryMemoryDescriptor query_mem_desc_;
  mutable std::shared_ptr<ResultSetStorage> storage_;
  AppendedStorage appended_storage_;
  mutable size_t crt_row_buff_idx_;
  mutable size_t fetched_so_far_;
  size_t drop_first_;
  size_t keep_first_;
  std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner_;
  Permutation permutation_;

  unsigned block_size_{0};
  unsigned grid_size_{0};
  QueryExecutionTimings timings_;

  int outer_table_id_ = 0;

  std::list<std::shared_ptr<Chunk_NS::Chunk>> chunks_;
  std::vector<std::shared_ptr<std::list<ChunkIter>>> chunk_iters_;
  // TODO(miyu): refine by using one buffer and
  //   setting offset instead of ptr in group by buffer.
  std::vector<std::vector<int8_t>> literal_buffers_;
  const std::vector<ColumnLazyFetchInfo> lazy_fetch_info_;
  std::vector<std::vector<std::vector<const int8_t*>>> col_buffers_;
  std::vector<std::vector<std::vector<int64_t>>> frag_offsets_;
  std::vector<std::vector<int64_t>> consistent_frag_sizes_;

  const std::shared_ptr<const hdk::ir::Estimator> estimator_;
  Data_Namespace::AbstractBuffer* device_estimator_buffer_{nullptr};
  mutable std::shared_ptr<int8_t> host_estimator_buffer_{nullptr};
  Data_Namespace::DataMgr* data_mgr_{nullptr};

  std::vector<SerializedVarlenBufferStorage> serialized_varlen_buffer_;
  bool separate_varlen_storage_valid_;
  std::string explanation_;
  const bool just_explain_;
  bool for_validation_only_;
  mutable std::atomic<int64_t> cached_row_count_;
  mutable std::mutex row_iteration_mutex_;

  friend class ResultSetRowIterator;
  friend class ColumnarResults;
};

ResultSetRowIterator::value_type ResultSetRowIterator::operator*() const {
  if (!global_entry_idx_valid_) {
    return {};
  }

  if (result_set_->just_explain_) {
    return {result_set_->explanation_};
  }

  return result_set_->getRowAt(
      global_entry_idx_, translate_strings_, decimal_to_double_, false, {});
}

inline ResultSetRowIterator& ResultSetRowIterator::operator++(void) {
  if (!result_set_->storage_ && !result_set_->just_explain_) {
    global_entry_idx_valid_ = false;
  } else if (result_set_->just_explain_) {
    global_entry_idx_valid_ = 0 == fetched_so_far_;
    fetched_so_far_ = 1;
  } else {
    result_set_->advanceCursorToNextEntry(*this);
  }
  return *this;
}

class RowSortException : public std::runtime_error {
 public:
  RowSortException(const std::string& cause) : std::runtime_error(cause) {}
};

namespace result_set {

bool can_use_parallel_algorithms(const ResultSet& rows);

bool use_parallel_algorithms(const ResultSet& rows);

}  // namespace result_set

#endif  // QUERYENGINE_RESULTSET_H
