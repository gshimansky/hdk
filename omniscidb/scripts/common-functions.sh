#!/bin/bash

HTTP_DEPS="https://dependencies.mapd.com/thirdparty"
SCRIPTS_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if [ "$TSAN" = "true" ]; then
  ARROW_TSAN="-DARROW_JEMALLOC=OFF -DARROW_USE_TSAN=ON"
  TBB_TSAN="-fsanitize=thread -fPIC -O1 -fno-omit-frame-pointer"
elif [ "$TSAN" = "false" ]; then
  ARROW_TSAN="-DARROW_JEMALLOC=BUNDLED"
  TBB_TSAN=""
fi

ARROW_USE_CUDA="-DARROW_CUDA=ON"
if [ "$NOCUDA" = "true" ]; then
  ARROW_USE_CUDA="-DARROW_CUDA=OFF"
fi

function download() {
    wget --continue "$1"
}

function extract() {
    tar xvf "$1"
}

function cmake_build_and_install() {
  cmake --build . --parallel && cmake --install .
}

function makej() {
  os=$(uname)
  if [ "$os" = "Darwin" ]; then
    nproc=$(sysctl -n hw.ncpu)
  else
    nproc=$(nproc)
  fi
  make -j ${nproc:-8}
}

function make_install() {
  # sudo is needed on osx
  os=$(uname)
  if [ "$os" = "Darwin" ]; then
    sudo make install
  else
    make install
  fi
}

function download_make_install() {
    name="$(basename $1)"
    download "$1"
    extract $name
    if [ -z "$2" ]; then
        pushd ${name%%.tar*}
    else
        pushd $2
    fi

    if [ -x ./Configure ]; then
        ./Configure --prefix=$PREFIX $3
    else
        ./configure --prefix=$PREFIX $3
    fi
    makej
    make_install
    popd
}

CMAKE_VERSION=3.16.5

function install_cmake() {
  CXXFLAGS="-pthread" CFLAGS="-pthread" download_make_install ${HTTP_DEPS}/cmake-${CMAKE_VERSION}.tar.gz
}

ARROW_VERSION=apache-arrow-6.0.0

function install_arrow() {
  download https://github.com/apache/arrow/archive/$ARROW_VERSION.tar.gz
  extract $ARROW_VERSION.tar.gz
  
  mkdir -p arrow-$ARROW_VERSION/cpp/build
  pushd arrow-$ARROW_VERSION/cpp/build
  cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$PREFIX \
    -DARROW_BUILD_SHARED=ON \
    -DARROW_BUILD_STATIC=ON \
    -DARROW_BUILD_TESTS=OFF \
    -DARROW_BUILD_BENCHMARKS=OFF \
    -DARROW_CSV=ON \
    -DARROW_JSON=ON \
    -DARROW_WITH_BROTLI=BUNDLED \
    -DARROW_WITH_ZLIB=BUNDLED \
    -DARROW_WITH_LZ4=BUNDLED \
    -DARROW_WITH_SNAPPY=BUNDLED \
    -DARROW_WITH_ZSTD=BUNDLED \
    -DARROW_USE_GLOG=OFF \
    -DARROW_BOOST_USE_SHARED=${ARROW_BOOST_USE_SHARED:="OFF"} \
    -DARROW_PARQUET=ON \
    -DARROW_FILESYSTEM=ON \
    -DARROW_S3=ON \
    -DTHRIFT_HOME=${THRIFT_HOME:-$PREFIX} \
    ${ARROW_USE_CUDA} \
    ${ARROW_TSAN} \
    ..
  makej
  make_install
  popd
}

SNAPPY_VERSION=1.1.7
function install_snappy() {
  download https://github.com/google/snappy/archive/$SNAPPY_VERSION.tar.gz
  extract $SNAPPY_VERSION.tar.gz
  mkdir -p snappy-$SNAPPY_VERSION/build
  pushd snappy-$SNAPPY_VERSION/build
  cmake \
    -DCMAKE_CXX_FLAGS="-fPIC" \
    -DCMAKE_INSTALL_PREFIX=$PREFIX \
    -DCMAKE_BUILD_TYPE=Release \
    -DSNAPPY_BUILD_TESTS=OFF \
    ..
  makej
  make_install
  popd
}

