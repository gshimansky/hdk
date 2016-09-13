/*
 * @file    ResultRows.h
 * @author  Alex Suhan <alex@mapd.com>
 *
 * Created on May 9, 2016, 3:45 PM
 */

#ifndef QUERYENGINE_RESULTROWS_H
#define QUERYENGINE_RESULTROWS_H

#include "QueryMemoryDescriptor.h"
#include "TargetValue.h"

#include "../Analyzer/Analyzer.h"
#include "../Shared/TargetInfo.h"
#include "../StringDictionary/StringDictionary.h"

#include <boost/noncopyable.hpp>
#include <glog/logging.h>

#include <list>
#include <set>
#include <unordered_set>

struct QueryMemoryDescriptor;
struct RelAlgExecutionUnit;
class RowSetMemoryOwner;

inline int64_t bitmap_set_size(const int64_t bitmap_ptr,
                               const int target_idx,
                               const CountDistinctDescriptors& count_distinct_descriptors) {
  const auto count_distinct_desc_it = count_distinct_descriptors.find(target_idx);
  CHECK(count_distinct_desc_it != count_distinct_descriptors.end());
  if (count_distinct_desc_it->second.impl_type_ != CountDistinctImplType::Bitmap) {
    CHECK(count_distinct_desc_it->second.impl_type_ == CountDistinctImplType::StdSet);
    return reinterpret_cast<std::set<int64_t>*>(bitmap_ptr)->size();
  }
  int64_t set_size{0};
  auto set_vals = reinterpret_cast<const int8_t*>(bitmap_ptr);
  for (size_t i = 0; i < count_distinct_desc_it->second.bitmapSizeBytes(); ++i) {
    for (auto bit_idx = 0; bit_idx < 8; ++bit_idx) {
      if (set_vals[i] & (1 << bit_idx)) {
        ++set_size;
      }
    }
  }
  return set_size;
}

inline void bitmap_set_unify(int8_t* lhs, int8_t* rhs, const size_t bitmap_sz) {
  for (size_t i = 0; i < bitmap_sz; ++i) {
    lhs[i] = rhs[i] = lhs[i] | rhs[i];
  }
}

typedef std::vector<int64_t> ValueTuple;

class ChunkIter;

class DictStrLiteralsOwner {
 public:
  DictStrLiteralsOwner(StringDictionary* string_dict) : string_dict_(string_dict) {}

  ~DictStrLiteralsOwner() { string_dict_->clearTransient(); }

 private:
  StringDictionary* string_dict_;
};

class RowSetMemoryOwner : boost::noncopyable {
 public:
  void setCountDistinctDescriptors(const CountDistinctDescriptors& count_distinct_descriptors) {
    if (count_distinct_descriptors_.empty()) {
      count_distinct_descriptors_ = count_distinct_descriptors;
    }
  }

  void addCountDistinctBuffer(int8_t* count_distinct_buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    count_distinct_bitmaps_.push_back(count_distinct_buffer);
  }

