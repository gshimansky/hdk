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

#include "QueryFragmentDescriptor.h"

#include "DataMgr/DataMgr.h"
#include "QueryEngine/Execute.h"
#include "Shared/misc.h"

#include "QueryEngine/CostModel/Dispatchers/DefaultExecutionPolicy.h"

QueryFragmentDescriptor::QueryFragmentDescriptor(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<InputTableInfo>& query_infos,
    const std::vector<Buffer_Namespace::MemoryInfo>& gpu_mem_infos,
    const double gpu_input_mem_limit_percent,
    std::vector<size_t> allowed_outer_fragment_indices)
    : allowed_outer_fragment_indices_(allowed_outer_fragment_indices)
    , gpu_input_mem_limit_percent_(gpu_input_mem_limit_percent) {
  const size_t input_desc_count{ra_exe_unit.input_descs.size()};
  CHECK_EQ(query_infos.size(), input_desc_count);
  for (size_t table_idx = 0; table_idx < input_desc_count; ++table_idx) {
    const auto table_ref = ra_exe_unit.input_descs[table_idx].getTableRef();
    if (!selected_tables_fragments_.count(table_ref)) {
      selected_tables_fragments_[table_ref] = &query_infos[table_idx].info.fragments;
    }
  }

  for (size_t device_id = 0; device_id < gpu_mem_infos.size(); device_id++) {
    const auto& gpu_mem_info = gpu_mem_infos[device_id];
    available_gpu_mem_bytes_[device_id] =
        gpu_mem_info.maxNumPages * gpu_mem_info.pageSize;
  }
}

void QueryFragmentDescriptor::computeAllTablesFragments(
    std::map<TableRef, const TableFragments*>& all_tables_fragments,
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<InputTableInfo>& query_infos) {
  for (size_t tab_idx = 0; tab_idx < ra_exe_unit.input_descs.size(); ++tab_idx) {
    int db_id = ra_exe_unit.input_descs[tab_idx].getDatabaseId();
    int table_id = ra_exe_unit.input_descs[tab_idx].getTableId();
    TableRef table_ref{db_id, table_id};
    CHECK_EQ(query_infos[tab_idx].table_id, table_id);
    const auto& fragments = query_infos[tab_idx].info.fragments;
    if (!all_tables_fragments.count(table_ref)) {
      all_tables_fragments.insert(std::make_pair(table_ref, &fragments));
    }
  }
}

void QueryFragmentDescriptor::buildFragmentKernelMap(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<uint64_t>& frag_offsets,
    const policy::ExecutionPolicy* policy,
    const int device_count,
    const bool enable_multifrag_kernels,
    Executor* executor,
    compiler::CodegenTraitsDescriptor cgen_traits_desc) {
  // For joins, only consider the cardinality of the LHS
  // columns in the bytes per row count.
  std::set<int> lhs_table_ids;
  for (const auto& input_desc : ra_exe_unit.input_descs) {
    if (input_desc.getNestLevel() == 0) {
      lhs_table_ids.insert(input_desc.getTableId());
    }
  }

  const auto num_bytes_for_row = executor->getNumBytesForFetchedRow(lhs_table_ids);

  if (ra_exe_unit.union_all) {
    buildFragmentPerKernelMapForUnion(ra_exe_unit,
                                      frag_offsets,
                                      policy,
                                      device_count,
                                      num_bytes_for_row,
                                      executor,
                                      cgen_traits_desc);
  } else if (enable_multifrag_kernels) {
    buildMultifragKernelMap(ra_exe_unit,
                            frag_offsets,
                            policy,
                            device_count,
                            num_bytes_for_row,
                            executor,
                            cgen_traits_desc);
  } else {
    buildFragmentPerKernelMap(ra_exe_unit,
                              frag_offsets,
                              policy,
                              device_count,
                              num_bytes_for_row,
                              executor,
                              cgen_traits_desc);
  }
}