AWSCPP_VERSION=1.7.301

function install_awscpp() {
    # default c++ standard support
    CPP_STANDARD=14
    # check c++17 support
    GNU_VERSION1=$(g++ --version|head -n1|awk '{print $4}'|cut -d'.' -f1)
    if [ "$GNU_VERSION1" = "7" ]; then
        CPP_STANDARD=17
    fi
    rm -rf aws-sdk-cpp-${AWSCPP_VERSION}
    download https://github.com/aws/aws-sdk-cpp/archive/${AWSCPP_VERSION}.tar.gz
    tar xvfz ${AWSCPP_VERSION}.tar.gz
    pushd aws-sdk-cpp-${AWSCPP_VERSION}
    mkdir build
    cd build
    cmake \
        -GNinja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$PREFIX \
        -DBUILD_ONLY="s3;transfer;config;sts;cognito-identity;identity-management" \
        -DBUILD_SHARED_LIBS=0 \
        -DCUSTOM_MEMORY_MANAGEMENT=0 \
        -DCPP_STANDARD=$CPP_STANDARD \
        -DENABLE_TESTING=off \
        ..
    cmake_build_and_install
    popd
}

LLVM_VERSION=9.0.1

function install_llvm() {
    VERS=${LLVM_VERSION}
    download ${HTTP_DEPS}/llvm/$VERS/llvm-$VERS.src.tar.xz
    download ${HTTP_DEPS}/llvm/$VERS/clang-$VERS.src.tar.xz
    download ${HTTP_DEPS}/llvm/$VERS/compiler-rt-$VERS.src.tar.xz
    download ${HTTP_DEPS}/llvm/$VERS/lldb-$VERS.src.tar.xz
    download ${HTTP_DEPS}/llvm/$VERS/lld-$VERS.src.tar.xz
    download ${HTTP_DEPS}/llvm/$VERS/libcxx-$VERS.src.tar.xz
    download ${HTTP_DEPS}/llvm/$VERS/libcxxabi-$VERS.src.tar.xz
    download ${HTTP_DEPS}/llvm/$VERS/clang-tools-extra-$VERS.src.tar.xz
    rm -rf llvm-$VERS.src
    extract llvm-$VERS.src.tar.xz
    extract clang-$VERS.src.tar.xz
    extract compiler-rt-$VERS.src.tar.xz
    extract lld-$VERS.src.tar.xz
    extract lldb-$VERS.src.tar.xz
    extract libcxx-$VERS.src.tar.xz
    extract libcxxabi-$VERS.src.tar.xz
    extract clang-tools-extra-$VERS.src.tar.xz
    mv clang-$VERS.src llvm-$VERS.src/tools/clang
    mv compiler-rt-$VERS.src llvm-$VERS.src/projects/compiler-rt
    mv lld-$VERS.src llvm-$VERS.src/tools/lld
    mv lldb-$VERS.src llvm-$VERS.src/tools/lldb
    mv libcxx-$VERS.src llvm-$VERS.src/projects/libcxx
    mv libcxxabi-$VERS.src llvm-$VERS.src/projects/libcxxabi
    mkdir -p llvm-$VERS.src/tools/clang/tools
    mv clang-tools-extra-$VERS.src llvm-$VERS.src/tools/clang/tools/extra

    # Patch llvm 9 for glibc 2.31+ support
    # from: https://bugs.gentoo.org/708430
    pushd llvm-$VERS.src/projects/
    sudo patch -p0 < $SCRIPTS_DIR/llvm-9-glibc-2.31-708430-remove-cyclades.patch
    popd

    rm -rf build.llvm-$VERS
    mkdir build.llvm-$VERS
    pushd build.llvm-$VERS

    LLVM_SHARED=""
    if [ "$LLVM_BUILD_DYLIB" = "true" ]; then
      LLVM_SHARED="-DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON"
    fi

    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX -DLLVM_ENABLE_RTTI=on -DLLVM_USE_INTEL_JITEVENTS=on $LLVM_SHARED ../llvm-$VERS.src
    makej
    make install
    popd
}

