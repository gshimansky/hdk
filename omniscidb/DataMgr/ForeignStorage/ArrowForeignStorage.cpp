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

#include "ArrowForeignStorage.h"

#include <arrow/api.h>
#include <arrow/csv/reader.h>
#include <arrow/io/file.h>
#include <arrow/util/decimal.h>
#include <tbb/parallel_for.h>
#include <tbb/task_group.h>
#include <array>
#include <future>
#include <vector>

#include "ArrowStorage/ArrowStorageUtils.h"
#include "Catalog/DataframeTableDescriptor.h"
#include "DataMgr/ForeignStorage/ForeignStorageInterface.h"
#include "DataMgr/StringNoneEncoder.h"
#include "Logger/Logger.h"
#include "QueryEngine/ArrowResultSet.h"
#include "Shared/ArrowUtil.h"
#include "Shared/measure.h"

struct Frag {
  size_t first_chunk;         // index of the first chunk assigned to the fragment
  size_t first_chunk_offset;  // offset from the begining of the first chunk
  size_t last_chunk;          // index of the last chunk
  size_t last_chunk_size;     // number of elements in the last chunk
};

struct ArrowFragment {
  int64_t offset{0};
  int64_t sz{0};
  std::vector<std::shared_ptr<arrow::ArrayData>> chunks;
};

class ArrowForeignStorageBase : public PersistentForeignStorageInterface {
 public:
  void append(const std::vector<ForeignStorageColumnBuffer>& column_buffers) override;

  void read(const ChunkKey& chunk_key,
            const SQLTypeInfo& sql_type,
            int8_t* dest,
            const size_t numBytes) override;

  int8_t* tryZeroCopy(const ChunkKey& chunk_key,
                      const SQLTypeInfo& sql_type,
                      const size_t numBytes) override;

  void parseArrowTable(Catalog_Namespace::Catalog* catalog,
                       std::pair<int, int> table_key,
                       const std::string& type,
                       const TableDescriptor& td,
                       const std::list<ColumnDescriptor>& cols,
                       Data_Namespace::AbstractBufferMgr* mgr,
                       const arrow::Table& table);

  template <typename T, typename ChunkType>
  std::shared_ptr<arrow::ChunkedArray> createDecimalColumn(
      const ColumnDescriptor& c,
      std::shared_ptr<arrow::ChunkedArray> arr_col_chunked_array);

  void getSizeAndOffset(const Frag& frag,
                        const std::shared_ptr<arrow::Array>& chunk,
                        size_t i,
                        int& size,
                        int& offset);

  int64_t makeFragment(const Frag& frag,
                       ArrowFragment& arrowFrag,
                       const std::vector<std::shared_ptr<arrow::Array>>& chunks,
                       bool is_varlen);

  std::map<std::array<int, 3>, std::vector<ArrowFragment>> m_columns;
};

void ArrowForeignStorageBase::getSizeAndOffset(const Frag& frag,
                                               const std::shared_ptr<arrow::Array>& chunk,
                                               size_t i,
                                               int& size,
                                               int& offset) {
  offset = (i == frag.first_chunk) ? frag.first_chunk_offset : 0;
  size = (i == frag.last_chunk) ? frag.last_chunk_size : (chunk->length() - offset);
}

int64_t ArrowForeignStorageBase::makeFragment(
    const Frag& frag,
    ArrowFragment& arrowFrag,
    const std::vector<std::shared_ptr<arrow::Array>>& chunks,
    bool is_varlen) {
  int64_t varlen = 0;
  arrowFrag.chunks.resize(frag.last_chunk - frag.first_chunk + 1);
  for (int i = frag.first_chunk, e = frag.last_chunk; i <= e; i++) {
    int size, offset;
    getSizeAndOffset(frag, chunks[i], i, size, offset);
    arrowFrag.offset += offset;
    arrowFrag.sz += size;
    arrowFrag.chunks[i - frag.first_chunk] = chunks[i]->data();
    auto& buffers = chunks[i]->data()->buffers;
    if (is_varlen) {
      if (buffers.size() <= 2) {
        throw std::runtime_error(
            "Importing fixed length arrow array as variable length column");
      }
      auto offsets_buffer = reinterpret_cast<const uint32_t*>(buffers[1]->data());
      varlen += offsets_buffer[offset + size] - offsets_buffer[offset];
    } else if (buffers.size() != 2) {
      throw std::runtime_error(
          "Importing varialbe length arrow array as fixed length column");
    }
  }
  // return length of string buffer if array is none encoded string
  return varlen;
}

