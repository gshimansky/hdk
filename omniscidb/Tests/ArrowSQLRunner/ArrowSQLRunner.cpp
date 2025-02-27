/*
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

#include "ArrowSQLRunner.h"
#include "RelAlgCache.h"

#include "Calcite/CalciteJNI.h"
#include "DataMgr/DataMgr.h"
#include "DataMgr/DataMgrDataProvider.h"
#include "QueryEngine/RelAlgExecutor.h"
#include "ResultSetRegistry/ResultSetRegistry.h"
#include "SchemaMgr/SchemaMgr.h"

#include "SQLiteComparator.h"

#include <gtest/gtest.h>

namespace TestHelpers::ArrowSQLRunner {

namespace {

class ArrowSQLRunnerImpl {
 public:
  static void init(ConfigPtr config, const std::string& udf_filename) {
    instance_.reset(new ArrowSQLRunnerImpl(config, udf_filename));
  }

  static void reset() { instance_.reset(); }

  static ArrowSQLRunnerImpl* get() {
    CHECK(instance_) << "ArrowSQLRunner is not initialized";
    return instance_.get();
  }

  Config& config() { return *config_; }

  ConfigPtr configPtr() { return config_; }

  bool gpusPresent() { return data_mgr_->gpusPresent(); }

  void printStats() {
    std::cout << "Total Calcite parsing time: " << (calcite_time_ / 1000) << "ms."
              << std::endl;
    std::cout << "Total execution time: " << (execution_time_ / 1000) << "ms."
              << std::endl;
  }

  void createTable(
      const std::string& table_name,
      const std::vector<ArrowStorage::ColumnDescription>& columns,
      const ArrowStorage::TableOptions& options = ArrowStorage::TableOptions()) {
    storage_->createTable(table_name, columns, options);
  }

  void dropTable(const std::string& table_name) { storage_->dropTable(table_name); }

  void insertCsvValues(const std::string& table_name, const std::string& values) {
    ArrowStorage::CsvParseOptions parse_options;
    parse_options.header = false;
    storage_->appendCsvData(values, table_name, parse_options);
  }

  void insertJsonValues(const std::string& table_name, const std::string& values) {
    storage_->appendJsonData(values, table_name);
  }

  std::string getSqlQueryRelAlg(const std::string& sql) {
    std::string query_ra;

    calcite_time_ += measure<std::chrono::microseconds>::execution(
        [&]() { query_ra = rel_alg_cache_->process("test_db", sql, {}, true); });

    return query_ra;
  }

  std::unique_ptr<RelAlgExecutor> makeRelAlgExecutor(const std::string& sql) {
    std::string query_ra = getSqlQueryRelAlg(sql);

    auto dag =
        std::make_unique<RelAlgDagBuilder>(query_ra, TEST_DB_ID, schema_mgr_, config_);

    return std::make_unique<RelAlgExecutor>(executor_.get(), schema_mgr_, std::move(dag));
  }

  ExecutionResult runSqlQuery(const std::string& sql,
                              const CompilationOptions& co,
                              const ExecutionOptions& eo) {
    LOG(INFO) << "Executing sql: " << sql << " on: " << co.device_type;
    auto ra_executor = makeRelAlgExecutor(sql);
    ExecutionResult res;

    execution_time_ += measure<std::chrono::microseconds>::execution(
        [&]() { res = ra_executor->executeRelAlgQuery(co, eo, false); });

    return res;
  }

  ExecutionResult runSqlQuery(const std::string& sql,
                              ExecutorDeviceType device_type,
                              const ExecutionOptions& eo) {
    return runSqlQuery(sql, getCompilationOptions(device_type), eo);
  }

  ExecutionResult runSqlQuery(const std::string& sql,
                              ExecutorDeviceType device_type,
                              bool allow_loop_joins) {
    return runSqlQuery(sql, device_type, getExecutionOptions(allow_loop_joins));
  }

  ExecutionOptions getExecutionOptions(bool allow_loop_joins, bool just_explain = false) {
    ExecutionOptions eo = ExecutionOptions::fromConfig(*config_);
    eo.allow_loop_joins = allow_loop_joins;
    eo.just_explain = just_explain;
    return eo;
  }

  CompilationOptions getCompilationOptions(ExecutorDeviceType device_type) {
    auto co = CompilationOptions::defaults(device_type);
    co.hoist_literals = config_->exec.codegen.hoist_literals;
    return co;
  }

  std::shared_ptr<ResultSet> run_multiple_agg(const std::string& query_str,
                                              const ExecutorDeviceType device_type,
                                              const bool allow_loop_joins = true) {
    return runSqlQuery(query_str, device_type, allow_loop_joins).getRows();
  }

  TargetValue run_simple_agg(const std::string& query_str,
                             const ExecutorDeviceType device_type,
                             const bool allow_loop_joins = true) {
    auto rows = run_multiple_agg(query_str, device_type, allow_loop_joins);
    auto crt_row = rows->getNextRow(true, true);
    CHECK_EQ(size_t(1), crt_row.size()) << query_str;
    return crt_row[0];
  }

  void run_sqlite_query(const std::string& query_string) {
    sqlite_comparator_.query(query_string);
  }

  void sqlite_batch_insert(const std::string& table_name,
                           std::vector<std::vector<std::string>>& insert_vals) {
    sqlite_comparator_.batch_insert(table_name, insert_vals);
  }

  void c(const std::string& query_string, const ExecutorDeviceType device_type) {
    sqlite_comparator_.compare(
        run_multiple_agg(query_string, device_type), query_string, device_type);
  }

  void c(const std::string& query_string,
         const std::string& sqlite_query_string,
         const ExecutorDeviceType device_type) {
    sqlite_comparator_.compare(
        run_multiple_agg(query_string, device_type), sqlite_query_string, device_type);
  }

  /* timestamp approximate checking for NOW() */
  void cta(const std::string& query_string,
           const std::string& sqlite_query_string,
           const ExecutorDeviceType device_type) {
    sqlite_comparator_.compare_timstamp_approx(
        run_multiple_agg(query_string, device_type), sqlite_query_string, device_type);
  }

  void check_arrow_dictionaries(
      const ArrowResultSet* arrow_result_set,
      const ResultSetPtr omnisci_results,
      const size_t min_result_size_for_bulk_dictionary_fetch,
      const double max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch) {
    const size_t num_columns = arrow_result_set->colCount();
    std::unordered_set<size_t> dictionary_encoded_col_idxs;
    std::vector<std::unordered_set<std::string>> per_column_dictionary_sets(num_columns);
    for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
      const auto column_type = arrow_result_set->colType(col_idx);
      if (!column_type->isExtDictionary()) {
        continue;
      }
      dictionary_encoded_col_idxs.emplace(col_idx);

      const auto dictionary_strings = arrow_result_set->getDictionaryStrings(col_idx);
      auto& dictionary_set = per_column_dictionary_sets[col_idx];
      for (const auto& dictionary_string : dictionary_strings) {
        ASSERT_EQ(dictionary_set.emplace(dictionary_string).second, true);
      }
    }
    const size_t row_count = arrow_result_set->rowCount();
    auto row_iterator = arrow_result_set->rowIterator(true, true);
    std::vector<std::unordered_set<std::string>> per_column_unique_strings(num_columns);
    for (size_t row_idx = 0; row_idx < row_count; ++row_idx) {
      const auto crt_row = *row_iterator++;
      for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
        if (dictionary_encoded_col_idxs.find(col_idx) ==
            dictionary_encoded_col_idxs.end()) {
          continue;
        }
        const auto omnisci_variant = crt_row[col_idx];
        const auto scalar_omnisci_variant =
            boost::get<ScalarTargetValue>(&omnisci_variant);
        CHECK(scalar_omnisci_variant);
        const auto omnisci_as_str_ptr =
            boost::get<NullableString>(scalar_omnisci_variant);
        ASSERT_NE(nullptr, omnisci_as_str_ptr);
        const auto omnisci_str_notnull_ptr = boost::get<std::string>(omnisci_as_str_ptr);
        if (omnisci_str_notnull_ptr) {
          const auto omnisci_str = *omnisci_str_notnull_ptr;
          CHECK(per_column_dictionary_sets[col_idx].find(omnisci_str) !=
                per_column_dictionary_sets[col_idx].end())
              << omnisci_str;
          per_column_unique_strings[col_idx].emplace(omnisci_str);
        }
      }
    }
    for (size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
      if (dictionary_encoded_col_idxs.find(col_idx) ==
          dictionary_encoded_col_idxs.end()) {
        continue;
      }
      const auto omnisci_col_type = omnisci_results->colType(col_idx);
      const auto dict_id = omnisci_col_type->as<hdk::ir::ExtDictionaryType>()->dictId();
      const auto str_dict_proxy = omnisci_results->getStringDictionaryProxy(dict_id);
      const size_t omnisci_dict_proxy_size = str_dict_proxy->entryCount();

      const auto col_dictionary_size = per_column_dictionary_sets[col_idx].size();
      const auto col_unique_strings = per_column_unique_strings[col_idx].size();
      const bool arrow_dictionary_definitely_sparse =
          col_dictionary_size < omnisci_dict_proxy_size;
      const bool arrow_dictionary_definitely_dense =
          col_unique_strings < col_dictionary_size;
      const double dictionary_to_result_size_ratio =
          static_cast<double>(omnisci_dict_proxy_size) / row_count;

      const bool arrow_dictionary_should_be_dense =
          row_count > min_result_size_for_bulk_dictionary_fetch &&
          dictionary_to_result_size_ratio <=
              max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch;

      if (arrow_dictionary_definitely_sparse) {
        ASSERT_EQ(col_unique_strings, col_dictionary_size);
        ASSERT_EQ(arrow_dictionary_should_be_dense, false);
      } else if (arrow_dictionary_definitely_dense) {
        ASSERT_EQ(col_dictionary_size, omnisci_dict_proxy_size);
        ASSERT_EQ(arrow_dictionary_should_be_dense, true);
      }
    }
  }

  void c_arrow(const std::string& query_string,
               const ExecutorDeviceType device_type,
               size_t min_result_size_for_bulk_dictionary_fetch,
               double max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch) {
    auto results = run_multiple_agg(query_string, device_type);
    auto arrow_omnisci_results = result_set_arrow_loopback(
        nullptr,
        results,
        device_type,
        min_result_size_for_bulk_dictionary_fetch,
        max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch);
    sqlite_comparator_.compare_arrow_output(
        arrow_omnisci_results, query_string, device_type);
    // Below we test the newly added sparse dictionary capability,
    // where only entries in a dictionary-encoded arrow column should be in the
    // corresponding dictionary (vs all the entries in the underlying OmniSci dictionary)
    check_arrow_dictionaries(
        arrow_omnisci_results.get(),
        results,
        min_result_size_for_bulk_dictionary_fetch,
        max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch);
  }

  void clearCpuMemory() {
    Executor::clearMemory(Data_Namespace::MemoryLevel::CPU_LEVEL, data_mgr_.get());
  }

  BufferPoolStats getBufferPoolStats(Data_Namespace::MemoryLevel mmeory_level) {
    return ::getBufferPoolStats(data_mgr_.get(), mmeory_level);
  }

  std::shared_ptr<ArrowStorage> getStorage() { return storage_; }

  SchemaProviderPtr getSchemaProvider() { return schema_mgr_; }

  std::shared_ptr<hdk::ResultSetRegistry> getResultSetRegistry() { return rs_registry_; }

  DataMgr* getDataMgr() { return data_mgr_.get(); }

  Executor* getExecutor() { return executor_.get(); }

  CalciteMgr* getCalcite() { return calcite_; }

  ~ArrowSQLRunnerImpl() {
    executor_.reset();
    storage_.reset();
    rs_registry_.reset();
    schema_mgr_.reset();
    rel_alg_cache_.reset();

    Executor::resetCodeCache();  // flush caches before tearing down buffer mgrs
    data_mgr_.reset();
  }

 protected:
  ArrowSQLRunnerImpl(ConfigPtr config, const std::string& udf_filename)
      : config_(std::move(config)) {
    if (!config_) {
      config_ = std::make_shared<Config>();
    }

    storage_ = std::make_shared<ArrowStorage>(TEST_SCHEMA_ID, "test", TEST_DB_ID);
    rs_registry_ = std::make_shared<hdk::ResultSetRegistry>(config_);
    schema_mgr_ = std::make_shared<SchemaMgr>();
    schema_mgr_->registerProvider(TEST_SCHEMA_ID, storage_);
    schema_mgr_->registerProvider(hdk::ResultSetRegistry::SCHEMA_ID, rs_registry_);

    data_mgr_ = std::make_unique<DataMgr>(*config_);
    auto* ps_mgr = data_mgr_->getPersistentStorageMgr();
    ps_mgr->registerDataProvider(TEST_SCHEMA_ID, storage_);
    ps_mgr->registerDataProvider(hdk::ResultSetRegistry::SCHEMA_ID, rs_registry_);

    executor_ = Executor::getExecutor(data_mgr_.get(), config_, "", "");
    executor_->setSchemaProvider(schema_mgr_);

    if (config_->debug.use_ra_cache.empty() || !config_->debug.build_ra_cache.empty()) {
      calcite_ = CalciteMgr::get(udf_filename, 1024);

      if (config_->debug.use_ra_cache.empty()) {
        ExtensionFunctionsWhitelist::add(calcite_->getExtensionFunctionWhitelist());
        if (!udf_filename.empty()) {
          ExtensionFunctionsWhitelist::addUdfs(
              calcite_->getUserDefinedFunctionWhitelist());
        }
      }

      calcite_->setRuntimeExtensionFunctions({}, /*is_runtime=*/false);
    }

    rel_alg_cache_ = std::make_shared<RelAlgCache>(calcite_, schema_mgr_, config_);
  }

  ConfigPtr config_;
  std::unique_ptr<DataMgr> data_mgr_;
  std::shared_ptr<Executor> executor_;
  std::shared_ptr<ArrowStorage> storage_;
  std::shared_ptr<hdk::ResultSetRegistry> rs_registry_;
  std::shared_ptr<SchemaMgr> schema_mgr_;
  CalciteMgr* calcite_;
  std::shared_ptr<RelAlgCache> rel_alg_cache_;

  SQLiteComparator sqlite_comparator_;
  int64_t calcite_time_ = 0;
  int64_t execution_time_ = 0;

  static std::unique_ptr<ArrowSQLRunnerImpl> instance_;
};

