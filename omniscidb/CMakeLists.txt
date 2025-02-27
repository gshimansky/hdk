cmake_minimum_required(VERSION 3.17)

# Required for LLVM / JIT, alternative to setting CMP0065 to OLD
set(CMAKE_ENABLE_EXPORTS True)

set(ENABLE_CONDA OFF)
if(DEFINED ENV{CONDA_PREFIX})
  set(ENABLE_CONDA ON)
  list(APPEND CMAKE_PREFIX_PATH "$ENV{CONDA_PREFIX}")
  # resolves link issue for zlib 
  link_directories("$ENV{CONDA_PREFIX}/lib")
  # various fixes and workarounds
  add_definitions("-Dsecure_getenv=getenv")
  # fixes `undefined reference to `boost::system::detail::system_category_instance'`:
  add_definitions("-DBOOST_ERROR_CODE_HEADER_ONLY")
  # Adding formating macros
  add_definitions("-D__STDC_FORMAT_MACROS=1")
  # fixes always_inline attribute errors
  add_compile_options("-fno-semantic-interposition")
  # Adding `--sysroot=...` resolves `no member named 'signbit' in the global namespace` error:
  set(CMAKE_SYSROOT "$ENV{CONDA_BUILD_SYSROOT}")
endif(DEFINED ENV{CONDA_PREFIX})

# force `Release` build type if left unspecified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LOWER)

# Force -O0 optimization level for debug builds.
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

if("${CMAKE_VERSION}" VERSION_GREATER 3.11.999)
  cmake_policy(SET CMP0074 NEW)
endif()

find_program(CCACHE_EXE ccache)
if(CCACHE_EXE)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_EXE}")
endif()

option(ENABLE_IWYU "Enable include-what-you-use" OFF)
if(ENABLE_IWYU)
  find_program(IWYU_EXE include-what-you-use)
  if(IWYU_EXE)
    set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${IWYU_EXE}")
  endif()
endif()

project(omnisci)

set(MAPD_EDITION "OS")
set(MAPD_EDITION "${MAPD_EDITION}" CACHE STRING "MapD edition" FORCE)
set_property(CACHE MAPD_EDITION PROPERTY STRINGS "EE" "CE" "OS")
add_definitions("-DMAPD_EDITION_${MAPD_EDITION}")
string(TOLOWER "${MAPD_EDITION}" MAPD_EDITION_LOWER)

set(MAPD_VERSION_MAJOR "5")
set(MAPD_VERSION_MINOR "8")
set(MAPD_VERSION_PATCH "0")
set(MAPD_VERSION_EXTRA "")
set(MAPD_VERSION_RAW "${MAPD_VERSION_MAJOR}.${MAPD_VERSION_MINOR}.${MAPD_VERSION_PATCH}${MAPD_VERSION_EXTRA}")
set(MAPD_IMMERSE_URL "http://builds.mapd.com/frontend/mapd2-dashboard-v2-137-release-prod.zip")
string(TIMESTAMP MAPD_BUILD_DATE "%Y%m%d")

if($ENV{BUILD_NUMBER})
  set(MAPD_BUILD_NUMBER "$ENV{BUILD_NUMBER}")
else()
  set(MAPD_BUILD_NUMBER "dev")
endif()
set(MAPD_VERSION "${MAPD_VERSION_RAW}-${MAPD_BUILD_NUMBER}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/Modules")

add_custom_target(clean-all
  COMMAND ${CMAKE_BUILD_TOOL} clean
 )

macro(set_alternate_linker linker)
  find_program(LINKER_EXECUTABLE ld.${USE_ALTERNATE_LINKER} ${USE_ALTERNATE_LINKER})
  if(LINKER_EXECUTABLE)
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" AND "${CMAKE_CXX_COMPILER_VERSION}" VERSION_LESS 12.0.0)
      add_link_options("-ld-path=${USE_ALTERNATE_LINKER}")
    else()
      add_link_options("-fuse-ld=${USE_ALTERNATE_LINKER}")
    endif()
  else()
    set(USE_ALTERNATE_LINKER "" CACHE STRING "Use alternate linker" FORCE)
  endif()
endmacro()

set(USE_ALTERNATE_LINKER "" CACHE STRING "Use alternate linker. Leave empty for system default; alternatives are 'gold', 'lld', 'bfd', 'mold'")
if(NOT "${USE_ALTERNATE_LINKER}" STREQUAL "")
  set_alternate_linker(${USE_ALTERNATE_LINKER})
endif()