std::vector<Frag> calculateFragmentsOffsets(const arrow::ChunkedArray& array,
                                            size_t maxFragRows) {
  std::vector<Frag> fragments;
  size_t sz = 0;
  size_t offset = 0;
  fragments.push_back({0, 0, 0, 0});
  size_t num_chunks = (size_t)array.num_chunks();
  for (size_t i = 0; i < num_chunks;) {
    auto& chunk = *array.chunk(i);
    auto& frag = *fragments.rbegin();
    if (maxFragRows - sz > chunk.length() - offset) {
      sz += chunk.length() - offset;
      if (i == num_chunks - 1) {
        fragments.rbegin()->last_chunk = num_chunks - 1;
        fragments.rbegin()->last_chunk_size =
            array.chunk((int)num_chunks - 1)->length() - offset;
      }
      offset = 0;
      i++;
    } else {
      frag.last_chunk = i;
      frag.last_chunk_size = maxFragRows - sz;
      offset += maxFragRows - sz;
      sz = 0;
      fragments.push_back({i, offset, 0, 0});
    }
  }
  if (fragments.rbegin()->first_chunk == fragments.rbegin()->first_chunk &&
      fragments.rbegin()->last_chunk_size == 0) {
    // remove empty fragment at the end if any
    fragments.pop_back();
  }
  return fragments;
}