  void addCountDistinctSet(std::set<int64_t>* count_distinct_set) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    count_distinct_sets_.push_back(count_distinct_set);
  }

  const CountDistinctDescriptors& getCountDistinctDescriptors() const { return count_distinct_descriptors_; }

  void addGroupByBuffer(int64_t* group_by_buffer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    group_by_buffers_.push_back(group_by_buffer);
  }

  std::string* addString(const std::string& str) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    strings_.emplace_back(str);
    return &strings_.back();
  }

  std::vector<int64_t>* addArray(const std::vector<int64_t>& arr) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    arrays_.emplace_back(arr);
    return &arrays_.back();
  }

  void addStringDict(StringDictionary* str_dict, const int dict_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = str_dict_owned_.find(dict_id);
    if (it != str_dict_owned_.end()) {
      CHECK_EQ(it->second, str_dict);
      return;
    }
    str_dict_owned_.emplace(dict_id, str_dict);
    str_dict_owners_.emplace_back(new DictStrLiteralsOwner(str_dict));
  }

  StringDictionary* getStringDict(const int dict_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = str_dict_owned_.find(dict_id);
    CHECK(it != str_dict_owned_.end());
    return it->second;
  }

  void addLiteralStringDict(std::shared_ptr<StringDictionary> lit_str_dict) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lit_str_dict_ = lit_str_dict;
  }

  ~RowSetMemoryOwner() {
    for (auto count_distinct_buffer : count_distinct_bitmaps_) {
      free(count_distinct_buffer);
    }
    for (auto count_distinct_set : count_distinct_sets_) {
      delete count_distinct_set;
    }
    for (auto group_by_buffer : group_by_buffers_) {
      free(group_by_buffer);
    }
    decltype(str_dict_owners_)().swap(str_dict_owners_);
    decltype(str_dict_owned_)().swap(str_dict_owned_);
    if (lit_str_dict_) {
      lit_str_dict_->clearTransient();
    }
  }

 private:
  CountDistinctDescriptors count_distinct_descriptors_;
  std::vector<int8_t*> count_distinct_bitmaps_;
  std::vector<std::set<int64_t>*> count_distinct_sets_;
  std::vector<int64_t*> group_by_buffers_;
  std::list<std::string> strings_;
  std::list<std::vector<int64_t>> arrays_;
  std::unordered_map<int, StringDictionary*> str_dict_owned_;
  std::vector<std::unique_ptr<DictStrLiteralsOwner>> str_dict_owners_;
  std::shared_ptr<StringDictionary> lit_str_dict_;
  mutable std::mutex state_mutex_;

  friend class ResultRows;
};

class InternalRow {
 public:
  InternalRow(RowSetMemoryOwner* row_set_mem_owner) : row_set_mem_owner_(row_set_mem_owner){};

  bool operator==(const InternalRow& other) const { return row_ == other.row_; }

  bool operator<(const InternalRow& other) const { return row_ < other.row_; }

  InternalTargetValue& operator[](const size_t i) { return row_[i]; }

  const InternalTargetValue& operator[](const size_t i) const { return row_[i]; }

  size_t size() const { return row_.size(); }

 private:
  void reserve(const size_t n) { row_.reserve(n); }

  void addValue(const int64_t val) { row_.emplace_back(val); }

  // used for kAVG
  void addValue(const int64_t val1, const int64_t val2) { row_.emplace_back(val1, val2); }

  void addValue(const std::string& val) { row_.emplace_back(row_set_mem_owner_->addString(val)); }

  void addValue(const std::vector<int64_t>& val) { row_.emplace_back(row_set_mem_owner_->addArray(val)); }

  void addValue() { row_.emplace_back(); }

  std::vector<InternalTargetValue> row_;
  RowSetMemoryOwner* row_set_mem_owner_;

  friend class RowStorage;
};

class RowStorage {
 private:
  size_t size() const { return rows_.size(); }

  void clear() { rows_.clear(); }

  void reserve(const size_t n) { rows_.reserve(n); }

  void beginRow(RowSetMemoryOwner* row_set_mem_owner) { rows_.emplace_back(row_set_mem_owner); }

  void reserveRow(const size_t n) { rows_.back().reserve(n); }

  void discardRow() { rows_.pop_back(); }

  void addValue(const int64_t val) { rows_.back().addValue(val); }

  // used for kAVG
  void addValue(const int64_t val1, const int64_t val2) { rows_.back().addValue(val1, val2); }

  void addValue(const std::string& val) { rows_.back().addValue(val); }

  void addValue(const std::vector<int64_t>& val) { rows_.back().addValue(val); }

  void addValue() { rows_.back().addValue(); }

  void push_back(const InternalRow& v) { rows_.push_back(v); }

  void append(const RowStorage& other) { rows_.insert(rows_.end(), other.rows_.begin(), other.rows_.end()); }

  void truncate(const size_t n) { rows_.erase(rows_.begin() + n, rows_.end()); }

  void drop(const size_t n) {
    if (n >= rows_.size()) {
      decltype(rows_)().swap(rows_);
      return;
    }
    decltype(rows_)(rows_.begin() + n, rows_.end()).swap(rows_);
  }

  InternalRow& operator[](const size_t i) { return rows_[i]; }