option(PREFER_STATIC_LIBS "Prefer linking against static libraries" OFF)
if(PREFER_STATIC_LIBS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(Arrow_USE_STATIC_LIBS ON)
  set(Boost_USE_STATIC_LIBS ON)
  set(OPENSSL_USE_STATIC_LIBS ON)
  if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")
  endif()

  set(CUDA_USE_STATIC_CUDA_RUNTIME ON CACHE STRING "Use static CUDA runtime")

  # On ppc, build failures occur for targets that depend on locale related functions due to unresolved symbols that are
  # present in the stdc++ library. Add the library flag to these targets to be used in resolving these symbols.
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ppc64le")
    set(LOCALE_LINK_FLAG "-lstdc++")
  endif()
else()
  add_definitions("-DBOOST_LOG_DYN_LINK")
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
if(BUILD_SHARED_LIBS)
  add_definitions("-DENABLE_SHARED_LIBS")
endif()

# Required for macOS with Boost 1.71.0+
# See https://gitlab.kitware.com/cmake/cmake/issues/19714
if(APPLE)
  set(Boost_NO_BOOST_CMAKE 1)
endif()

option(ENABLE_JAVA_REMOTE_DEBUG "Enable Java Remote Debug" OFF )
if(ENABLE_JAVA_REMOTE_DEBUG)
  add_definitions("-DENABLE_JAVA_REMOTE_DEBUG")
endif()

option(ENABLE_L0 "Enable level zero support" OFF)
if(ENABLE_L0)
  find_package(LevelZero REQUIRED COMPONENTS ${LevelZero_COMPONENTS})
  include_directories(${LevelZero_INCLUDE_DIRS})
  add_definitions("-DHAVE_L0")
endif()

option(ENABLE_CUDA "Enable CUDA support" ON)
if(ENABLE_CUDA)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  # A temporary workaround for non-conda envs
  include_directories(${CUDAToolkit_INCLUDE_DIRS})
  list(APPEND CUDA_LIBRARIES CUDA::cudart CUDA::cuda_driver)
  add_definitions("-DHAVE_CUDA")

else()
  set(CUDA_LIBRARIES "")
  set(MAPD_PACKAGE_FLAGS "${MAPD_PACKAGE_FLAGS}-cpu")
endif()

# CUDA architecture flags
if("${CMAKE_BUILD_TYPE_LOWER}" STREQUAL "debug")
  option(ENABLE_ONLY_ONE_ARCH "Enable quicker building for only one GPU arch" ON)
else()
  option(ENABLE_ONLY_ONE_ARCH "Enable quicker building for only one GPU arch" OFF)
endif()
if(ENABLE_CUDA)
  set(MAPD_CUDA_OPTIONS)
  # Set Thrust debug mode for CUDA compilation project-wide
  string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPERCASE)
  if(CMAKE_BUILD_TYPE_UPPERCASE MATCHES DEBUG)
    list(APPEND MAPD_CUDA_OPTIONS -DTHRUST_DEBUG --debug)
  else()
    list(APPEND MAPD_CUDA_OPTIONS -O3)
  endif()

  list(APPEND MAPD_CUDA_OPTIONS -Xcompiler -fPIC -D_FORCE_INLINES -std=c++17)

  if(ENABLE_ONLY_ONE_ARCH)
    execute_process(
      COMMAND cmake -S ${CMAKE_SOURCE_DIR}/NvidiaComputeCapability -B NvidiaComputeCapability
      OUTPUT_QUIET
      ERROR_QUIET
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    execute_process(
      COMMAND cmake --build NvidiaComputeCapability
      OUTPUT_FILE ${CMAKE_BINARY_DIR}/NvidiaComputeCapability/build.out.txt
      ERROR_FILE ${CMAKE_BINARY_DIR}/NvidiaComputeCapability/build.err.txt
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    set(NVIDIA_COMPUTE_CAPABILITY "")
    if (EXISTS ${CMAKE_BINARY_DIR}/NvidiaComputeCapability.txt)
      file(STRINGS ${CMAKE_BINARY_DIR}/NvidiaComputeCapability.txt NVIDIA_COMPUTE_CAPABILITY)
    endif()
  endif()
  if (ENABLE_ONLY_ONE_ARCH AND NOT "${NVIDIA_COMPUTE_CAPABILITY}" STREQUAL "")
    if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.18.0")
      set(CMAKE_CUDA_ARCHITECTURES ${NVIDIA_COMPUTE_CAPABILITY}-virtual)
      list(APPEND MAPD_CUDA_OPTIONS -Wno-deprecated-gpu-targets)
      message(STATUS "CUDA_ARCHITECTURES: ${CMAKE_CUDA_ARCHITECTURES}")
    else()
      set (CUDA_COMPILATION_ARCH
        -gencode=arch=compute_${NVIDIA_COMPUTE_CAPABILITY},code=compute_${NVIDIA_COMPUTE_CAPABILITY}
        -Wno-deprecated-gpu-targets
      )
      message(STATUS "CUDA_COMPILATION_ARCH: ${CUDA_COMPILATION_ARCH}")
      add_compile_options("$<$<COMPILE_LANGUAGE:CUDA>:${CUDA_COMPILATION_ARCH}>")
    endif()
    add_custom_target(clean_nvidia_compute_capability
      COMMAND ${CMAKE_BUILD_TOOL} clean
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/NvidiaComputeCapability
    )
    add_dependencies(clean-all clean_nvidia_compute_capability)
  else()
    if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.18.0")
      message(STATUS "CMake 3.18+, Setting CUDA_ARCHITECTURES.")
      set(CMAKE_CUDA_ARCHITECTURES
          50-virtual
          60-virtual
          70-virtual
          75-virtual
          80-virtual)
      list(APPEND MAPD_CUDA_OPTIONS -Wno-deprecated-gpu-targets)
      message(STATUS "CUDA_ARCHITECTURES: ${CMAKE_CUDA_ARCHITECTURES}")
    else()
      message(STATUS "CMake 3.17 or under, setting CUDA architecture flags manually.")
      set(CUDA_COMPILATION_ARCH
        -gencode=arch=compute_50,code=compute_50;
        -gencode=arch=compute_60,code=compute_60;
        -gencode=arch=compute_70,code=compute_70;
        -gencode=arch=compute_75,code=compute_75;
        -gencode=arch=compute_80,code=compute_80;
        -Wno-deprecated-gpu-targets)
      message(STATUS "CUDA_COMPILATION_ARCH: ${CUDA_COMPILATION_ARCH}")
      list(APPEND MAPD_CUDA_OPTIONS ${CUDA_COMPILATION_ARCH})
    endif()
    if(ENABLE_ONLY_ONE_ARCH)
      message(STATUS "ENABLE_ONLY_ONE_ARCH ignored because NvidiaComputeCapability.txt not found or not readable")
    endif()
  endif()
  if("${CMAKE_CUDA_COMPILER_ID}" STREQUAL "NVIDIA")
    include(ProcessorCount)
    ProcessorCount(N)
    if(CMAKE_CUDA_COMPILER_VERSION GREATER_EQUAL 11.3 AND NOT N EQUAL 0)
      message(STATUS "Enabling NVCC multi-threaded compilation with ${N} threads.")
      list(APPEND MAPD_CUDA_OPTIONS --threads ${N})
      set(NVCC_THREADS --threads ${N})
    endif()
  endif()

  add_compile_options("$<$<COMPILE_LANGUAGE:CUDA>:${MAPD_CUDA_OPTIONS}>")
endif()

option(ENABLE_NVTX "Enable NVidia Tools Extension library" OFF)
if(ENABLE_NVTX)
  if(NOT ENABLE_CUDA)
    set(ENABLE_NVTX OFF CACHE BOOL "Enable NVidia Tools Extension library" FORCE)
    message(STATUS "Cuda must be enabled to use NVTX, disabling NVTX support.")
  else()
    find_package(NVTX)
    if (NVTX_FOUND)
      message(STATUS "Using NVTX profiling markers")
      add_definitions("-DHAVE_NVTX")
    else()
      set(ENABLE_NVTX OFF CACHE BOOL "Enable NVidia Tools Extension library" FORCE)
      message(STATUS "NVTX not found, disabling NVTX support.")
    endif()
  endif()
endif()

option(SUPPRESS_NULL_LOGGER_DEPRECATION_WARNINGS "Suppress NullLogger deprecated warnings.")
if (SUPPRESS_NULL_LOGGER_DEPRECATION_WARNINGS)
  add_definitions("-DSUPPRESS_NULL_LOGGER_DEPRECATION_WARNINGS")
endif()

option(ENABLE_CUDA_KERNEL_DEBUG "Enable debugging symbols for CUDA device Kernels" OFF)

option(ENABLE_JIT_DEBUG "Enable debugging symbols for the JIT" OFF)
if (ENABLE_JIT_DEBUG)
  add_definitions("-DWITH_JIT_DEBUG")
endif()

if(XCODE)
  if(ENABLE_CUDA)
    set(CMAKE_EXE_LINKER_FLAGS "-F/Library/Frameworks -framework CUDA")
  endif()
  add_definitions("-DXCODE")
endif()

# fixme: hack works for Homebrew, might not work for Conda
if(ENABLE_CONDA)
  set(OPENSSL_ROOT_DIR "$ENV{CONDA_PREFIX}")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin" )
  set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl/")
endif()

find_package(OpenSSL REQUIRED)
include_directories(${OPENSSL_INCLUDE_DIR})

if(MSVC)
  add_definitions(/bigobj)
endif()

find_package(Git)
find_package(ZLIB REQUIRED)

option(ENABLE_FOLLY "Use Folly" ON)
if(ENABLE_FOLLY)
  find_package(Folly)
  if(NOT Folly_FOUND)
    set(ENABLE_FOLLY OFF CACHE BOOL "Use Folly" FORCE)
  else()
    set(FOLLY_LIBRARIES "")
    add_definitions("-DHAVE_FOLLY")
    find_package(gflags CONFIG REQUIRED)
    list(APPEND Folly_LIBRARIES Folly::folly gflags)
    # TODO: use Folly::folly_deps?
    if(MSVC)
      find_package(Libevent COMPONENTS core REQUIRED)
      list(APPEND Folly_LIBRARIES libevent::core)
    endif()
  endif()
endif()

if(MSVC)
  include_directories(include_directories("${LIBS_PATH}/include/pdcurses"))
else()
  find_package(Curses)
  include_directories(${CURSES_INCLUDE_DIRS})
  if (CURSES_HAVE_NCURSES_CURSES_H AND NOT CURSES_HAVE_CURSES_H)
    include_directories(${CURSES_INCLUDE_DIRS}/ncurses/)
  endif()
endif()

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)

option(FORCE_COLORED_COMPILER_OUTPUT "Enable ANSI-coloring for gnu/clang." OFF)

if(MSVC)
  option(ENABLE_NO_WINWARNINGS "disable most windows warnings" ON)
  add_compile_definitions("NOMINMAX")
  if(ENABLE_NO_WINWARNINGS)
    add_compile_definitions("_STL_EXTRA_DISABLED_WARNINGS=4146 4242 4244 4267 4355 4365 4458 4624 4820 4996 5204 5219" "NOMINMAX")
    # disable 4702 unreachable code warning
    # with /Qspectre set, disable the warning C5045
    add_compile_options(/W0 /wd4702 /wd5045)
  else()
    add_compile_options(/W4 /permisive-)
  endif()
  add_compile_options(/EHsc /std:c++17 /Qspectre)
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-unused-local-typedefs -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS")
  if (${FORCE_COLORED_COMPILER_OUTPUT})
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      add_compile_options(-fdiagnostics-color)
    else()
      add_compile_options(-fdiagnostics-color=always)
    endif()
  else()
    add_compile_options(-fdiagnostics-color=auto)
  endif()
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-register")
  endif()
  if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
  endif()
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-deprecated-declarations")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-declarations")
endif()