std::unique_ptr<ArrowSQLRunnerImpl> ArrowSQLRunnerImpl::instance_;

}  // namespace

void init(ConfigPtr config, const std::string& udf_filename) {
  ArrowSQLRunnerImpl::init(config, udf_filename);
}

void reset() {
  ArrowSQLRunnerImpl::reset();
}

Config& config() {
  return ArrowSQLRunnerImpl::get()->config();
}

ConfigPtr configPtr() {
  return ArrowSQLRunnerImpl::get()->configPtr();
}

bool gpusPresent() {
  return ArrowSQLRunnerImpl::get()->gpusPresent();
}

void printStats() {
  return ArrowSQLRunnerImpl::get()->printStats();
}

void createTable(const std::string& table_name,
                 const std::vector<ArrowStorage::ColumnDescription>& columns,
                 const ArrowStorage::TableOptions& options) {
  ArrowSQLRunnerImpl::get()->createTable(table_name, columns, options);
}

void dropTable(const std::string& table_name) {
  ArrowSQLRunnerImpl::get()->dropTable(table_name);
}

void insertCsvValues(const std::string& table_name, const std::string& values) {
  ArrowSQLRunnerImpl::get()->insertCsvValues(table_name, values);
}

void insertJsonValues(const std::string& table_name, const std::string& values) {
  ArrowSQLRunnerImpl::get()->insertJsonValues(table_name, values);
}

