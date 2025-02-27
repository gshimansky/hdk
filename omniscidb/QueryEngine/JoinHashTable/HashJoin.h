/*
 * Copyright 2020 OmniSci, Inc.
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

#pragma once

#include <llvm/IR/Value.h>
#include <cstdint>
#include <set>
#include <string>

#include "DataMgr/Allocators/ThrustAllocator.h"
#include "DataProvider/DataProvider.h"
#include "IR/Expr.h"
#include "QueryEngine/CompilationOptions.h"
#include "QueryEngine/InputMetadata.h"
#include "QueryEngine/JoinHashTable/HashTable.h"
#include "QueryEngine/JoinHashTable/Runtime/HashJoinRuntime.h"
#include "ResultSet/RowSetMemoryOwner.h"
#include "ResultSetRegistry/ColumnarResults.h"

class TooManyHashEntries : public std::runtime_error {
 public:
  TooManyHashEntries()
      : std::runtime_error("Hash tables with more than 2B entries not supported yet") {}

  TooManyHashEntries(const std::string& reason) : std::runtime_error(reason) {}
};

class TableMustBeReplicated : public std::runtime_error {
 public:
  TableMustBeReplicated(const std::string& table_name)
      : std::runtime_error("Hash join failed: Table '" + table_name +
                           "' must be replicated.") {}
};

class HashJoinFail : public std::runtime_error {
 public:
  HashJoinFail(const std::string& reason) : std::runtime_error(reason) {}
};

class NeedsOneToManyHash : public HashJoinFail {
 public:
  NeedsOneToManyHash() : HashJoinFail("Needs one to many hash") {}
};

class FailedToFetchColumn : public HashJoinFail {
 public:
  FailedToFetchColumn()
      : HashJoinFail("Not enough memory for columns involved in join") {}
};

class FailedToJoinOnVirtualColumn : public HashJoinFail {
 public:
  FailedToJoinOnVirtualColumn() : HashJoinFail("Cannot join on rowid") {}
};

using InnerOuter = std::pair<const hdk::ir::ColumnVar*, const hdk::ir::Expr*>;

struct ColumnsForDevice {
  const std::vector<JoinColumn> join_columns;
  const std::vector<JoinColumnTypeInfo> join_column_types;
  const std::vector<std::shared_ptr<Chunk_NS::Chunk>> chunks_owner;
  const std::vector<std::shared_ptr<void>> malloc_owner;
};

struct HashJoinMatchingSet {
  llvm::Value* elements;
  llvm::Value* count;
  llvm::Value* slot;
};

struct CompositeKeyInfo {
  std::vector<const void*> sd_inner_proxy_per_key;
  std::vector<const void*> sd_outer_proxy_per_key;
  std::vector<ChunkKey> cache_key_chunks;  // used for the cache key
};

class DeviceAllocator;

class HashJoin {
 public:
  virtual std::string toString(const ExecutorDeviceType device_type,
                               const int device_id = 0,
                               bool raw = false) const = 0;

  virtual std::string toStringFlat64(const ExecutorDeviceType device_type,
                                     const int device_id) const;

  virtual std::string toStringFlat32(const ExecutorDeviceType device_type,
                                     const int device_id) const;

  virtual DecodedJoinHashBufferSet toSet(const ExecutorDeviceType device_type,
                                         const int device_id) const = 0;

  virtual llvm::Value* codegenSlot(const CompilationOptions&, const size_t) = 0;

  virtual HashJoinMatchingSet codegenMatchingSet(const CompilationOptions&,
                                                 const size_t) = 0;

  virtual int getInnerDbId() const noexcept = 0;

  virtual int getInnerTableId() const noexcept = 0;

  virtual int getInnerTableRteIdx() const noexcept = 0;

  virtual HashType getHashType() const noexcept = 0;

  static bool layoutRequiresAdditionalBuffers(HashType layout) noexcept {
    return (layout == HashType::ManyToMany || layout == HashType::OneToMany);
  }

  static std::string getHashTypeString(HashType ht) noexcept {
    const char* HashTypeStrings[3] = {"OneToOne", "OneToMany", "ManyToMany"};
    return HashTypeStrings[static_cast<int>(ht)];
  };

  static HashJoinMatchingSet codegenMatchingSet(
      const std::vector<llvm::Value*>& hash_join_idx_args_in,
      const bool col_is_nullable,
      const bool is_bw_eq,
      const int64_t sub_buff_size,
      Executor* executor,
      const CompilationOptions& co,
      const bool is_bucketized = false);

  static llvm::Value* codegenHashTableLoad(const size_t table_idx, Executor* executor);

  virtual Data_Namespace::MemoryLevel getMemoryLevel() const noexcept = 0;

  virtual int getDeviceCount() const noexcept = 0;

  virtual size_t offsetBufferOff() const noexcept = 0;

  virtual size_t countBufferOff() const noexcept = 0;

  virtual size_t payloadBufferOff() const noexcept = 0;

  virtual std::string getHashJoinType() const = 0;

  virtual bool isBitwiseEq() const = 0;

  JoinColumn fetchJoinColumn(const hdk::ir::ColumnVar* hash_col,
                             const std::vector<FragmentInfo>& fragment_info,
                             const Data_Namespace::MemoryLevel effective_memory_level,
                             const int device_id,
                             std::vector<std::shared_ptr<Chunk_NS::Chunk>>& chunks_owner,
                             DeviceAllocator* dev_buff_owner,
                             std::vector<std::shared_ptr<void>>& malloc_owner,
                             Executor* executor,
                             ColumnCacheMap* column_cache);

  //! Make hash table from an in-flight SQL query's parse tree etc.
  static std::shared_ptr<HashJoin> getInstance(
      const std::shared_ptr<const hdk::ir::BinOper> qual_bin_oper,
      const std::vector<InputTableInfo>& query_infos,
      const Data_Namespace::MemoryLevel memory_level,
      const JoinType join_type,
      const HashType preferred_hash_type,
      const int device_count,
      DataProvider* data_provider,
      ColumnCacheMap& column_cache,
      Executor* executor,
      const HashTableBuildDagMap& hashtable_build_dag_map,
      const TableIdToNodeMap& table_id_to_node_map);

  //! Make hash table from named tables and columns (such as for testing).
  static std::shared_ptr<HashJoin> getSyntheticInstance(
      int db_id,
      std::string_view table1,
      std::string_view column1,
      std::string_view table2,
      std::string_view column2,
      const Data_Namespace::MemoryLevel memory_level,
      const HashType preferred_hash_type,
      const int device_count,
      DataProvider* data_provider,
      ColumnCacheMap& column_cache,
      Executor* executor);

  //! Make hash table from named tables and columns (such as for testing).
  static std::shared_ptr<HashJoin> getSyntheticInstance(
      const std::shared_ptr<const hdk::ir::BinOper> qual_bin_oper,
      const Data_Namespace::MemoryLevel memory_level,
      const HashType preferred_hash_type,
      const int device_count,
      DataProvider* data_provider,
      ColumnCacheMap& column_cache,
      Executor* executor);

  static std::pair<std::string, std::shared_ptr<HashJoin>> getSyntheticInstance(
      std::vector<std::shared_ptr<const hdk::ir::BinOper>>,
      const Data_Namespace::MemoryLevel memory_level,
      const HashType preferred_hash_type,
      const int device_count,
      DataProvider* data_provider,
      ColumnCacheMap& column_cache,
      Executor* executor);

  static int getInnerTableId(const std::vector<InnerOuter>& inner_outer_pairs) {
    CHECK(!inner_outer_pairs.empty());
    const auto first_inner_col = inner_outer_pairs.front().first;
    return first_inner_col->tableId();
  }

  // Swap the columns if needed and make the inner column the first component.
  static InnerOuter normalizeColumnPair(const hdk::ir::Expr* lhs,
                                        const hdk::ir::Expr* rhs,
                                        SchemaProviderPtr schema_provider,
                                        const TemporaryTables* temporary_tables);

  // Normalize each expression tuple
  static std::vector<InnerOuter> normalizeColumnPairs(
      const hdk::ir::BinOper* condition,
      SchemaProviderPtr schema_provider,
      const TemporaryTables* temporary_tables);

  HashTable* getHashTableForDevice(const size_t device_id) const {
    CHECK_LT(device_id, hash_tables_for_device_.size());
    return hash_tables_for_device_[device_id].get();
  }

  size_t getJoinHashBufferSize(const ExecutorDeviceType device_type) {
    CHECK(device_type == ExecutorDeviceType::CPU);
    return getJoinHashBufferSize(device_type, 0);
  }

  size_t getJoinHashBufferSize(const ExecutorDeviceType device_type,
                               const int device_id) const {
    auto hash_table = getHashTableForDevice(device_id);
    if (!hash_table) {
      return 0;
    }
    return hash_table->getHashTableBufferSize(device_type);
  }

  int64_t getJoinHashBuffer(const ExecutorDeviceType device_type,
                            const int device_id) const;

  void freeHashBufferMemory() {
    auto empty_hash_tables =
        decltype(hash_tables_for_device_)(hash_tables_for_device_.size());
    hash_tables_for_device_.swap(empty_hash_tables);
  }

  static CompositeKeyInfo getCompositeKeyInfo(
      const std::vector<InnerOuter>& inner_outer_pairs,
      const Executor* executor);

  static std::vector<const StringDictionaryProxy::IdMap*>
  translateCompositeStrDictProxies(const CompositeKeyInfo& composite_key_info,
                                   const Executor* executor);

  static std::pair<const StringDictionaryProxy*, const StringDictionaryProxy*>
  getStrDictProxies(const InnerOuter& cols, const Executor* executor);

  static const StringDictionaryProxy::IdMap* translateInnerToOuterStrDictProxies(
      const InnerOuter& cols,
      const Executor* executor);

 protected:
  HashJoin(DataProvider* data_provider) : data_provider_(data_provider) {}

  virtual size_t getComponentBufferSize() const noexcept = 0;

  std::vector<std::shared_ptr<HashTable>> hash_tables_for_device_;
  DataProvider* data_provider_;
};

std::ostream& operator<<(std::ostream& os, const DecodedJoinHashBufferEntry& e);

std::ostream& operator<<(std::ostream& os, const DecodedJoinHashBufferSet& s);

std::shared_ptr<const hdk::ir::ColumnVar> getSyntheticColumnVar(int db_id,
                                                                std::string_view table,
                                                                std::string_view column,
                                                                int rte_idx,
                                                                Executor* executor);