  const InternalRow& operator[](const size_t i) const { return rows_[i]; }

  InternalRow& front() { return rows_.front(); }

  const InternalRow& front() const { return rows_.front(); }

  const InternalRow& back() const { return rows_.back(); }

  void top(const int64_t n, const std::function<bool(const InternalRow& lhs, const InternalRow& rhs)> compare) {
    std::make_heap(rows_.begin(), rows_.end(), compare);
    decltype(rows_) top_target_values;
    top_target_values.reserve(n);
    for (int64_t i = 0; i < n && !rows_.empty(); ++i) {
      top_target_values.push_back(rows_.front());
      std::pop_heap(rows_.begin(), rows_.end(), compare);
      rows_.pop_back();
    }
    rows_.swap(top_target_values);
  }

  void sort(const std::function<bool(const InternalRow& lhs, const InternalRow& rhs)> compare) {
    std::sort(rows_.begin(), rows_.end(), compare);
  }

  void removeDuplicates() {
    std::sort(rows_.begin(), rows_.end());
    rows_.erase(std::unique(rows_.begin(), rows_.end()), rows_.end());
  }

  std::vector<InternalRow> rows_;

  friend class ResultRows;
};

inline const Analyzer::AggExpr* cast_to_agg_expr(const Analyzer::Expr* target_expr) {
  return dynamic_cast<const Analyzer::AggExpr*>(target_expr);
}

inline const Analyzer::AggExpr* cast_to_agg_expr(const std::shared_ptr<Analyzer::Expr> target_expr) {
  return dynamic_cast<const Analyzer::AggExpr*>(target_expr.get());
}

template <class PointerType>
inline TargetInfo target_info(const PointerType target_expr) {
  const auto agg_expr = cast_to_agg_expr(target_expr);
  bool notnull = target_expr->get_type_info().get_notnull();
  if (!agg_expr) {
    return {false,
            kMIN,
            target_expr ? target_expr->get_type_info() : SQLTypeInfo(kBIGINT, notnull),
            SQLTypeInfo(kNULLT, false),
            false,
            false};
  }
  const auto agg_type = agg_expr->get_aggtype();
  const auto agg_arg = agg_expr->get_arg();
  if (!agg_arg) {
    CHECK_EQ(kCOUNT, agg_type);
    CHECK(!agg_expr->get_is_distinct());
    return {true, kCOUNT, SQLTypeInfo(kINT, notnull), SQLTypeInfo(kNULLT, false), false, false};
  }

  const auto& agg_arg_ti = agg_arg->get_type_info();
  bool is_distinct{false};
  if (agg_expr->get_aggtype() == kCOUNT) {
    is_distinct = agg_expr->get_is_distinct();
  }

  return {true,
          agg_expr->get_aggtype(),
          agg_type == kCOUNT ? SQLTypeInfo(is_distinct ? kBIGINT : kINT, notnull)
                             : (agg_type == kAVG ? agg_arg_ti : agg_expr->get_type_info()),
          agg_arg_ti,
          !agg_arg_ti.get_notnull(),
          is_distinct};
}

struct GpuQueryMemory;

class ResultRows {
 public:
  ResultRows(const QueryMemoryDescriptor& query_mem_desc,
             const std::vector<Analyzer::Expr*>& targets,
             const Executor* executor,
             const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
             const std::vector<int64_t>& init_vals,
             const ExecutorDeviceType device_type,
             int64_t* group_by_buffer = nullptr,
             const int32_t groups_buffer_entry_count = 0,
             const int64_t min_val = 0,
             const int8_t warp_count = 0,
             const int64_t queue_time_ms = 0)
      : executor_(executor),
        query_mem_desc_(query_mem_desc),
        row_set_mem_owner_(row_set_mem_owner),
        agg_init_vals_(init_vals),
        group_by_buffer_(group_by_buffer),
        groups_buffer_entry_count_(groups_buffer_entry_count),
        group_by_buffer_idx_(0),
        min_val_(min_val),
        warp_count_(warp_count),
        output_columnar_(false),
        in_place_(false),
        device_type_(device_type),
        device_id_(-1),
        crt_row_idx_(0),
        crt_row_buff_idx_(0),
        drop_first_(0),
        keep_first_(0),
        fetch_started_(false),
        in_place_buff_idx_(0),
        just_explain_(false),
        queue_time_ms_(queue_time_ms) {
    for (const auto target_expr : targets) {
      targets_.push_back(target_info(target_expr));
    }
  }