void QueryFragmentDescriptor::buildFragmentPerKernelForTable(
    const TableFragments* fragments,
    const RelAlgExecutionUnit& ra_exe_unit,
    const InputDescriptor& table_desc,
    const std::vector<uint64_t>& frag_offsets,
    const policy::ExecutionPolicy* policy,
    const int device_count,
    const size_t num_bytes_for_row,
    const std::optional<size_t> table_desc_offset,
    Executor* executor,
    compiler::CodegenTraitsDescriptor cgen_traits_desc) {
  for (size_t i = 0; i < fragments->size(); i++) {
    if (!allowed_outer_fragment_indices_.empty()) {
      if (std::find(allowed_outer_fragment_indices_.begin(),
                    allowed_outer_fragment_indices_.end(),
                    i) == allowed_outer_fragment_indices_.end()) {
        continue;
      }
    }

    const auto& fragment = (*fragments)[i];
    const auto skip_frag = executor->skipFragment(table_desc,
                                                  fragment,
                                                  ra_exe_unit.simple_quals,
                                                  frag_offsets,
                                                  i,
                                                  cgen_traits_desc);
    if (skip_frag.first) {
      continue;
    }
    rowid_lookup_key_ = std::max(rowid_lookup_key_, skip_frag.second);

    const auto [device_type, device_id] =
        policy->scheduleSingleFragment(fragment, i, fragments->size());
    const int chosen_device_count =
        device_type == ExecutorDeviceType::CPU ? 1 : device_count;
    CHECK_GT(chosen_device_count, 0);

    if (device_type == ExecutorDeviceType::GPU) {
      checkDeviceMemoryUsage(fragment, device_id, num_bytes_for_row);
    }

    ExecutionKernelDescriptor execution_kernel_desc{
        device_id, {}, fragment.getNumTuples()};
    if (table_desc_offset) {
      const auto frag_ids =
          executor->getTableFragmentIndices(ra_exe_unit,
                                            device_type,
                                            *table_desc_offset,
                                            i,
                                            selected_tables_fragments_,
                                            executor->getInnerTabIdToJoinCond());
      const auto db_id = ra_exe_unit.input_descs[*table_desc_offset].getDatabaseId();
      const auto table_id = ra_exe_unit.input_descs[*table_desc_offset].getTableId();
      execution_kernel_desc.fragments.emplace_back(
          FragmentsPerTable{db_id, table_id, frag_ids});

    } else {
      for (size_t j = 0; j < ra_exe_unit.input_descs.size(); ++j) {
        const auto frag_ids =
            executor->getTableFragmentIndices(ra_exe_unit,
                                              device_type,
                                              j,
                                              i,
                                              selected_tables_fragments_,
                                              executor->getInnerTabIdToJoinCond());
        const auto db_id = ra_exe_unit.input_descs[j].getDatabaseId();
        const auto table_id = ra_exe_unit.input_descs[j].getTableId();
        auto table_frags_it = selected_tables_fragments_.find({db_id, table_id});
        CHECK(table_frags_it != selected_tables_fragments_.end());

        execution_kernel_desc.fragments.emplace_back(
            FragmentsPerTable{db_id, table_id, frag_ids});
      }
    }

    auto itr = execution_kernels_per_device_[device_type].find(device_id);
    if (itr == execution_kernels_per_device_[device_type].end()) {
      auto const pair = execution_kernels_per_device_[device_type].insert(std::make_pair(
          device_id,
          std::vector<ExecutionKernelDescriptor>{std::move(execution_kernel_desc)}));
      CHECK(pair.second);
    } else {
      itr->second.emplace_back(std::move(execution_kernel_desc));
    }
  }
}

void QueryFragmentDescriptor::buildFragmentPerKernelMapForUnion(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<uint64_t>& frag_offsets,
    const policy::ExecutionPolicy* policy,
    const int device_count,
    const size_t num_bytes_for_row,
    Executor* executor,
    compiler::CodegenTraitsDescriptor cgen_traits_desc) {
  const auto& schema_provider = executor->getSchemaProvider();

  for (size_t j = 0; j < ra_exe_unit.input_descs.size(); ++j) {
    auto const& table_desc = ra_exe_unit.input_descs[j];
    auto table_ref = table_desc.getTableRef();
    TableFragments const* fragments = selected_tables_fragments_.at(table_ref);

    buildFragmentPerKernelForTable(fragments,
                                   ra_exe_unit,
                                   table_desc,
                                   frag_offsets,
                                   policy,
                                   device_count,
                                   num_bytes_for_row,
                                   j,
                                   executor,
                                   cgen_traits_desc);

    std::vector<int> table_cpu_ids =
        std::accumulate(execution_kernels_per_device_[ExecutorDeviceType::CPU][0].begin(),
                        execution_kernels_per_device_[ExecutorDeviceType::CPU][0].end(),
                        std::vector<int>(),
                        [](auto&& vec, auto& exe_kern) {
                          vec.push_back(exe_kern.fragments[0].table_id);
                          return vec;
                        });
    std::vector<int> table_gpu_ids =
        std::accumulate(execution_kernels_per_device_[ExecutorDeviceType::GPU][0].begin(),
                        execution_kernels_per_device_[ExecutorDeviceType::GPU][0].end(),
                        std::vector<int>(),
                        [](auto&& vec, auto& exe_kern) {
                          vec.push_back(exe_kern.fragments[0].table_id);
                          return vec;
                        });
    VLOG(1) << "execution_kernels_per_device_[CPU].size()="
            << execution_kernels_per_device_[ExecutorDeviceType::CPU].size()
            << " execution_kernels_per_device_[CPU][0][*].fragments[0].table_id="
            << shared::printContainer(table_cpu_ids);
    VLOG(1) << "execution_kernels_per_device_[GPU].size()="
            << execution_kernels_per_device_[ExecutorDeviceType::GPU].size()
            << " execution_kernels_per_device_[GPU][0][*].fragments[0].table_id="
            << shared::printContainer(table_gpu_ids);
  }
}