# address and thread sanitizer
option(ENABLE_STANDALONE_CALCITE "Require standalone Calcite server" OFF)
option(ENABLE_ASAN "Enable address sanitizer" OFF)
option(ENABLE_TSAN "Enable thread sanitizer" OFF)
option(ENABLE_UBSAN "Enable undefined behavior sanitizer" OFF)
if(ENABLE_ASAN)
  set(SAN_FLAGS "-fsanitize=address -O1 -fno-omit-frame-pointer")
  add_definitions("-DWITH_DECODERS_BOUNDS_CHECKING")
elseif(ENABLE_TSAN)
  add_definitions("-DHAVE_TSAN")
  # Copy the config directory to the build dir for TSAN suppressions
  file(COPY config DESTINATION ${CMAKE_BINARY_DIR})

  set(SAN_FLAGS "-fsanitize=thread -fPIC -O1 -fno-omit-frame-pointer")
  # required for older GCC, see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=64354
  add_definitions("-D__SANITIZE_THREAD__")
elseif(ENABLE_UBSAN)
  set(SAN_FLAGS "-fsanitize=undefined -fPIC -O1 -fno-omit-frame-pointer")
endif()
if(ENABLE_ASAN OR ENABLE_TSAN OR ENABLE_UBSAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SAN_FLAGS}")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SAN_FLAGS}")
  set(ENABLE_STANDALONE_CALCITE ON)