  ResultRows(const QueryMemoryDescriptor& query_mem_desc,
             const std::vector<Analyzer::Expr*>& targets,
             const std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
             const std::vector<int64_t>& init_vals,
             int64_t* group_by_buffer,
             const size_t groups_buffer_entry_count,
             const bool output_columnar,
             const std::vector<std::vector<const int8_t*>>& col_buffers,
             const ExecutorDeviceType device_type,
             const int device_id);

  ResultRows(const std::string& explanation, int64_t queue_time_ms)
      : query_mem_desc_{},
        group_by_buffer_idx_(0),
        output_columnar_(false),
        in_place_(false),
        device_type_(ExecutorDeviceType::Hybrid),
        device_id_(-1),
        crt_row_idx_(0),
        crt_row_buff_idx_(0),
        drop_first_(0),
        keep_first_(0),
        fetch_started_(false),
        in_place_buff_idx_(0),
        just_explain_(true),
        explanation_(explanation),
        queue_time_ms_(queue_time_ms) {}

  ResultRows(const std::string& explanation, int64_t queue_time_ms, int64_t render_time_ms)
      : query_mem_desc_{},
        group_by_buffer_idx_(0),
        output_columnar_(false),
        in_place_(false),
        device_type_(ExecutorDeviceType::Hybrid),
        device_id_(-1),
        crt_row_idx_(0),
        crt_row_buff_idx_(0),
        drop_first_(0),
        keep_first_(0),
        fetch_started_(false),
        in_place_buff_idx_(0),
        just_explain_(true),
        explanation_(explanation),
        queue_time_ms_(queue_time_ms),
        render_time_ms_(render_time_ms) {}

  explicit ResultRows(const std::string& explanation)
      : query_mem_desc_{},
        group_by_buffer_idx_(0),
        output_columnar_(false),
        in_place_(false),
        device_type_(ExecutorDeviceType::Hybrid),
        device_id_(-1),
        crt_row_idx_(0),
        crt_row_buff_idx_(0),
        drop_first_(0),
        keep_first_(0),
        fetch_started_(false),
        in_place_buff_idx_(0),
        just_explain_(true),
        explanation_(explanation),
        queue_time_ms_(0) {}

  void moveToBegin() const {
    crt_row_idx_ = 0;
    crt_row_buff_idx_ = 0;
    in_place_buff_idx_ = 0;
    group_by_buffer_idx_ = 0;
    fetch_started_ = false;
  }
  void beginRow() { target_values_.beginRow(row_set_mem_owner_.get()); }

  void beginRow(const int64_t key) {
    CHECK(multi_keys_.empty());
    simple_keys_.push_back(key);
    target_values_.beginRow(row_set_mem_owner_.get());
  }

  void beginRow(const std::vector<int64_t>& key) {
    CHECK(simple_keys_.empty());
    multi_keys_.push_back(key);
    target_values_.beginRow(row_set_mem_owner_.get());
  }

  bool reduceSingleRow(const int8_t* row_ptr,
                       const int8_t warp_count,
                       const bool is_columnar,
                       const bool replace_bitmap_ptr_with_bitmap_sz,
                       std::vector<int64_t>& agg_vals) const;

  void addKeylessGroupByBuffer(const int64_t* group_by_buffer,
                               const int32_t groups_buffer_entry_count,
                               const int64_t min_val,
                               const int8_t warp_count,
                               const bool is_columnar);

  void addValue(const int64_t val) { target_values_.addValue(val); }

  // used for kAVG
  void addValue(const int64_t val1, const int64_t val2) { target_values_.addValue(val1, val2); }

  void addValue(const std::string& val) { target_values_.addValue(val); }

  void addValue(const std::vector<int64_t>& val) { target_values_.addValue(val); }

