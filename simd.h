/*
 * Copyright 2017 Google Inc.
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

#ifndef DIMSUM_SIMD_H_
#define DIMSUM_SIMD_H_

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <type_traits>

#include "index_sequence.h"
#include "integral_types.h"

// Clang with version <= 3.8 has a bug, that errors on inline specializations on
// deleted primary function template.
#if defined(__clang__) && __clang_major__ == 3 && __clang_minor__ <= 8
#define DIMSUM_DELETE
#else
#define DIMSUM_DELETE = delete
#endif

namespace dimsum {
template <typename T, typename Abi>
class Simd;

namespace detail {

template <typename T, size_t num_bytes>
struct GccVecTraits {};

#define GCC_VEC_SPECIALIZATION(T, NUM_BYTES)                \
  template <>                                               \
  struct GccVecTraits<T, NUM_BYTES> {                       \
    using type = T __attribute__((vector_size(NUM_BYTES))); \
  }
#define GCC_VEC_SPECIALIZE_ON_NUM_BYTES(NUM_BYTES) \
  GCC_VEC_SPECIALIZATION(uint8, NUM_BYTES);        \
  GCC_VEC_SPECIALIZATION(uint16, NUM_BYTES);       \
  GCC_VEC_SPECIALIZATION(uint32, NUM_BYTES);       \
  GCC_VEC_SPECIALIZATION(uint64, NUM_BYTES);       \
  GCC_VEC_SPECIALIZATION(int8, NUM_BYTES);         \
  GCC_VEC_SPECIALIZATION(int16, NUM_BYTES);        \
  GCC_VEC_SPECIALIZATION(int32, NUM_BYTES);        \
  GCC_VEC_SPECIALIZATION(int64, NUM_BYTES);        \
  GCC_VEC_SPECIALIZATION(float, NUM_BYTES);        \
  GCC_VEC_SPECIALIZATION(double, NUM_BYTES)

GCC_VEC_SPECIALIZE_ON_NUM_BYTES(8);
GCC_VEC_SPECIALIZE_ON_NUM_BYTES(16);
GCC_VEC_SPECIALIZE_ON_NUM_BYTES(32);
GCC_VEC_SPECIALIZE_ON_NUM_BYTES(64);
GCC_VEC_SPECIALIZE_ON_NUM_BYTES(128);
GCC_VEC_SPECIALIZE_ON_NUM_BYTES(256);

GCC_VEC_SPECIALIZATION(uint8, 4);
GCC_VEC_SPECIALIZATION(uint16, 4);
GCC_VEC_SPECIALIZATION(uint32, 4);
GCC_VEC_SPECIALIZATION(int8, 4);
GCC_VEC_SPECIALIZATION(int16, 4);
GCC_VEC_SPECIALIZATION(int32, 4);
GCC_VEC_SPECIALIZATION(float, 4);

GCC_VEC_SPECIALIZATION(uint8, 2);
GCC_VEC_SPECIALIZATION(uint16, 2);
GCC_VEC_SPECIALIZATION(int8, 2);
GCC_VEC_SPECIALIZATION(int16, 2);

GCC_VEC_SPECIALIZATION(uint8, 1);
GCC_VEC_SPECIALIZATION(int8, 1);

#undef GCC_VEC_SPECIALIZE_ON_NUM_BYTES
#undef GCC_VEC_SPECIALIZATION

template <typename T, typename Abi>
struct SimdTraits;

// Different kinds of supported elements.
enum class NumberKind {
  kUInt,
  kSInt,
  kFloat,
};

// NumberTraits maps (width, kind) to a primitive integer type.
template <size_t width, NumberKind kind>
struct NumberTraits;

template <>
struct NumberTraits<1, NumberKind::kSInt> {
  using type = int8;
};
template <>
struct NumberTraits<2, NumberKind::kSInt> {
  using type = int16;
};
template <>
struct NumberTraits<4, NumberKind::kSInt> {
  using type = int32;
};
template <>
struct NumberTraits<8, NumberKind::kSInt> {
  using type = int64;
};
template <>
struct NumberTraits<1, NumberKind::kUInt> {
  using type = uint8;
};
template <>
struct NumberTraits<2, NumberKind::kUInt> {
  using type = uint16;
};
template <>
struct NumberTraits<4, NumberKind::kUInt> {
  using type = uint32;
};
template <>
struct NumberTraits<8, NumberKind::kUInt> {
  using type = uint64;
};
template <>
struct NumberTraits<4, NumberKind::kFloat> {
  using type = float;
};
template <>
struct NumberTraits<8, NumberKind::kFloat> {
  using type = double;
};

template <size_t width, NumberKind kind>
using Number = typename NumberTraits<width, kind>::type;

// The most suitable width of a single simd register.
static constexpr size_t kMachineWidth =
#ifdef __AVX2__
    32;
#else
    16;
#endif

// Returns the NumberKind of T.
template <typename T>
constexpr NumberKind get_number_kind() {
  static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value,
                "Unexpected type");
  return std::is_signed<T>::value
             ? NumberKind::kSInt
             : (std::is_unsigned<T>::value ? NumberKind::kUInt
                                           : NumberKind::kFloat);
}

// Returns a DestType with the value clamped into its representable range.
template <typename DestType, typename SrcType>
DestType saturated_convert(SrcType val) {
  return val > std::numeric_limits<DestType>::max()
             ? std::numeric_limits<DestType>::max()
             : (val < std::numeric_limits<DestType>::min()
                    ? std::numeric_limits<DestType>::min()
                    : static_cast<DestType>(val));
}

enum class StoragePolicy {
  kSimulated,
  kXmm,
  kYmm,
  kVsxReg,
  kNeon,
};

// Canonical implementation of abi. Currently it consists of the storage policy,
// and the width. Notice that we want to support storage like use "half of XMM
// register", "use 2 XMM registers", "use 1 YMM register", as they have
// different performance characteristics.
//
// One may add StoragePolicy::kArray in the future to implement scalar SIMD for
// old/embedded devices.
//
// TODO(dimsum): currently they are all implemented in terms of "GCC vector
// extensions", through GccVecTraits. Distinguishing "use 2 Xmm registers" from
// "use 1 YMM register" needs more compiler support.
template <StoragePolicy kStorage, size_t kNumBytes>
struct Abi {
  static_assert(kNumBytes > 0, "Abi should contain more than 0 bytes");
  static_assert(((kNumBytes - 1) & kNumBytes) == 0,
                "Abi should contain power of 2 bytes");
};

template <typename SimdType, typename NewElementType, size_t kNewSize>
struct RebindTraits {};

template <typename T, StoragePolicy kStorage, size_t kNumBytes,
          typename NewElementType, size_t kNewNumElements>
struct RebindTraits<Simd<T, Abi<kStorage, kNumBytes>>, NewElementType,
                    kNewNumElements> {
  using type = Simd<NewElementType,
                    Abi<kStorage, sizeof(NewElementType) * kNewNumElements>>;
};

template <typename T, typename Abi, typename Flags>
struct LoadImpl;

template <typename To, typename From>
constexpr auto IsNarrowingConversionImpl(From a[[gnu::unused]])
    -> decltype(To{a}, false) {
  return false;
}

template <typename To>
constexpr bool IsNarrowingConversionImpl(...) {
  return true;
}

template <typename To, typename From>
constexpr bool IsNarrowingConversion() {
  static_assert(
      std::is_arithmetic<To>::value && std::is_arithmetic<From>::value, "");
  return IsNarrowingConversionImpl<To>(From{});
}

}  // namespace detail

namespace flags {

struct element_aligned_tag {};

constexpr element_aligned_tag element_aligned{};

struct vector_aligned_tag {};

constexpr vector_aligned_tag vector_aligned{};

}  // namespace flags

// Returns a Simd type that's based on SimdType, but with a different size.
template <typename SimdType, size_t kNewSize>
using ResizeTo =
    typename detail::RebindTraits<SimdType, typename SimdType::value_type,
                                  kNewSize>::type;

// Returns a Simd type that's based on SimdType, but with the size
// SimdType::size() * kNumerator / kDenominator.
template <typename SimdType, size_t kNumerator, size_t kDenominator = 1>
using ResizeBy =
    ResizeTo<SimdType, SimdType::size() * kNumerator / kDenominator>;

// Returns a scalar type with width sizeof(T) * kNumerator / kDenominator.
// For example, ScaleBy<int16, 2> gives int32, ScaleBy<uint16, 1, 2> gives
// uint8.
template <typename T, size_t kNumerator, size_t kDenominator = 1>
using ScaleBy = detail::Number<sizeof(T) * kNumerator / kDenominator,
                               detail::get_number_kind<T>()>;

// Returns a new Simd type based on the old one, with element type NewType.
template <typename SimdType, typename NewType>
using ChangeElemTo =
    typename detail::RebindTraits<SimdType, NewType, SimdType::size()>::type;

// Returns a numeric type that's based on T, but with a different width by the
// ratio kNumerator / kDenominator.
template <typename SimdType, size_t kNumerator, size_t kDenominator = 1>
using ScaleElemBy = ChangeElemTo<
    SimdType, ScaleBy<typename SimdType::value_type, kNumerator, kDenominator>>;

// Simd provides a portable interface of the vector/simd units in the hardware.
//
// Simd should only be instantiated with detail::Abi.
template <typename T, typename Abi>
class Simd {};

template <typename T, detail::StoragePolicy kStorage, size_t kNumBytes>
class Simd<T, detail::Abi<kStorage, kNumBytes>> {
  using Traits =
      ::dimsum::detail::SimdTraits<T, detail::Abi<kStorage, kNumBytes>>;

  using NumberKind = ::dimsum::detail::NumberKind;

  static constexpr NumberKind kKind = ::dimsum::detail::get_number_kind<T>();

  template <size_t element_width, NumberKind kind = kKind>
  using Number = ::dimsum::detail::Number<element_width, kind>;

 public:
  // The element type.
  using value_type = T;

  // The ABI type.
  using abi_type = detail::Abi<kStorage, kNumBytes>;

  // The element type that comparison operations return.
  using ComparisonResultType = Number<sizeof(T), NumberKind::kUInt>;

  constexpr Simd() = default;

  // Returns the number of elements in this class.
  static constexpr size_t size() { return kNumBytes / sizeof(T); }

  static constexpr size_t num_bits() { return kNumBytes * CHAR_BIT; }

  // Initialize a Simd object from a function. The function takes a index type,
  // that's convertible from integral_constant<size_t, ...>().
  //
  // Example: Simd128<int32>([](int i) { return 2 * i }) gives [0, 2, 4, 6].
  template <typename Generator,
            typename = decltype(Simd(std::declval<Generator>()(
                std::integral_constant<size_t, 0>())))>
  explicit Simd(const Generator& gen) {
    GeneratorInit(gen, dimsum::make_index_sequence<size()>{});
  }

  // Initialize a Simd object by elements. Number of elements must be the same
  // as Simd<>::size().
  template <typename... Args>
  static Simd list(Args... args) {
    static_assert(sizeof...(Args) == size(), "Mismatched number of elements.");
    T buffer[size()] = {static_cast<T>(args)...};
    return Simd(buffer, flags::element_aligned);
  }

  // Constructs a Simd object from an arch-specific raw type.
  // It exists only to help transition and refactoring.
  /* implicit */ Simd(typename Traits::ExternalType storage) {  // NOLINT
    static_assert(sizeof(storage) == sizeof(storage_), "Simd width mismatch");
    memcpy(&storage_, &storage, sizeof(storage));
  }

  typename Traits::ExternalType raw() const {
    typename Traits::ExternalType ret;
    static_assert(sizeof(ret) == sizeof(storage_), "Simd width mismatch");
    memcpy(&ret, &storage_, sizeof(ret));
    return ret;
  }

  // Implicitly casts the Simd type to the arch-specific raw type.
  // It exists only to help transition and refactoring.
  //
  // TODO(timshen): Now that we have raw() back, should we remove the implicit
  // cast, for consistency?
  /* implicit */ operator typename Traits::ExternalType() const {  // NOLINT
    return raw();
  }

  // Constructs a Simd object from the first size() elements of the buffer.
  template <typename Flags>
  explicit Simd(const T* buffer, Flags flags) {
    memload(buffer, flags);
  }

  // Constructs a Simd object, using a single value for all elements.
  Simd(T value) {  // NOLINT
    for (int i = 0; i < size(); i++) {
      storage_[i] = value;
    }
  }

  // Returns the ith element.
  value_type operator[](size_t i) const { return storage_[i]; }

  // Changes the current object to Simd(buffer).
  template <typename Flags>
  void memload(const T* buffer, Flags flags) {
    *this = detail::LoadImpl<T, detail::Abi<kStorage, kNumBytes>, Flags>::Apply(
        buffer);
  }

  // Stores the Simd object to the buffer.
  template <typename Flags>
  void memstore(T* buffer, Flags) const {
    constexpr size_t bytes = sizeof(storage_);
    if (std::is_same<Flags, flags::vector_aligned_tag>::value) {
      memcpy(__builtin_assume_aligned(buffer, bytes), &storage_, bytes);
    } else {
      memcpy(buffer, &storage_, bytes);
    }
  }

  // Sets the ith element.
  void set(size_t i, T value) { storage_[i] = value; }

  template <typename Tp, typename Ap>
  friend class Simd;

  template <typename Tp, typename Abi, typename Flags>
  friend struct detail::LoadImpl;

  template <size_t... indices, typename Tp, typename Ap>
  friend ResizeTo<Simd<Tp, Ap>, sizeof...(indices)> shuffle(Simd<Tp, Ap> lhs,
                                                            Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> add(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> add_saturated(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> sub(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> sub_saturated(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> mul(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> shl_simd(Simd<Tp, Ap> simd, Simd<Tp, Ap> count);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> shl(Simd<Tp, Ap> simd, int count);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> shr_simd(Simd<Tp, Ap> simd, Simd<Tp, Ap> count);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> shr(Simd<Tp, Ap> simd, int count);

  template <typename Tp, typename Ap>
  friend Simd<typename Simd<Tp, Ap>::ComparisonResultType, Ap> cmp_eq(
      Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<typename Simd<Tp, Ap>::ComparisonResultType, Ap> cmp_ne(
      Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<typename Simd<Tp, Ap>::ComparisonResultType, Ap> cmp_lt(
      Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<typename Simd<Tp, Ap>::ComparisonResultType, Ap> cmp_le(
      Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<typename Simd<Tp, Ap>::ComparisonResultType, Ap> cmp_gt(
      Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<typename Simd<Tp, Ap>::ComparisonResultType, Ap> cmp_ge(
      Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> min(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> max(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> negate(Simd<Tp, Ap> simd);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> bit_and(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> bit_or(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> bit_xor(Simd<Tp, Ap> lhs, Simd<Tp, Ap> rhs);

  template <typename Tp, typename Ap>
  friend Simd<Tp, Ap> bit_not(Simd<Tp, Ap> simd);

  template <typename Dp, typename Tp, typename Ap>
  friend Simd<Dp, Ap> bit_cast(Simd<Tp, Ap> simd);

  template <typename Dp, typename Sp, typename Ap>
  friend ChangeElemTo<Simd<Sp, Ap>, Dp> static_simd_cast(Simd<Sp, Ap> simd);

 private:
  static Simd from_storage(typename Traits::InternalType storage) {
    Simd ret;
    ret.storage_ = storage;
    return ret;
  }

  template <typename Generator, size_t... indices>
  void GeneratorInit(const Generator& gen, dimsum::index_sequence<indices...>) {
    T buffer[size()] = {
        static_cast<T>(gen(std::integral_constant<size_t, indices>{}))...};
    memload(buffer, flags::element_aligned);
  }

  typename Traits::InternalType storage_;
};

// Here defines NativeSimd. It's actual definition varies on platforms. See
// *.inc files for all of them. NativeSimd is for the "most efficient" ABI on
// the current architecuture.
//   template <typename T>
//   using NativeSimd = ...

// Here defines Simd128. It's actual definition varies on platforms. See *.inc
// files for all of them.
// Simd128 is guaranteed to have 128 bits.
//   template <typename T>
//   using Simd128 = ...

// Here defines Simd64. It's actual definition varies on platforms. See *.inc
// files for all of them.
// Simd64 is guaranteed to have 64 bits.
//   template <typename T>
//   using Simd64 = ...

// Unlike p0214r5, we don't provide compatible for now. Anything related to
// cross-module calling should be interfaced with a template ABI type, e.g.:
//   template <typename T, typename Abi>
//   void PublicFunction(Simd<T, Abi> a);

// Most functions defined here are generic. Functions
// marked with `= delete` are specialized in separate *_impl-inl.inc files for
// different platforms. Others defined inside the class (e.g. abs()) may provide
// a default implementation for common types and have
// instantiations in *_impl-inl.inc files.
//
// ----------------- Primitive Operations -----------------
// Returns the concatenated result of the input parameters.
//
// The result type is a Simd<> object, with the size of the total number of T
// objects in the input parameters.
//
// Example: concat(Simd64<int32>, Simd64<int32>) returns Simd128<int32>.
// However, not all return types can be written directly. Therefore it's
// represented through `ResizeBy`.
template <typename T, typename Abi, typename... Abis>
ResizeBy<Simd<T, Abi>, 1 + sizeof...(Abis)> concat(Simd<T, Abi> simd,
                                                   Simd<T, Abis>... simds) {
  return concat(
      std::array<Simd<T, Abi>, 1 + sizeof...(Abis)>{{simd, simds...}});
}

// Similar to the above variadic template version, but all input Simd objects
// are in an array.
template <typename T, typename Abi, size_t N>
ResizeBy<Simd<T, Abi>, N> concat(std::array<Simd<T, Abi>, N> arr) {
  ResizeBy<Simd<T, Abi>, N> ret;
  constexpr size_t size = Simd<T, Abi>::size();
  for (int i = 0; i < ret.size(); i++) {
    ret.set(i, arr[i / size][i % size]);
  }
  return ret;
}

// Partitions the input Simd object into equaly-long N parts, and pack
// all partitions into an array, then return it. The partition doesn't change
// the relative order of the elements.
//
// Formally, for the returned array a, a[i][j] is initialized by
//     simd[i * (Simd<T, Abi>::size() / N) + j].
//
// By default, it's split into two smaller Simd objects.
//
// Example: split(Simd128<int32>) returns std::array<Simd64<int32>, 2>.
template <size_t N = 2, typename T, typename Abi>
std::array<ResizeBy<Simd<T, Abi>, 1, N>, N> split(Simd<T, Abi> simd) {
  static_assert(simd.size() % N == 0, "");

  using ArrayElem = ResizeBy<Simd<T, Abi>, 1, N>;
  std::array<ArrayElem, N> ret;
  constexpr size_t size = ArrayElem::size();
  for (int i = 0; i < simd.size(); i++) {
    ret[i / size].set(i % size, simd[i]);
  }
  return ret;
}

// Hypothetically concatenates lhs and rhs, index them from 0 to 2N-1, and then
// returns a Simd object with elements in the concatenated result, pointed by
// `indices`.
template <size_t... indices, typename T, typename SrcAbi>
ResizeTo<Simd<T, SrcAbi>, sizeof...(indices)> shuffle(
    Simd<T, SrcAbi> lhs, Simd<T, SrcAbi> rhs = {}) {
  using DestSimd = ResizeTo<Simd<T, SrcAbi>, sizeof...(indices)>;
#if defined(__clang__)
  return DestSimd::from_storage(
      __builtin_shufflevector(lhs.storage_, rhs.storage_, indices...));
#else
  // TODO(dimsum): GCC intrinsic doesn't support when sizeof...(indices) !=
  // lhs.size(), so we have to simulate it. Still, we can invoke the intrinsic
  // __builtin_shuffle when the input/output sizes are the same.
  T a[sizeof...(indices)];
  int i = 0;
  for (auto index : {indices...}) {
    a[i++] = index < lhs.size() ? lhs[index] : rhs[index - lhs.size()];
  }
  return DestSimd(a, flags::element_aligned);
#endif
}

namespace detail {

template <size_t... indices, typename T, typename Abi>
ResizeBy<Simd<T, Abi>, 2> zip_impl(Simd<T, Abi> lhs, Simd<T, Abi> rhs,
                                   dimsum::index_sequence<indices...>) {
  constexpr auto size = Simd<T, Abi>::size();
  return dimsum::shuffle<(indices / 2 + indices % 2 * size)...>(lhs, rhs);
}

}  // namespace detail

// Returns the element-wise absolute value.
// The element type can be signed, unsigned or floating point.
// The default implementation handles the unsigned case.
// We have specializations for signed and floating point types.
// For signed element type, the result in each lane is
// undefined if it is the minimum value.
template <typename T, typename Abi>
Simd<T, Abi> abs(Simd<T, Abi> simd) {
  // The default implementation is likely to be optimized out and generates no
  // instructions.
  static_assert(detail::get_number_kind<T>() == detail::NumberKind::kUInt,
                "The default implementation only handles unsigned types");
  return simd;
}

// Returns the element-wise sum of two Simd objects.
// The behavior of overflow is undefined.
template <typename Tp, typename Abi>
Simd<Tp, Abi> add(Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<Tp, Abi>::from_storage(lhs.storage_ + rhs.storage_);
}

// Returns the element-wise saturated sum of two Simd objects.
template <typename Tp, typename Abi>
Simd<Tp, Abi> add_saturated(Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs);

// Returns the element-wise difference of two Simd objects.
// The behavior of overflow is undefined.
template <typename Tp, typename Abi>
Simd<Tp, Abi> sub(Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<Tp, Abi>::from_storage(lhs.storage_ - rhs.storage_);
}

// Returns the element-wise saturated difference of two Simd objects.
template <typename Tp, typename Abi>
Simd<Tp, Abi> sub_saturated(Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs);

// Returns the element-wise products of two objects.
// The behavior of overflow is undefined.
template <typename Tp, typename Abi>
Simd<Tp, Abi> mul(Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<Tp, Abi>::from_storage(lhs.storage_ * rhs.storage_);
}

// Left shifts each lane by the number of bits specified in count.
// If count is negative or greater than or equal to the number of bits,
// the result is undefined.
template <typename Tp, typename Abi>
Simd<Tp, Abi> shl_simd(Simd<Tp, Abi> simd, Simd<Tp, Abi> count) {
  static_assert(std::is_integral<Tp>::value,
                "Only integral types are supported");
  return Simd<Tp, Abi>::from_storage(simd.storage_ << count.storage_);
}

// Returns shl_simd(Simd(count)).
template <typename Tp, typename Abi>
Simd<Tp, Abi> shl(Simd<Tp, Abi> simd, int count) {
  return shl_simd(simd, Simd<Tp, Abi>(count));
}

// Right shifts each lane by the number of bits specified in count.
// If the left operand is negative or greater than or equal to the number of
// bits, the result is undefined.
template <typename Tp, typename Abi>
Simd<Tp, Abi> shr_simd(Simd<Tp, Abi> simd, Simd<Tp, Abi> count) {
  static_assert(std::is_integral<Tp>::value,
                "Only integral types are supported");
  return Simd<Tp, Abi>::from_storage(simd.storage_ >> count.storage_);
}

// Returns shr_simd(Simd(count)).
template <typename Tp, typename Abi>
Simd<Tp, Abi> shr(Simd<Tp, Abi> simd, int count) {
  return shr_simd(simd, Simd<Tp, Abi>(count));
}

// Returns the element-wise comparison result.
// Each element in the result is 0 for false and non-zero for true.
// The result in each lane is undefined if either of the argument is NaN.
// TODO(maskray) define NaN comparison if it is needed.
//
// On x86_64, powerpc64le and aarch64, there is a stronger commitment that
// each element is either 0 for false or ~0 for true.
//
// For integral element types (e.g. int16), the result element type is the
// corresponding unsigned type (e.g. Simd<uint16>).
// For floating point types, the result element type is the integral type
// with the exact width (float => uint32, double => uint64).
template <typename Tp, typename Abi>
Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi> cmp_eq(
    Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi>::from_storage(
      lhs.storage_ == rhs.storage_);
}

template <typename Tp, typename Abi>
Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi> cmp_ne(
    Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi>::from_storage(
      lhs.storage_ != rhs.storage_);
}

template <typename Tp, typename Abi>
Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi> cmp_lt(
    Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi>::from_storage(
      lhs.storage_ < rhs.storage_);
}

template <typename Tp, typename Abi>
Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi> cmp_le(
    Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi>::from_storage(
      lhs.storage_ <= rhs.storage_);
}

template <typename Tp, typename Abi>
Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi> cmp_gt(
    Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi>::from_storage(
      lhs.storage_ > rhs.storage_);
}

template <typename Tp, typename Abi>
Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi> cmp_ge(
    Simd<Tp, Abi> lhs, Simd<Tp, Abi> rhs) {
  return Simd<typename Simd<Tp, Abi>::ComparisonResultType, Abi>::from_storage(
      lhs.storage_ >= rhs.storage_);
}

// For input lhs[0], lhs[1], ..., lhs[n-1], rhs[0], rhs[1], ..., rhs[n-1],
// returns lhs[0], rhs[0], lhs[1], rhs[1], ..., lhs[n-1], rhs[n-1].
template <typename T, typename Abi>
ResizeBy<Simd<T, Abi>, 2> zip(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  return zip_impl(lhs, rhs,
                  dimsum::make_index_sequence<Simd<T, Abi>::size() * 2>{});
}

namespace detail {

template <typename T, typename Abi, typename Flags>
struct LoadImpl {
  static Simd<T, Abi> Apply(const T* buffer) {
    Simd<T, Abi> ret;
    memcpy(&ret.storage_, buffer, sizeof(ret));
    return ret;
  }
};

template <size_t kArity>
struct ReduceAddImpl;

template <>
struct ReduceAddImpl<1> {
  template <typename T, typename Abi>
  static Simd<T, Abi> Apply(Simd<T, Abi> simd) {
    return simd;
  }
};

template <>
struct ReduceAddImpl<2> {
  template <typename SimdType, size_t... indices>
  static ResizeBy<SimdType, 1, 2> ApplyImpl(
      SimdType simd, dimsum::index_sequence<indices...>) {
    return shuffle<(2 * indices)...>(simd) +
           shuffle<(2 * indices + 1)...>(simd);
  }

  template <typename SimdType>
  static ResizeBy<SimdType, 1, 2> Apply(SimdType simd) {
    return ApplyImpl(simd, dimsum::make_index_sequence<simd.size() / 2>{});
  }
};

template <size_t kArity>
struct ReduceAddImpl {
  template <typename SimdType>
  static ResizeBy<SimdType, 1, kArity> Apply(SimdType simd) {
    return ReduceAddImpl<kArity / 2>::Apply(ReduceAddImpl<2>::Apply(simd));
  }
};

}  // namespace detail

// Perform a kArity-way summation.
//
// Put every adjacent kArity elements into a group, and calculate the sum of
// such a group. Return all sums in a Simd object.
//
// Formally, for the result a,
//     a[i] = sum(simd[i * kArity ... (i + 1) * kArity - 1])
template <size_t kArity, typename T, typename Abi>
ResizeBy<Simd<T, Abi>, 1, kArity> reduce_add(Simd<T, Abi> simd) {
  static_assert(std::is_integral<T>::value,
                "The element types needs to be integrals.");
  static_assert(simd.size() % kArity == 0, "");
  return detail::ReduceAddImpl<kArity>::Apply(simd);
}

// Equivalent to reduce_add<simd.size()>. The result is always by size 1.
template <typename T, typename Abi>
ResizeTo<Simd<T, Abi>, 1> reduce_add(Simd<T, Abi> simd) {
  return reduce_add<simd.size()>(simd);
}

// Narrows elements of the input Simd objects by half, and concatenate them
// together. Always produce a signed result.
template <typename T, typename Abi>
Simd<detail::Number<sizeof(T) / 2, detail::NumberKind::kSInt>, Abi>
pack_saturated(Simd<T, Abi> lhs, Simd<T, Abi> rhs) DIMSUM_DELETE;

// Narrows elements of the input Simd objects by half, and concatenate them
// together. Always produce an unsigned result.
template <typename T, typename Abi>
Simd<detail::Number<sizeof(T) / 2, detail::NumberKind::kUInt>, Abi>
packu_saturated(Simd<T, Abi> lhs, Simd<T, Abi> rhs) DIMSUM_DELETE;

// Returns the element-wise, widening multiplication. For input
// Simd<(u)intn>, returns a Simd object with element type (u)int2n, and with the
// same amount of elements as lhs/rhs.
template <typename T, typename Abi>
ScaleElemBy<Simd<T, Abi>, 2> mul_widened(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  auto ls = split<2>(lhs);
  auto rs = split<2>(rhs);
  return concat(mul_widened(ls[0], rs[0]), mul_widened(ls[1], rs[1]));
}

// Returns the element-wise min result. Elements should not contain NaN.
template <typename T, typename Abi>
Simd<T, Abi> min(Simd<T, Abi> lhs, Simd<T, Abi> rhs);

// Returns the element-wise max result. Elements should not contain NaN.
template <typename T, typename Abi>
Simd<T, Abi> max(Simd<T, Abi> lhs, Simd<T, Abi> rhs);

// Returns the element-wise negation.
template <typename T, typename Abi>
Simd<T, Abi> negate(Simd<T, Abi> simd) {
  return Simd<T, Abi>::from_storage(-simd.storage_);
}

// Returns the element-wise bit and result.
template <typename T, typename Abi>
Simd<T, Abi> bit_and(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  static_assert(std::is_integral<T>::value, "Only integer types are supported");
  return Simd<T, Abi>::from_storage(lhs.storage_ & rhs.storage_);
}

// Returns the element-wise bit or result.
template <typename T, typename Abi>
Simd<T, Abi> bit_or(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  static_assert(std::is_integral<T>::value, "Only integer types are supported");
  return Simd<T, Abi>::from_storage(lhs.storage_ | rhs.storage_);
}

// Returns the element-wise bit xor result.
template <typename T, typename Abi>
Simd<T, Abi> bit_xor(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  static_assert(std::is_integral<T>::value, "Only integer types are supported");
  return Simd<T, Abi>::from_storage(lhs.storage_ ^ rhs.storage_);
}

// Returns the element-wise bit not result.
template <typename T, typename Abi>
Simd<T, Abi> bit_not(Simd<T, Abi> simd) {
  static_assert(std::is_integral<T>::value, "Only integer types are supported");
  // GCC 4.9 reports `simd` set but not used. Make it happy.
  (void)simd;
  return Simd<T, Abi>::from_storage(~simd.storage_);
}

// Returns the element-wise estimate of reciprocal.
// On x86, the relative error is less or equal than 1.5/4096.
// On POWER, the relative error is less or equal than 1/16384.
// On ARM, the relative error may be slightly greater than 1/512.
template <typename T, typename Abi>
Simd<T, Abi> reciprocal_estimate(Simd<T, Abi>) DIMSUM_DELETE;

// Returns the element-wise square root.
template <typename T, typename Abi>
Simd<T, Abi> sqrt(Simd<T, Abi>) DIMSUM_DELETE;

// Returns the element-wise estimate of reciprocal square root.
template <typename T, typename Abi>
Simd<T, Abi> reciprocal_sqrt_estimate(Simd<T, Abi>) DIMSUM_DELETE;

// Element-wise round to integral floating points using the round-toeven rule.
// On x86 and ARM, specific instructions are used to guarantee round-to-even.
// On Power, vec_rint is used which rounds floating points according to Rounding
// Control field RN in FPSCR, which defaults to round-to-even.
template <typename T, typename Abi>
Simd<T, Abi> round(Simd<T, Abi>) DIMSUM_DELETE;

// Casts to another vector type of the same width without changing any bits.
template <typename Dest, typename T, typename Abi>
Simd<Dest, Abi> bit_cast(Simd<T, Abi> simd) {
  Simd<Dest, Abi> ret;
  static_assert(sizeof(ret.storage_) == sizeof(simd.storage_),
                "Simd width mismatch");
  memcpy(&ret.storage_, &simd.storage_, sizeof(ret.storage_));
  return ret;
}

// Element-wise static_cast<Dest>().
template <typename Dest, typename Src, typename Abi>
ChangeElemTo<Simd<Src, Abi>, Dest> static_simd_cast(Simd<Src, Abi> simd) {
  using DestSimd = ChangeElemTo<Simd<Src, Abi>, Dest>;
#if defined(__clang__)
  return DestSimd(__builtin_convertvector(
      simd.storage_, typename DestSimd::Traits::InternalType));
#else
  DestSimd ret;
  for (int i = 0; i < ret.size(); i++) {
    ret.storage_[i] = static_cast<Dest>(simd.storage_[i]);
  }
  return ret;
#endif
}

// Element-wise static_cast<Dest>() prohibiting narrowing cast, e.g. every
// possible value of the element type can be represented with Dest.
template <typename Dest, typename Src, typename Abi>
ChangeElemTo<Simd<Src, Abi>, Dest> simd_cast(Simd<Src, Abi> simd) {
  static_assert(!detail::IsNarrowingConversion<Dest, Src>(),
                "simd_cast does not support narrowing cast. Use "
                "static_simd_cast instead.");
  return static_simd_cast<Dest, Src, Abi>(simd);
}

template <typename Dest, typename Src, typename Abi>
__attribute__((
    deprecated("simd_cast for non-narrowing cast and static_simd_cast for "
               "narrowing cast."))) ChangeElemTo<Simd<Src, Abi>, Dest>
convert(Simd<Src, Abi> simd) {
  return static_simd_cast<Dest, Src, Abi>(simd);
}

// Overloaded operators.
template <typename T, typename Abi>
Simd<T, Abi> operator-(Simd<T, Abi> a) {
  return negate(a);
}

template <typename T, typename Abi>
Simd<T, Abi> operator+(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  return add(lhs, rhs);
}

template <typename T, typename Abi>
Simd<T, Abi> operator-(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  return sub(lhs, rhs);
}

template <typename T, typename Abi>
Simd<T, Abi> operator*(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  return mul(lhs, rhs);
}

template <typename T, typename Abi>
Simd<T, Abi> operator&(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  return bit_and(lhs, rhs);
}

template <typename T, typename Abi>
Simd<T, Abi> operator|(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  return bit_or(lhs, rhs);
}

template <typename T, typename Abi>
Simd<T, Abi> operator^(Simd<T, Abi> lhs, Simd<T, Abi> rhs) {
  return bit_xor(lhs, rhs);
}

template <typename T, typename Abi>
void operator+=(Simd<T, Abi>& lhs, Simd<T, Abi> rhs) {
  lhs = lhs + rhs;
}

template <typename T, typename Abi>
void operator-=(Simd<T, Abi>& lhs, Simd<T, Abi> rhs) {
  lhs = lhs - rhs;
}

template <typename T, typename Abi>
void operator*=(Simd<T, Abi>& lhs, Simd<T, Abi> rhs) {
  lhs = lhs * rhs;
}

template <typename T, typename Abi>
void operator&=(Simd<T, Abi>& lhs, Simd<T, Abi> rhs) {
  lhs = lhs & rhs;
}

template <typename T, typename Abi>
void operator|=(Simd<T, Abi>& lhs, Simd<T, Abi> rhs) {
  lhs = lhs | rhs;
}

template <typename T, typename Abi>
void operator^=(Simd<T, Abi>& lhs, Simd<T, Abi> rhs) {
  lhs = lhs ^ rhs;
}

template <typename T, typename Abi>
Simd<T, Abi> operator~(Simd<T, Abi> a) {
  return bit_not(a);
}

// ----------------- Compound Operations -----------------

// Similar to static_simd_cast<Dest>(round(simd)), but possibly faster.
//
// Currently only Simd<int32, Abi> round_to_integer(Simd<float, Abi>) is
// accepted. On Power, vec_rint is used which rounds floating points according
// to Rounding Control field RN in FPSCR, which defaults to round-to-even.
template <typename Dest, typename T, typename Abi>
Simd<Dest, Abi> round_to_integer(Simd<T, Abi>) DIMSUM_DELETE;

// Equivalent to reduce_add<kArity>(static_simd_cast<ScaleBy<T, kArity>>(simd)),
// but possibly faster.
template <size_t kArity, typename T, typename Abi>
ResizeBy<ScaleElemBy<Simd<T, Abi>, kArity>, 1, kArity> reduce_add_widened(
    Simd<T, Abi> simd) {
  return reduce_add<kArity>(static_simd_cast<ScaleBy<T, kArity>>(simd));
}

// Equivalent to acc + reduce_add<2>(mul_widened(lhs, rhs)), but probably
// faster.
//
// e.g. mul_sum(Simd128<int16>, Simd128<int16>, Simd128<int32>)
// computes 8 int16*int16 products ({s[0], ..., s[7]}) first, then does pairwise
// horizontal summation and gets {s[0]+s[1], s[2]+s[3], s[4]+s[5], s[6]+s[7]}),
// so that the result is still 128-bit, finally returns {acc[0]+s[0]+s[1],
// acc[1]+s[2]+s[3], acc[2]+s[4]+s[5], acc[3]+s[6]+s[7]}.
template <typename T, typename Abi>
ResizeBy<ScaleElemBy<Simd<T, Abi>, 2>, 1, 2> mul_sum(
    Simd<T, Abi> lhs, Simd<T, Abi> rhs,
    ResizeBy<ScaleElemBy<Simd<T, Abi>, 2>, 1, 2> acc =
        ResizeBy<ScaleElemBy<Simd<T, Abi>, 2>, 1, 2>(0)) {
  return acc + reduce_add<2>(mul_widened(lhs, rhs));
}

}  // namespace dimsum

#endif  // DIMSUM_SIMD_H_
