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

#pragma once

#include "IRCodegenUtils.h"
#include "InValuesBitmap.h"
#include "InputMetadata.h"
#include "LLVMGlobalContext.h"
#include "StringDictionaryTranslationMgr.h"

#include "../Analyzer/Analyzer.h"
#include "../Shared/InsertionOrderedMap.h"
#include "IR/Expr.h"
#include "QueryEngine/ExtensionModules.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

struct ArrayLoadCodegen {
  llvm::Value* buffer;
  llvm::Value* size;
  llvm::Value* is_null;
};

struct CgenState {
 public:
  CgenState(const size_t num_query_infos,
            const bool contains_left_deep_outer_join,
            const bool enable_automatic_ir_metadata,
            ExtensionModuleContext* ext_module_context,
            llvm::LLVMContext& context);
  CgenState(const Config& config, llvm::LLVMContext& context);

  size_t getOrAddLiteral(const hdk::ir::Constant* constant,
                         const bool use_dict_encoding,
                         const int dict_id,
                         const int device_id) {
    auto type = constant->type();
    switch (type->id()) {
      case hdk::ir::Type::kBoolean:
        return getOrAddLiteral(constant->isNull()
                                   ? int8_t(inline_int_null_value(type))
                                   : int8_t(constant->value().boolval ? 1 : 0),
                               device_id);
      case hdk::ir::Type::kInteger:
      case hdk::ir::Type::kDecimal:
        switch (type->size()) {
          case 1:
            return getOrAddLiteral(constant->isNull()
                                       ? int8_t(inline_int_null_value(type))
                                       : constant->value().tinyintval,
                                   device_id);
          case 2:
            return getOrAddLiteral(constant->isNull()
                                       ? int16_t(inline_int_null_value(type))
                                       : constant->value().smallintval,
                                   device_id);
          case 4:
            return getOrAddLiteral(constant->isNull()
                                       ? int32_t(inline_int_null_value(type))
                                       : constant->value().intval,
                                   device_id);
          case 8:
            return getOrAddLiteral(constant->isNull()
                                       ? int64_t(inline_int_null_value(type))
                                       : constant->value().bigintval,
                                   device_id);
          default:
            abort();
        }
        break;
      case hdk::ir::Type::kFloatingPoint:
        switch (type->as<hdk::ir::FloatingPointType>()->precision()) {
          case hdk::ir::FloatingPointType::kFloat:
            return getOrAddLiteral(constant->isNull() ? float(inline_fp_null_value(type))
                                                      : constant->value().floatval,
                                   device_id);
          case hdk::ir::FloatingPointType::kDouble:
            return getOrAddLiteral(constant->isNull() ? inline_fp_null_value(type)
                                                      : constant->value().doubleval,
                                   device_id);
          default:
            abort();
        }
        break;
      case hdk::ir::Type::kExtDictionary:
      case hdk::ir::Type::kText:
      case hdk::ir::Type::kVarChar:
        if (use_dict_encoding) {
          if (constant->isNull()) {
            return getOrAddLiteral((int32_t)inline_int_null_value<int32_t>(), device_id);
          }
          return getOrAddLiteral(std::make_pair(*constant->value().stringval, dict_id),
                                 device_id);
        }
        if (constant->isNull()) {
          throw std::runtime_error(
              "CHAR / VARCHAR NULL literal not supported in this context");  // TODO(alex):
                                                                             // support
                                                                             // null
        }
        return getOrAddLiteral(*constant->value().stringval, device_id);
      case hdk::ir::Type::kTime:
      case hdk::ir::Type::kTimestamp:
      case hdk::ir::Type::kDate:
      case hdk::ir::Type::kInterval:
        // TODO(alex): support null
        return getOrAddLiteral(constant->value().bigintval, device_id);
      case hdk::ir::Type::kFixedLenArray:
      case hdk::ir::Type::kVarLenArray: {
        if (!use_dict_encoding) {
          auto elem_type = type->as<hdk::ir::ArrayBaseType>()->elemType();
          if (elem_type->isFp64()) {
            std::vector<double> double_array_literal;
            for (const auto& value : constant->valueList()) {
              const auto c = dynamic_cast<const hdk::ir::Constant*>(value.get());
              CHECK(c);
              double d = c->value().doubleval;
              double_array_literal.push_back(d);
            }
            return getOrAddLiteral(double_array_literal, device_id);
          }
          if (elem_type->isInt32()) {
            std::vector<int32_t> int32_array_literal;
            for (const auto& value : constant->valueList()) {
              const auto c = dynamic_cast<const hdk::ir::Constant*>(value.get());
              CHECK(c);
              int32_t i = c->value().intval;
              int32_array_literal.push_back(i);
            }
            return getOrAddLiteral(int32_array_literal, device_id);
          }
          if (elem_type->isInt8()) {
            std::vector<int8_t> int8_array_literal;
            for (const auto& value : constant->valueList()) {
              const auto c = dynamic_cast<const hdk::ir::Constant*>(value.get());
              CHECK(c);
              int8_t i = c->value().tinyintval;
              int8_array_literal.push_back(i);
            }
            return getOrAddLiteral(int8_array_literal, device_id);
          }
          throw std::runtime_error("Unsupported literal array");
        }
        throw std::runtime_error("Encoded literal arrays are not supported");
      }
      default:
        abort();
    }
  }

