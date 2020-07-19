/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- attribute_utils.h - Helpers for BEF Attributes -----------*- C++ -*-===//
//
// This file declares helper routines for reading BEF Attributes.
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_HOST_CONTEXT_ATTRIBUTE_UTILS_H_
#define TFRT_HOST_CONTEXT_ATTRIBUTE_UTILS_H_

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "tfrt/support/bef_encoding.h"
#include "tfrt/support/byte_order.h"
#include "tfrt/support/forward_decls.h"

namespace tfrt {

class StringAttribute;
template <typename T>
class ArrayAttribute;
class AggregateAttribute;

namespace internal {

template <typename>
struct is_array_attribute : std::false_type {};
template <typename T>
struct is_array_attribute<ArrayAttribute<T>> : std::true_type {};

}  // namespace internal

// Kernels should use this so we know they have an attribute input.
template <typename T>
class Attribute {
 public:
  explicit Attribute(const void* value)
      : value_(*reinterpret_cast<const T*>(value)) {
    ASSERT_LITTLE_ENDIAN();
  }

  const T& get() const { return value_; }
  const T* operator->() const { return &value_; }
  const T& operator*() const { return value_; }

 private:
  static_assert(!std::is_same<T, std::string>(),
                "Use StringAttribute instead of Attribute<std::string>");
  static_assert(
      !std::is_same<T, StringAttribute>(),
      "Use StringAttribute directly instead of Attribute<StringAttribute>");
  static_assert(!std::is_same<T, AggregateAttribute>(),
                "Use AggregateAttribute directly instead of "
                "Attribute<AggregateAttribute>");
  static_assert(!internal::is_array_attribute<T>(),
                "Use ArrayAttribute directly instead of "
                "Attribute<ArrayAttribute<T>>");

  const T& value_;
};

// Like Attribute, but specifically for strings. We use this instead of
// Attribute<std::string> because strings are stored as character arrays and we
// don't want unnecessary deep copies.
//
// StringAttribute is equivalent to ArrayAttribute<char>, but
// StringAttribute provides a string_view, while ArrayAttribute<char>
// provides an ArrayRef<char>.
class StringAttribute {
 public:
  explicit StringAttribute(const void* value) {
    ASSERT_LITTLE_ENDIAN();
    auto char_array = DecodeArrayFromBEFAttributes<char>(value);
    value_ = string_view(char_array.data(), char_array.size());
  }

  string_view get() const { return value_; }
  operator string_view() const { return value_; }
  std::string str() const { return std::string(value_); }

 private:
  string_view value_;
};

// Kernels should use this so we know it has an array attribute.
template <typename T>
class ArrayAttribute {
 public:
  explicit ArrayAttribute(const void* data)
      : data_(DecodeArrayFromBEFAttributes<T>(data)) {
    ASSERT_LITTLE_ENDIAN();
  }

  ArrayRef<T> data() const { return data_; }
  size_t size() const { return data_.size(); }
  const T& operator[](size_t i) const { return data_[i]; }

 private:
  ArrayRef<T> data_;
};

// TypedAttrBase is the base class for all typed attributes below. It provides
// llvm style cast (isa, cast, dyn_cast, etc) for efficient down-casting to
// subclasses.
class TypedAttrBase {
 public:
  TypedAttrBase() = default;
  explicit TypedAttrBase(const void* base)
      : base_(static_cast<const BEFAttrBase*>(base)) {}

  BEFAttributeType type() const { return base_->type; }

  const void* data() const { return static_cast<const void*>(base_); }
  size_t size() const { return base_->byte_count; }

  template <typename T>
  bool isa() const {
    return T::classof(*this);
  }
  template <typename T>
  T dyn_cast() const {
    return isa<T>() ? T(base_) : T(nullptr);
  }
  template <typename T>
  T cast() const {
    assert(isa<T>());
    return T(base_);
  }

  explicit operator bool() const { return base_ != nullptr; }

 private:
  const BEFAttrBase* base_ = nullptr;
};

namespace internal {

// An intermediate class template that provides the header decoding method for
// all subclasses.
template <typename AttrClass, typename HeaderType>
class AttrHeaderBase : public TypedAttrBase {
 public:
  using TypedAttrBase::TypedAttrBase;
  using Base = AttrHeaderBase;

  AttrHeaderBase(const void* data) : TypedAttrBase(data) {
    assert(isa<AttrClass>());
  }

 protected:
  const HeaderType& header() const {
    return *static_cast<const HeaderType*>(data());
  }
};

// An intermediate class template for all fixed-width attributes. It provides
// the common GetValue() method for all fixed-width attributes.
template <typename DataTypeAttrClass, typename HeaderType,
          BEFDataType DataTypeEnum, typename DataType>
class DataTypeAttrBase : public AttrHeaderBase<DataTypeAttrClass, HeaderType> {
 public:
  using AttrHeaderBase<DataTypeAttrClass, HeaderType>::AttrHeaderBase;
  using Base = DataTypeAttrBase;

  DataType GetValue() const {
    DataType value;
    std::memcpy(&value, &this->header().data, sizeof(DataType));
    return value;
  }