endif()

# Code coverage
option(ENABLE_CODE_COVERAGE "Enable compile time code coverage" OFF)
if(ENABLE_CODE_COVERAGE)
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    set(COVERAGE_FLAGS "-fprofile-instr-generate -fcoverage-mapping")
  else()
    message(FATAL_ERROR "Code coverage currently only supported with Clang compiler")
  endif()
  set(CMAKE_CXX_OUTPUT_EXTENSION_REPLACE ON)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COVERAGE_FLAGS}")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COVERAGE_FLAGS}")
endif()

option(ENABLE_DECODERS_BOUNDS_CHECKING "Enable bounds checking for column decoding" OFF)

if(ENABLE_STANDALONE_CALCITE)
  add_definitions("-DSTANDALONE_CALCITE")
endif()

include_directories(${CMAKE_SOURCE_DIR}
                    ${CMAKE_CURRENT_BINARY_DIR})

## Dependencies

# Intel ITT notify
option(ENABLE_ITT "Enable Intel ittnotify for VTune and Advisor" OFF)
if(ENABLE_ITT)
  find_package(ITTnotify REQUIRED)
  add_definitions(-DENABLE_ITT)
  include_directories("${ITTnotify_INCLUDE_DIRS}/")
  set(VT_LIBS ${ITTnotify_LIBRARIES} dl)
  option(ENABLE_INTEL_JIT_LISTENER "Enable Intel Vtune JIT Listener" ON)
