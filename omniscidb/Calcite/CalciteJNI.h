/*
 * Copyright 2022 Intel Corporation.
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

#include <future>
#include <queue>

#include "QueryEngine/ExtensionFunctionsWhitelist.h"
#include "SchemaMgr/SchemaProvider.h"
#include "Shared/Config.h"

struct FilterPushDownInfo {
  int input_prev;
  int input_start;
  int input_next;
};

class CalciteJNI;

/**
 * Run CalciteJNI on a single thread.
 */
class CalciteMgr {
 public:
  using Task = std::packaged_task<std::string(CalciteJNI* calcite_jni)>;

  CalciteMgr(const CalciteMgr&) = delete;

  ~CalciteMgr();

  static CalciteMgr* get(const std::string& udf_filename = "",
                         size_t calcite_max_mem_mb = 1024);

  std::string process(const std::string& db_name,
                      const std::string& sql_string,
                      SchemaProvider* schema_provider,
                      Config* config,
                      const std::vector<FilterPushDownInfo>& filter_push_down_info = {},
                      const bool legacy_syntax = false,
                      const bool is_explain = false,
                      const bool is_view_optimize = false);
  std::string getExtensionFunctionWhitelist();
  std::string getUserDefinedFunctionWhitelist();
  std::string getRuntimeExtensionFunctionWhitelist();

  void setRuntimeExtensionFunctions(const std::vector<ExtensionFunction>& udfs,
                                    bool is_runtime = true);

 private:
  explicit CalciteMgr(const std::string& udf_filename, size_t calcite_max_mem_mb);

  void worker(const std::string& udf_filename, size_t calcite_max_mem_mb);

  void submitTaskToQueue(Task&& task);

  std::mutex queue_mutex_;
  std::condition_variable worker_cv_;
  std::thread worker_;

  std::queue<Task> queue_;

  bool should_exit_{false};
  static std::once_flag instance_init_flag_;
  static std::unique_ptr<CalciteMgr> instance_;
};
