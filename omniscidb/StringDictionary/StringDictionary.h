/*
 * Copyright 2021 OmniSci, Inc.
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

#ifndef STRINGDICTIONARY_STRINGDICTIONARY_H
#define STRINGDICTIONARY_STRINGDICTIONARY_H

#include "../Shared/mapd_shared_mutex.h"
#include "DictRef.h"
#include "DictionaryCache.hpp"

#include <functional>
#include <future>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

extern bool g_enable_stringdict_parallel;

using string_dict_hash_t = uint32_t;

using StringLookupCallback = std::function<bool(std::string_view, int32_t string_id)>;

class StringDictionary {
 public:
  StringDictionary(const DictRef& dict_ref,
                   const bool materializeHashes = false,
                   size_t initial_capacity = 256);
  ~StringDictionary() noexcept;

  int32_t getDbId() const noexcept;
  int32_t getDictId() const noexcept;

  class StringCallback {
   public:
    virtual ~StringCallback() = default;
    virtual void operator()(std::string const&, int32_t const string_id) = 0;
    virtual void operator()(std::string_view const, int32_t const string_id) = 0;
  };

  // Functors passed to eachStringSerially() must derive from StringCallback.
  // Each std::string const& (if isClient()) or std::string_view (if !isClient())
  // plus string_id is passed to the callback functor.
  void eachStringSerially(int64_t const generation, StringCallback&) const;
  friend class StringLocalCallback;

  int32_t getOrAdd(const std::string_view& str) noexcept;
  template <class T, class String>
  size_t getBulk(const std::vector<String>& string_vec, T* encoded_vec) const;
  template <class T, class String>
  size_t getBulk(const std::vector<String>& string_vec,
                 T* encoded_vec,
                 const int64_t generation) const;
  template <class T, class String>
  void getOrAddBulk(const std::vector<String>& string_vec, T* encoded_vec);
  template <class T, class String>
  void getOrAddBulkParallel(const std::vector<String>& string_vec, T* encoded_vec);
  template <class String>
  void getOrAddBulkArray(const std::vector<std::vector<String>>& string_array_vec,
                         std::vector<std::vector<int32_t>>& ids_array_vec);
  template <class String>
  int32_t getIdOfString(const String&) const;
  std::string getString(int32_t string_id) const;
  std::pair<char*, size_t> getStringBytes(int32_t string_id) const noexcept;
  size_t storageEntryCount() const;

  std::vector<int32_t> getLike(const std::string& pattern,
                               const bool icase,
                               const bool is_simple,
                               const char escape,
                               const size_t generation) const;

  std::vector<int32_t> getCompare(const std::string& pattern,
                                  const std::string& comp_operator,
                                  const size_t generation);

  std::vector<int32_t> getRegexpLike(const std::string& pattern,
                                     const char escape,
                                     const size_t generation) const;

  std::vector<std::string> copyStrings() const;

  std::vector<std::string_view> getStringViews() const;
  std::vector<std::string_view> getStringViews(const size_t generation) const;

  std::vector<int32_t> buildDictionaryTranslationMap(
      const std::shared_ptr<StringDictionary> dest_dict,
      StringLookupCallback const& dest_transient_lookup_callback) const;

  size_t buildDictionaryTranslationMap(
      const StringDictionary* dest_dict,
      int32_t* translated_ids,
      const int64_t source_generation,
      const int64_t dest_generation,
      const bool dest_has_transients,
      StringLookupCallback const& dest_transient_lookup_callback) const;

  static constexpr int32_t INVALID_STR_ID = -1;
  static constexpr size_t MAX_STRLEN = (1 << 15) - 1;
  static constexpr size_t MAX_STRCOUNT = (1U << 31) - 1;

 private:
  struct StringIdxEntry {
    uint64_t off : 48;
    uint64_t size : 16;
  };

  // In the compare_cache_value_t index represents the index of the sorted cache.
  // The diff component represents whether the index the cache is pointing to is equal to
  // the pattern it is cached for. We want to use diff so we don't have compare string
  // again when we are retrieving it from the cache.
  struct compare_cache_value_t {
    int32_t index;
    int32_t diff;
  };

  struct PayloadString {
    char* c_str_ptr;
    size_t size;
    bool canary;
  };

  bool fillRateIsHigh(const size_t num_strings) const noexcept;
  void increaseHashTableCapacity() noexcept;
  template <class String>
  void increaseHashTableCapacityFromStorageAndMemory(
      const size_t str_count,
      const size_t storage_high_water_mark,
      const std::vector<String>& input_strings,
      const std::vector<size_t>& string_memory_ids,
      const std::vector<string_dict_hash_t>& input_strings_hashes) noexcept;
  template <class String>
  void hashStrings(const std::vector<String>& string_vec,
                   std::vector<string_dict_hash_t>& hashes) const noexcept;

  int32_t getUnlocked(const std::string_view sv) const noexcept;
  std::string getStringUnlocked(int32_t string_id) const noexcept;
  std::string getStringChecked(const int string_id) const noexcept;
  std::pair<char*, size_t> getStringBytesChecked(const int string_id) const noexcept;
  template <class String>
  uint32_t computeBucket(
      const string_dict_hash_t hash,
      const String& input_string,
      const std::vector<int32_t>& string_id_string_dict_hash_table) const noexcept;
  template <class String>
  uint32_t computeBucketFromStorageAndMemory(
      const string_dict_hash_t input_string_hash,
      const String& input_string,
      const std::vector<int32_t>& string_id_string_dict_hash_table,
      const size_t storage_high_water_mark,
      const std::vector<String>& input_strings,
      const std::vector<size_t>& string_memory_ids) const noexcept;
  uint32_t computeUniqueBucketWithHash(
      const string_dict_hash_t hash,
      const std::vector<int32_t>& string_id_string_dict_hash_table) noexcept;
  void checkAndConditionallyIncreasePayloadCapacity(const size_t write_length);
  void checkAndConditionallyIncreaseOffsetCapacity(const size_t write_length);

  template <class String>
  void appendToStorage(const String str) noexcept;
  template <class String>
  void appendToStorageBulk(const std::vector<String>& input_strings,
                           const std::vector<size_t>& string_memory_ids,
                           const size_t sum_new_strings_lengths) noexcept;
  PayloadString getStringFromStorage(const int string_id) const noexcept;
  std::string_view getStringFromStorageFast(const int string_id) const noexcept;
  void addPayloadCapacity(const size_t min_capacity_requested = 0) noexcept;
  void addOffsetCapacity(const size_t min_capacity_requested = 0) noexcept;
  void* addMemoryCapacity(void* addr,
                          size_t& mem_size,
                          const size_t min_capacity_requested = 0) noexcept;
  void invalidateInvertedIndex() noexcept;
  std::vector<int32_t> getEquals(std::string pattern,
                                 std::string comp_operator,
                                 size_t generation);
  void buildSortedCache();
  void sortCache(std::vector<int32_t>& cache);
  void mergeSortedCache(std::vector<int32_t>& temp_sorted_cache);

  const DictRef dict_ref_;
  size_t str_count_;
  size_t collisions_;
  std::vector<int32_t> string_id_string_dict_hash_table_;
  std::vector<string_dict_hash_t> hash_cache_;
  std::vector<int32_t> sorted_cache;
  bool materialize_hashes_;
  StringIdxEntry* offset_map_;
  char* payload_map_;
  size_t offset_file_size_;
  size_t payload_file_size_;
  size_t payload_file_off_;
  mutable mapd_shared_mutex rw_mutex_;
  mutable std::map<std::tuple<std::string, bool, bool, char>, std::vector<int32_t>>
      like_cache_;
  mutable std::map<std::pair<std::string, char>, std::vector<int32_t>> regex_cache_;
  mutable std::map<std::string, int32_t> equal_cache_;
  mutable DictionaryCache<std::string, compare_cache_value_t> compare_cache_;
  mutable std::shared_ptr<std::vector<std::string>> strings_cache_;

  char* CANARY_BUFFER{nullptr};
  size_t canary_buffer_size = 0;
};

int32_t truncate_to_generation(const int32_t id, const size_t generation);

#endif  // STRINGDICTIONARY_STRINGDICTIONARY_H