FOLLY_VERSION=2021.02.01.00
FMT_VERSION=7.1.3
function install_folly() {
  # Folly depends on fmt
  download https://github.com/fmtlib/fmt/archive/$FMT_VERSION.tar.gz
  extract $FMT_VERSION.tar.gz
  BUILD_DIR="fmt-$FMT_VERSION/build"
  mkdir -p $BUILD_DIR
  pushd $BUILD_DIR
  cmake -GNinja \
        -DCMAKE_CXX_FLAGS="-fPIC" \
        -DFMT_DOC=OFF \
        -DFMT_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX=$PREFIX ..
  cmake_build_and_install
  popd

  download https://github.com/facebook/folly/archive/v$FOLLY_VERSION.tar.gz
  extract v$FOLLY_VERSION.tar.gz
  pushd folly-$FOLLY_VERSION/build/

  source /etc/os-release
  if [ "$ID" == "ubuntu"  ] ; then
    FOLLY_SHARED=ON
  else
    FOLLY_SHARED=OFF
  fi

  # jemalloc disabled due to issue with clang build on Ubuntu
  # see: https://github.com/facebook/folly/issues/976
  cmake -GNinja \
        -DCMAKE_CXX_FLAGS="-fPIC -pthread" \
        -DFOLLY_USE_JEMALLOC=OFF \
        -DBUILD_SHARED_LIBS=${FOLLY_SHARED} \
        -DCMAKE_INSTALL_PREFIX=$PREFIX ..
  cmake_build_and_install

  popd
}


RDKAFKA_VERSION=1.1.0

function install_rdkafka() {
    if [ "$1" == "static" ]; then
      STATIC="ON"
    else
      STATIC="OFF"
    fi
    VERS=${RDKAFKA_VERSION}
    download https://github.com/edenhill/librdkafka/archive/v$VERS.tar.gz
    extract v$VERS.tar.gz
    BDIR="librdkafka-$VERS/build"
    mkdir -p $BDIR
    pushd $BDIR
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$PREFIX \
        -DRDKAFKA_BUILD_STATIC=$STATIC \
        -DRDKAFKA_BUILD_EXAMPLES=OFF \
        -DRDKAFKA_BUILD_TESTS=OFF \
        -DWITH_SASL=OFF \
        -DWITH_SSL=ON \
        ..
    makej
    make install
    popd
}

GO_VERSION=1.15.6

