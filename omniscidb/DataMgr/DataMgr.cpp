/**
 * @file    DataMgr.cpp
 * @author Todd Mostak <todd@mapd.com>
 */

#include "DataMgr.h"
#include "../CudaMgr/CudaMgr.h"
#include "BufferMgr/CpuBufferMgr/CpuBufferMgr.h"
#include "BufferMgr/GpuCudaBufferMgr/GpuCudaBufferMgr.h"
#include "FileMgr/FileMgr.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <limits>

using namespace std;
using namespace Buffer_Namespace;
using namespace File_Namespace;

namespace Data_Namespace {

DataMgr::DataMgr(const string& dataDir,
                 const size_t cpuBufferSize,
                 const bool useGpus,
                 const int numGpus,
                 const int startGpu,
                 const size_t reservedGpuMem,
                 const int start_epoch,
                 const size_t numReaderThreads)
    : dataDir_(dataDir) {
  if (useGpus) {
    try {
      cudaMgr_ = new CudaMgr_Namespace::CudaMgr(numGpus, startGpu);
      reservedGpuMem_ = reservedGpuMem;
      hasGpus_ = true;
    } catch (std::runtime_error& error) {
      hasGpus_ = false;
      cudaMgr_ = 0;
    }
  } else {
    hasGpus_ = false;
    cudaMgr_ = 0;
  }

  populateMgrs(cpuBufferSize, numReaderThreads, start_epoch);
}

DataMgr::~DataMgr() {
  int numLevels = bufferMgrs_.size();
  for (int level = numLevels - 1; level >= 0; --level) {
    for (size_t device = 0; device < bufferMgrs_[level].size(); device++) {
      delete bufferMgrs_[level][device];
    }
  }
  if (hasGpus_) {
    delete cudaMgr_;
  }
}

size_t DataMgr::getTotalSystemMemory() {
#ifdef __APPLE__
  int mib[2];
  size_t physical_memory;
  size_t length;
  // Get the Physical memory size
  mib[0] = CTL_HW;
  mib[1] = HW_MEMSIZE;
  length = sizeof(size_t);
  sysctl(mib, 2, &physical_memory, &length, NULL, 0);
  return physical_memory;

#else
  long pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGE_SIZE);
  return pages * page_size;
#endif
}

void DataMgr::populateMgrs(const size_t userSpecifiedCpuBufferSize,
                           const size_t userSpecifiedNumReaderThreads,
                           const int start_epoch) {
  bufferMgrs_.resize(2);
  bufferMgrs_[0].push_back(new FileMgr(0, dataDir_, userSpecifiedNumReaderThreads, start_epoch));
  levelSizes_.push_back(1);
  size_t cpuBufferSize = userSpecifiedCpuBufferSize;
  if (cpuBufferSize == 0)                          // if size is not specified
    cpuBufferSize = getTotalSystemMemory() * 0.8;  // should get free memory instead of this ugly heuristic
  size_t cpuSlabSize = std::min(static_cast<size_t>(1L << 32), cpuBufferSize);
  // cpuSlabSize -= cpuSlabSize % 512 == 0 ? 0 : 512 - (cpuSlabSize % 512);
  cpuSlabSize = (cpuSlabSize / 512) * 512;
  LOG(INFO) << "cpuSlabSize is " << (float)cpuSlabSize / (1024 * 1024) << "M";
  if (hasGpus_) {
    LOG(INFO) << "reserved GPU memory is " << (float)reservedGpuMem_ / (1024 * 1024)
              << "M includes render buffer allocation";
    bufferMgrs_.resize(3);
    bufferMgrs_[1].push_back(
        new CpuBufferMgr(0, cpuBufferSize, CUDA_HOST, cudaMgr_, cpuSlabSize, 512, bufferMgrs_[0][0]));
    levelSizes_.push_back(1);
    int numGpus = cudaMgr_->getDeviceCount();
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      size_t gpuMaxMemSize = (cudaMgr_->deviceProperties[gpuNum].globalMem) - (reservedGpuMem_);
      size_t gpuSlabSize = std::min(static_cast<size_t>(1L << 31), gpuMaxMemSize);
      gpuSlabSize -= gpuSlabSize % 512 == 0 ? 0 : 512 - (gpuSlabSize % 512);
      LOG(INFO) << "gpuSlabSize is " << (float)gpuSlabSize / (1024 * 1024) << "M";
      bufferMgrs_[2].push_back(
          new GpuCudaBufferMgr(gpuNum, gpuMaxMemSize, cudaMgr_, gpuSlabSize, 512, bufferMgrs_[1][0]));
    }
    levelSizes_.push_back(numGpus);
  } else {
    bufferMgrs_[1].push_back(
        new CpuBufferMgr(0, cpuBufferSize, CPU_HOST, cudaMgr_, cpuSlabSize, 512, bufferMgrs_[0][0]));
    levelSizes_.push_back(1);
  }
}