  using LiteralValue = boost::variant<int8_t,
                                      int16_t,
                                      int32_t,
                                      int64_t,
                                      float,
                                      double,
                                      std::pair<std::string, int>,
                                      std::string,
                                      std::vector<double>,
                                      std::vector<int32_t>,
                                      std::vector<int8_t>,
                                      std::pair<std::vector<int8_t>, int>>;
  using LiteralValues = std::vector<LiteralValue>;

  const std::unordered_map<int, LiteralValues>& getLiterals() const { return literals_; }

  llvm::Value* addStringConstant(const std::string& str, const CompilationOptions& co) {
    llvm::Value* str_lv = ir_builder_.CreateGlobalString(
        str, "str_const_" + std::to_string(std::hash<std::string>()(str)));
    auto i8_ptr = llvm::PointerType::get(get_int_type(8, context_),
                                         co.codegen_traits_desc.local_addr_space_);
    str_constants_.push_back(str_lv);
    str_lv = ir_builder_.CreateBitCast(str_lv, i8_ptr);
    return str_lv;
  }

  const StringDictionaryTranslationMgr* moveStringDictionaryTranslationMgr(
      std::unique_ptr<const StringDictionaryTranslationMgr>&& str_dict_translation_mgr) {
    str_dict_translation_mgrs_.emplace_back(std::move(str_dict_translation_mgr));
    return str_dict_translation_mgrs_.back().get();
  }

  const InValuesBitmap* addInValuesBitmap(
      std::unique_ptr<InValuesBitmap>& in_values_bitmap) {
    if (in_values_bitmap->isEmpty()) {
      return in_values_bitmap.get();
    }
    in_values_bitmaps_.emplace_back(std::move(in_values_bitmap));
    return in_values_bitmaps_.back().get();
  }
  void moveInValuesBitmap(std::unique_ptr<const InValuesBitmap>& in_values_bitmap) {
    if (!in_values_bitmap->isEmpty()) {
      in_values_bitmaps_.emplace_back(std::move(in_values_bitmap));
    }
  }
  // look up a runtime function based on the name, return type and type of
  // the arguments and call it; x64 only, don't call from GPU codegen
  llvm::Value* emitExternalCall(
      const std::string& fname,
      llvm::Type* ret_type,
      const std::vector<llvm::Value*> args,
      const std::vector<llvm::Attribute::AttrKind>& fnattrs = {},
      const bool has_struct_return = false) {
    std::vector<llvm::Type*> arg_types;
    for (const auto arg : args) {
      CHECK(arg);
      arg_types.push_back(arg->getType());
    }
    auto func_ty = llvm::FunctionType::get(ret_type, arg_types, false);
    llvm::AttributeList attrs;
    if (!fnattrs.empty()) {
      std::vector<std::pair<unsigned, llvm::Attribute>> indexedAttrs;
      indexedAttrs.reserve(fnattrs.size());
      for (auto attr : fnattrs) {
        indexedAttrs.emplace_back(llvm::AttributeList::FunctionIndex,
                                  llvm::Attribute::get(context_, attr));
      }
      attrs = llvm::AttributeList::get(context_,
                                       {&indexedAttrs.front(), indexedAttrs.size()});
    }

    auto func_p = module_->getOrInsertFunction(fname, func_ty, attrs);
    CHECK(func_p);
    auto callee = func_p.getCallee();
    llvm::Function* func{nullptr};
    if (auto callee_cast = llvm::dyn_cast<llvm::ConstantExpr>(callee)) {
      // Get or insert function automatically adds a ConstantExpr cast if the return type
      // of the existing function does not match the supplied return type.
      CHECK(callee_cast->isCast());
      CHECK_EQ(callee_cast->getNumOperands(), size_t(1));
      func = llvm::dyn_cast<llvm::Function>(callee_cast->getOperand(0));
    } else {
      func = llvm::dyn_cast<llvm::Function>(callee);
    }
    CHECK(func);
    llvm::FunctionType* func_type = func_p.getFunctionType();
    CHECK(func_type);
    if (has_struct_return) {
      const auto arg_ti = func_type->getParamType(0);
      CHECK(arg_ti->isPointerTy() && arg_ti->getPointerElementType()->isStructTy());
      auto attr_list = func->getAttributes();
#if LLVM_VERSION_MAJOR > 13
      llvm::AttrBuilder arr_arg_builder(context_, attr_list.getParamAttrs(0));
#else
      llvm::AttrBuilder arr_arg_builder(attr_list.getParamAttributes(0));
#endif
      arr_arg_builder.addAttribute(llvm::Attribute::StructRet);
      func->addParamAttrs(0, arr_arg_builder);
    }
    const size_t arg_start = has_struct_return ? 1 : 0;
    for (size_t i = arg_start; i < func->arg_size(); i++) {
      const auto arg_ti = func_type->getParamType(i);
      if (arg_ti->isPointerTy() && arg_ti->getPointerElementType()->isStructTy()) {
        auto attr_list = func->getAttributes();
#if LLVM_VERSION_MAJOR > 13
        llvm::AttrBuilder arr_arg_builder(context_, attr_list.getParamAttrs(i));
#else
        llvm::AttrBuilder arr_arg_builder(attr_list.getParamAttributes(i));
#endif
        arr_arg_builder.addByValAttr(arg_ti->getPointerElementType());
        func->addParamAttrs(i, arr_arg_builder);
      }
    }
    llvm::Value* result = ir_builder_.CreateCall(func_p, args);
    // check the assumed type
    CHECK_EQ(result->getType(), ret_type);
    return result;
  }