void ArrowForeignStorageBase::parseArrowTable(Catalog_Namespace::Catalog* catalog,
                                              std::pair<int, int> table_key,
                                              const std::string& type,
                                              const TableDescriptor& td,
                                              const std::list<ColumnDescriptor>& cols,
                                              Data_Namespace::AbstractBufferMgr* mgr,
                                              const arrow::Table& table) {
  std::map<std::array<int, 3>, StringDictionary*> dictionaries;
  for (auto& c : cols) {
    std::array<int, 3> col_key{table_key.first, table_key.second, c.columnId};
    m_columns[col_key] = {};
    // fsi registerTable runs under SqliteLock which does not allow invoking
    // getMetadataForDict in other threads
    if (c.columnType.is_dict_encoded_string()) {
      auto dictDesc = catalog->getMetadataForDict(c.columnType.get_comp_param());
      dictionaries[col_key] = dictDesc->stringDict.get();
    }
  }

  tbb::task_group tg;

  tbb::parallel_for(
      tbb::blocked_range(0, (int)cols.size()),
      [this, &tg, &table_key, &td, mgr, &table, &cols, &dictionaries](auto range) {
        auto columnIter = std::next(cols.begin(), range.begin());
        for (auto col_idx = range.begin(); col_idx != range.end(); col_idx++) {
          auto& c = *(columnIter++);

          if (c.isSystemCol) {
            continue;  // must be processed by base interface implementation
          }

          // data comes like this - database_id, table_id, column_id, fragment_id
          ChunkKey key{table_key.first, table_key.second, c.columnId, 0};
          std::array<int, 3> col_key{table_key.first, table_key.second, c.columnId};

          if (col_idx >= table.num_columns()) {
            LOG(ERROR) << "Number of columns read from Arrow (" << table.num_columns()
                       << ") mismatch CREATE TABLE request: " << cols.size();
            break;
          }

          auto arr_col_chunked_array = table.column(col_idx);
          auto column_type = c.columnType.get_type();

          if (column_type != kDECIMAL && column_type != kNUMERIC &&
              !c.columnType.is_string()) {
            arr_col_chunked_array = replaceNullValues(arr_col_chunked_array, column_type);
          }

          if (c.columnType.is_dict_encoded_string()) {
            StringDictionary* dict = dictionaries[col_key];

            switch (arr_col_chunked_array->type()->id()) {
              case arrow::Type::STRING:
                arr_col_chunked_array =
                    createDictionaryEncodedColumn(dict, arr_col_chunked_array, c.columnType);
                break;
              case arrow::Type::DICTIONARY:
                arr_col_chunked_array =
                    convertArrowDictionary(dict, arr_col_chunked_array, c.columnType);
                break;
              default:
                CHECK(false);
            }
          } else if (column_type == kDECIMAL || column_type == kNUMERIC) {
            arr_col_chunked_array =
                convertDecimalToInteger(arr_col_chunked_array, c.columnType);
          }

          auto fragments =
              calculateFragmentsOffsets(*arr_col_chunked_array, td.maxFragRows);

          auto ctype = c.columnType.get_type();
          auto& col = m_columns[col_key];
          col.resize(fragments.size());

          for (size_t f = 0; f < fragments.size(); f++) {
            key[3] = f;
            auto& frag = col[f];
            bool is_varlen = ctype == kTEXT && !c.columnType.is_dict_encoded_string();
            size_t varlen = makeFragment(
                fragments[f], frag, arr_col_chunked_array->chunks(), is_varlen);

            // create buffer descriptors
            if (ctype == kTEXT && !c.columnType.is_dict_encoded_string()) {
              auto k = key;
              k.push_back(1);
              {
                auto b = mgr->createBuffer(k);
                b->setSize(varlen);
                b->initEncoder(c.columnType);
              }
              k[4] = 2;
              {
                auto b = mgr->createBuffer(k);
                b->setSqlType(SQLTypeInfo(kINT, false));
                b->setSize(frag.sz * b->getSqlType().get_size());
              }
            } else {
              auto b = mgr->createBuffer(key);
              b->setSize(frag.sz * c.columnType.get_size());
              b->initEncoder(c.columnType);
              size_t type_size = c.columnType.get_size();
              tg.run([b, fr = &frag, type_size]() {
                size_t sz = 0;
                for (size_t i = 0; i < fr->chunks.size(); i++) {
                  auto& chunk = fr->chunks[i];
                  int offset = (i == 0) ? fr->offset : 0;
                  size_t size = (i == fr->chunks.size() - 1) ? (fr->sz - sz)
                                                             : (chunk->length - offset);
                  sz += size;
                  auto data = chunk->buffers[1]->data();
                  b->getEncoder()->updateStatsEncoded(
                      (const int8_t*)data + offset * type_size, size);
                }
              });
              b->getEncoder()->setNumElems(frag.sz);
            }
          }
        }
      });  // each col and fragment

  // wait untill all stats have been updated
  tg.wait();
}

void ArrowForeignStorageBase::append(
    const std::vector<ForeignStorageColumnBuffer>& column_buffers) {
  CHECK(false);
}

