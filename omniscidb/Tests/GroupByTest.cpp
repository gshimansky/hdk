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

#include "TestHelpers.h"

#include "ArrowSQLRunner/ArrowSQLRunner.h"
#include "DataMgr/DataMgrBufferProvider.h"
#include "DataMgr/DataMgrDataProvider.h"
#include "QueryEngine/CardinalityEstimator.h"
#include "QueryEngine/Execute.h"
#include "QueryEngine/InputMetadata.h"

#include <boost/filesystem.hpp>
#include <fstream>

EXTERN extern bool g_is_test_env;

using namespace TestHelpers;
using namespace TestHelpers::ArrowSQLRunner;

bool skip_tests(const ExecutorDeviceType device_type) {
#ifdef HAVE_CUDA
  return device_type == ExecutorDeviceType::GPU && !gpusPresent();
#else
  return device_type == ExecutorDeviceType::GPU;
#endif
}

#define SKIP_NO_GPU()                                        \
  if (skip_tests(dt)) {                                      \
    CHECK(dt == ExecutorDeviceType::GPU);                    \
    LOG(WARNING) << "GPU not available, skipping GPU tests"; \
    continue;                                                \
  }
class HighCardinalityStringEnv : public ::testing::Test {
 protected:
  void SetUp() override {
    createTable("high_cardinality_str",
                {{"x", ctx().int32()}, {"str", ctx().extDict(ctx().text(), 0)}});
    insertCsvValues("high_cardinality_str", "1,hi\n2,bye");
  }

  void TearDown() override { dropTable("high_cardinality_str"); }
};

TEST_F(HighCardinalityStringEnv, PerfectHashNoFallback) {
  // make our own executor with a custom col ranges cache
  auto executor = Executor::getExecutor(getDataMgr());
  auto storage = getStorage();
  executor->setSchemaProvider(storage);

  auto tinfo = storage->getTableInfo(TEST_DB_ID, "high_cardinality_str");
  auto colStrInfo = storage->getColumnInfo(*tinfo, "str");
  auto colXInfo = storage->getColumnInfo(*tinfo, "x");

  InputColDescriptor group_col_desc{colStrInfo, 0};
  InputColDescriptor filter_col_desc{colXInfo, 0};

  std::unordered_set<InputColDescriptor> col_descs{group_col_desc, filter_col_desc};
  std::unordered_set<std::pair<int, int>> phys_table_ids;
  phys_table_ids.insert({group_col_desc.getDatabaseId(), group_col_desc.getTableId()});
  executor->setupCaching(getDataMgr()->getDataProvider(), col_descs, phys_table_ids);

  auto input_descs =
      std::vector<InputDescriptor>{InputDescriptor(tinfo->db_id, tinfo->table_id, 0)};
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  input_col_descs.push_back(std::make_shared<InputColDescriptor>(colStrInfo, 0));
  input_col_descs.push_back(std::make_shared<InputColDescriptor>(colXInfo, 0));

  std::vector<InputTableInfo> table_infos = get_table_infos(input_descs, executor.get());

  auto count_expr = hdk::ir::makeExpr<hdk::ir::AggExpr>(
      ctx().int64(), hdk::ir::AggType::kCount, nullptr, false, nullptr);
  auto group_expr = hdk::ir::makeExpr<hdk::ir::ColumnVar>(colStrInfo, 0);
  auto filter_col_expr = hdk::ir::makeExpr<hdk::ir::ColumnVar>(colXInfo, 0);
  Datum d{int64_t(1)};
  auto filter_val_expr = hdk::ir::makeExpr<hdk::ir::Constant>(ctx().int32(), false, d);
  auto simple_filter_expr = hdk::ir::makeExpr<hdk::ir::BinOper>(ctx().boolean(),
                                                                false,
                                                                hdk::ir::OpType::kEq,
                                                                hdk::ir::Qualifier::kOne,
                                                                filter_col_expr,
                                                                filter_val_expr);
  RelAlgExecutionUnit ra_exe_unit{input_descs,
                                  input_col_descs,
                                  {simple_filter_expr},
                                  {},
                                  {},
                                  {group_expr},
                                  {count_expr.get()},
                                  nullptr,
                                  SortInfo{},
                                  0};

  ColumnCacheMap column_cache;
  size_t max_groups_buffer_entry_guess = 1;

  auto result =
      executor->executeWorkUnit(max_groups_buffer_entry_guess,
                                /*is_agg=*/true,
                                table_infos,
                                ra_exe_unit,
                                CompilationOptions::defaults(ExecutorDeviceType::CPU),
                                ExecutionOptions::fromConfig(executor->getConfig()),
                                /*has_cardinality_estimation=*/false,
                                getDataMgr()->getDataProvider(),
                                column_cache)[0];
  EXPECT_TRUE(result);
  EXPECT_EQ(result->rowCount(), size_t(1));
  auto row = result->getNextRow(false, false);
  EXPECT_EQ(row.size(), size_t(1));
  EXPECT_EQ(v<int64_t>(row[0]), 1);
}