  llvm::Value* emitCall(const std::string& fname, const std::vector<llvm::Value*>& args);

  size_t getLiteralBufferUsage(const int device_id) {
    return literal_bytes_[device_id];
  }

  llvm::Value* castToTypeIn(llvm::Value* val, const size_t bit_width);

  std::pair<llvm::ConstantInt*, llvm::ConstantInt*> inlineIntMaxMin(
      const size_t byte_width,
      const bool is_signed);

  llvm::ConstantInt* inlineIntNull(const hdk::ir::Type*);
  llvm::ConstantFP* inlineFpNull(const hdk::ir::Type*);
  llvm::Constant* inlineNull(const hdk::ir::Type*);

  template <class T>
  llvm::ConstantInt* llInt(const T v) const {
    return ::ll_int(v, context_);
  }

  llvm::ConstantFP* llFp(const float v) const {
    return static_cast<llvm::ConstantFP*>(
        llvm::ConstantFP::get(llvm::Type::getFloatTy(context_), v));
  }

  llvm::ConstantFP* llFp(const double v) const {
    return static_cast<llvm::ConstantFP*>(
        llvm::ConstantFP::get(llvm::Type::getDoubleTy(context_), v));
  }

  llvm::ConstantInt* llBool(const bool v) const {
    return ::ll_bool(v, context_);
  }

  void emitErrorCheck(llvm::Value* condition, llvm::Value* errorCode, std::string label);

  std::vector<std::string> gpuFunctionsToReplace(llvm::Function* fn);

  void replaceFunctionForGpu(const std::string& fcn_to_replace, llvm::Function* fn);

  void set_module_shallow_copy(const std::unique_ptr<llvm::Module>& module,
                               bool always_clone = false);

  /*
    Quoting https://groups.google.com/g/llvm-dev/c/kuil5XjasUs/m/7PBpOWZFDAAJ :
    """
    The state of Module/Context ownership is very muddled in the
    codebase. As you have discovered: LLVMContext’s notionally own
    their modules (via raw pointers deleted upon destruction of the
    context), however in practice we always hold llvm Modules by
    unique_ptr. Since the Module destructor removes the raw pointer
    from the Context, this doesn’t usually blow up. It’s pretty broken
    though.

    I would argue that you should use unique_ptr and ignore
    LLVMContext ownership.
    """

    Here we do the opposite to the last argument because we store
    llvm::Module pointers in CodeCache and it is hard to sync the
    destruction of LLVMContext and removing its modules from the
    CodeCache. Instead, we'll never explicitly delete llvm::Module
    instances and we'll let LLVMContext or unique_ptr to manage the
    destruction of llvm::Modules. As a result, whenever LLVMContext is
    destroyed, the corresponding llvm::Module pointers in CodeCache
    become invalid and it is recommended to clear the CodeCache as
    well.
   */