else()
  option(ENABLE_INTEL_JIT_LISTENER "Enable Intel Vtune JIT Listener" OFF)
endif(ENABLE_ITT)
if(ENABLE_INTEL_JIT_LISTENER)
  add_definitions(-DENABLE_INTEL_JIT_LISTENER)
endif()

# LLVM
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  list(APPEND CMAKE_PREFIX_PATH "/usr/local/opt/llvm")
endif()
find_package(LLVM CONFIG REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

find_library(CLANG_LIB clang-cpp)
find_library(LLVM_LIB LLVM)
# Deps builds use separate libs for each clang component, while some distros now bundle into a single lib
if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin" OR NOT LLVM_LIB)
  set(LLVM_COMPONENTS support orcjit core irreader option linker)

  if(ENABLE_INTEL_JIT_LISTENER)
    list(APPEND LLVM_COMPONENTS inteljitevents)
  endif()

  llvm_map_components_to_libnames(llvm_libs ${LLVM_TARGETS_TO_BUILD} ${LLVM_COMPONENTS})
  set(clang_libs
      clangFrontend
      clangSerialization
      clangDriver
      clangTooling
      clangParse
      clangSema
      clangAnalysis
      clangEdit
      clangAST
      clangLex
      clangBasic
      clangRewrite
      clangRewriteFrontend)

  # LLVMSupport explicitly lists tinfo in its INTERFACE_LINK_LIBRARIES, even
  # though we provide it in our build of ncurses. Since LLVMSupport is listed
  # as a requirement for other llvm libs, we need to walk through the entire
  # list in order to remove all instances of tinfo.
  foreach(lib ${llvm_libs})
    get_target_property(interface_libs ${lib} INTERFACE_LINK_LIBRARIES)
    list(REMOVE_ITEM interface_libs tinfo z rt pthread -lpthread m dl)
    set_target_properties(${lib} PROPERTIES INTERFACE_LINK_LIBRARIES "${interface_libs}")
  endforeach()

  list(APPEND llvm_libs ${CURSES_NCURSES_LIBRARY})
else()
  if(NOT CLANG_LIB)
    message(FATAL_ERROR "Could not find CLANG library.")
  endif()

  set(clang_libs ${CLANG_LIB})
  set(llvm_libs ${LLVM_LIB})
endif()

if(MSVC) 
    list(APPEND LLVM_COMPONENTS Passes)
endif(MSVC)

# SPIRV-Translator
if(ENABLE_L0)
  find_package(PkgConfig REQUIRED)
  pkg_search_module(LLVMSPIRVLib REQUIRED IMPORTED_TARGET LLVMSPIRVLib)
  message(STATUS "Found LLVMSPIRVLib of version ${LLVMSPIRVLib_VERSION}")
  list(PREPEND llvm_libs PkgConfig::LLVMSPIRVLib)
endif()

# Boost
find_package(Boost COMPONENTS log log_setup filesystem program_options regex system thread timer locale iostreams REQUIRED)
include_directories(${Boost_INCLUDE_DIR})
# Avoid spurios deprecation warnings, see https://github.com/boostorg/graph/issues/228
if (${Boost_VERSION} VERSION_GREATER_EQUAL 1.74 AND ${Boost_VERSION} VERSION_LESS 1.78)
  add_definitions("-DBOOST_ALLOW_DEPRECATED_HEADERS -DBOOST_BIND_GLOBAL_PLACEHOLDERS")
endif()

# Allow explicit include statements to access third party headers directly.
# Ex: raft/canonical/include/raft.h
include_directories(ThirdParty/)

# Google Test and Google Mock
if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  add_definitions("-DGTEST_USE_OWN_TR1_TUPLE=0")
endif()
include_directories(ThirdParty/googletest)
add_subdirectory(ThirdParty/googletest)

# Google Benchmark
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Suppressing benchmark's tests" FORCE)
if(WIN32)
  set(HAVE_POSIX_REGEX 0)
endif()
add_subdirectory(ThirdParty/googlebenchmark)

# aws-sdk
option(ENABLE_AWS_S3 "Enable AWS S3 support" OFF)
if(ENABLE_AWS_S3)
  find_package(LibAwsS3)
  if(NOT LibAwsS3_FOUND)
    set(ENABLE_AWS_S3 OFF CACHE BOOL "Enable AWS S3 support" FORCE)
  else()
    add_definitions("-DHAVE_AWS_S3")
  endif()
endif()

# Arrow
find_package(Arrow REQUIRED)
add_definitions("-DARROW_NO_DEPRECATED_API")
include_directories(${Arrow_INCLUDE_DIRS})

find_package(Parquet REQUIRED)

list(APPEND Arrow_LIBRARIES ${Snappy_LIBRARIES})
if(ENABLE_AWS_S3)
  list(INSERT Arrow_LIBRARIES 0 ${LibAwsS3_LIBRARIES})
endif()
if (ENABLE_CUDA)
  list(INSERT Arrow_LIBRARIES 0 ${Arrow_GPU_CUDA_LIBRARIES})
endif()

# RapidJSON
include_directories(ThirdParty/rapidjson)
add_definitions(-DRAPIDJSON_HAS_STDSTRING)
if(NOT MSVC)
  # At the present time the current vcpkg version of rapidjson is 2020-09-14:
  # https://github.com/microsoft/vcpkg/blob/master/versions/r-/rapidjson.json
  # and the Windows build fails because it does not have this fix:
  # https://github.com/Tencent/rapidjson/pull/1568
  # Once vcpkg's rapidjson has this fix then let's try not making this exception for MSVC.
  # When this changes, remove this exception from all other similar CMakeLists.txt files too.
  add_definitions(-DRAPIDJSON_NOMEMBERITERATORCLASS)
endif()

# SQLite
find_package(SQLite3 REQUIRED)
include_directories(${SQLite3_INCLUDE_DIRS})

find_package(CURL REQUIRED QUIET)
if(CURL_FOUND)
  if(ENABLE_AWS_S3)
    set(CURL_LIBRARIES ${LibAwsS3_SUPPORT_LIBRARIES})
  endif()
endif()

# PicoSHA2
include_directories(ThirdParty/PicoSHA2)

if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
# opensaml
  option(ENABLE_SAML "Enable SAML support" ON)
  if(ENABLE_SAML)
    find_package(OpenSaml)
    if(NOT OpenSaml_FOUND)
      set(ENABLE_SAML OFF CACHE BOOL "Enable SAML support" FORCE)
    else()
      add_definitions("-DHAVE_SAML")
    endif()
  endif()
endif()

# TBB

option(ENABLE_TBB "Enable OneTBB for threading (if found)" ON)
set(TBB_LIBS "")
find_package(TBB)
if(TBB_FOUND)
  message(STATUS "TBB library is found with ${TBB_DIR}")
  add_definitions("-DHAVE_TBB")
  add_definitions("-DTBB_PREVIEW_TASK_GROUP_EXTENSIONS=1")
  list(APPEND TBB_LIBS ${TBB_LIBRARIES})
  if(ENABLE_TBB)
    add_definitions("-DENABLE_TBB")
  else()
    message(STATUS "Using TBB for threading is DISABLED")
  endif()
else()
  set(ENABLE_TBB OFF)
endif()

option(DISABLE_CONCURRENCY "Disable parallellism at the threading layer" OFF)
if(DISABLE_CONCURRENCY)
  add_definitions("-DDISABLE_CONCURRENCY")
endif()

list(APPEND ADDITIONAL_MAKE_CLEAN_FILES ${CMAKE_BINARY_DIR}/gen-cpp/)

set(TIME_LIMITED_NUMBER_OF_DAYS "30" CACHE STRING "Number of days this build is valid for if build is time limited")

option(TIME_LIMITED_BUILD "Build Time Limited Build" OFF)
if(TIME_LIMITED_BUILD)
  list(APPEND TIME_LIMITED_DEFINITIONS "TIME_LIMITED_BUILD")
  list(APPEND TIME_LIMITED_DEFINITIONS "TIME_LIMITED_NUMBER_OF_DAYS=${TIME_LIMITED_NUMBER_OF_DAYS}")
  set(MAPD_PACKAGE_FLAGS "${MAPD_PACKAGE_FLAGS}-${TIME_LIMITED_NUMBER_OF_DAYS}d")
endif()

option(ENABLE_PROFILER "Enable google perftools" OFF)
if(ENABLE_PROFILER)
  find_package(Gperftools REQUIRED COMPONENTS TCMALLOC PROFILER)
  set(PROFILER_LIBS ${Gperftools_TCMALLOC} ${Gperftools_PROFILER})
  add_definitions("-DHAVE_PROFILER")
else()
  set(PROFILER_LIBS "")
endif()

add_subdirectory(SqliteConnector)

add_subdirectory(StringDictionary)
add_subdirectory(Calcite)

add_subdirectory(ArrowStorage)
add_subdirectory(Analyzer)
add_subdirectory(ConfigBuilder)
add_subdirectory(QueryBuilder)
add_subdirectory(QueryEngine)
add_subdirectory(QueryOptimizer)
add_subdirectory(DataMgr)
add_subdirectory(CudaMgr)
add_subdirectory(IR)
add_subdirectory(L0Mgr)
add_subdirectory(Logger)
add_subdirectory(ResultSet)
add_subdirectory(ResultSetRegistry)
add_subdirectory(Shared)
add_subdirectory(SchemaMgr)
add_subdirectory(OSDependent)
add_subdirectory(Utils)
add_subdirectory(UdfCompiler)

option(ENABLE_ODBC "Build ODBC driver" OFF)
if(ENABLE_ODBC)
  add_subdirectory(ODBC)
endif()

set(MAPD_LIBRARIES OSDependent Shared SqliteConnector Analyzer QueryEngine DataMgr Logger)

if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
  if(ENABLE_DISTRIBUTED_5_0)
    list(APPEND MAPD_LIBRARIES StringDictionaryThread)
  endif()
endif()

list(APPEND MAPD_LIBRARIES Calcite)

list(APPEND MAPD_LIBRARIES ${Arrow_LIBRARIES})

if(ENABLE_FOLLY)
  list(APPEND MAPD_LIBRARIES ${Folly_LIBRARIES})
endif()

if(ENABLE_LICENSING_AWS)
  list(APPEND MAPD_LIBRARIES AWSMarketplace)
endif()

list(APPEND MAPD_LIBRARIES ${TBB_LIBS})

option(ENABLE_TESTS "Build unit tests" ON)
if (ENABLE_TESTS)
  enable_testing()
  add_subdirectory(Tests/ArrowSQLRunner)
  add_subdirectory(Tests)
endif()

option(ENABLE_BENCHMARKS "Build benchmarks" ON)
if (ENABLE_TESTS AND ENABLE_BENCHMARKS)
  add_subdirectory(Benchmarks/taxi)
endif()

execute_process(
  COMMAND ${GIT_EXECUTABLE} rev-parse --short=10 HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE MAPD_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  )
file(WRITE ${CMAKE_BINARY_DIR}/MAPD_GIT_HASH.txt "${MAPD_GIT_HASH}\n")
file(STRINGS ${CMAKE_BINARY_DIR}/MAPD_GIT_HASH.txt MAPD_GIT_HASH)
set(CPACK_PACKAGE_VERSION "${MAPD_VERSION_RAW}-${MAPD_BUILD_DATE}-${MAPD_GIT_HASH}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/Shared/release.h"
  "${CMAKE_BINARY_DIR}/MapDRelease.h"
  @ONLY
  )
list(APPEND ADDITIONAL_MAKE_CLEAN_FILES ${CMAKE_BINARY_DIR}/MAPD_GIT_HASH.txt)
list(APPEND ADDITIONAL_MAKE_CLEAN_FILES ${CMAKE_BINARY_DIR}/MapDRelease.h)
# required to force regen of MAPD_GIT_HASH.txt, MapDRelease.h
add_custom_target(rerun_cmake ALL
  COMMAND cmake .
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )

macro(set_dpkg_arch arch_in arch_out)
  if("${arch_in}" STREQUAL "x86_64")
    set(${arch_out} "amd64")
  elseif("${arch_in}" STREQUAL "aarch64")
    set(${arch_out} "arm64")
  elseif("${arch_in}" STREQUAL "ppc64le")
    set(${arch_out} "ppc64el")
  else()
    set(${arch_out} "${arch_in}")
  endif()
endmacro()

# clang-tidy
find_program(JQ_EXECUTABLE NAMES jq)
if (NOT ${JQ_EXECUTABLE} STREQUAL "JQ_EXECUTABLE-NOTFOUND")
  file(WRITE ${CMAKE_BINARY_DIR}/jq.filter "map(select(.file | test(\".*/(build|ThirdParty)/.*\") | not))")
  add_custom_target(run-clang-tidy
    COMMAND mkdir -p clang-tidy
    COMMAND ${JQ_EXECUTABLE} -f jq.filter ${CMAKE_BINARY_DIR}/compile_commands.json > clang-tidy/compile_commands.json
    COMMAND cd clang-tidy && ${CMAKE_SOURCE_DIR}/ThirdParty/clang/run-clang-tidy.py -quiet -format -fix -header-filter="${CMAKE_SOURCE_DIR}/.*" 2> /dev/null
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
else()
  message(STATUS "jq not found, disabling run-clang-tidy target")
endif()

# Packaging

if(NOT "${CMAKE_BUILD_TYPE_LOWER}" STREQUAL "debug" AND NOT "${CMAKE_BUILD_TYPE_LOWER}" STREQUAL "relwithdebinfo")
  set(CPACK_STRIP_FILES ON)
else()
  set(MAPD_PACKAGE_FLAGS "${MAPD_PACKAGE_FLAGS}-debug")
endif()
set(CPACK_PACKAGE_VENDOR "OmniSci, Inc.")
set(CPACK_PACKAGE_CONTACT "support@omnisci.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OmniSci Core Database")
set(CPACK_PROJECT_CONFIG_FILE ${CMAKE_SOURCE_DIR}/CMakePackaging.txt)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "default-jre-headless | openjdk-8-jre-headless | java8-runtime-headless, bsdmainutils, curl | wget")
set(CPACK_RPM_PACKAGE_REQUIRES "java-headless, util-linux, curl")
set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
set(CPACK_RPM_SPEC_MORE_DEFINE "%define __jar_repack %{nil}")
if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libldap-2.4-2")
endif()

set_dpkg_arch(${CMAKE_SYSTEM_PROCESSOR} CPACK_DEBIAN_PACKAGE_ARCHITECTURE)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/licenses" DESTINATION "ThirdParty" COMPONENT "doc")

# OmniSciTypes.h local includes (for UDF)
install(FILES Shared/funcannotations.h DESTINATION "Shared/" COMPONENT "include")
install(FILES Shared/InlineNullValues.h DESTINATION "Shared/" COMPONENT "include")
install(FILES Logger/Logger.h DESTINATION "Logger/" COMPONENT "include")

# Frontend
option(MAPD_IMMERSE_DOWNLOAD "Download OmniSci Immerse for packaging" OFF)
set(MAPD_IMMERSE_URL ${MAPD_IMMERSE_URL} CACHE STRING "URL to bundled frontend")
if(MAPD_IMMERSE_DOWNLOAD)
  include(ExternalProject)
  externalproject_add(frontend
    URL ${MAPD_IMMERSE_URL}
    PREFIX external
    CONFIGURE_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    LOG_DOWNLOAD on
    )
  externalproject_get_property(frontend source_dir)

  install(DIRECTORY ${source_dir}/ DESTINATION "frontend/" PATTERN .git EXCLUDE PATTERN node_modules EXCLUDE)
  add_custom_command(TARGET frontend COMMAND ${CMAKE_COMMAND} -E copy_directory ${source_dir} frontend)
  list(APPEND ADDITIONAL_MAKE_CLEAN_FILES ${CMAKE_BINARY_DIR}/frontend)
endif()


set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES "${ADDITIONAL_MAKE_CLEAN_FILES}")