std::string getSqlQueryRelAlg(const std::string& query_str) {
  return ArrowSQLRunnerImpl::get()->getSqlQueryRelAlg(query_str);
}

ExecutionResult runSqlQuery(const std::string& sql,
                            const CompilationOptions& co,
                            const ExecutionOptions& eo) {
  return ArrowSQLRunnerImpl::get()->runSqlQuery(sql, co, eo);
}

ExecutionResult runSqlQuery(const std::string& sql,
                            ExecutorDeviceType device_type,
                            const ExecutionOptions& eo) {
  return ArrowSQLRunnerImpl::get()->runSqlQuery(sql, device_type, eo);
}

ExecutionResult runSqlQuery(const std::string& sql,
                            ExecutorDeviceType device_type,
                            bool allow_loop_joins) {
  return ArrowSQLRunnerImpl::get()->runSqlQuery(sql, device_type, allow_loop_joins);
}

ExecutionOptions getExecutionOptions(bool allow_loop_joins, bool just_explain) {
  return ArrowSQLRunnerImpl::get()->getExecutionOptions(allow_loop_joins, just_explain);
}

CompilationOptions getCompilationOptions(ExecutorDeviceType device_type) {
  return ArrowSQLRunnerImpl::get()->getCompilationOptions(device_type);
}

