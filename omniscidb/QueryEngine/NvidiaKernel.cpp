#include "NvidiaKernel.h"

#include "../CudaMgr/CudaMgr.h"

#include <glog/logging.h>


namespace {

void fill_options(std::vector<CUjit_option>& option_keys,
                  std::vector<void*>& option_values,
                  const unsigned block_size_x) {
  option_keys.push_back(CU_JIT_LOG_VERBOSE);
  option_values.push_back(reinterpret_cast<void*>(1));
  option_keys.push_back(CU_JIT_THREADS_PER_BLOCK);
  option_values.push_back(reinterpret_cast<void*>(block_size_x));
}

}  // namespace

GpuCompilationContext::GpuCompilationContext(char* ptx,
                                             const std::string& func_name,
                                             const std::string& lib_path,
                                             const int device_id,
                                             const void* cuda_mgr,
                                             const unsigned block_size_x)
  : module_(nullptr)
  , kernel_(nullptr)
  , link_state_(nullptr)
  , device_id_(device_id)
  , cuda_mgr_(cuda_mgr) {
  CHECK(ptx);
  static_cast<const CudaMgr_Namespace::CudaMgr*>(cuda_mgr_)->setContext(device_id_);
  std::vector<CUjit_option> option_keys;
  std::vector<void*> option_values;
  fill_options(option_keys, option_values, block_size_x);
  CHECK_EQ(option_values.size(), option_keys.size());
  unsigned num_options = option_keys.size();
  checkCudaErrors(cuLinkCreate(num_options, &option_keys[0], &option_values[0], &link_state_));
  if (!lib_path.empty()) {
    // How to create a static CUDA library:
    // 1. nvcc -std=c++11 -arch=sm_30 --device-link -c [list of .cu files]
    // 2. nvcc -std=c++11 -arch=sm_30 -lib [list of .o files generated by step 1] -o [library_name.a]
    checkCudaErrors(cuLinkAddFile(link_state_, CU_JIT_INPUT_LIBRARY, lib_path.c_str(),
      num_options, &option_keys[0], &option_values[0]));
  }
  checkCudaErrors(cuLinkAddData(link_state_, CU_JIT_INPUT_PTX, static_cast<void*>(ptx), strlen(ptx) + 1,
    0, num_options, &option_keys[0], &option_values[0]));
  void* cubin;
  size_t cubinSize;
  checkCudaErrors(cuLinkComplete(link_state_, &cubin, &cubinSize));
  checkCudaErrors(cuModuleLoadDataEx(&module_, cubin, num_options, &option_keys[0], &option_values[0]));
  CHECK(module_);
  checkCudaErrors(cuModuleGetFunction(&kernel_, module_, func_name.c_str()));
}

GpuCompilationContext::~GpuCompilationContext() {
  static_cast<const CudaMgr_Namespace::CudaMgr*>(cuda_mgr_)->setContext(device_id_);
  auto status = cuModuleUnload(module_);
  // TODO(alex): handle this race better
  if (status == CUDA_ERROR_DEINITIALIZED) {
    return;
  }
  checkCudaErrors(status);
  checkCudaErrors(cuLinkDestroy(link_state_));
}
