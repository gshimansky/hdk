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

#ifdef __CUDACC__
#error This code is not intended to be compiled with a CUDA C++ compiler
#endif  // __CUDACC__

#ifdef _MSC_VER
#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#endif

#include "RuntimeFunctions.h"
#include "HyperLogLogRank.h"
#include "MurmurHash.h"
#include "Shared/BufferCompaction.h"
#include "Shared/funcannotations.h"
#include "Shared/quantile.h"
#include "TypePunning.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

// decoder implementations

#include "DecodersImpl.h"

// arithmetic operator implementations

#define DEF_ARITH_NULLABLE(type, null_type, opname, opsym)                 \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE type opname##_##type##_nullable( \
      const type lhs, const type rhs, const null_type null_val) {          \
    if (lhs != null_val && rhs != null_val) {                              \
      return lhs opsym rhs;                                                \
    }                                                                      \
    return null_val;                                                       \
  }

#define DEF_ARITH_NULLABLE_LHS(type, null_type, opname, opsym)                 \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE type opname##_##type##_nullable_lhs( \
      const type lhs, const type rhs, const null_type null_val) {              \
    if (lhs != null_val) {                                                     \
      return lhs opsym rhs;                                                    \
    }                                                                          \
    return null_val;                                                           \
  }

#define DEF_ARITH_NULLABLE_RHS(type, null_type, opname, opsym)                 \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE type opname##_##type##_nullable_rhs( \
      const type lhs, const type rhs, const null_type null_val) {              \
    if (rhs != null_val) {                                                     \
      return lhs opsym rhs;                                                    \
    }                                                                          \
    return null_val;                                                           \
  }

#define DEF_CMP_NULLABLE(type, null_type, opname, opsym)                     \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE int8_t opname##_##type##_nullable( \
      const type lhs,                                                        \
      const type rhs,                                                        \
      const null_type null_val,                                              \
      const int8_t null_bool_val) {                                          \
    if (lhs != null_val && rhs != null_val) {                                \
      return lhs opsym rhs;                                                  \
    }                                                                        \
    return null_bool_val;                                                    \
  }

#define DEF_CMP_NULLABLE_LHS(type, null_type, opname, opsym)                     \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE int8_t opname##_##type##_nullable_lhs( \
      const type lhs,                                                            \
      const type rhs,                                                            \
      const null_type null_val,                                                  \
      const int8_t null_bool_val) {                                              \
    if (lhs != null_val) {                                                       \
      return lhs opsym rhs;                                                      \
    }                                                                            \
    return null_bool_val;                                                        \
  }

#define DEF_CMP_NULLABLE_RHS(type, null_type, opname, opsym)                     \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE int8_t opname##_##type##_nullable_rhs( \
      const type lhs,                                                            \
      const type rhs,                                                            \
      const null_type null_val,                                                  \
      const int8_t null_bool_val) {                                              \
    if (rhs != null_val) {                                                       \
      return lhs opsym rhs;                                                      \
    }                                                                            \
    return null_bool_val;                                                        \
  }

#define DEF_SAFE_DIV_NULLABLE(type, null_type, opname)            \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE type safe_div_##type(   \
      const type lhs, const type rhs, const null_type null_val) { \
    if (lhs != null_val && rhs != null_val && rhs != 0) {         \
      return lhs / rhs;                                           \
    }                                                             \
    return null_val;                                              \
  }

#define DEF_SAFE_INF_DIV_NULLABLE(type, null_type, opname)                      \
  extern "C" ALWAYS_INLINE type safe_inf_div_##type(const type lhs,             \
                                                    const type rhs,             \
                                                    const null_type inf_val,    \
                                                    const null_type null_val) { \
    if (rhs != 0) {                                                             \
      return lhs / rhs;                                                         \
    }                                                                           \
    if (lhs > 0) {                                                              \
      return inf_val;                                                           \
    } else if (lhs == 0) {                                                      \
      return null_val;                                                          \
    } else {                                                                    \
      return -inf_val;                                                          \
    }                                                                           \
  }

#define DEF_BINARY_NULLABLE_ALL_OPS(type, null_type) \
  DEF_ARITH_NULLABLE(type, null_type, add, +)        \
  DEF_ARITH_NULLABLE(type, null_type, sub, -)        \
  DEF_ARITH_NULLABLE(type, null_type, mul, *)        \
  DEF_ARITH_NULLABLE(type, null_type, div, /)        \
  DEF_SAFE_DIV_NULLABLE(type, null_type, safe_div)   \
  DEF_ARITH_NULLABLE_LHS(type, null_type, add, +)    \
  DEF_ARITH_NULLABLE_LHS(type, null_type, sub, -)    \
  DEF_ARITH_NULLABLE_LHS(type, null_type, mul, *)    \
  DEF_ARITH_NULLABLE_LHS(type, null_type, div, /)    \
  DEF_ARITH_NULLABLE_RHS(type, null_type, add, +)    \
  DEF_ARITH_NULLABLE_RHS(type, null_type, sub, -)    \
  DEF_ARITH_NULLABLE_RHS(type, null_type, mul, *)    \
  DEF_ARITH_NULLABLE_RHS(type, null_type, div, /)    \
  DEF_CMP_NULLABLE(type, null_type, eq, ==)          \
  DEF_CMP_NULLABLE(type, null_type, ne, !=)          \
  DEF_CMP_NULLABLE(type, null_type, lt, <)           \
  DEF_CMP_NULLABLE(type, null_type, gt, >)           \
  DEF_CMP_NULLABLE(type, null_type, le, <=)          \
  DEF_CMP_NULLABLE(type, null_type, ge, >=)          \
  DEF_CMP_NULLABLE_LHS(type, null_type, eq, ==)      \
  DEF_CMP_NULLABLE_LHS(type, null_type, ne, !=)      \
  DEF_CMP_NULLABLE_LHS(type, null_type, lt, <)       \
  DEF_CMP_NULLABLE_LHS(type, null_type, gt, >)       \
  DEF_CMP_NULLABLE_LHS(type, null_type, le, <=)      \
  DEF_CMP_NULLABLE_LHS(type, null_type, ge, >=)      \
  DEF_CMP_NULLABLE_RHS(type, null_type, eq, ==)      \
  DEF_CMP_NULLABLE_RHS(type, null_type, ne, !=)      \
  DEF_CMP_NULLABLE_RHS(type, null_type, lt, <)       \
  DEF_CMP_NULLABLE_RHS(type, null_type, gt, >)       \
  DEF_CMP_NULLABLE_RHS(type, null_type, le, <=)      \
  DEF_CMP_NULLABLE_RHS(type, null_type, ge, >=)