install(FILES ${CMAKE_BINARY_DIR}/MAPD_GIT_HASH.txt DESTINATION "." COMPONENT "doc")
if(ENABLE_CUDA)
  install(FILES ${CMAKE_BINARY_DIR}/QueryEngine/cuda_mapd_rt.fatbin DESTINATION QueryEngine COMPONENT "exe")
endif()

if(NOT PREFER_STATIC_LIBS AND NOT ENABLE_CONDA)
  install(FILES ${Boost_LIBRARIES} DESTINATION ThirdParty/lib)
endif()

if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
  set(EULA_FILE "${CMAKE_SOURCE_DIR}/EULA-EE.txt")
else()
  set(EULA_FILE "${CMAKE_SOURCE_DIR}/LICENSE.md")
endif()

if("${MAPD_EDITION_LOWER}" STREQUAL "os")
  install(FILES LICENSE.md DESTINATION "." COMPONENT "doc")
endif()

set(CPACK_RESOURCE_FILE_LICENSE "${EULA_FILE}")
install(FILES "${EULA_FILE}" DESTINATION "."  COMPONENT "doc")

exec_program(uname ARGS -m OUTPUT_VARIABLE MAPD_HOST_SYSTEM_ARCHITECTURE) # does not account for cross-compiling or Windows
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${MAPD_EDITION_LOWER}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${MAPD_HOST_SYSTEM_ARCHITECTURE}${MAPD_PACKAGE_FLAGS}")

set(CPACK_GENERATOR "STGZ")

include(CPack)

if(DOXYGEN_FOUND)
  add_custom_target(sphinx
    COMMAND python3 -m venv sphinx-env
    COMMAND . sphinx-env/bin/activate && pip install -r requirements.txt
    COMMAND rm -rf build
    COMMAND . sphinx-env/bin/activate && make html SPHINXOPTS="-D version=${MAPD_VERSION_MAJOR}.${MAPD_VERSION_MINOR}.${MAPD_VERSION_PATCH}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/docs
    )

  add_dependencies(sphinx doxygen)
endif(DOXYGEN_FOUND)