void QueryFragmentDescriptor::buildFragmentPerKernelMap(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<uint64_t>& frag_offsets,
    const policy::ExecutionPolicy* policy,
    const int device_count,
    const size_t num_bytes_for_row,
    Executor* executor,
    compiler::CodegenTraitsDescriptor cgen_traits_desc) {
  const auto& outer_table_desc = ra_exe_unit.input_descs.front();
  const auto outer_table_ref = outer_table_desc.getTableRef();
  auto it = selected_tables_fragments_.find(outer_table_ref);
  CHECK(it != selected_tables_fragments_.end());
  const auto outer_fragments = it->second;
  outer_fragments_size_ = outer_fragments->size();

  buildFragmentPerKernelForTable(outer_fragments,
                                 ra_exe_unit,
                                 outer_table_desc,
                                 frag_offsets,
                                 policy,
                                 device_count,
                                 num_bytes_for_row,
                                 std::nullopt,
                                 executor,
                                 cgen_traits_desc);
}

void QueryFragmentDescriptor::buildMultifragKernelMap(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<uint64_t>& frag_offsets,
    const policy::ExecutionPolicy* policy,
    const int device_count,
    const size_t num_bytes_for_row,
    Executor* executor,
    compiler::CodegenTraitsDescriptor cgen_traits_desc) {
  // Allocate all the fragments of the tables involved in the query to available
  // devices. The basic idea: the device is decided by the outer table in the
  // query (the first table in a join) and we need to broadcast the fragments
  // in the inner table to each device. Sharding will change this model.
  const auto& outer_table_desc = ra_exe_unit.input_descs.front();
  const auto outer_table_ref = outer_table_desc.getTableRef();
  auto it = selected_tables_fragments_.find(outer_table_ref);
  CHECK(it != selected_tables_fragments_.end());
  const auto outer_fragments = it->second;
  outer_fragments_size_ = outer_fragments->size();

  const auto inner_table_id_to_join_condition = executor->getInnerTabIdToJoinCond();

  for (size_t outer_frag_id = 0; outer_frag_id < outer_fragments->size();
       ++outer_frag_id) {
    if (!allowed_outer_fragment_indices_.empty()) {
      if (std::find(allowed_outer_fragment_indices_.begin(),
                    allowed_outer_fragment_indices_.end(),
                    outer_frag_id) == allowed_outer_fragment_indices_.end()) {
        continue;
      }
    }

    const auto& fragment = (*outer_fragments)[outer_frag_id];
    auto skip_frag = executor->skipFragment(outer_table_desc,
                                            fragment,
                                            ra_exe_unit.simple_quals,
                                            frag_offsets,
                                            outer_frag_id,
                                            cgen_traits_desc);
    if (skip_frag == std::pair<bool, int64_t>(false, -1)) {
      skip_frag = executor->skipFragmentInnerJoins(outer_table_desc,
                                                   ra_exe_unit,
                                                   fragment,
                                                   frag_offsets,
                                                   outer_frag_id,
                                                   cgen_traits_desc);
    }
    if (skip_frag.first) {
      continue;
    }
    auto [device_type, device_id] =
        policy->scheduleSingleFragment(fragment, outer_frag_id, outer_fragments_size_);

    if (device_type == ExecutorDeviceType::GPU) {
      checkDeviceMemoryUsage(fragment, device_id, num_bytes_for_row);
    }
    for (size_t j = 0; j < ra_exe_unit.input_descs.size(); ++j) {
      const auto db_id = ra_exe_unit.input_descs[j].getDatabaseId();
      const auto table_id = ra_exe_unit.input_descs[j].getTableId();
      auto table_frags_it = selected_tables_fragments_.find({db_id, table_id});
      CHECK(table_frags_it != selected_tables_fragments_.end());
      const auto frag_ids =
          executor->getTableFragmentIndices(ra_exe_unit,
                                            device_type,
                                            j,
                                            outer_frag_id,
                                            selected_tables_fragments_,
                                            inner_table_id_to_join_condition);

      if (execution_kernels_per_device_[device_type].find(device_id) ==
          execution_kernels_per_device_[device_type].end()) {
        std::vector<ExecutionKernelDescriptor> kernel_descs{
            ExecutionKernelDescriptor{device_id, FragmentsList{}, std::nullopt}};
        CHECK(execution_kernels_per_device_[device_type]
                  .insert(std::make_pair(device_id, kernel_descs))
                  .second);
      }

      // Multifrag kernels only have one execution kernel per device. Grab the execution
      // kernel object and push back into its fragments list.
      CHECK_EQ(execution_kernels_per_device_[device_type][device_id].size(), size_t(1));
      auto& execution_kernel =
          execution_kernels_per_device_[device_type][device_id].front();

      auto& kernel_frag_list = execution_kernel.fragments;
      if (kernel_frag_list.size() < j + 1) {
        kernel_frag_list.emplace_back(FragmentsPerTable{db_id, table_id, frag_ids});
      } else {
        CHECK_EQ(kernel_frag_list[j].table_id, table_id);
        auto& curr_frag_ids = kernel_frag_list[j].fragment_ids;
        for (const int frag_id : frag_ids) {
          if (std::find(curr_frag_ids.begin(), curr_frag_ids.end(), frag_id) ==
              curr_frag_ids.end()) {
            curr_frag_ids.push_back(frag_id);
          }
        }
      }
    }
    rowid_lookup_key_ = std::max(rowid_lookup_key_, skip_frag.second);
  }
}