DEF_BINARY_NULLABLE_ALL_OPS(int8_t, int64_t)
DEF_BINARY_NULLABLE_ALL_OPS(int16_t, int64_t)
DEF_BINARY_NULLABLE_ALL_OPS(int32_t, int64_t)
DEF_BINARY_NULLABLE_ALL_OPS(int64_t, int64_t)
DEF_BINARY_NULLABLE_ALL_OPS(float, float)
DEF_BINARY_NULLABLE_ALL_OPS(double, double)
DEF_ARITH_NULLABLE(int8_t, int64_t, mod, %)
DEF_ARITH_NULLABLE(int16_t, int64_t, mod, %)
DEF_ARITH_NULLABLE(int32_t, int64_t, mod, %)
DEF_ARITH_NULLABLE(int64_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_LHS(int8_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_LHS(int16_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_LHS(int32_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_LHS(int64_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_RHS(int8_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_RHS(int16_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_RHS(int32_t, int64_t, mod, %)
DEF_ARITH_NULLABLE_RHS(int64_t, int64_t, mod, %)
DEF_SAFE_INF_DIV_NULLABLE(float, float, safe_inf_div)
DEF_SAFE_INF_DIV_NULLABLE(double, double, safe_inf_div)

#undef DEF_BINARY_NULLABLE_ALL_OPS
#undef DEF_SAFE_DIV_NULLABLE
#undef DEF_CMP_NULLABLE_RHS
#undef DEF_CMP_NULLABLE_LHS
#undef DEF_CMP_NULLABLE
#undef DEF_ARITH_NULLABLE_RHS
#undef DEF_ARITH_NULLABLE_LHS
#undef DEF_ARITH_NULLABLE
#undef DEF_SAFE_INF_DIV_NULLABLE

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t
scale_decimal_up(const int64_t operand,
                 const uint64_t scale,
                 const int64_t operand_null_val,
                 const int64_t result_null_val) {
  return operand != operand_null_val ? operand * scale : result_null_val;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t
scale_decimal_down_nullable(const int64_t operand,
                            const int64_t scale,
                            const int64_t null_val) {
  // rounded scale down of a decimal
  if (operand == null_val) {
    return null_val;
  }

  int64_t tmp = scale >> 1;
  tmp = operand >= 0 ? operand + tmp : operand - tmp;
  return tmp / scale;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t
scale_decimal_down_not_nullable(const int64_t operand,
                                const int64_t scale,
                                const int64_t null_val) {
  int64_t tmp = scale >> 1;
  tmp = operand >= 0 ? operand + tmp : operand - tmp;
  return tmp / scale;
}

// Return floor(dividend / divisor).
// Assumes 0 < divisor.
extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t floor_div_lhs(const int64_t dividend,
                                                              const int64_t divisor) {
  return (dividend < 0 ? dividend - (divisor - 1) : dividend) / divisor;
}

// Return floor(dividend / divisor) or NULL if dividend IS NULL.
// Assumes 0 < divisor.
extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t
floor_div_nullable_lhs(const int64_t dividend,
                       const int64_t divisor,
                       const int64_t null_val) {
  return dividend == null_val ? null_val : floor_div_lhs(dividend, divisor);
}

#define DEF_UMINUS_NULLABLE(type, null_type)                             \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE type uminus_##type##_nullable( \
      const type operand, const null_type null_val) {                    \
    return operand == null_val ? null_val : -operand;                    \
  }

DEF_UMINUS_NULLABLE(int8_t, int8_t)
DEF_UMINUS_NULLABLE(int16_t, int16_t)
DEF_UMINUS_NULLABLE(int32_t, int32_t)
DEF_UMINUS_NULLABLE(int64_t, int64_t)
DEF_UMINUS_NULLABLE(float, float)
DEF_UMINUS_NULLABLE(double, double)

#undef DEF_UMINUS_NULLABLE

#define DEF_CAST_NULLABLE(from_type, to_type)                                   \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE to_type                               \
      cast_##from_type##_to_##to_type##_nullable(const from_type operand,       \
                                                 const from_type from_null_val, \
                                                 const to_type to_null_val) {   \
    return operand == from_null_val ? to_null_val : operand;                    \
  }

#define DEF_CAST_SCALED_NULLABLE(from_type, to_type)                                   \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE to_type                                      \
      cast_##from_type##_to_##to_type##_scaled_nullable(const from_type operand,       \
                                                        const from_type from_null_val, \
                                                        const to_type to_null_val,     \
                                                        const to_type multiplier) {    \
    return operand == from_null_val ? to_null_val : multiplier * operand;              \
  }

#define DEF_CAST_NULLABLE_BIDIR(type1, type2) \
  DEF_CAST_NULLABLE(type1, type2)             \
  DEF_CAST_NULLABLE(type2, type1)

#define DEF_ROUND_NULLABLE(from_type, to_type)                                  \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE to_type                               \
      cast_##from_type##_to_##to_type##_nullable(const from_type operand,       \
                                                 const from_type from_null_val, \
                                                 const to_type to_null_val) {   \
    return operand == from_null_val                                             \
               ? to_null_val                                                    \
               : static_cast<to_type>(operand + (operand < from_type(0)         \
                                                     ? from_type(-0.5)          \
                                                     : from_type(0.5)));        \
  }

DEF_CAST_NULLABLE_BIDIR(int8_t, int16_t)
DEF_CAST_NULLABLE_BIDIR(int8_t, int32_t)
DEF_CAST_NULLABLE_BIDIR(int8_t, int64_t)
DEF_CAST_NULLABLE_BIDIR(int16_t, int32_t)
DEF_CAST_NULLABLE_BIDIR(int16_t, int64_t)
DEF_CAST_NULLABLE_BIDIR(int32_t, int64_t)
DEF_CAST_NULLABLE_BIDIR(float, double)

DEF_CAST_NULLABLE(int8_t, float)
DEF_CAST_NULLABLE(int16_t, float)
DEF_CAST_NULLABLE(int32_t, float)
DEF_CAST_NULLABLE(int64_t, float)
DEF_CAST_NULLABLE(int8_t, double)
DEF_CAST_NULLABLE(int16_t, double)
DEF_CAST_NULLABLE(int32_t, double)
DEF_CAST_NULLABLE(int64_t, double)

DEF_ROUND_NULLABLE(float, int8_t)
DEF_ROUND_NULLABLE(float, int16_t)
DEF_ROUND_NULLABLE(float, int32_t)
DEF_ROUND_NULLABLE(float, int64_t)
DEF_ROUND_NULLABLE(double, int8_t)
DEF_ROUND_NULLABLE(double, int16_t)
DEF_ROUND_NULLABLE(double, int32_t)
DEF_ROUND_NULLABLE(double, int64_t)

DEF_CAST_NULLABLE(uint8_t, int32_t)
DEF_CAST_NULLABLE(uint16_t, int32_t)
DEF_CAST_SCALED_NULLABLE(int64_t, float)
DEF_CAST_SCALED_NULLABLE(int64_t, double)

#undef DEF_ROUND_NULLABLE
#undef DEF_CAST_NULLABLE_BIDIR
#undef DEF_CAST_SCALED_NULLABLE
#undef DEF_CAST_NULLABLE

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int8_t logical_not(const int8_t operand,
                                                           const int8_t null_val) {
  return operand == null_val ? operand : (operand ? 0 : 1);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int8_t logical_and(const int8_t lhs,
                                                           const int8_t rhs,
                                                           const int8_t null_val) {
  if (lhs == null_val) {
    return rhs == 0 ? rhs : null_val;
  }
  if (rhs == null_val) {
    return lhs == 0 ? lhs : null_val;
  }
  return (lhs && rhs) ? 1 : 0;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int8_t logical_or(const int8_t lhs,
                                                          const int8_t rhs,
                                                          const int8_t null_val) {
  if (lhs == null_val) {
    return rhs == 0 ? null_val : rhs;
  }
  if (rhs == null_val) {
    return lhs == 0 ? null_val : lhs;
  }
  return (lhs || rhs) ? 1 : 0;
}

// aggregator implementations

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint64_t
agg_count(GENERIC_ADDR_SPACE uint64_t* agg, const int64_t) {
  return (*agg)++;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_count_distinct_bitmap(
    GENERIC_ADDR_SPACE int64_t* agg,
    const int64_t val,
    const int64_t min_val) {
  const uint64_t bitmap_idx = val - min_val;
  reinterpret_cast<GENERIC_ADDR_SPACE int8_t*>(*agg)[bitmap_idx >> 3] |=
      (1 << (bitmap_idx & 7));
}

#ifdef _MSC_VER
#define GPU_RT_STUB NEVER_INLINE
#else
#define GPU_RT_STUB NEVER_INLINE __attribute__((optnone))
#endif

extern "C" GPU_RT_STUB void agg_count_distinct_bitmap_gpu(GENERIC_ADDR_SPACE int64_t*,
                                                          const int64_t,
                                                          const int64_t,
                                                          const int64_t,
                                                          const int64_t,
                                                          const uint64_t,
                                                          const uint64_t) {}

extern "C" RUNTIME_EXPORT NEVER_INLINE void agg_approximate_count_distinct(
    GENERIC_ADDR_SPACE int64_t* agg,
    const int64_t key,
    const uint32_t b) {
  const uint64_t hash = MurmurHash64A(&key, sizeof(key), 0);
  const uint32_t index = hash >> (64 - b);
  const uint8_t rank = get_rank(hash << b, 64 - b);
  GENERIC_ADDR_SPACE uint8_t* M = reinterpret_cast<GENERIC_ADDR_SPACE uint8_t*>(*agg);
  M[index] = std::max(static_cast<uint8_t>(M[index]), rank);
}

extern "C" GPU_RT_STUB void agg_approximate_count_distinct_gpu(
    GENERIC_ADDR_SPACE int64_t*,
    const int64_t,
    const uint32_t,
    const int64_t,
    const int64_t) {}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int8_t bit_is_set(const int64_t bitset,
                                                          const int64_t val,
                                                          const int64_t min_val,
                                                          const int64_t max_val,
                                                          const int64_t null_val,
                                                          const int8_t null_bool_val) {
  if (val == null_val) {
    return null_bool_val;
  }
  if (val < min_val || val > max_val) {
    return 0;
  }
  if (!bitset) {
    return 0;
  }
  const uint64_t bitmap_idx = val - min_val;
  return (reinterpret_cast<GENERIC_ADDR_SPACE const int8_t*>(bitset))[bitmap_idx >> 3] &
                 (1 << (bitmap_idx & 7))
             ? 1
             : 0;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t agg_sum(GENERIC_ADDR_SPACE int64_t* agg,
                                                        const int64_t val) {
  const auto old = *agg;
  *agg += val;
  return old;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_max(GENERIC_ADDR_SPACE int64_t* agg,
                                                     const int64_t val) {
  *agg = std::max(static_cast<int64_t>(*agg), val);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_min(GENERIC_ADDR_SPACE int64_t* agg,
                                                     const int64_t val) {
  *agg = std::min(static_cast<int64_t>(*agg), val);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_id(GENERIC_ADDR_SPACE int64_t* agg,
                                                    const int64_t val) {
  *agg = val;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int8_t* agg_id_varlen(
    GENERIC_ADDR_SPACE int8_t* varlen_buffer,
    const int64_t offset,
    GENERIC_ADDR_SPACE const int8_t* value,
    const int64_t size_bytes) {
  for (auto i = 0; i < size_bytes; i++) {
    varlen_buffer[offset + i] = value[i];
  }
  return &varlen_buffer[offset];
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
checked_single_agg_id(GENERIC_ADDR_SPACE int64_t* agg,
                      const int64_t val,
                      const int64_t null_val) {
  if (val == null_val) {
    return 0;
  }

  if (*agg == val) {
    return 0;
  } else if (*agg == null_val) {
    *agg = val;
    return 0;
  } else {
    // see Execute::ERR_SINGLE_VALUE_FOUND_MULTIPLE_VALUES
    return 15;
  }
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_count_distinct_bitmap_skip_val(
    GENERIC_ADDR_SPACE int64_t* agg,
    const int64_t val,
    const int64_t min_val,
    const int64_t skip_val) {
  if (val != skip_val) {
    agg_count_distinct_bitmap(agg, val, min_val);
  }
}

extern "C" GPU_RT_STUB void agg_count_distinct_bitmap_skip_val_gpu(
    GENERIC_ADDR_SPACE int64_t*,
    const int64_t,
    const int64_t,
    const int64_t,
    const int64_t,
    const int64_t,
    const uint64_t,
    const uint64_t) {}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint32_t
agg_count_int32(GENERIC_ADDR_SPACE uint32_t* agg, const int32_t) {
  return (*agg)++;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
agg_sum_int32(GENERIC_ADDR_SPACE int32_t* agg, const int32_t val) {
  const auto old = *agg;
  *agg += val;
  return old;
}

#define DEF_AGG_MAX_INT(n)                                        \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_max_int##n(    \
      GENERIC_ADDR_SPACE int##n##_t* agg, const int##n##_t val) { \
    *agg = std::max(static_cast<int##n##_t>(*agg), val);          \
  }

DEF_AGG_MAX_INT(32)
DEF_AGG_MAX_INT(16)
DEF_AGG_MAX_INT(8)
#undef DEF_AGG_MAX_INT

#define DEF_AGG_MIN_INT(n)                                        \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_min_int##n(    \
      GENERIC_ADDR_SPACE int##n##_t* agg, const int##n##_t val) { \
    *agg = std::min(static_cast<int##n##_t>(*agg), val);          \
  }

DEF_AGG_MIN_INT(32)
DEF_AGG_MIN_INT(16)
DEF_AGG_MIN_INT(8)
#undef DEF_AGG_MIN_INT

#define DEF_AGG_ID_INT(n)                                         \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_id_int##n(     \
      GENERIC_ADDR_SPACE int##n##_t* agg, const int##n##_t val) { \
    *agg = val;                                                   \
  }

#define DEF_CHECKED_SINGLE_AGG_ID_INT(n)                                        \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t checked_single_agg_id_int##n( \
      GENERIC_ADDR_SPACE int##n##_t* agg,                                       \
      const int##n##_t val,                                                     \
      const int##n##_t null_val) {                                              \
    if (val == null_val) {                                                      \
      return 0;                                                                 \
    }                                                                           \
    if (*agg == val) {                                                          \
      return 0;                                                                 \
    } else if (*agg == null_val) {                                              \
      *agg = val;                                                               \
      return 0;                                                                 \
    } else {                                                                    \
      /* see Execute::ERR_SINGLE_VALUE_FOUND_MULTIPLE_VALUES*/                  \
      return 15;                                                                \
    }                                                                           \
  }

DEF_AGG_ID_INT(32)
DEF_AGG_ID_INT(16)
DEF_AGG_ID_INT(8)

DEF_CHECKED_SINGLE_AGG_ID_INT(32)
DEF_CHECKED_SINGLE_AGG_ID_INT(16)
DEF_CHECKED_SINGLE_AGG_ID_INT(8)

#undef DEF_AGG_ID_INT
#undef DEF_CHECKED_SINGLE_AGG_ID_INT

#define DEF_WRITE_PROJECTION_INT(n)                                      \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void write_projection_int##n(  \
      GENERIC_ADDR_SPACE int8_t* slot_ptr,                               \
      const int##n##_t val,                                              \
      const int64_t init_val) {                                          \
    if (val != init_val) {                                               \
      *reinterpret_cast<GENERIC_ADDR_SPACE int##n##_t*>(slot_ptr) = val; \
    }                                                                    \
  }

DEF_WRITE_PROJECTION_INT(64)
DEF_WRITE_PROJECTION_INT(32)
#undef DEF_WRITE_PROJECTION_INT

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t
agg_sum_skip_val(GENERIC_ADDR_SPACE int64_t* agg,
                 const int64_t val,
                 const int64_t skip_val) {
  const auto old = *agg;
  if (val != skip_val) {
    if (old != skip_val) {
      return agg_sum(agg, val);
    } else {
      *agg = val;
    }
  }
  return old;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
agg_sum_int32_skip_val(GENERIC_ADDR_SPACE int32_t* agg,
                       const int32_t val,
                       const int32_t skip_val) {
  const auto old = *agg;
  if (val != skip_val) {
    if (old != skip_val) {
      return agg_sum_int32(agg, val);
    } else {
      *agg = val;
    }
  }
  return old;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint64_t
agg_count_skip_val(GENERIC_ADDR_SPACE uint64_t* agg,
                   const int64_t val,
                   const int64_t skip_val) {
  if (val != skip_val) {
    return agg_count(agg, val);
  }
  return *agg;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint32_t
agg_count_int32_skip_val(GENERIC_ADDR_SPACE uint32_t* agg,
                         const int32_t val,
                         const int32_t skip_val) {
  if (val != skip_val) {
    return agg_count_int32(agg, val);
  }
  return *agg;
}

#define DEF_SKIP_AGG_ADD(base_agg_func)                                          \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void base_agg_func##_skip_val(         \
      GENERIC_ADDR_SPACE DATA_T* agg, const DATA_T val, const DATA_T skip_val) { \
    if (val != skip_val) {                                                       \
      base_agg_func(agg, val);                                                   \
    }                                                                            \
  }

#define DEF_SKIP_AGG(base_agg_func)                                              \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void base_agg_func##_skip_val(         \
      GENERIC_ADDR_SPACE DATA_T* agg, const DATA_T val, const DATA_T skip_val) { \
    if (val != skip_val) {                                                       \
      const DATA_T old_agg = *agg;                                               \
      if (old_agg != skip_val) {                                                 \
        base_agg_func(agg, val);                                                 \
      } else {                                                                   \
        *agg = val;                                                              \
      }                                                                          \
    }                                                                            \
  }

#define DATA_T int64_t
DEF_SKIP_AGG(agg_max)
DEF_SKIP_AGG(agg_min)
#undef DATA_T

#define DATA_T int32_t
DEF_SKIP_AGG(agg_max_int32)
DEF_SKIP_AGG(agg_min_int32)
#undef DATA_T

#define DATA_T int16_t
DEF_SKIP_AGG(agg_max_int16)
DEF_SKIP_AGG(agg_min_int16)
#undef DATA_T

#define DATA_T int8_t
DEF_SKIP_AGG(agg_max_int8)
DEF_SKIP_AGG(agg_min_int8)
#undef DATA_T

#undef DEF_SKIP_AGG_ADD
#undef DEF_SKIP_AGG

// TODO(alex): fix signature

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint64_t
agg_count_double(GENERIC_ADDR_SPACE uint64_t* agg, const double val) {
  return (*agg)++;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_sum_double(
    GENERIC_ADDR_SPACE int64_t* agg,
    const double val) {
  const auto r = *reinterpret_cast<GENERIC_ADDR_SPACE const double*>(agg) + val;
  *agg = *reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(may_alias_ptr(&r));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_max_double(
    GENERIC_ADDR_SPACE int64_t* agg,
    const double val) {
  const auto r = std::max(
      static_cast<const double>(*reinterpret_cast<GENERIC_ADDR_SPACE const double*>(agg)),
      val);
  *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(may_alias_ptr(&r)));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_min_double(
    GENERIC_ADDR_SPACE int64_t* agg,
    const double val) {
  const auto r = std::min(
      static_cast<const double>(*reinterpret_cast<GENERIC_ADDR_SPACE const double*>(agg)),
      val);
  *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(may_alias_ptr(&r)));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_id_double(
    GENERIC_ADDR_SPACE int64_t* agg,
    const double val) {
  *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(may_alias_ptr(&val)));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
checked_single_agg_id_double(GENERIC_ADDR_SPACE int64_t* agg,
                             const double val,
                             const double null_val) {
  if (val == null_val) {
    return 0;
  }

  if (*agg ==
      *(reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(may_alias_ptr(&val)))) {
    return 0;
  } else if (*agg == *(reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(
                         may_alias_ptr(&null_val)))) {
    *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(may_alias_ptr(&val)));
    return 0;
  } else {
    // see Execute::ERR_SINGLE_VALUE_FOUND_MULTIPLE_VALUES
    return 15;
  }
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint32_t
agg_count_float(GENERIC_ADDR_SPACE uint32_t* agg, const float val) {
  return (*agg)++;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_sum_float(
    GENERIC_ADDR_SPACE int32_t* agg,
    const float val) {
  const auto r = *reinterpret_cast<GENERIC_ADDR_SPACE const float*>(agg) + val;
  *agg = *reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(may_alias_ptr(&r));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_max_float(
    GENERIC_ADDR_SPACE int32_t* agg,
    const float val) {
  const auto r = std::max(
      static_cast<const float>(*reinterpret_cast<GENERIC_ADDR_SPACE const float*>(agg)),
      val);
  *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(may_alias_ptr(&r)));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_min_float(
    GENERIC_ADDR_SPACE int32_t* agg,
    const float val) {
  const auto r = std::min(
      static_cast<const float>(*reinterpret_cast<GENERIC_ADDR_SPACE const float*>(agg)),
      val);
  *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(may_alias_ptr(&r)));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void agg_id_float(GENERIC_ADDR_SPACE int32_t* agg,
                                                          const float val) {
  *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(may_alias_ptr(&val)));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
checked_single_agg_id_float(GENERIC_ADDR_SPACE int32_t* agg,
                            const float val,
                            const float null_val) {
  if (val == null_val) {
    return 0;
  }

  if (*agg ==
      *(reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(may_alias_ptr(&val)))) {
    return 0;
  } else if (*agg == *(reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(
                         may_alias_ptr(&null_val)))) {
    *agg = *(reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(may_alias_ptr(&val)));
    return 0;
  } else {
    // see Execute::ERR_SINGLE_VALUE_FOUND_MULTIPLE_VALUES
    return 15;
  }
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint64_t
agg_count_double_skip_val(GENERIC_ADDR_SPACE uint64_t* agg,
                          const double val,
                          const double skip_val) {
  if (val != skip_val) {
    return agg_count_double(agg, val);
  }
  return *agg;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint32_t
agg_count_float_skip_val(GENERIC_ADDR_SPACE uint32_t* agg,
                         const float val,
                         const float skip_val) {
  if (val != skip_val) {
    return agg_count_float(agg, val);
  }
  return *agg;
}

#define DEF_SKIP_AGG_ADD(base_agg_func)                                          \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void base_agg_func##_skip_val(         \
      GENERIC_ADDR_SPACE ADDR_T* agg, const DATA_T val, const DATA_T skip_val) { \
    if (val != skip_val) {                                                       \
      base_agg_func(agg, val);                                                   \
    }                                                                            \
  }

#define DEF_SKIP_AGG(base_agg_func)                                                      \
  extern "C" RUNTIME_EXPORT ALWAYS_INLINE void base_agg_func##_skip_val(                 \
      GENERIC_ADDR_SPACE ADDR_T* agg, const DATA_T val, const DATA_T skip_val) {         \
    if (val != skip_val) {                                                               \
      const ADDR_T old_agg = *agg;                                                       \
      if (old_agg != *reinterpret_cast<GENERIC_ADDR_SPACE const ADDR_T*>(                \
                         may_alias_ptr(&skip_val))) {                                    \
        base_agg_func(agg, val);                                                         \
      } else {                                                                           \
        *agg = *reinterpret_cast<GENERIC_ADDR_SPACE const ADDR_T*>(may_alias_ptr(&val)); \
      }                                                                                  \
    }                                                                                    \
  }

#define DATA_T double
#define ADDR_T int64_t
DEF_SKIP_AGG(agg_sum_double)
DEF_SKIP_AGG(agg_max_double)
DEF_SKIP_AGG(agg_min_double)
#undef ADDR_T
#undef DATA_T

#define DATA_T float
#define ADDR_T int32_t
DEF_SKIP_AGG(agg_sum_float)
DEF_SKIP_AGG(agg_max_float)
DEF_SKIP_AGG(agg_min_float)
#undef ADDR_T
#undef DATA_T

#undef DEF_SKIP_AGG_ADD
#undef DEF_SKIP_AGG

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t decimal_floor(const int64_t x,
                                                              const int64_t scale) {
  if (x >= 0) {
    return x / scale * scale;
  }
  if (!(x % scale)) {
    return x;
  }
  return x / scale * scale - scale;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t decimal_ceil(const int64_t x,
                                                             const int64_t scale) {
  return decimal_floor(x, scale) + (x % scale ? scale : 0);
}

// Shared memory aggregators. Should never be called,
// real implementations are in cuda_mapd_rt.cu.
#define DEF_SHARED_AGG_RET_STUBS(base_agg_func)                                      \
  extern "C" GPU_RT_STUB uint64_t base_agg_func##_shared(                            \
      GENERIC_ADDR_SPACE uint64_t* agg, const int64_t val) {                         \
    return 0;                                                                        \
  }                                                                                  \
                                                                                     \
  extern "C" GPU_RT_STUB uint64_t base_agg_func##_skip_val_shared(                   \
      GENERIC_ADDR_SPACE uint64_t* agg, const int64_t val, const int64_t skip_val) { \
    return 0;                                                                        \
  }                                                                                  \
  extern "C" GPU_RT_STUB uint32_t base_agg_func##_int32_shared(                      \
      GENERIC_ADDR_SPACE uint32_t* agg, const int32_t val) {                         \
    return 0;                                                                        \
  }                                                                                  \
                                                                                     \
  extern "C" GPU_RT_STUB uint32_t base_agg_func##_int32_skip_val_shared(             \
      GENERIC_ADDR_SPACE uint32_t* agg, const int32_t val, const int32_t skip_val) { \
    return 0;                                                                        \
  }                                                                                  \
                                                                                     \
  extern "C" GPU_RT_STUB uint64_t base_agg_func##_double_shared(                     \
      GENERIC_ADDR_SPACE uint64_t* agg, const double val) {                          \
    return 0;                                                                        \
  }                                                                                  \
                                                                                     \
  extern "C" GPU_RT_STUB uint64_t base_agg_func##_double_skip_val_shared(            \
      GENERIC_ADDR_SPACE uint64_t* agg, const double val, const double skip_val) {   \
    return 0;                                                                        \
  }                                                                                  \
  extern "C" GPU_RT_STUB uint32_t base_agg_func##_float_shared(                      \
      GENERIC_ADDR_SPACE uint32_t* agg, const float val) {                           \
    return 0;                                                                        \
  }                                                                                  \
                                                                                     \
  extern "C" GPU_RT_STUB uint32_t base_agg_func##_float_skip_val_shared(             \
      GENERIC_ADDR_SPACE uint32_t* agg, const float val, const float skip_val) {     \
    return 0;                                                                        \
  }

#define DEF_SHARED_AGG_STUBS(base_agg_func)                                           \
  extern "C" GPU_RT_STUB void base_agg_func##_shared(GENERIC_ADDR_SPACE int64_t* agg, \
                                                     const int64_t val) {}            \
                                                                                      \
  extern "C" GPU_RT_STUB void base_agg_func##_skip_val_shared(                        \
      GENERIC_ADDR_SPACE int64_t* agg, const int64_t val, const int64_t skip_val) {}  \
  extern "C" GPU_RT_STUB void base_agg_func##_int32_shared(                           \
      GENERIC_ADDR_SPACE int32_t* agg, const int32_t val) {}                          \
  extern "C" GPU_RT_STUB void base_agg_func##_int16_shared(                           \
      GENERIC_ADDR_SPACE int16_t* agg, const int16_t val) {}                          \
  extern "C" GPU_RT_STUB void base_agg_func##_int8_shared(                            \
      GENERIC_ADDR_SPACE int8_t* agg, const int8_t val) {}                            \
                                                                                      \
  extern "C" GPU_RT_STUB void base_agg_func##_int32_skip_val_shared(                  \
      GENERIC_ADDR_SPACE int32_t* agg, const int32_t val, const int32_t skip_val) {}  \
                                                                                      \
  extern "C" GPU_RT_STUB void base_agg_func##_double_shared(                          \
      GENERIC_ADDR_SPACE int64_t* agg, const double val) {}                           \
                                                                                      \
  extern "C" GPU_RT_STUB void base_agg_func##_double_skip_val_shared(                 \
      GENERIC_ADDR_SPACE int64_t* agg, const double val, const double skip_val) {}    \
  extern "C" GPU_RT_STUB void base_agg_func##_float_shared(                           \
      GENERIC_ADDR_SPACE int32_t* agg, const float val) {}                            \
                                                                                      \
  extern "C" GPU_RT_STUB void base_agg_func##_float_skip_val_shared(                  \
      GENERIC_ADDR_SPACE int32_t* agg, const float val, const float skip_val) {}

DEF_SHARED_AGG_RET_STUBS(agg_count)
DEF_SHARED_AGG_STUBS(agg_max)
DEF_SHARED_AGG_STUBS(agg_min)
DEF_SHARED_AGG_STUBS(agg_id)

extern "C" GPU_RT_STUB GENERIC_ADDR_SPACE int8_t* agg_id_varlen_shared(
    GENERIC_ADDR_SPACE int8_t* varlen_buffer,
    const int64_t offset,
    GENERIC_ADDR_SPACE const int8_t* value,
    const int64_t size_bytes) {
  return nullptr;
}

extern "C" GPU_RT_STUB int32_t
checked_single_agg_id_shared(GENERIC_ADDR_SPACE int64_t* agg,
                             const int64_t val,
                             const int64_t null_val) {
  return 0;
}

extern "C" GPU_RT_STUB int32_t
checked_single_agg_id_int32_shared(GENERIC_ADDR_SPACE int32_t* agg,
                                   const int32_t val,
                                   const int32_t null_val) {
  return 0;
}
extern "C" GPU_RT_STUB int32_t
checked_single_agg_id_int16_shared(GENERIC_ADDR_SPACE int16_t* agg,
                                   const int16_t val,
                                   const int16_t null_val) {
  return 0;
}
extern "C" GPU_RT_STUB int32_t
checked_single_agg_id_int8_shared(GENERIC_ADDR_SPACE int8_t* agg,
                                  const int8_t val,
                                  const int8_t null_val) {
  return 0;
}

extern "C" GPU_RT_STUB int32_t
checked_single_agg_id_double_shared(GENERIC_ADDR_SPACE int64_t* agg,
                                    const double val,
                                    const double null_val) {
  return 0;
}

extern "C" GPU_RT_STUB int32_t
checked_single_agg_id_float_shared(GENERIC_ADDR_SPACE int32_t* agg,
                                   const float val,
                                   const float null_val) {
  return 0;
}

extern "C" GPU_RT_STUB void agg_max_int16_skip_val_shared(GENERIC_ADDR_SPACE int16_t* agg,
                                                          const int16_t val,
                                                          const int16_t skip_val) {}

extern "C" GPU_RT_STUB void agg_max_int8_skip_val_shared(GENERIC_ADDR_SPACE int8_t* agg,
                                                         const int8_t val,
                                                         const int8_t skip_val) {}

extern "C" GPU_RT_STUB void agg_min_int16_skip_val_shared(GENERIC_ADDR_SPACE int16_t* agg,
                                                          const int16_t val,
                                                          const int16_t skip_val) {}

extern "C" GPU_RT_STUB void agg_min_int8_skip_val_shared(GENERIC_ADDR_SPACE int8_t* agg,
                                                         const int8_t val,
                                                         const int8_t skip_val) {}

extern "C" GPU_RT_STUB void agg_id_double_shared_slow(
    GENERIC_ADDR_SPACE int64_t* agg,
    GENERIC_ADDR_SPACE const double* val) {}

extern "C" GPU_RT_STUB int64_t agg_sum_shared(GENERIC_ADDR_SPACE int64_t* agg,
                                              const int64_t val) {
  return 0;
}

extern "C" GPU_RT_STUB int64_t agg_sum_skip_val_shared(GENERIC_ADDR_SPACE int64_t* agg,
                                                       const int64_t val,
                                                       const int64_t skip_val) {
  return 0;
}
extern "C" GPU_RT_STUB int32_t agg_sum_int32_shared(GENERIC_ADDR_SPACE int32_t* agg,
                                                    const int32_t val) {
  return 0;
}

extern "C" GPU_RT_STUB int32_t
agg_sum_int32_skip_val_shared(GENERIC_ADDR_SPACE int32_t* agg,
                              const int32_t val,
                              const int32_t skip_val) {
  return 0;
}

extern "C" GPU_RT_STUB void agg_sum_double_shared(GENERIC_ADDR_SPACE int64_t* agg,
                                                  const double val) {}

extern "C" GPU_RT_STUB void agg_sum_double_skip_val_shared(
    GENERIC_ADDR_SPACE int64_t* agg,
    const double val,
    const double skip_val) {}
extern "C" GPU_RT_STUB void agg_sum_float_shared(GENERIC_ADDR_SPACE int32_t* agg,
                                                 const float val) {}

extern "C" GPU_RT_STUB void agg_sum_float_skip_val_shared(GENERIC_ADDR_SPACE int32_t* agg,
                                                          const float val,
                                                          const float skip_val) {}

extern "C" GPU_RT_STUB void force_sync() {}

extern "C" GPU_RT_STUB void sync_warp() {}
extern "C" GPU_RT_STUB void sync_warp_protected(int64_t thread_pos, int64_t row_count) {}
extern "C" GPU_RT_STUB void sync_threadblock() {}

extern "C" GPU_RT_STUB void write_back_non_grouped_agg(
    GENERIC_ADDR_SPACE int64_t* input_buffer,
    GENERIC_ADDR_SPACE int64_t* output_buffer,
    const int32_t num_agg_cols){};
// x64 stride functions

extern "C" RUNTIME_EXPORT NEVER_INLINE int32_t
pos_start_impl(GENERIC_ADDR_SPACE int32_t* error_code) {
  int32_t row_index_resume{0};
  if (error_code) {
    row_index_resume = error_code[0];
    error_code[0] = 0;
  }
  return row_index_resume;
}

extern "C" RUNTIME_EXPORT NEVER_INLINE int32_t group_buff_idx_impl() {
  return pos_start_impl(nullptr);
}

extern "C" RUNTIME_EXPORT NEVER_INLINE int32_t pos_step_impl() {
  return 1;
}

extern "C" GPU_RT_STUB int8_t thread_warp_idx(const int8_t warp_sz) {
  return 0;
}

extern "C" GPU_RT_STUB int64_t get_thread_index() {
  return 0;
}

extern "C" GPU_RT_STUB GENERIC_ADDR_SPACE int64_t* declare_dynamic_shared_memory() {
  return nullptr;
}

extern "C" GPU_RT_STUB int64_t get_block_index() {
  return 0;
}

#undef GPU_RT_STUB

extern "C" RUNTIME_EXPORT ALWAYS_INLINE void record_error_code(
    const int32_t err_code,
    GLOBAL_ADDR_SPACE int32_t* error_codes) {
  // NB: never override persistent error codes (with code greater than zero).
  // On GPU, a projection query with a limit can run out of slots without it
  // being an actual error if the limit has been hit. If a persistent error
  // (division by zero, for example) occurs before running out of slots, we
  // have to avoid overriding it, because there's a risk that the query would
  // go through if we override with a potentially benign out-of-slots code.
  if (err_code && error_codes[pos_start_impl(nullptr)] <= 0) {
    error_codes[pos_start_impl(nullptr)] = err_code;
  }
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
get_error_code(GLOBAL_ADDR_SPACE int32_t* error_codes) {
  return error_codes[pos_start_impl(nullptr)];
}

// group by helpers

extern "C" RUNTIME_EXPORT NEVER_INLINE GENERIC_ADDR_SPACE const int64_t*
init_shared_mem_nop(GENERIC_ADDR_SPACE const int64_t* groups_buffer,
                    const int32_t groups_buffer_size) {
  return groups_buffer;
}

extern "C" RUNTIME_EXPORT NEVER_INLINE void write_back_nop(
    GENERIC_ADDR_SPACE int64_t* dest,
    GENERIC_ADDR_SPACE int64_t* src,
    const int32_t sz) {
#if !defined(_WIN32) && !defined(L0_RUNTIME_ENABLED)
  // the body is not really needed, just make sure the call is not optimized away
  assert(dest);
#endif
}

extern "C" RUNTIME_EXPORT int64_t* init_shared_mem(
    GENERIC_ADDR_SPACE const int64_t* global_groups_buffer,
    const int32_t groups_buffer_size) {
  return nullptr;
}

extern "C" RUNTIME_EXPORT NEVER_INLINE void init_group_by_buffer_gpu(
    GENERIC_ADDR_SPACE int64_t* groups_buffer,
    GENERIC_ADDR_SPACE const int64_t* init_vals,
    const uint32_t groups_buffer_entry_count,
    const uint32_t key_qw_count,
    const uint32_t agg_col_count,
    const bool keyless,
    const int8_t warp_size) {
#if !defined(_WIN32) && !defined(L0_RUNTIME_ENABLED)
  // the body is not really needed, just make sure the call is not optimized away
  assert(groups_buffer);
#endif
}

extern "C" RUNTIME_EXPORT NEVER_INLINE void init_columnar_group_by_buffer_gpu(
    GENERIC_ADDR_SPACE int64_t* groups_buffer,
    GENERIC_ADDR_SPACE const int64_t* init_vals,
    const uint32_t groups_buffer_entry_count,
    const uint32_t key_qw_count,
    const uint32_t agg_col_count,
    const bool keyless,
    const bool blocks_share_memory,
    const int32_t frag_idx) {
#if !defined(_WIN32) && !defined(L0_RUNTIME_ENABLED)
  // the body is not really needed, just make sure the call is not optimized away
  assert(groups_buffer);
#endif
}

extern "C" RUNTIME_EXPORT NEVER_INLINE void init_group_by_buffer_impl(
    GENERIC_ADDR_SPACE int64_t* groups_buffer,
    GENERIC_ADDR_SPACE const int64_t* init_vals,
    const uint32_t groups_buffer_entry_count,
    const uint32_t key_qw_count,
    const uint32_t agg_col_count,
    const bool keyless,
    const int8_t warp_size) {
#if !defined(_WIN32) && !defined(L0_RUNTIME_ENABLED)
  // the body is not really needed, just make sure the call is not optimized away
  assert(groups_buffer);
#endif
}

template <typename T>
ALWAYS_INLINE GENERIC_ADDR_SPACE int64_t* get_matching_group_value(
    GENERIC_ADDR_SPACE int64_t* groups_buffer,
    const uint32_t h,
    const T* key,
    const uint32_t key_count,
    const uint32_t row_size_quad) {
  auto off = h * row_size_quad;
  auto row_ptr = reinterpret_cast<T*>(groups_buffer + off);
  if (*row_ptr == get_empty_key<typename remove_addr_space<T>::type>()) {
    memcpy(reinterpret_cast<GENERIC_ADDR_SPACE void*>(row_ptr),
           reinterpret_cast<GENERIC_ADDR_SPACE const void*>(key),
           key_count * sizeof(T));
    auto row_ptr_i8 = reinterpret_cast<GENERIC_ADDR_SPACE int8_t*>(row_ptr + key_count);
    return reinterpret_cast<GENERIC_ADDR_SPACE int64_t*>(align_to_int64(row_ptr_i8));
  }
  if (memcmp(row_ptr, key, key_count * sizeof(T)) == 0) {
    auto row_ptr_i8 = reinterpret_cast<GENERIC_ADDR_SPACE int8_t*>(row_ptr + key_count);
    return reinterpret_cast<GENERIC_ADDR_SPACE int64_t*>(align_to_int64(row_ptr_i8));
  }
  return nullptr;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int64_t*
get_matching_group_value(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                         const uint32_t h,
                         GENERIC_ADDR_SPACE const int64_t* key,
                         const uint32_t key_count,
                         const uint32_t key_width,
                         const uint32_t row_size_quad) {
  switch (key_width) {
    case 4:
      return get_matching_group_value(
          groups_buffer,
          h,
          reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(key),
          key_count,
          row_size_quad);
    case 8:
      return get_matching_group_value(groups_buffer, h, key, key_count, row_size_quad);
    default:;
  }
  return nullptr;
}

template <typename T>
ALWAYS_INLINE int32_t
get_matching_group_value_columnar_slot(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                                       const uint32_t entry_count,
                                       const uint32_t h,
                                       const T* key,
                                       const uint32_t key_count) {
  auto off = h;
  auto key_buffer = reinterpret_cast<T*>(groups_buffer);
  if (key_buffer[off] == get_empty_key<typename remove_addr_space<T>::type>()) {
    for (size_t i = 0; i < key_count; ++i) {
      key_buffer[off] = key[i];
      off += entry_count;
    }
    return h;
  }
  off = h;
  for (size_t i = 0; i < key_count; ++i) {
    if (key_buffer[off] != key[i]) {
      return -1;
    }
    off += entry_count;
  }
  return h;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
get_matching_group_value_columnar_slot(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                                       const uint32_t entry_count,
                                       const uint32_t h,
                                       GENERIC_ADDR_SPACE const int64_t* key,
                                       const uint32_t key_count,
                                       const uint32_t key_width) {
  switch (key_width) {
    case 4:
      return get_matching_group_value_columnar_slot(
          groups_buffer,
          entry_count,
          h,
          reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(key),
          key_count);
    case 8:
      return get_matching_group_value_columnar_slot(
          groups_buffer, entry_count, h, key, key_count);
    default:
      return -1;
  }
  return -1;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int64_t*
get_matching_group_value_columnar(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                                  const uint32_t h,
                                  GENERIC_ADDR_SPACE const int64_t* key,
                                  const uint32_t key_qw_count,
                                  const size_t entry_count) {
  auto off = h;
  if (groups_buffer[off] == EMPTY_KEY_64) {
    for (size_t i = 0; i < key_qw_count; ++i) {
      groups_buffer[off] = key[i];
      off += entry_count;
    }
    return &groups_buffer[off];
  }
  off = h;
  for (size_t i = 0; i < key_qw_count; ++i) {
    if (groups_buffer[off] != key[i]) {
      return nullptr;
    }
    off += entry_count;
  }
  return &groups_buffer[off];
}

/*
 * For a particular hashed_index, returns the row-wise offset
 * to the first matching agg column in memory.
 * It also checks the corresponding group column, and initialize all
 * available keys if they are not empty (it is assumed all group columns are
 * 64-bit wide).
 *
 * Memory layout:
 *
 * | prepended group columns (64-bit each) | agg columns |
 */
extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int64_t*
get_matching_group_value_perfect_hash(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                                      const uint32_t hashed_index,
                                      GENERIC_ADDR_SPACE const int64_t* key,
                                      const uint32_t key_count,
                                      const uint32_t row_size_quad) {
  uint32_t off = hashed_index * row_size_quad;
  if (groups_buffer[off] == EMPTY_KEY_64) {
    for (uint32_t i = 0; i < key_count; ++i) {
      groups_buffer[off + i] = key[i];
    }
  }
  return groups_buffer + off + key_count;
}

/**
 * For a particular hashed index (only used with multi-column perfect hash group by)
 * it returns the row-wise offset of the group in the output buffer.
 * Since it is intended for keyless hash use, it assumes there is no group columns
 * prepending the output buffer.
 */
extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int64_t*
get_matching_group_value_perfect_hash_keyless(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                                              const uint32_t hashed_index,
                                              const uint32_t row_size_quad) {
  return groups_buffer + row_size_quad * hashed_index;
}

/*
 * For a particular hashed_index, find and initialize (if necessary) all the group
 * columns corresponding to a key. It is assumed that all group columns are 64-bit wide.
 */
extern "C" RUNTIME_EXPORT ALWAYS_INLINE void
set_matching_group_value_perfect_hash_columnar(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                                               const uint32_t hashed_index,
                                               GENERIC_ADDR_SPACE const int64_t* key,
                                               const uint32_t key_count,
                                               const uint32_t entry_count) {
  if (groups_buffer[hashed_index] == EMPTY_KEY_64) {
    for (uint32_t i = 0; i < key_count; i++) {
      groups_buffer[i * entry_count + hashed_index] = key[i];
    }
  }
}

#include "GroupByRuntime.cpp"
#include "JoinHashTable/Runtime/JoinHashTableQueryRuntime.cpp"

extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int64_t*
get_group_value_fast_keyless(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                             const int64_t key,
                             const int64_t min_key,
                             const int64_t /* bucket */,
                             const uint32_t row_size_quad) {
  return groups_buffer + row_size_quad * (key - min_key);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int64_t*
get_group_value_fast_keyless_semiprivate(GENERIC_ADDR_SPACE int64_t* groups_buffer,
                                         const int64_t key,
                                         const int64_t min_key,
                                         const int64_t /* bucket */,
                                         const uint32_t row_size_quad,
                                         const uint8_t thread_warp_idx,
                                         const uint8_t warp_size) {
  return groups_buffer + row_size_quad * (warp_size * (key - min_key) + thread_warp_idx);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE GENERIC_ADDR_SPACE int8_t* extract_str_ptr(
    const uint64_t str_and_len) {
  return reinterpret_cast<GENERIC_ADDR_SPACE int8_t*>(str_and_len & 0xffffffffffff);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int32_t
extract_str_len(const uint64_t str_and_len) {
  return static_cast<int64_t>(str_and_len) >> 48;
}

extern "C" RUNTIME_EXPORT NEVER_INLINE GENERIC_ADDR_SPACE int8_t*
extract_str_ptr_noinline(const uint64_t str_and_len) {
  return extract_str_ptr(str_and_len);
}

extern "C" RUNTIME_EXPORT NEVER_INLINE int32_t
extract_str_len_noinline(const uint64_t str_and_len) {
  return extract_str_len(str_and_len);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE uint64_t
string_pack(GENERIC_ADDR_SPACE const int8_t* ptr, const int32_t len) {
  return (reinterpret_cast<const uint64_t>(ptr) & 0xffffffffffff) |
         (static_cast<const uint64_t>(len) << 48);
}

#if defined(__clang__) && !defined(L0_RUNTIME_ENABLED)
#include "../Utils/StringLike.cpp"
#endif

#if !defined(__CUDACC__) && !defined(L0_RUNTIME_ENABLED)
#include "TopKRuntime.cpp"
#endif

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE int32_t
char_length(GENERIC_ADDR_SPACE const char* str, const int32_t str_len) {
  return str_len;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE int32_t
char_length_nullable(GENERIC_ADDR_SPACE const char* str,
                     const int32_t str_len,
                     const int32_t int_null) {
  if (!str) {
    return int_null;
  }
  return str_len;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE int32_t
key_for_string_encoded(const int32_t str_id) {
  return str_id;
}

extern "C" ALWAYS_INLINE DEVICE int32_t
map_string_dict_id(const int32_t string_id,
                   const int64_t translation_map_handle,
                   const int32_t min_source_id) {
  GENERIC_ADDR_SPACE const int32_t* translation_map =
      reinterpret_cast<GENERIC_ADDR_SPACE const int32_t*>(translation_map_handle);
  return translation_map[string_id - min_source_id];
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE bool sample_ratio(
    const double proportion,
    const int64_t row_offset) {
  const int64_t threshold = 4294967296 * proportion;
  return (row_offset * 2654435761) % 4294967296 < threshold;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE double width_bucket(
    const double target_value,
    const double lower_bound,
    const double upper_bound,
    const double scale_factor,
    const int32_t partition_count) {
  if (target_value < lower_bound) {
    return 0;
  } else if (target_value >= upper_bound) {
    return partition_count + 1;
  }
  return ((target_value - lower_bound) * scale_factor) + 1;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE double width_bucket_reversed(
    const double target_value,
    const double lower_bound,
    const double upper_bound,
    const double scale_factor,
    const int32_t partition_count) {
  if (target_value > lower_bound) {
    return 0;
  } else if (target_value <= upper_bound) {
    return partition_count + 1;
  }
  return ((lower_bound - target_value) * scale_factor) + 1;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double width_bucket_nullable(
    const double target_value,
    const double lower_bound,
    const double upper_bound,
    const double scale_factor,
    const int32_t partition_count,
    const double null_val) {
  if (target_value == null_val) {
    return INT32_MIN;
  }
  return width_bucket(
      target_value, lower_bound, upper_bound, scale_factor, partition_count);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double width_bucket_reversed_nullable(
    const double target_value,
    const double lower_bound,
    const double upper_bound,
    const double scale_factor,
    const int32_t partition_count,
    const double null_val) {
  if (target_value == null_val) {
    return INT32_MIN;
  }
  return width_bucket_reversed(
      target_value, lower_bound, upper_bound, scale_factor, partition_count);
}

// width_bucket with no out-of-bound check version which can be called
// if we can assure the input target_value expr always resides in the valid range
// (so we can also avoid null checking)
extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE double width_bucket_no_oob_check(
    const double target_value,
    const double lower_bound,
    const double scale_factor) {
  return ((target_value - lower_bound) * scale_factor) + 1;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE double width_bucket_reversed_no_oob_check(
    const double target_value,
    const double lower_bound,
    const double scale_factor) {
  return ((lower_bound - target_value) * scale_factor) + 1;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE double width_bucket_expr(
    const double target_value,
    const bool reversed,
    const double lower_bound,
    const double upper_bound,
    const int32_t partition_count) {
  if (reversed) {
    return width_bucket_reversed(target_value,
                                 lower_bound,
                                 upper_bound,
                                 partition_count / (lower_bound - upper_bound),
                                 partition_count);
  }
  return width_bucket(target_value,
                      lower_bound,
                      upper_bound,
                      partition_count / (upper_bound - lower_bound),
                      partition_count);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE double width_bucket_expr_nullable(
    const double target_value,
    const bool reversed,
    const double lower_bound,
    const double upper_bound,
    const int32_t partition_count,
    const double null_val) {
  if (target_value == null_val) {
    return INT32_MIN;
  }
  return width_bucket_expr(
      target_value, reversed, lower_bound, upper_bound, partition_count);
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE double width_bucket_expr_no_oob_check(
    const double target_value,
    const bool reversed,
    const double lower_bound,
    const double upper_bound,
    const int32_t partition_count) {
  if (reversed) {
    return width_bucket_reversed_no_oob_check(
        target_value, lower_bound, partition_count / (lower_bound - upper_bound));
  }
  return width_bucket_no_oob_check(
      target_value, lower_bound, partition_count / (upper_bound - lower_bound));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE int64_t
row_number_window_func(const int64_t output_buff, const int64_t pos) {
  return reinterpret_cast<GENERIC_ADDR_SPACE const int64_t*>(output_buff)[pos];
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double percent_window_func(
    const int64_t output_buff,
    const int64_t pos) {
  return reinterpret_cast<GENERIC_ADDR_SPACE const double*>(output_buff)[pos];
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double load_double(
    GENERIC_ADDR_SPACE const int64_t* agg) {
  return *reinterpret_cast<GENERIC_ADDR_SPACE const double*>(may_alias_ptr(agg));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE float load_float(
    GENERIC_ADDR_SPACE const int32_t* agg) {
  return *reinterpret_cast<GENERIC_ADDR_SPACE const float*>(may_alias_ptr(agg));
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double load_avg_int(
    GENERIC_ADDR_SPACE const int64_t* sum,
    GENERIC_ADDR_SPACE const int64_t* count,
    const double null_val) {
  return *count != 0 ? static_cast<double>(*sum) / *count : null_val;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double load_avg_decimal(
    GENERIC_ADDR_SPACE const int64_t* sum,
    GENERIC_ADDR_SPACE const int64_t* count,
    const double null_val,
    const uint32_t scale) {
  return *count != 0 ? (static_cast<double>(*sum) / pow(10, scale)) / *count : null_val;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double load_avg_double(
    GENERIC_ADDR_SPACE const int64_t* agg,
    GENERIC_ADDR_SPACE const int64_t* count,
    const double null_val) {
  return *count != 0
             ? *reinterpret_cast<GENERIC_ADDR_SPACE const double*>(may_alias_ptr(agg)) /
                   *count
             : null_val;
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE double load_avg_float(
    GENERIC_ADDR_SPACE const int32_t* agg,
    GENERIC_ADDR_SPACE const int32_t* count,
    const double null_val) {
  return *count != 0
             ? *reinterpret_cast<GENERIC_ADDR_SPACE const float*>(may_alias_ptr(agg)) /
                   *count
             : null_val;
}

extern "C" RUNTIME_EXPORT NEVER_INLINE void linear_probabilistic_count(
    GENERIC_ADDR_SPACE uint8_t* bitmap,
    const uint32_t bitmap_bytes,
    GENERIC_ADDR_SPACE const uint8_t* key_bytes,
    const uint32_t key_len) {
  const uint32_t bit_pos = MurmurHash3(key_bytes, key_len, 0) % (bitmap_bytes * 8);
  const uint32_t word_idx = bit_pos / 32;
  const uint32_t bit_idx = bit_pos % 32;
  reinterpret_cast<GENERIC_ADDR_SPACE uint32_t*>(bitmap)[word_idx] |= 1 << bit_idx;
}

extern "C" RUNTIME_EXPORT NOREMOVE NEVER_INLINE void query_stub_hoisted_literals(
    const int8_t GENERIC_ADDR_SPACE* GENERIC_ADDR_SPACE* col_buffers,
    GENERIC_ADDR_SPACE const int8_t* literals,
    GENERIC_ADDR_SPACE const int64_t* num_rows,
    GENERIC_ADDR_SPACE const uint64_t* frag_row_offsets,
    GENERIC_ADDR_SPACE const int32_t* max_matched,
    GENERIC_ADDR_SPACE const int64_t* init_agg_value,
    int64_t GENERIC_ADDR_SPACE* GENERIC_ADDR_SPACE* out,
    uint32_t frag_idx,
    GENERIC_ADDR_SPACE const int64_t* join_hash_tables,
    GENERIC_ADDR_SPACE int32_t* error_code,
    GENERIC_ADDR_SPACE int32_t* total_matched) {
#if !defined(_WIN32) && !defined(L0_RUNTIME_ENABLED)
  assert(col_buffers || literals || num_rows || frag_row_offsets || max_matched ||
         init_agg_value || out || frag_idx || error_code || join_hash_tables ||
         total_matched);
#endif
}

#ifndef _MSC_VER
#define RESTRICT __restrict__
#else
#define RESTRICT
#endif

extern "C" RUNTIME_EXPORT void multifrag_query_hoisted_literals(
    const int8_t GLOBAL_ADDR_SPACE* GLOBAL_ADDR_SPACE* GLOBAL_ADDR_SPACE* col_buffers,
    GLOBAL_ADDR_SPACE const uint64_t* __restrict__ num_fragments,
    GLOBAL_ADDR_SPACE const int8_t* literals,
    GLOBAL_ADDR_SPACE const int64_t* num_rows,
    GLOBAL_ADDR_SPACE const uint64_t* frag_row_offsets,
    GLOBAL_ADDR_SPACE const int32_t* max_matched,
    GLOBAL_ADDR_SPACE int32_t* total_matched,
    GLOBAL_ADDR_SPACE const int64_t* init_agg_value,
    int64_t GLOBAL_ADDR_SPACE* GLOBAL_ADDR_SPACE* out,
    GLOBAL_ADDR_SPACE int32_t* error_code,
    GLOBAL_ADDR_SPACE const uint32_t* __restrict__ num_tables_ptr,
    GLOBAL_ADDR_SPACE const int64_t* join_hash_tables) {
  for (uint32_t i = 0; i < *num_fragments; ++i) {
    query_stub_hoisted_literals(
        col_buffers
            ? reinterpret_cast<const int8_t GENERIC_ADDR_SPACE * GENERIC_ADDR_SPACE *
                               GENERIC_ADDR_SPACE*>(col_buffers)[i]
            : nullptr,
        literals,
        &num_rows[i * (*num_tables_ptr)],
        &frag_row_offsets[i * (*num_tables_ptr)],
        max_matched,
        init_agg_value,
        reinterpret_cast<int64_t GENERIC_ADDR_SPACE * GENERIC_ADDR_SPACE*>(out),
        i,
        join_hash_tables,
        total_matched,
        error_code);
  }
}

extern "C" RUNTIME_EXPORT NOREMOVE NEVER_INLINE void query_stub(
    const int8_t GENERIC_ADDR_SPACE* GENERIC_ADDR_SPACE* col_buffers,
    GENERIC_ADDR_SPACE const int64_t* num_rows,
    GENERIC_ADDR_SPACE const uint64_t* frag_row_offsets,
    GENERIC_ADDR_SPACE const int32_t* max_matched,
    GENERIC_ADDR_SPACE const int64_t* init_agg_value,
    int64_t GENERIC_ADDR_SPACE* GENERIC_ADDR_SPACE* out,
    uint32_t frag_idx,
    GENERIC_ADDR_SPACE const int64_t* join_hash_tables,
    GENERIC_ADDR_SPACE int32_t* error_code,
    GENERIC_ADDR_SPACE int32_t* total_matched) {
#if !defined(_WIN32) && !defined(L0_RUNTIME_ENABLED)
  assert(col_buffers || num_rows || frag_row_offsets || max_matched || init_agg_value ||
         out || frag_idx || error_code || join_hash_tables || total_matched);
#endif
}

extern "C" RUNTIME_EXPORT void multifrag_query(
    const int8_t GLOBAL_ADDR_SPACE* GLOBAL_ADDR_SPACE* GLOBAL_ADDR_SPACE* col_buffers,
    GLOBAL_ADDR_SPACE const uint64_t* __restrict__ num_fragments,
    GLOBAL_ADDR_SPACE const int64_t* num_rows,
    GLOBAL_ADDR_SPACE const uint64_t* frag_row_offsets,
    GLOBAL_ADDR_SPACE const int32_t* max_matched,
    GLOBAL_ADDR_SPACE int32_t* total_matched,
    GLOBAL_ADDR_SPACE const int64_t* init_agg_value,
    int64_t GLOBAL_ADDR_SPACE* GLOBAL_ADDR_SPACE* out,
    GLOBAL_ADDR_SPACE int32_t* error_code,
    GLOBAL_ADDR_SPACE const uint32_t* __restrict__ num_tables_ptr,
    GLOBAL_ADDR_SPACE const int64_t* join_hash_tables) {
  for (uint32_t i = 0; i < *num_fragments; ++i) {
    query_stub(col_buffers ? reinterpret_cast<const int8_t GENERIC_ADDR_SPACE *
                                              GENERIC_ADDR_SPACE * GENERIC_ADDR_SPACE*>(
                                 col_buffers)[i]
                           : nullptr,
               &num_rows[i * (*num_tables_ptr)],
               &frag_row_offsets[i * (*num_tables_ptr)],
               max_matched,
               init_agg_value,
               reinterpret_cast<int64_t GENERIC_ADDR_SPACE * GENERIC_ADDR_SPACE*>(out),
               i,
               join_hash_tables,
               total_matched,
               error_code);
  }
}

extern "C" RUNTIME_EXPORT ALWAYS_INLINE DEVICE bool check_interrupt() {
  if (check_interrupt_init(static_cast<unsigned>(INT_CHECK))) {
    return true;
  }
  return false;
}

extern "C" RUNTIME_EXPORT bool check_interrupt_init(unsigned command) {
  static std::atomic_bool runtime_interrupt_flag{false};

  if (command == static_cast<unsigned>(INT_CHECK)) {
    if (runtime_interrupt_flag.load()) {
      return true;
    }
    return false;
  }
  if (command == static_cast<unsigned>(INT_ABORT)) {
    runtime_interrupt_flag.store(true);
    return false;
  }
  if (command == static_cast<unsigned>(INT_RESET)) {
    runtime_interrupt_flag.store(false);
    return false;
  }
  return false;
}