  void addValue() { target_values_.addValue(); }

  void append(const ResultRows& more_results) {
    simple_keys_.insert(simple_keys_.end(), more_results.simple_keys_.begin(), more_results.simple_keys_.end());
    multi_keys_.insert(multi_keys_.end(), more_results.multi_keys_.begin(), more_results.multi_keys_.end());
    target_values_.append(more_results.target_values_);
    if (in_place_) {
      in_place_group_by_buffers_.insert(in_place_group_by_buffers_.end(),
                                        more_results.in_place_group_by_buffers_.begin(),
                                        more_results.in_place_group_by_buffers_.end());
      in_place_groups_by_buffers_entry_count_.insert(in_place_groups_by_buffers_entry_count_.end(),
                                                     more_results.in_place_groups_by_buffers_entry_count_.begin(),
                                                     more_results.in_place_groups_by_buffers_entry_count_.end());
    }
  }

  void reduce(const ResultRows& other_results, const QueryMemoryDescriptor& query_mem_desc, const bool output_columnar);

  void sort(const std::list<Analyzer::OrderEntry>& order_entries, const bool remove_duplicates, const int64_t top_n);

  void keepFirstN(const size_t n) {
    CHECK(n);
    if (in_place_ || group_by_buffer_) {
      keep_first_ = n;
      return;
    }
    if (n >= rowCount()) {
      return;
    }
    target_values_.truncate(n);
  }

  void dropFirstN(const size_t n) {
    if (in_place_ || group_by_buffer_) {
      drop_first_ = n;
      return;
    }
    if (!n) {
      return;
    }
    target_values_.drop(n);
  }

  size_t rowCount() const {
    if (in_place_ || group_by_buffer_) {
      moveToBegin();
      size_t row_count{0};
      while (true) {
        auto crt_row = getNextRow(false, false);
        if (crt_row.empty()) {
          break;
        }
        ++row_count;
      }
      moveToBegin();
      return row_count;
    }
    return just_explain_ ? 1 : target_values_.size();
  }

  size_t colCount() const { return just_explain_ ? 1 : targets_.size(); }

  bool definitelyHasNoRows() const {
    if (in_place_) {
      return in_place_group_by_buffers_.empty();
    }
    return !group_by_buffer_ && !just_explain_ && !rowCount();
  }

  static bool isNull(const SQLTypeInfo& ti, const InternalTargetValue& val);

  TargetValue getRowAt(const size_t row_idx,
                       const size_t col_idx,
                       const bool translate_strings,
                       const bool decimal_to_double = true) const;

  std::vector<TargetValue> getNextRow(const bool translate_strings, const bool decimal_to_double) const;

  SQLTypeInfo getColType(const size_t col_idx) const {
    if (just_explain_) {
      return SQLTypeInfo(kTEXT, false);
    }
    return targets_[col_idx].agg_kind == kAVG ? SQLTypeInfo(kDOUBLE, false) : targets_[col_idx].sql_type;
  }

  int64_t getQueueTime() const { return queue_time_ms_; }
  int64_t getRenderTime() const { return render_time_ms_; }

  bool isInPlace() const { return in_place_; }

  void setQueueTime(int64_t queue_time) { queue_time_ms_ = queue_time; }

  const QueryMemoryDescriptor& getQueryMemDesc() const { return query_mem_desc_; }

  static void inplaceSortGpuImpl(const std::list<Analyzer::OrderEntry>&,
                                 const QueryMemoryDescriptor&,
                                 const GpuQueryMemory&,
                                 int64_t*);

 private:
  void reduceSingleColumn(int8_t* crt_val_i1,
                          int8_t* crt_val_i2,
                          const int8_t* new_val_i1,
                          const int8_t* new_val_i2,
                          const int64_t agg_skip_val,
                          const size_t target_idx,
                          size_t crt_byte_width = sizeof(int64_t),
                          size_t next_byte_width = sizeof(int64_t));

  void reduceDispatch(int64_t* group_by_buffer,
                      const int64_t* other_group_by_buffer,
                      const QueryMemoryDescriptor& query_mem_desc_in,
                      const size_t start,
                      const size_t end);