namespace {

bool is_sample_query(const RelAlgExecutionUnit& ra_exe_unit) {
  const bool result = ra_exe_unit.input_descs.size() == 1 &&
                      ra_exe_unit.simple_quals.empty() && ra_exe_unit.quals.empty() &&
                      ra_exe_unit.sort_info.order_entries.empty() &&
                      ra_exe_unit.scan_limit;
  if (result) {
    CHECK_EQ(size_t(1), ra_exe_unit.groupby_exprs.size());
    CHECK(!ra_exe_unit.groupby_exprs.front());
  }
  return result;
}

}  // namespace

bool QueryFragmentDescriptor::terminateDispatchMaybe(
    size_t& tuple_count,
    const RelAlgExecutionUnit& ra_exe_unit,
    const ExecutionKernelDescriptor& kernel) const {
  const auto sample_query_limit =
      ra_exe_unit.sort_info.limit + ra_exe_unit.sort_info.offset;
  if (!kernel.outer_tuple_count) {
    return false;
  } else {
    tuple_count += *kernel.outer_tuple_count;
    if (is_sample_query(ra_exe_unit) && sample_query_limit > 0 &&
        tuple_count >= sample_query_limit) {
      return true;
    }
  }
  return false;
}

void QueryFragmentDescriptor::checkDeviceMemoryUsage(const FragmentInfo& fragment,
                                                     const int device_id,
                                                     const size_t num_bytes_for_row) {
  CHECK_GE(device_id, 0);
  tuple_count_per_gpu_device_[device_id] += fragment.getNumTuples();
  const size_t gpu_bytes_limit =
      available_gpu_mem_bytes_[device_id] * gpu_input_mem_limit_percent_;
  if (tuple_count_per_gpu_device_[device_id] * num_bytes_for_row > gpu_bytes_limit) {
    LOG(WARNING) << "Not enough memory on device " << device_id
                 << " for input chunks totaling "
                 << tuple_count_per_gpu_device_[device_id] * num_bytes_for_row
                 << " bytes (available device memory: " << gpu_bytes_limit << " bytes)";
    throw QueryMustRunOnCpu();
  }
}

std::ostream& operator<<(std::ostream& os, FragmentsPerTable const& fragments_per_table) {
  os << "table_id(" << fragments_per_table.table_id << ") fragment_ids";
  for (size_t i = 0; i < fragments_per_table.fragment_ids.size(); ++i) {
    os << (i ? ' ' : '(') << fragments_per_table.fragment_ids[i];
  }
  return os << ')';
}
