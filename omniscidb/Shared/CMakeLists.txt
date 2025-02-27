set(shared_source_files
    StringTransform.cpp
    DateTimeParser.cpp
    StackTrace.cpp
    base64.cpp
    misc.cpp
    thread_count.cpp
    threading.cpp
    MathUtils.cpp
    file_path_util.cpp
    globals.cpp)

include_directories(${CMAKE_SOURCE_DIR})
if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
  list(APPEND shared_source_files ee/Encryption.cpp)
endif()

if(ENABLE_NVTX)
  list(APPEND shared_source_files nvtx_helpers.cpp)
endif()

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/funcannotations.h
     DESTINATION ${CMAKE_BINARY_DIR}/Shared/)

add_library(Shared ${shared_source_files} "cleanup_global_namespace.h"
                   "boost_stacktrace.hpp")
target_link_libraries(Shared OSDependent Logger ${Boost_LIBRARIES} TBB::tbb ${Folly_LIBRARIES})
if("${MAPD_EDITION_LOWER}" STREQUAL "ee")
  target_link_libraries(Shared ${OPENSSL_LIBRARIES})
endif()