void ArrowForeignStorageBase::read(const ChunkKey& chunk_key,
                                   const SQLTypeInfo& sql_type,
                                   int8_t* dest,
                                   const size_t numBytes) {
  std::array<int, 3> col_key{chunk_key[0], chunk_key[1], chunk_key[2]};
  auto& frag = m_columns.at(col_key).at(chunk_key[3]);

  CHECK(!frag.chunks.empty() || !chunk_key[3]);
  int64_t sz = 0, copied = 0;
  int varlen_offset = 0;
  size_t read_size = 0;
  for (size_t i = 0; i < frag.chunks.size(); i++) {
    auto& array_data = frag.chunks[i];
    int offset = (i == 0) ? frag.offset : 0;
    size_t size = (i == frag.chunks.size() - 1) ? (frag.sz - read_size)
                                                : (array_data->length - offset);
    read_size += size;
    arrow::Buffer* bp = nullptr;
    if (sql_type.is_dict_encoded_string()) {
      // array_data->buffers[1] stores dictionary indexes
      bp = array_data->buffers[1].get();
    } else if (sql_type.get_type() == kTEXT) {
      CHECK_GE(array_data->buffers.size(), 3UL);
      // array_data->buffers[2] stores string array
      bp = array_data->buffers[2].get();
    } else if (array_data->null_count != array_data->length) {
      // any type except strings (none encoded strings offsets go here as well)
      CHECK_GE(array_data->buffers.size(), 2UL);
      bp = array_data->buffers[1].get();
    }
    CHECK(bp);
    // offset buffer for none encoded strings need to be merged
    if (chunk_key.size() == 5 && chunk_key[4] == 2) {
      auto data = reinterpret_cast<const uint32_t*>(bp->data()) + offset;
      auto dest_ui32 = reinterpret_cast<uint32_t*>(dest);
      // as size contains count of string in chunk slice it would always be one less
      // then offsets array size
      sz = (size + 1) * sizeof(uint32_t);
      if (sz > 0) {
        if (i != 0) {
          // We merge arrow chunks with string offsets into a single contigous fragment.
          // Each string is represented by a pair of offsets, thus size of offset table
          // is num strings + 1. When merging two chunks, the last number in the first
          // chunk duplicates the first number in the second chunk, so we skip it.
          data++;
          sz -= sizeof(uint32_t);
        } else {
          // As we support cases when fragment starts with offset of arrow chunk we need
          // to substract the first element of the first chunk from all elements in that
          // fragment
          varlen_offset -= data[0];
        }
        // We also re-calculate offsets in the second chunk as it is a continuation of
        // the first one.
        std::transform(data,
                       data + (sz / sizeof(uint32_t)),
                       dest_ui32,
                       [varlen_offset](uint32_t val) { return val + varlen_offset; });
        varlen_offset += data[(sz / sizeof(uint32_t)) - 1];
      }
    } else {
      auto fixed_type = dynamic_cast<arrow::FixedWidthType*>(array_data->type.get());
      if (fixed_type) {
        std::memcpy(
            dest,
            bp->data() + (array_data->offset + offset) * (fixed_type->bit_width() / 8),
            sz = size * (fixed_type->bit_width() / 8));
      } else {
        auto offsets_buffer =
            reinterpret_cast<const uint32_t*>(array_data->buffers[1]->data());
        auto string_buffer_offset = offsets_buffer[offset + array_data->offset];
        auto string_buffer_size =
            offsets_buffer[offset + array_data->offset + size] - string_buffer_offset;
        std::memcpy(dest, bp->data() + string_buffer_offset, sz = string_buffer_size);
      }
    }
    dest += sz;
    copied += sz;
  }
  CHECK_EQ(numBytes, size_t(copied));
}

int8_t* ArrowForeignStorageBase::tryZeroCopy(const ChunkKey& chunk_key,
                                             const SQLTypeInfo& sql_type,
                                             const size_t numBytes) {
  std::array<int, 3> col_key{chunk_key[0], chunk_key[1], chunk_key[2]};
  auto& frag = m_columns.at(col_key).at(chunk_key[3]);

  // fragment should be continious to allow zero copy
  if (frag.chunks.size() != 1) {
    return nullptr;
  }

  auto& array_data = frag.chunks[0];
  int offset = frag.offset;

  arrow::Buffer* bp = nullptr;
  if (sql_type.is_dict_encoded_string()) {
    // array_data->buffers[1] stores dictionary indexes
    bp = array_data->buffers[1].get();
  } else if (sql_type.get_type() == kTEXT) {
    CHECK_GE(array_data->buffers.size(), 3UL);
    // array_data->buffers[2] stores string array
    bp = array_data->buffers[2].get();
  } else if (array_data->null_count != array_data->length) {
    // any type except strings (none encoded strings offsets go here as well)
    CHECK_GE(array_data->buffers.size(), 2UL);
    bp = array_data->buffers[1].get();
  }

  // arrow buffer is empty, it means we should fill fragment with null's in read function
  if (!bp) {
    return nullptr;
  }

  auto data = reinterpret_cast<int8_t*>(const_cast<uint8_t*>(bp->data()));

  // if buffer is null encoded string index buffer
  if (chunk_key.size() == 5 && chunk_key[4] == 2) {
    // if offset != 0 we need to recalculate index buffer by adding  offset to each index
    if (offset != 0) {
      return nullptr;
    } else {
      return data;
    }
  }

  auto fixed_type = dynamic_cast<arrow::FixedWidthType*>(array_data->type.get());
  if (fixed_type) {
    return data + (array_data->offset + offset) * (fixed_type->bit_width() / 8);
  }
  // if buffer is none encoded string data buffer
  // then we should find it's offset in offset buffer
  auto offsets_buffer = reinterpret_cast<const uint32_t*>(array_data->buffers[1]->data());
  auto string_buffer_offset = offsets_buffer[offset + array_data->offset];
  return data + string_buffer_offset;
}