memorySummary DataMgr::getMemorySummary() {
  memorySummary ms;
  ms.cpuMemoryInUse = bufferMgrs_[MemoryLevel::CPU_LEVEL][0]->getInUseSize();
  if (hasGpus_) {
    int numGpus = cudaMgr_->getDeviceCount();
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      gpuMemorySummary gms;
      gms.max = bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getMaxSize();
      gms.inUse = bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getInUseSize();
      gms.allocated = bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getAllocated();
      gms.isAllocationCapped = bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->isAllocationCapped();
      ms.gpuSummary.push_back(gms);
    }
  }
  return ms;
}
//  std::ostringstream tss;
//  size_t mb = 1024 * 1024;
//  tss << std::endl;
//  // tss << "CPU RAM TOTAL AVAILABLE   : "  std::fixed << setw(9) << setprecision(2) <<
//  // ((float)bufferMgrs_[MemoryLevel::CPU_LEVEL][0]->getMaxSize() / mb)
//  //    << std::endl;
//  tss << "CPU RAM IN BUFFER USE     : " << std::fixed << setw(9) << setprecision(2)
//      << ((float)bufferMgrs_[MemoryLevel::CPU_LEVEL][0]->getInUseSize() / mb) << " MB" << std::endl;
//  if (hasGpus_) {
//    int numGpus = cudaMgr_->getDeviceCount();
//    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
//      tss << "GPU" << setfill(' ') << setw(2) << gpuNum << " RAM TOTAL AVAILABLE : " << std::fixed << setw(9)
//          << setprecision(2) << ((float)bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getMaxSize() / mb) << " MB"
//          << std::endl;
//      tss << "GPU" << setfill(' ') << setw(2) << gpuNum << " RAM IN BUFFER USE   : " << std::fixed << setw(9)
//          << setprecision(2) << ((float)bufferMgrs_[MemoryLevel::GPU_LEVEL][gpuNum]->getInUseSize() / mb) << " MB"
//          << std::endl;
//    }
//  }
//  return tss.str();
//}

std::string DataMgr::dumpLevel(const MemoryLevel memLevel) {
  // if gpu we need to iterate through all the buffermanagers for each card
  if (memLevel == MemoryLevel::GPU_LEVEL) {
    int numGpus = cudaMgr_->getDeviceCount();
    std::ostringstream tss;
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      tss << bufferMgrs_[memLevel][gpuNum]->printSlabs();
    }
    return tss.str();
  } else {
    return bufferMgrs_[memLevel][0]->printSlabs();
  }
}

void DataMgr::clearMemory(const MemoryLevel memLevel) {
  // if gpu we need to iterate through all the buffermanagers for each card
  if (memLevel == MemoryLevel::GPU_LEVEL) {
    int numGpus = cudaMgr_->getDeviceCount();
    for (int gpuNum = 0; gpuNum < numGpus; ++gpuNum) {
      LOG(INFO) << "clear slabs on gpu " << gpuNum;
      bufferMgrs_[memLevel][gpuNum]->clearSlabs();
    }
  } else {
    bufferMgrs_[memLevel][0]->clearSlabs();
  }
}