  llvm::Module* module_;
  llvm::Function* row_func_;
  llvm::Function* filter_func_;
  llvm::Function* current_func_;
  llvm::BasicBlock* row_func_bb_;
  llvm::BasicBlock* filter_func_bb_;
  llvm::CallInst* row_func_call_;
  llvm::CallInst* filter_func_call_;
  std::vector<llvm::Function*> helper_functions_;
  llvm::LLVMContext& context_;    // LLVMContext instance is held by an Executor instance.
  llvm::ValueToValueMapTy vmap_;  // used for cloning the runtime module
  llvm::IRBuilder<> ir_builder_;
  std::unordered_map<int, std::vector<llvm::Value*>> fetch_cache_;

  ExtensionModuleContext* ext_module_context_;

  struct FunctionOperValue {
    const hdk::ir::FunctionOper* foper;
    llvm::Value* lv;
  };
  std::vector<FunctionOperValue> ext_call_cache_;
  std::vector<llvm::Value*> group_by_expr_cache_;
  std::vector<llvm::Value*> str_constants_;
  std::vector<llvm::Value*> frag_offsets_;
  const bool contains_left_deep_outer_join_;
  std::vector<llvm::Value*> outer_join_match_found_per_level_;
  std::unordered_map<int, llvm::Value*> scan_idx_to_hash_pos_;
  InsertionOrderedMap filter_func_args_;
  std::vector<std::unique_ptr<const InValuesBitmap>> in_values_bitmaps_;
  std::vector<std::unique_ptr<const StringDictionaryTranslationMgr>>
      str_dict_translation_mgrs_;
  std::map<std::pair<llvm::Value*, llvm::Value*>, ArrayLoadCodegen>
      array_load_cache_;  // byte stream to array info
  bool needs_error_check_;
  bool automatic_ir_metadata_;

  llvm::Function* query_func_;
  llvm::IRBuilder<> query_func_entry_ir_builder_;
  std::unordered_map<int, std::vector<llvm::Value*>> query_func_literal_loads_;

  struct HoistedLiteralLoadLocator {
    int offset_in_literal_buffer;
    int index_of_literal_load;
  };
  std::unordered_map<llvm::Value*, HoistedLiteralLoadLocator> row_func_hoisted_literals_;

  static size_t literalBytes(const CgenState::LiteralValue& lit) {
    switch (lit.which()) {
      case 0:
        return 1;  // int8_t
      case 1:
        return 2;  // int16_t
      case 2:
        return 4;  // int32_t
      case 3:
        return 8;  // int64_t
      case 4:
        return 4;  // float
      case 5:
        return 8;  // double
      case 6:
        return 4;  // std::pair<std::string, int>
      case 7:
        return 4;  // std::string
      case 8:
        return 4;  // std::vector<double>
      case 9:
        return 4;  // std::vector<int32_t>
      case 10:
        return 4;  // std::vector<int8_t>
      case 11:
        return 4;  // std::pair<std::vector<int8_t>, int>
      default:
        abort();
    }
  }

  static size_t addAligned(const size_t off_in, const size_t alignment) {
    size_t off = off_in;
    if (off % alignment != 0) {
      off += (alignment - off % alignment);
    }
    return off + alignment;
  }

  void maybeCloneFunctionRecursive(llvm::Function* fn, bool is_l0);

 private:
  template <class T>
  size_t getOrAddLiteral(const T& val, const int device_id) {
    const LiteralValue var_val(val);
    size_t literal_found_off{0};
    auto& literals = literals_[device_id];
    for (const auto& literal : literals) {
      const auto lit_bytes = literalBytes(literal);
      literal_found_off = addAligned(literal_found_off, lit_bytes);
      if (literal == var_val) {
        return literal_found_off - lit_bytes;
      }
    }
    literals.emplace_back(val);
    const auto lit_bytes = literalBytes(var_val);
    literal_bytes_[device_id] = addAligned(literal_bytes_[device_id], lit_bytes);
    return literal_bytes_[device_id] - lit_bytes;
  }

  std::unordered_map<int, LiteralValues> literals_;
  std::unordered_map<int, size_t> literal_bytes_;
};

#include "AutomaticIRMetadataGuard.h"