class ArrowForeignStorage : public ArrowForeignStorageBase {
 public:
  ArrowForeignStorage() {}

  void prepareTable(const int db_id,
                    const std::string& type,
                    TableDescriptor& td,
                    std::list<ColumnDescriptor>& cols) override;
  void registerTable(Catalog_Namespace::Catalog* catalog,
                     std::pair<int, int> table_key,
                     const std::string& type,
                     const TableDescriptor& td,
                     const std::list<ColumnDescriptor>& cols,
                     Data_Namespace::AbstractBufferMgr* mgr) override;

  std::string getType() const override;

  std::string name;

  static std::map<std::string, std::shared_ptr<arrow::Table>> tables;
};

std::map<std::string, std::shared_ptr<arrow::Table>> ArrowForeignStorage::tables =
    std::map<std::string, std::shared_ptr<arrow::Table>>();

void ArrowForeignStorage::prepareTable(const int db_id,
                                       const std::string& name,
                                       TableDescriptor& td,
                                       std::list<ColumnDescriptor>& cols) {
  this->name = name;
  auto table = tables[name];
  for (auto& field : table->schema()->fields()) {
    ColumnDescriptor cd;
    cd.columnName = field->name();
    cd.columnType = getOmnisciType(*field->type());
    cols.push_back(cd);
  }
}

void ArrowForeignStorage::registerTable(Catalog_Namespace::Catalog* catalog,
                                        std::pair<int, int> table_key,
                                        const std::string& info,
                                        const TableDescriptor& td,
                                        const std::list<ColumnDescriptor>& cols,
                                        Data_Namespace::AbstractBufferMgr* mgr) {
  parseArrowTable(catalog, table_key, info, td, cols, mgr, *(tables[name].get()));
}

std::string ArrowForeignStorage::getType() const {
  LOG(INFO) << "CSV backed temporary tables has been activated. Create table `with "
               "(storage_type='CSV:path/to/file.csv');`\n";
  return "ARROW";
}

void setArrowTable(std::string name, std::shared_ptr<arrow::Table> table) {
  ArrowForeignStorage::tables[name] = table;
}

void releaseArrowTable(std::string name) {
  ArrowForeignStorage::tables.erase(name);
}

void registerArrowForeignStorage(std::shared_ptr<ForeignStorageInterface> fsi) {
  fsi->registerPersistentStorageInterface(std::make_unique<ArrowForeignStorage>());
}

class ArrowCsvForeignStorage : public ArrowForeignStorageBase {
 public:
  ArrowCsvForeignStorage() {}

  void prepareTable(const int db_id,
                    const std::string& type,
                    TableDescriptor& td,
                    std::list<ColumnDescriptor>& cols) override;
  void registerTable(Catalog_Namespace::Catalog* catalog,
                     std::pair<int, int> table_key,
                     const std::string& type,
                     const TableDescriptor& td,
                     const std::list<ColumnDescriptor>& cols,
                     Data_Namespace::AbstractBufferMgr* mgr) override;