std::shared_ptr<ResultSet> run_multiple_agg(const std::string& query_str,
                                            const ExecutorDeviceType device_type,
                                            const bool allow_loop_joins) {
  return ArrowSQLRunnerImpl::get()->run_multiple_agg(
      query_str, device_type, allow_loop_joins);
}

TargetValue run_simple_agg(const std::string& query_str,
                           const ExecutorDeviceType device_type,
                           const bool allow_loop_joins) {
  return ArrowSQLRunnerImpl::get()->run_simple_agg(
      query_str, device_type, allow_loop_joins);
}

void run_sqlite_query(const std::string& query_string) {
  ArrowSQLRunnerImpl::get()->run_sqlite_query(query_string);
}

void sqlite_batch_insert(const std::string& table_name,
                         std::vector<std::vector<std::string>>& insert_vals) {
  ArrowSQLRunnerImpl::get()->sqlite_batch_insert(table_name, insert_vals);
}

void c(const std::string& query_string, const ExecutorDeviceType device_type) {
  ArrowSQLRunnerImpl::get()->c(query_string, device_type);
}

void c(const std::string& query_string,
       const std::string& sqlite_query_string,
       const ExecutorDeviceType device_type) {
  ArrowSQLRunnerImpl::get()->c(query_string, sqlite_query_string, device_type);
}