function install_go() {
    VERS=${GO_VERSION}
    ARCH=$(uname -m)
    ARCH=${ARCH//x86_64/amd64}
    ARCH=${ARCH//aarch64/arm64}
    # https://dl.google.com/go/go$VERS.linux-$ARCH.tar.gz
    download ${HTTP_DEPS}/go$VERS.linux-$ARCH.tar.gz
    extract go$VERS.linux-$ARCH.tar.gz
    rm -rf $PREFIX/go || true
    mv go $PREFIX
}

NINJA_VERSION=1.10.0

function install_ninja() {
  download https://github.com/ninja-build/ninja/releases/download/v${NINJA_VERSION}/ninja-linux.zip
  unzip -u ninja-linux.zip
  mkdir -p $PREFIX/bin/
  mv ninja $PREFIX/bin/
}

MAVEN_VERSION=3.6.3

function install_maven() {
    download ${HTTP_DEPS}/apache-maven-${MAVEN_VERSION}-bin.tar.gz
    extract apache-maven-${MAVEN_VERSION}-bin.tar.gz
    rm -rf $PREFIX/maven || true
    mv apache-maven-${MAVEN_VERSION} $PREFIX/maven
}

# The version of Thrust included in CUDA 11.0 and CUDA 11.1 does not support newer TBB
function patch_old_thrust_tbb() {
  NVCC_VERSION=$(nvcc --version | grep release | grep -o '[0-9][0-9]\.[0-9]' | head -n1)

  if [ "${NVCC_VERSION}" == "11.0" ] || [ "${NVCC_VERSION}" == "11.1"  ]; then
    pushd $(dirname $(which nvcc))/../include/
		cat > /tmp/cuda-11.0-tbb-thrust.patch << EOF
diff -ru thrust-old/system/tbb/detail/reduce_by_key.inl thrust/system/tbb/detail/reduce_by_key.inl
--- thrust-old/system/tbb/detail/reduce_by_key.inl      2021-10-12 15:59:23.693909272 +0000
+++ thrust/system/tbb/detail/reduce_by_key.inl  2021-10-12 16:00:05.314080478 +0000
@@ -27,8 +27,8 @@
 #include <thrust/detail/range/tail_flags.h>
 #include <tbb/blocked_range.h>
 #include <tbb/parallel_for.h>
-#include <tbb/tbb_thread.h>
 #include <cassert>
+#include <thread>
 
 
 namespace thrust
@@ -281,7 +281,7 @@
   }
 
   // count the number of processors
-  const unsigned int p = thrust::max<unsigned int>(1u, ::tbb::tbb_thread::hardware_concurrency());
+  const unsigned int p = thrust::max<unsigned int>(1u, std::thread::hardware_concurrency());
 
   // generate O(P) intervals of sequential work
   // XXX oversubscribing is a tuning opportunity
EOF
		patch -p0 --forward -r- < /tmp/cuda-11.0-tbb-thrust.patch || true
    popd
  fi
}

TBB_VERSION=2021.3.0

function install_tbb() {
  patch_old_thrust_tbb
  download https://github.com/oneapi-src/oneTBB/archive/v${TBB_VERSION}.tar.gz
  extract v${TBB_VERSION}.tar.gz
  pushd oneTBB-${TBB_VERSION}
  mkdir build
  pushd build
  if [ "$1" == "static" ]; then
    cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$PREFIX \
      -DTBB_TEST=off -DBUILD_SHARED_LIBS=off \
      ${TBB_TSAN} \
      ..
  else
    cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$PREFIX \
      -DTBB_TEST=off \
      -DBUILD_SHARED_LIBS=on \
      ${TBB_TSAN} \
      ..
  fi
  makej
  make install
  popd
  popd
}

LIBUV_VERSION=1.41.0

function install_libuv() {
  download https://dist.libuv.org/dist/v${LIBUV_VERSION}/libuv-v${LIBUV_VERSION}.tar.gz
  extract libuv-v${LIBUV_VERSION}.tar.gz
  pushd libuv-v${LIBUV_VERSION}
  mkdir -p build
  pushd build
  cmake -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=$PREFIX ..
  makej
  make_install
  popd
  popd
}

LIBNUMA_VERSION=2.0.14
MEMKIND_VERSION=1.11.0

function install_memkind() {
  download_make_install https://github.com/numactl/numactl/releases/download/v${LIBNUMA_VERSION}/numactl-${LIBNUMA_VERSION}.tar.gz
  
  download https://github.com/memkind/memkind/archive/refs/tags/v${MEMKIND_VERSION}.tar.gz
  extract v${MEMKIND_VERSION}.tar.gz
  pushd memkind-${MEMKIND_VERSION}
  ./autogen.sh
  if [[ $(cat /etc/os-release) = *"fedora"* ]]; then
    ./configure --prefix=${PREFIX} --libdir=${PREFIX}/lib64
  else
    ./configure --prefix=${PREFIX} --libdir=${PREFIX}/lib
  fi
  makej
  make_install
  popd
}