  static bool classof(TypedAttrBase base) {
    return IsDataTypeAttribute(base.type()) &&
           GetDataType(base.type()) == DataTypeEnum;
  }
};

}  // namespace internal

class BoolAttr
    : public internal::DataTypeAttrBase<BoolAttr, BEFFixed8Attr,
                                        BEFDataType::kBool, uint8_t> {
 public:
  using Base::Base;

  bool GetValue() const { return static_cast<bool>(Base::GetValue()); }
};

class I8Attr : public internal::DataTypeAttrBase<I8Attr, BEFFixed8Attr,
                                                 BEFDataType::kI8, uint8_t> {
 public:
  using Base::Base;
};

class I32Attr : public internal::DataTypeAttrBase<I32Attr, BEFFixed32Attr,
                                                  BEFDataType::kI32, int32_t> {
 public:
  using Base::Base;
};

class F32Attr : public internal::DataTypeAttrBase<F32Attr, BEFFixed32Attr,
                                                  BEFDataType::kF32, float> {
 public:
  using Base::Base;
};

class I64Attr : public internal::DataTypeAttrBase<I64Attr, BEFFixed64Attr,
                                                  BEFDataType::kI64, int64_t> {
 public:
  using Base::Base;
};

class BF16Attr
    : public internal::DataTypeAttrBase<BF16Attr, BEFFixed16Attr,
                                        BEFDataType::kBF16, uint16_t> {
 public:
  using Base::Base;
};

class F64Attr : public internal::DataTypeAttrBase<F64Attr, BEFFixed64Attr,
                                                  BEFDataType::kF64, double> {
 public:
  using Base::Base;
};

class TypeAttr : public internal::AttrHeaderBase<TypeAttr, BEFFixed8Attr> {
 public:
  using Base::Base;

  BEFDataType GetValue() const {
    return static_cast<BEFDataType>(header().data);
  }

  static bool classof(TypedAttrBase base) {
    return base.type() == BEFAttributeType::kType;
  }
};

class ArrayAttr : public internal::AttrHeaderBase<ArrayAttr, BEFArrayAttr> {
 public:
  using Base::Base;

  BEFAttributeType GetElementType() const {
    return GetElementAttributeType(type());
  }

  const void* GetElements() const {
    const auto* bytes = static_cast<const uint8_t*>(data());
    return bytes + header().element_offset;
  }

  template <typename T>
  ArrayRef<T> GetValue() const {
    // For empty arrays, we don't care the element type.
    if (GetNumElements() == 0) return {};
    assert(GetBEFAttributeType<T>() == GetElementType());
    return llvm::makeArrayRef(static_cast<const T*>(GetElements()),
                              GetNumElements());
  }

  size_t GetNumElements() const { return header().num_elements; }

  static bool classof(TypedAttrBase base) {
    return IsArrayAttribute(base.type());
  }
};

class StringAttr : public internal::AttrHeaderBase<StringAttr, BEFStringAttr> {
 public:
  using Base::Base;

  string_view GetValue() const {
    return string_view(reinterpret_cast<const char*>(header().data),
                       header().base.byte_count - sizeof(BEFAttrBase));
  }

  static bool classof(TypedAttrBase base) {
    return IsDataTypeAttribute(base.type()) &&
           GetDataType(base.type()) == BEFDataType::kString;
  }
};

class ShapeAttr : public internal::AttrHeaderBase<ShapeAttr, BEFShapeAttr> {
 public:
  using Base::Base;

  static constexpr size_t Alignment() { return alignof(int64_t); }

  int GetRank() const { return header().rank; }

  ArrayRef<int64_t> GetShape() const {
    return llvm::makeArrayRef(header().dims, GetRank());
  }

  static bool classof(TypedAttrBase base) {
    return base.type() == BEFAttributeType::kShape;
  }
};

class DenseAttr : public internal::AttrHeaderBase<DenseAttr, BEFDenseAttr> {
 public:
  using Base::Base;

  static constexpr size_t Alignment() { return alignof(int64_t); }

  BEFDataType dtype() const { return GetDataType(type()); }

  llvm::ArrayRef<ssize_t> shape() const {
    const auto* bytes = static_cast<const uint8_t*>(data());
    const auto& header = this->header();

    // BEF currently stores shapes in int64_t. In the long term, since BEF is
    // designed to be target specific, we plan to use int32_t to store shape
    // dimensions in BEF for 32-bit architecture.
    return llvm::makeArrayRef(
        reinterpret_cast<const ssize_t*>(bytes + header.shape_offset),
        header.rank);
  }

  size_t GetNumElements() const { return header().num_elements; }

  const void* GetElements() const {
    const auto* bytes = static_cast<const uint8_t*>(data());
    return bytes + header().element_offset;
  }

  static bool classof(TypedAttrBase base) {
    return IsDenseAttribute(base.type());
  }
};

class AggregateAttr
    : public internal::AttrHeaderBase<AggregateAttr, BEFAggregateAttr> {
 public:
  using Base::Base;

  TypedAttrBase GetAttribute(int index) const {
    assert(index < GetNumElements());
    auto offset = header().offsets[index];
    const auto* bytes = reinterpret_cast<const uint8_t*>(data());
    return TypedAttrBase(reinterpret_cast<const BEFAttrBase*>(bytes + offset));
  }

  template <typename AttrClass>
  AttrClass GetAttributeOfType(int index) const {
    return GetAttribute(index).cast<AttrClass>();
  }

  size_t GetNumElements() const { return header().num_elements; }

  static bool classof(TypedAttrBase base) {
    // Empty typed arrays have the same layout as empty aggregates. So it is
    // allowed to use AggregateAttr on BEFArrayAttr that is empty.
    return base.type() == BEFAttributeType::kAggregate ||
           base.type() == BEFAttributeType::kEmptyArray;
  }
};

}  // namespace tfrt

#endif  // TFRT_HOST_CONTEXT_ATTRIBUTE_UTILS_H_