std::unordered_set<InputColDescriptor> setup_str_col_caching(
    InputColDescriptor& group_col_desc,
    const int64_t min,
    const int64_t max,
    InputColDescriptor& filter_col_desc,
    DataProvider* data_provider,
    Executor* executor) {
  std::unordered_set<InputColDescriptor> col_descs{group_col_desc, filter_col_desc};
  std::unordered_set<std::pair<int, int>> phys_table_ids;
  phys_table_ids.insert({group_col_desc.getDatabaseId(), group_col_desc.getTableId()});
  executor->setupCaching(data_provider, col_descs, phys_table_ids);
  auto filter_col_range = executor->getColRange({filter_col_desc.getColId(),
                                                 filter_col_desc.getTableId(),
                                                 filter_col_desc.getDatabaseId()});
  // reset the col range to trigger the optimization
  AggregatedColRange col_range_cache;
  col_range_cache.setColRange({group_col_desc.getColId(),
                               group_col_desc.getTableId(),
                               group_col_desc.getDatabaseId()},
                              ExpressionRange::makeIntRange(min, max, 0, false));
  col_range_cache.setColRange({filter_col_desc.getColId(),
                               filter_col_desc.getTableId(),
                               group_col_desc.getDatabaseId()},
                              filter_col_range);
  executor->setColRangeCache(col_range_cache);
  return col_descs;
}