bool DataMgr::isBufferOnDevice(const ChunkKey& key, const MemoryLevel memLevel, const int deviceId) {
  return bufferMgrs_[memLevel][deviceId]->isBufferOnDevice(key);
}

void DataMgr::getChunkMetadataVec(std::vector<std::pair<ChunkKey, ChunkMetadata>>& chunkMetadataVec) {
  // Can we always assume this will just be at the disklevel bc we just
  // started?
  bufferMgrs_[0][0]->getChunkMetadataVec(chunkMetadataVec);
}

void DataMgr::getChunkMetadataVecForKeyPrefix(std::vector<std::pair<ChunkKey, ChunkMetadata>>& chunkMetadataVec,
                                              const ChunkKey& keyPrefix) {
  bufferMgrs_[0][0]->getChunkMetadataVecForKeyPrefix(chunkMetadataVec, keyPrefix);
}

AbstractBuffer* DataMgr::createChunkBuffer(const ChunkKey& key, const MemoryLevel memoryLevel, const int deviceId) {
  int level = static_cast<int>(memoryLevel);
  return bufferMgrs_[level][deviceId]->createBuffer(key);
}

AbstractBuffer* DataMgr::getChunkBuffer(const ChunkKey& key,
                                        const MemoryLevel memoryLevel,
                                        const int deviceId,
                                        const size_t numBytes) {
  auto level = static_cast<size_t>(memoryLevel);
  assert(level < levelSizes_.size());     // make sure we have a legit buffermgr
  assert(deviceId < levelSizes_[level]);  // make sure we have a legit buffermgr
  return bufferMgrs_[level][deviceId]->getBuffer(key, numBytes);
}

void DataMgr::deleteChunksWithPrefix(const ChunkKey& keyPrefix) {
  int numLevels = bufferMgrs_.size();
  for (int level = numLevels - 1; level >= 0; --level) {
    for (int device = 0; device < levelSizes_[level]; ++device) {
      bufferMgrs_[level][device]->deleteBuffersWithPrefix(keyPrefix);
    }
  }
}

AbstractBuffer* DataMgr::alloc(const MemoryLevel memoryLevel, const int deviceId, const size_t numBytes) {
  int level = static_cast<int>(memoryLevel);
  assert(deviceId < levelSizes_[level]);
  return bufferMgrs_[level][deviceId]->alloc(numBytes);
}

void DataMgr::free(AbstractBuffer* buffer) {
  int level = static_cast<int>(buffer->getType());
  bufferMgrs_[level][buffer->getDeviceId()]->free(buffer);
}

void DataMgr::freeAllBuffers() {
  ChunkKey keyPrefix = {-1};
  deleteChunksWithPrefix(keyPrefix);
}

void DataMgr::copy(AbstractBuffer* destBuffer, AbstractBuffer* srcBuffer) {
  destBuffer->write(srcBuffer->getMemoryPtr(), srcBuffer->size(), 0, srcBuffer->getType(), srcBuffer->getDeviceId());
}

// could add function below to do arbitrary copies between buffers

// void DataMgr::copy(AbstractBuffer *destBuffer, const AbstractBuffer *srcBuffer, const size_t numBytes, const size_t
// destOffset, const size_t srcOffset) {
//} /

void DataMgr::checkpoint() {
  for (auto levelIt = bufferMgrs_.rbegin(); levelIt != bufferMgrs_.rend(); ++levelIt) {
    // use reverse iterator so we start at GPU level, then CPU then DISK
    for (auto deviceIt = levelIt->begin(); deviceIt != levelIt->end(); ++deviceIt) {
      (*deviceIt)->checkpoint();
    }
  }
}
}  // Data_Namespace