void cta(const std::string& query_string,
         const std::string& sqlite_query_string,
         const ExecutorDeviceType device_type) {
  ArrowSQLRunnerImpl::get()->cta(query_string, sqlite_query_string, device_type);
}

void c_arrow(const std::string& query_string,
             const ExecutorDeviceType device_type,
             size_t min_result_size_for_bulk_dictionary_fetch,
             double max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch) {
  ArrowSQLRunnerImpl::get()->c_arrow(
      query_string,
      device_type,
      min_result_size_for_bulk_dictionary_fetch,
      max_dictionary_to_result_size_ratio_for_bulk_dictionary_fetch);
}

void clearCpuMemory() {
  ArrowSQLRunnerImpl::get()->clearCpuMemory();
}

BufferPoolStats getBufferPoolStats(const Data_Namespace::MemoryLevel memory_level) {
  return ArrowSQLRunnerImpl::get()->getBufferPoolStats(memory_level);
}

std::shared_ptr<ArrowStorage> getStorage() {
  return ArrowSQLRunnerImpl::get()->getStorage();
}

SchemaProviderPtr getSchemaProvider() {
  return ArrowSQLRunnerImpl::get()->getSchemaProvider();
}

std::shared_ptr<hdk::ResultSetRegistry> getResultSetRegistry() {
  return ArrowSQLRunnerImpl::get()->getResultSetRegistry();
}

DataMgr* getDataMgr() {
  return ArrowSQLRunnerImpl::get()->getDataMgr();
}

Executor* getExecutor() {
  return ArrowSQLRunnerImpl::get()->getExecutor();
}

CalciteMgr* getCalcite() {
  return ArrowSQLRunnerImpl::get()->getCalcite();
}

std::unique_ptr<RelAlgExecutor> makeRelAlgExecutor(const std::string& query_str) {
  return ArrowSQLRunnerImpl::get()->makeRelAlgExecutor(query_str);
}

}  // namespace TestHelpers::ArrowSQLRunner