TEST_F(HighCardinalityStringEnv, BaselineFallbackTest) {
  // make our own executor with a custom col ranges cache
  auto executor = Executor::getExecutor(getDataMgr());
  auto storage = getStorage();
  executor->setSchemaProvider(storage);

  auto tinfo = storage->getTableInfo(TEST_DB_ID, "high_cardinality_str");
  auto colStrInfo = storage->getColumnInfo(*tinfo, "str");
  auto colXInfo = storage->getColumnInfo(*tinfo, "x");

  InputColDescriptor group_col_desc{colStrInfo, 0};
  InputColDescriptor filter_col_desc{colXInfo, 0};

  // 134217728 is 1 additional value over the max buffer size
  auto phys_inputs = setup_str_col_caching(group_col_desc,
                                           /*min=*/0,
                                           /*max=*/134217728,
                                           filter_col_desc,
                                           getDataMgr()->getDataProvider(),
                                           executor.get());

  auto input_descs =
      std::vector<InputDescriptor>{InputDescriptor(tinfo->db_id, tinfo->table_id, 0)};
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  input_col_descs.push_back(std::make_shared<InputColDescriptor>(colStrInfo, 0));
  input_col_descs.push_back(std::make_shared<InputColDescriptor>(colXInfo, 0));

  std::vector<InputTableInfo> table_infos = get_table_infos(input_descs, executor.get());

  auto count_expr = hdk::ir::makeExpr<hdk::ir::AggExpr>(
      ctx().int64(), hdk::ir::AggType::kCount, nullptr, false, nullptr);
  auto group_expr = hdk::ir::makeExpr<hdk::ir::ColumnVar>(colStrInfo, 0);
  auto filter_col_expr = hdk::ir::makeExpr<hdk::ir::ColumnVar>(colXInfo, 0);
  Datum d{int64_t(1)};
  auto filter_val_expr = hdk::ir::makeExpr<hdk::ir::Constant>(ctx().int32(), false, d);
  auto simple_filter_expr = hdk::ir::makeExpr<hdk::ir::BinOper>(ctx().boolean(),
                                                                false,
                                                                hdk::ir::OpType::kEq,
                                                                hdk::ir::Qualifier::kOne,
                                                                filter_col_expr,
                                                                filter_val_expr);
  RelAlgExecutionUnit ra_exe_unit{input_descs,
                                  input_col_descs,
                                  {simple_filter_expr},
                                  {},
                                  {},
                                  {group_expr},
                                  {count_expr.get()},
                                  nullptr,
                                  SortInfo{},
                                  0};

  ColumnCacheMap column_cache;
  size_t max_groups_buffer_entry_guess = 1;
  // expect throw w/out cardinality estimation
  EXPECT_THROW(
      executor->executeWorkUnit(max_groups_buffer_entry_guess,
                                /*is_agg=*/true,
                                table_infos,
                                ra_exe_unit,
                                CompilationOptions::defaults(ExecutorDeviceType::CPU),
                                ExecutionOptions::fromConfig(executor->getConfig()),
                                /*has_cardinality_estimation=*/false,
                                getDataMgr()->getDataProvider(),
                                column_cache),
      CardinalityEstimationRequired);

  auto result =
      executor->executeWorkUnit(max_groups_buffer_entry_guess,
                                /*is_agg=*/true,
                                table_infos,
                                ra_exe_unit,
                                CompilationOptions::defaults(ExecutorDeviceType::CPU),
                                ExecutionOptions::fromConfig(executor->getConfig()),
                                /*has_cardinality_estimation=*/true,
                                getDataMgr()->getDataProvider(),
                                column_cache)[0];
  EXPECT_TRUE(result);
  EXPECT_EQ(result->rowCount(), size_t(1));
  auto row = result->getNextRow(false, false);
  EXPECT_EQ(row.size(), size_t(1));
  EXPECT_EQ(v<int64_t>(row[0]), 1);
}

TEST_F(HighCardinalityStringEnv, BaselineNoFilters) {
  // make our own executor with a custom col ranges cache
  auto executor = Executor::getExecutor(getDataMgr());
  auto storage = getStorage();
  executor->setSchemaProvider(storage);

  auto tinfo = storage->getTableInfo(TEST_DB_ID, "high_cardinality_str");
  auto colStrInfo = storage->getColumnInfo(*tinfo, "str");
  auto colXInfo = storage->getColumnInfo(*tinfo, "x");

  InputColDescriptor group_col_desc{colStrInfo, 0};
  InputColDescriptor filter_col_desc{colXInfo, 0};

  // 134217728 is 1 additional value over the max buffer size
  auto phys_inputs = setup_str_col_caching(group_col_desc,
                                           /*min=*/0,
                                           /*max=*/134217728,
                                           filter_col_desc,
                                           getDataMgr()->getDataProvider(),
                                           executor.get());

  auto input_descs =
      std::vector<InputDescriptor>{InputDescriptor(tinfo->db_id, tinfo->table_id, 0)};
  std::list<std::shared_ptr<const InputColDescriptor>> input_col_descs;
  input_col_descs.push_back(std::make_shared<InputColDescriptor>(colStrInfo, 0));
  input_col_descs.push_back(std::make_shared<InputColDescriptor>(colXInfo, 0));

  std::vector<InputTableInfo> table_infos = get_table_infos(input_descs, executor.get());

  auto count_expr = hdk::ir::makeExpr<hdk::ir::AggExpr>(
      ctx().int64(), hdk::ir::AggType::kCount, nullptr, false, nullptr);
  auto group_expr = hdk::ir::makeExpr<hdk::ir::ColumnVar>(colStrInfo, 0);

  RelAlgExecutionUnit ra_exe_unit{input_descs,
                                  input_col_descs,
                                  {},
                                  {},
                                  {},
                                  {group_expr},
                                  {count_expr.get()},
                                  nullptr,
                                  SortInfo{},
                                  0};

  ColumnCacheMap column_cache;
  size_t max_groups_buffer_entry_guess = 1;
  // no filters, so expect no throw w/out cardinality estimation
  auto result =
      executor->executeWorkUnit(max_groups_buffer_entry_guess,
                                /*is_agg=*/true,
                                table_infos,
                                ra_exe_unit,
                                CompilationOptions::defaults(ExecutorDeviceType::CPU),
                                ExecutionOptions::fromConfig(executor->getConfig()),
                                /*has_cardinality_estimation=*/false,
                                getDataMgr()->getDataProvider(),
                                column_cache)[0];
  EXPECT_TRUE(result);
  EXPECT_EQ(result->rowCount(), size_t(2));
  {
    auto row = result->getNextRow(false, false);
    EXPECT_EQ(row.size(), size_t(1));
    EXPECT_EQ(v<int64_t>(row[0]), 1);
  }
  {
    auto row = result->getNextRow(false, false);
    EXPECT_EQ(row.size(), size_t(1));
    EXPECT_EQ(v<int64_t>(row[0]), 1);
  }
}

class LowCardinalityThresholdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    createTable("low_cardinality",
                {{"fl", ctx().extDict(ctx().text(), 0)},
                 {"ar", ctx().extDict(ctx().text(), 0)},
                 {"dep", ctx().extDict(ctx().text(), 0)}});

    std::stringstream ss;
    for (size_t i = 0; i < config().exec.group_by.big_group_threshold; i++) {
      ss << i << ", " << i + 1 << ", " << i + 2 << std::endl;
    }
    insertCsvValues("low_cardinality", ss.str());
  }

  void TearDown() override { dropTable("low_cardinality"); }
};

TEST_F(LowCardinalityThresholdTest, GroupBy) {
  for (auto dt : {ExecutorDeviceType::CPU, ExecutorDeviceType::GPU}) {
    SKIP_NO_GPU();

    auto result = run_multiple_agg(
        R"(select fl,ar,dep from low_cardinality group by fl,ar,dep;)", dt);
    EXPECT_EQ(result->rowCount(), config().exec.group_by.big_group_threshold);
  }
}

class BigCardinalityThresholdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config().exec.watchdog.enable = true;
    initial_baseline_max_groups = config().exec.watchdog.baseline_max_groups;
    config().exec.watchdog.baseline_max_groups =
        config().exec.group_by.big_group_threshold + 1;

    createTable("big_cardinality",
                {{"fl", ctx().extDict(ctx().text(), 0)},
                 {"ar", ctx().extDict(ctx().text(), 0)},
                 {"dep", ctx().extDict(ctx().text(), 0)}});

    std::stringstream ss;
    // add enough groups to trigger the watchdog exception if we use a poor estimate
    for (size_t i = 0; i < config().exec.watchdog.baseline_max_groups; i++) {
      ss << i << ", " << i + 1 << ", " << i + 2 << std::endl;
    }
    insertCsvValues("big_cardinality", ss.str());
  }

  void TearDown() override {
    config().exec.watchdog.enable = false;
    config().exec.watchdog.baseline_max_groups = initial_baseline_max_groups;
    dropTable("big_cardinality");
  }

  size_t initial_baseline_max_groups{0};
};

TEST_F(BigCardinalityThresholdTest, EmptyFilters) {
  for (auto dt : {ExecutorDeviceType::CPU, ExecutorDeviceType::GPU}) {
    SKIP_NO_GPU();

    auto result = run_multiple_agg(
        R"(SELECT fl,ar,dep FROM big_cardinality WHERE fl = 'a' GROUP BY fl,ar,dep;)",
        dt);
    EXPECT_EQ(result->rowCount(), size_t(0));
  }
}

int main(int argc, char** argv) {
  g_is_test_env = true;

  TestHelpers::init_logger_stderr_only(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  namespace po = boost::program_options;

  po::options_description desc("Options");

  logger::LogOptions log_options(argv[0]);
  log_options.max_files_ = 0;  // stderr only by default
  desc.add(log_options.get_options());

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
  po::notify(vm);

  init();

  int err{0};
  try {
    err = RUN_ALL_TESTS();
  } catch (const std::exception& e) {
    LOG(ERROR) << e.what();
  }
  reset();
  return err;
}