  std::string getType() const override;
};

void ArrowCsvForeignStorage::prepareTable(const int db_id,
                                          const std::string& type,
                                          TableDescriptor& td,
                                          std::list<ColumnDescriptor>& cols) {}

void ArrowCsvForeignStorage::registerTable(Catalog_Namespace::Catalog* catalog,
                                           std::pair<int, int> table_key,
                                           const std::string& info,
                                           const TableDescriptor& td,
                                           const std::list<ColumnDescriptor>& cols,
                                           Data_Namespace::AbstractBufferMgr* mgr) {
  const DataframeTableDescriptor* df_td =
      dynamic_cast<const DataframeTableDescriptor*>(&td);
  bool isDataframe = df_td ? true : false;
  std::unique_ptr<DataframeTableDescriptor> df_td_owned;
  if (!isDataframe) {
    df_td_owned = std::make_unique<DataframeTableDescriptor>(td);
    CHECK(df_td_owned);
    df_td = df_td_owned.get();
  }

#ifdef ENABLE_ARROW_4
  auto io_context = arrow::io::default_io_context();
#else
  auto io_context = arrow::default_memory_pool();
#endif
  auto arrow_parse_options = arrow::csv::ParseOptions::Defaults();
  arrow_parse_options.quoting = false;
  arrow_parse_options.escaping = false;
  arrow_parse_options.newlines_in_values = false;
  arrow_parse_options.delimiter = *df_td->delimiter.c_str();
  auto arrow_read_options = arrow::csv::ReadOptions::Defaults();
  arrow_read_options.use_threads = true;

  arrow_read_options.block_size = 20 * 1024 * 1024;
  arrow_read_options.autogenerate_column_names = false;
  arrow_read_options.skip_rows =
      df_td->hasHeader ? (df_td->skipRows + 1) : df_td->skipRows;

  auto arrow_convert_options = arrow::csv::ConvertOptions::Defaults();
  arrow_convert_options.check_utf8 = false;
  arrow_convert_options.include_columns = arrow_read_options.column_names;
  arrow_convert_options.strings_can_be_null = true;

  for (auto& c : cols) {
    if (c.isSystemCol) {
      continue;  // must be processed by base interface implementation
    }
    arrow_convert_options.column_types.emplace(c.columnName,
                                               getArrowImportType(c.columnType));
    arrow_read_options.column_names.push_back(c.columnName);
  }

  std::shared_ptr<arrow::io::ReadableFile> inp;
  auto file_result = arrow::io::ReadableFile::Open(info.c_str());
  ARROW_THROW_NOT_OK(file_result.status());
  inp = file_result.ValueOrDie();

  auto table_reader_result = arrow::csv::TableReader::Make(
      io_context, inp, arrow_read_options, arrow_parse_options, arrow_convert_options);
  ARROW_THROW_NOT_OK(table_reader_result.status());
  auto table_reader = table_reader_result.ValueOrDie();

  std::shared_ptr<arrow::Table> arrowTable;
  auto time = measure<>::execution([&]() {
    auto arrow_table_result = table_reader->Read();
    ARROW_THROW_NOT_OK(arrow_table_result.status());
    arrowTable = arrow_table_result.ValueOrDie();
  });

  VLOG(1) << "Read Arrow CSV file " << info << " in " << time << "ms";

  arrow::Table& table = *arrowTable.get();
  parseArrowTable(catalog, table_key, info, td, cols, mgr, table);
}

std::string ArrowCsvForeignStorage::getType() const {
  LOG(INFO) << "CSV backed temporary tables has been activated. Create table `with "
               "(storage_type='CSV:path/to/file.csv');`\n";
  return "CSV";
}

void registerArrowCsvForeignStorage(std::shared_ptr<ForeignStorageInterface> fsi) {
  fsi->registerPersistentStorageInterface(std::make_unique<ArrowCsvForeignStorage>());
}