  void reduceInPlaceDispatch(int64_t** group_by_buffer_ptr,
                             const int64_t* other_group_by_buffer,
                             const int32_t groups_buffer_entry_count,
                             const GroupByColRangeType hash_type,
                             const QueryMemoryDescriptor& query_mem_desc_in,
                             const size_t start,
                             const size_t end);

  void reduceInPlace(int64_t** group_by_buffer_ptr,
                     const int64_t* other_group_by_buffer,
                     const int32_t groups_buffer_entry_count,
                     const int32_t other_groups_buffer_entry_count,
                     const GroupByColRangeType hash_type,
                     const QueryMemoryDescriptor& query_mem_desc_in);

  bool fetchLazyOrBuildRow(std::vector<TargetValue>& row,
                           const std::vector<std::vector<const int8_t*>>& col_buffers,
                           const std::vector<Analyzer::Expr*>& targets,
                           const bool translate_strings,
                           const bool decimal_to_double,
                           const bool fetch_lazy) const;

  void addValues(const std::vector<int64_t>& vals) {
    target_values_.reserveRow(vals.size());
    for (size_t target_idx = 0, agg_col_idx = 0; target_idx < targets_.size() && agg_col_idx < vals.size();
         ++target_idx, ++agg_col_idx) {
      const auto& agg_info = targets_[target_idx];
      if (kAVG == agg_info.agg_kind) {
        target_values_.addValue(vals[agg_col_idx], vals[agg_col_idx + 1]);
        ++agg_col_idx;
      } else {
        target_values_.addValue(vals[agg_col_idx]);
      }
    }
  }

  void discardRow() {
    CHECK_NE(simple_keys_.empty(), multi_keys_.empty());
    if (!simple_keys_.empty()) {
      simple_keys_.pop_back();
    } else {
      multi_keys_.pop_back();
    }
    target_values_.discardRow();
  }

  void createReductionMap() const {
    if (!as_map_.empty() || !as_unordered_map_.empty()) {
      return;
    }
    CHECK_NE(simple_keys_.empty(), multi_keys_.empty());
    for (size_t i = 0; i < simple_keys_.size(); ++i) {
      as_unordered_map_.insert(std::make_pair(simple_keys_[i], target_values_[i]));
    }
    for (size_t i = 0; i < multi_keys_.size(); ++i) {
      as_map_.insert(std::make_pair(multi_keys_[i], target_values_[i]));
    }
  }

  void inplaceSortGpu(const std::list<Analyzer::OrderEntry>& order_entries);

  void inplaceSortCpu(const std::list<Analyzer::OrderEntry>& order_entries);

  std::vector<TargetInfo> targets_;
  std::vector<int64_t> simple_keys_;
  typedef std::vector<int64_t> MultiKey;
  std::vector<MultiKey> multi_keys_;
  RowStorage target_values_;
  mutable std::map<MultiKey, InternalRow> as_map_;
  mutable std::unordered_map<int64_t, InternalRow> as_unordered_map_;
  const Executor* executor_;
  QueryMemoryDescriptor query_mem_desc_;
  std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner_;

  std::vector<int64_t> agg_init_vals_;
  int64_t* group_by_buffer_;
  size_t groups_buffer_entry_count_;
  mutable size_t group_by_buffer_idx_;
  int64_t min_val_;
  int8_t warp_count_;
  bool output_columnar_;
  bool in_place_;
  ExecutorDeviceType device_type_;
  int device_id_;
  mutable size_t crt_row_idx_;
  mutable size_t crt_row_buff_idx_;
  size_t drop_first_;
  size_t keep_first_;
  mutable bool fetch_started_;
  mutable size_t in_place_buff_idx_;
  std::vector<int32_t> in_place_groups_by_buffers_entry_count_;
  std::vector<int64_t*> in_place_group_by_buffers_;
  bool just_explain_;
  std::string explanation_;
  std::unordered_set<int64_t> unkown_top_keys_;
  int64_t queue_time_ms_;
  int64_t render_time_ms_;
};

#endif  // QUERYENGINE_RESULTROWS_H
