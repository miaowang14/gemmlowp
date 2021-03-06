// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// pack.h: packing blocks of the LHS and RHS into the data layout
// that is expected by compute.h and eventually by kernels.
// Because this data layout depends on the kernel format, code here
// is templated in KernelLhsFormat/KernelRhsFormat.
//
// Readers note: an important theme around here is that we try hard
// to handle both Lhs and Rhs with a single piece of code. We indifferently
// refer to the Lhs and Rhs as a 'Side'. Instead of addressing matrices
// by (row, column) indices, we address them by (width, depth), as explained
// in kernel.h. This allows us to handle both Lhs and Rhs on an equal footing,
// at once.

#ifndef GEMMLOWP_INTERNAL_PACK_H_
#define GEMMLOWP_INTERNAL_PACK_H_

#include <cstring>

#include "block_params.h"
#include "kernel.h"
#include "common.h"
#include "allocator.h"
#include "bit_depth_util.h"

namespace gemmlowp {

// A PackedSideBlock instance is a packed block of either the LHS or RHS
// (whence the generic 'Side' name).
//
// 'Packed' means that it is laid out in the storage order that
// is expected by the specified kernel format. From a block of the input
// LHS or RHS matrix, one obtains a PackedSideBlock by calling PackLhs()
// or PackRhs().
template <typename tKernelSideFormat>
class PackedSideBlock {
 public:
  typedef tKernelSideFormat KernelSideFormat;

  PackedSideBlock(Side side, Allocator* allocator,
                  const BlockParams& block_params,
                  int rank_one_update_multiplier)
      : allocator_(allocator),
        rank_one_update_multiplier_(rank_one_update_multiplier),
        pos_(0) {
    GetSideBlockParams(side, &params_, block_params);
    data_handle_ =
        allocator_->Reserve<std::uint8_t>(params_.l2_width * params_.l2_depth);
    rank_one_update_handle_ =
        allocator_->Reserve<std::int32_t>(params_.l2_width);
  }

  ~PackedSideBlock() {}

  void seek_run(int start_width, int start_depth) const {
    int kernel_run_depth =
        std::min<int>(params_.l1_depth, params_.l2_depth - start_depth);
    pos_ = params_.l2_width * start_depth + start_width * kernel_run_depth;
  }

  void seek_next_cell() const { pos_ += KernelSideFormat::Cell::kSize; }

  void seek_forward_n_cells(int n) const {
    pos_ += n * KernelSideFormat::Cell::kSize;
  }

  const std::uint8_t* current_data() const {
    return allocator_->GetPointer<std::uint8_t>(data_handle_) + pos_;
  }

  std::uint8_t* current_data() {
    return allocator_->GetPointer<std::uint8_t>(data_handle_) + pos_;
  }

  std::int32_t* rank_one_update() {
    return allocator_->GetPointer<std::int32_t>(rank_one_update_handle_);
  }

  const std::int32_t* rank_one_update() const {
    return allocator_->GetPointer<const std::int32_t>(rank_one_update_handle_);
  }

  std::int32_t rank_one_update_multiplier() const {
    return rank_one_update_multiplier_;
  }

  const SideBlockParams& params() const { return params_; }

 private:
  // The block size parameters that this PackedSizeBlock follows.
  // The L2 parameters determine its overall size, while the L1 parameters,
  // together with the kernel format template parameter, determine
  // the fine details of the storage/traversal order.
  SideBlockParams params_;

  // Pointer to the allocator provided by the caller. Not owned.
  // The Allocator is assumed to outlive the PackedSideBlock.
  Allocator* const allocator_;

  // Handle on the buffer backing this packed block. Owned.
  Allocator::Handle data_handle_;

  // Handle on the additional buffer backing the rank-one-update vector
  // associated with this block. Owned.
  Allocator::Handle rank_one_update_handle_;

  // The constant multiplier of the rank one update vector.
  std::int32_t rank_one_update_multiplier_;

  // pos_ is the current position in the buffer, which we access
  // sequentially, like a file.
  // The idea is that we pack data in the same order as it is
  // going to be traversed during the computation, which for
  // cache-friendliness reasons is complicated to random-access,
  // as the offsets calculations would be intricate. So we
  // give up random-access addressing, and instead content ourselves
  // with sequential access.
  //
  // pos_ is mutable because during the computation we will want to
  // be able to iterate on the data in a const PackedSideBlock.
  mutable int pos_;
};

// WidthMajor and DepthMajor are custom phrases modelled after the
// standard terminology 'row-major' and 'column-major'. Their meaning
// should be transparent once one has read the explanation in kernel.h:
// for example, in the Lhs, the 'width' dimension is the rows dimension,
// so there WidthMajor means RowMajor, while in the Rhs it is the opposite.
// Another way to put it: WidthMajor means that contiguous storage is used
// for entries having the same 'width' index.
enum class SideMapOrder { WidthMajor, DepthMajor };

// Similar to MatrixMap from map.h, but in terms of width/depth instead of
// rows/columns. Used to address blocks of the input LHS/RHS matrices when
// packing them.
template <typename tScalar, SideMapOrder tOrder>
class SideMap {
 public:
  typedef tScalar Scalar;
  static const SideMapOrder kOrder = tOrder;

  SideMap(Scalar* data, int width, int depth, int stride)
      : data_(data), width_(width), depth_(depth), stride_(stride) {}

  SideMap(Scalar* data, int width, int depth)
      : data_(data), width_(width), depth_(depth) {
    stride_ = kOrder == SideMapOrder::WidthMajor ? depth_ : width_;
  }

  SideMap(const SideMap& other)
      : data_(other.data_),
        width_(other.width_),
        depth_(other.depth_),
        stride_(other.stride_) {}

  int width() const { return width_; }
  int depth() const { return depth_; }
  int stride() const { return stride_; }
  int width_stride() const {
    return kOrder == SideMapOrder::DepthMajor ? 1 : stride_;
  }
  int depth_stride() const {
    return kOrder == SideMapOrder::WidthMajor ? 1 : stride_;
  }
  Scalar* data() const { return data_; }
  Scalar* data(int w, int d) const {
    return data_ + w * width_stride() + d * depth_stride();
  }
  Scalar operator()(int w, int d) const { return *data(w, d); }
  Scalar& operator()(int w, int d) { return *data(w, d); }

  SideMap block(int start_width, int start_depth, int block_width,
                int block_depth) const {
    assert(start_width >= 0);
    assert(start_width + block_width <= width_);
    assert(start_depth >= 0);
    assert(start_depth + block_depth <= depth_);

    return SideMap(data(start_width, start_depth), block_width, block_depth,
                   stride_);
  }

 private:
  Scalar* data_;  // not owned.
  int width_, depth_, stride_;
};

// A cheap and reasonably good PRNG producing nonzero uint8's.
//
// This uses a 8-bit Xorshift. This choice is motivated by the following
// two reasons:
//
//   1. We want a value uniformly distributed on 255 different values, since
//      we will be dividing by 255. Xorshift naturally provies a uniform
//      distribution on [1..255]. By contrast, a LCG would produce a uniform
//      distribution on [0..255] from which is wouldn't be obvious how to
//      get exactly what we need.
//
//   2. All our attempts to get an unbiased distribution from a 8-bit LCG
//      resulted in still higher bias than this 8-bit Xorshift on
//      TestWithRealData. For example, one can null the lsb from the output
//      of a 8-bit LCG, to get uniform *even* values in [0..254], which
//      should be good enough and unbiased as far as we are concerned;
//      however, that still gives these results in TestWithRealData:
//        median unsigned diff: 2 (tolerating 2)
//        max unsigned diff: 15 (tolerating 10)
//        median signed diff: 0 (tolerating 0)
//        mean signed diff: 0.474 (tolerating 0.2)
//      By contrast, this Xorshift8 gives us the much better results:
//        median unsigned diff: 1 (tolerating 2)
//        max unsigned diff: 9 (tolerating 10)
//        median signed diff: 0 (tolerating 0)
//        mean signed diff: 0.00997 (tolerating 0.2)
//
class DefaultPseudoRandomNonzeroBytesGenerator {
 public:
  DefaultPseudoRandomNonzeroBytesGenerator() { x_ = 128; }

  std::uint8_t get() {
    std::uint8_t result = x_;
    // Xorshift8(7,5,3)
    x_ ^= x_ << 7;
    x_ ^= x_ >> 5;
    x_ ^= x_ << 3;
    return result;
  }

 private:
  // State
  std::uint8_t x_;
};

// Requantizes a source uint8 value in [0..255] range
// to the range specified by BitDepth, [0..((2^bits)-1)].
// Bias must be avoided. Currently this is achieved
// by probabilistic rounding.
template <typename BitDepth, RoundingMode Rounding>
std::uint8_t Requantize(std::uint8_t raw_src_val,
                        DefaultPseudoRandomNonzeroBytesGenerator* prng) {
  static const int kBits = BitDepth::kBits;
  static const std::uint8_t kMaxVal = (1 << kBits) - 1;

  if (kBits == 8) {
    return raw_src_val;
  }

  std::uint16_t scaled = static_cast<std::uint16_t>(raw_src_val) * kMaxVal;

  std::uint8_t rounding_offset;

  switch (Rounding) {
    case RoundingMode::Nearest:
      rounding_offset = 127;
      break;
    case RoundingMode::Probabilistic:
      rounding_offset = prng->get() - 1;
      break;
    default:
      assert(false);
      rounding_offset = 0;
  }

  return (scaled + rounding_offset) / 255;
}

// A PackingRegisterBlock is a small fixed-size block of a matrix being
// packed. This class is the generic non-optimized implementation,
// it is inherited by the generic implementation of PackingRegisterBlock,
// which may be overriden by template specialization. Overriding it is how
// one may provide optimized packing code paths.
//
// The packing of a block proceeds in two steps:
//   1. Ensuring that we have a complete block of source data, i.e. a block of
//      the compile-time prescribed size. This is where we handle unaligned
//      boundaries: if we don't have a complete block of source data, then
//      we copy and zero-extend it into a local temporary (complete_src_),
//      see MakeCompleteSrc. In the generic case, we do have a complete block,
//      so we just use it in-place, see UseCompleteSrcInPlace.
//   2. Packing a complete block into the destination, see Pack. This is the
//      most critical part, so it's convenient that unaligned boundaries have
//      already been handled in step 1.
template <typename SrcMapType, typename PackedSideBlock>
class PackingRegisterBlockBase {
 public:
  typedef typename PackedSideBlock::KernelSideFormat KernelSideFormat;
  typedef typename KernelSideFormat::Cell CellFormat;
  static const int kCells = KernelSideFormat::kCells;
  static const int kCellWidth = CellFormat::kWidth;
  static const int kKernelWidth = CellFormat::kWidth * kCells;
  static const int kCellDepth = CellFormat::kDepth;
  static const int kCellSize = CellFormat::kSize;

  static const SideMapOrder kSrcOrder = SrcMapType::kOrder;

  typedef DefaultPseudoRandomNonzeroBytesGenerator
      PseudoRandomNonzeroBytesGenerator;

  PackingRegisterBlockBase() : complete_src_(nullptr, 0, 0, 0) {}

 protected:
  // The source data that's ready for packing. May point to
  // in-place actual source data if it's already a complete block,
  // (see UseCompleteSrcInPlace)
  // or to the local buf_ below into which we copy incomplete blocks
  // (see MakeCompleteSrc)
  SrcMapType complete_src_;

  // Temporary buffer for loading incomplete blocks to,
  // in the source storage order
  std::uint8_t buf_[kKernelWidth * kRegisterSize];

 public:
  // Selects a block if in-place source data that's already a complete block
  void UseCompleteSrcInPlace(const SrcMapType& src) { complete_src_ = src; }
  // Copies an incomplete block of source data into a local temporary
  // complete block by zero-extending it.
  void MakeCompleteSrc(const SrcMapType& src) {
    memset(buf_, 0, kKernelWidth * kRegisterSize);
    if (kSrcOrder == SideMapOrder::WidthMajor) {
      for (int w = 0; w < src.width(); w++) {
        memcpy(buf_ + w * kRegisterSize, src.data(w, 0), src.depth());
      }
    } else {
      assert(kSrcOrder == SideMapOrder::DepthMajor);
      for (int d = 0; d < src.depth(); d++) {
        memcpy(buf_ + d * kKernelWidth, src.data(0, d), src.width());
      }
    }
    complete_src_ = SrcMapType(buf_, kKernelWidth, kRegisterSize);
  }
  // Packs a complete block into the destination. This is the most
  // critical part and the part that we most typically want to
  // override in architecture-specific optimized specializations.
  template <typename BitDepth, RoundingMode Rounding>
  void Pack(PackedSideBlock* dst, int start_width,
            PseudoRandomNonzeroBytesGenerator* prng) {
    std::uint8_t* dst_ptr = dst->current_data();
    for (int cell_start_depth = 0; cell_start_depth < kRegisterSize;
         cell_start_depth += kCellDepth) {
      for (int cell_start_width = 0; cell_start_width < kKernelWidth;
           cell_start_width += kCellWidth) {
        std::int32_t* cell_rank_one_update_ptr =
            dst->rank_one_update() + start_width + cell_start_width;
        const SideMap<const std::uint8_t, kSrcOrder> src_cell_map(
            complete_src_.block(cell_start_width, cell_start_depth, kCellWidth,
                                kCellDepth));
        for (int w = 0; w < kCellWidth; w++) {
          std::int32_t sum = 0;
          for (int d = 0; d < kCellDepth; d++) {
            const std::uint8_t raw_src_val = src_cell_map(w, d);
            const std::uint8_t requantized =
                Requantize<BitDepth, Rounding>(raw_src_val, prng);
            dst_ptr[OffsetIntoCell<CellFormat>(w, d)] = requantized;
            sum += requantized;
          }
          cell_rank_one_update_ptr[w] +=
              sum * dst->rank_one_update_multiplier();
        }
        dst_ptr += kCellSize;
      }
    }
    dst->seek_forward_n_cells(kCells * kRegisterSize / kCellDepth);
  }
};

template <typename SrcMapType, typename PackedSideBlock>
class PackingRegisterBlock
    : public PackingRegisterBlockBase<SrcMapType, PackedSideBlock> {};

// Large-scale implementation of packing.
template <typename BitDepth, typename SrcMapType, typename PackedSideBlock>
class PackSideBlockImpl {
 public:
  typedef typename PackedSideBlock::KernelSideFormat KernelSideFormat;
  typedef typename KernelSideFormat::Cell CellFormat;
  static const int kCells = KernelSideFormat::kCells;
  static const int kCellWidth = CellFormat::kWidth;
  static const int kKernelWidth = CellFormat::kWidth * kCells;
  static const int kCellDepth = CellFormat::kDepth;

  typedef typename PackingRegisterBlock<SrcMapType, PackedSideBlock>::
      PseudoRandomNonzeroBytesGenerator PseudoRandomNonzeroBytesGenerator;

  PackSideBlockImpl(PackedSideBlock* packed_side_block,
                    const SrcMapType& src_map)
      : packed_side_block_(packed_side_block),
        src_map_(src_map),
        rounding_mode_(ChooseRoundingMode<BitDepth>(src_map.depth())) {}

  PackedSideBlock* packed_side_block() const { return packed_side_block_; }

  const SrcMapType& src_map() const { return src_map_; }

  // The public entry point to pack a block.
  void PackL2() {
    memset(packed_side_block_->rank_one_update(), 0,
           sizeof(std::int32_t) * packed_side_block_->params().l2_width);
    for (int d = 0; d < src_map_.depth();
         d += packed_side_block_->params().l1_depth) {
      int ds = std::min<int>(packed_side_block_->params().l1_depth,
                             src_map_.depth() - d);

      for (int w = 0; w < src_map_.width();
           w += packed_side_block_->params().l1_width) {
        int ws = std::min<int>(packed_side_block_->params().l1_width,
                               src_map_.width() - w);

        PrefetchL1(w, ws, d, ds);
        PackL1(w, ws, d, ds);
      }
    }
  }

 protected:
  // The intermediate-level loops, between PackL2 and PackRun.
  void PackL1(int start_width, int width, int start_depth, int depth) {
    for (int w = 0; w < width; w += kKernelWidth) {
      int ws = std::min(+kKernelWidth, width - w);
      packed_side_block_->seek_run(start_width + w, start_depth);
      PackRun(start_width + w, ws, start_depth, depth);
    }
  }

  // Prefetches the data that will be read by PackL1
  void PrefetchL1(int start_width, int width, int start_depth, int depth) {
    if (SrcMapType::kOrder == SideMapOrder::WidthMajor) {
      for (int d = 0; d < depth; d += kDefaultCacheLineSize) {
        for (int w = 0; w < width; w += 1) {
          Prefetch(src_map_.data(start_width + w, start_depth + d));
        }
      }
    } else {
      for (int d = 0; d < depth; d++) {
        for (int w = 0; w < width; w += kDefaultCacheLineSize) {
          Prefetch(src_map_.data(start_width + w, start_depth + d));
        }
      }
    }
  }

  // PackRun packs only a run i.e. is the inner loop in the depth dimension.
  template <RoundingMode Rounding>
  void PackRun(int start_width, int width, int start_depth, int depth) {
    PackingRegisterBlock<SrcMapType, PackedSideBlock> b;
    if (width == kKernelWidth) {
      const int register_aligned_depth = RoundDown<kRegisterSize>(depth);
      if (register_aligned_depth) {
        for (int d = 0; d < register_aligned_depth; d += kRegisterSize) {
          b.UseCompleteSrcInPlace(src_map_.block(start_width, start_depth + d,
                                                 width, kRegisterSize));
          b.template Pack<BitDepth, Rounding>(packed_side_block_, start_width,
                                              &prng_);
        }
      }
      if (register_aligned_depth < depth) {
        b.MakeCompleteSrc(
            src_map_.block(start_width, start_depth + register_aligned_depth,
                           width, depth - register_aligned_depth));
        b.template Pack<BitDepth, Rounding>(packed_side_block_, start_width,
                                            &prng_);
      }
    } else {
      assert(width < kKernelWidth);
      for (int d = 0; d < depth; d += kRegisterSize) {
        const int ds = std::min(+kRegisterSize, depth - d);
        b.MakeCompleteSrc(
            src_map_.block(start_width, start_depth + d, width, ds));
        b.template Pack<BitDepth, Rounding>(packed_side_block_, start_width,
                                            &prng_);
      }
    }
  }

  // Dispatches the runtime rounding mode parameter to compile-time
  // template instantiations.
  void PackRun(int start_width, int width, int start_depth, int depth) {
    switch (rounding_mode_) {
      case RoundingMode::Nearest:
        return this->PackRun<RoundingMode::Nearest>(start_width, width,
                                                    start_depth, depth);
      case RoundingMode::Probabilistic:
        return this->PackRun<RoundingMode::Probabilistic>(start_width, width,
                                                          start_depth, depth);
      default:
        assert(false);
    }
  }

  // The PackedSideBlock being packed, i.e. the 'destination'.
  PackedSideBlock* const packed_side_block_;

  // A map on the block of the original matrix block being packed,
  // i.e. the 'source'.
  const SrcMapType& src_map_;

  const RoundingMode rounding_mode_;

  // Used for probabilistic requantization in the less-than-8-bit case.
  // Otherwise unused.
  PseudoRandomNonzeroBytesGenerator prng_;
};

// Packs a block of the input LHS matrix, into a PackedSideBlock
template <BitDepthSetting BitDepth, typename PackedSideBlock,
          typename MatrixMapType>
void PackLhs(PackedSideBlock* dst, const MatrixMapType& src) {
  ScopedProfilingLabel label("pack LHS");
  static const SideMapOrder kSideMapOrder =
      MatrixMapType::kOrder == MapOrder::RowMajor ? SideMapOrder::WidthMajor
                                                  : SideMapOrder::DepthMajor;
  typedef typename MatrixMapType::Scalar Scalar;
  typedef SideMap<Scalar, kSideMapOrder> SideMapType;
  SideMapType src_side_map(src.data(), src.rows(), src.cols(), src.stride());
  typedef PackSideBlockImpl<LhsBitDepth<BitDepth>, SideMapType, PackedSideBlock>
      ImplType;
  ImplType impl(dst, src_side_map);
  impl.PackL2();
}

// Packs a block of the input RHS matrix, into a PackedSideBlock
template <BitDepthSetting BitDepth, typename PackedSideBlock,
          typename MatrixMapType>
void PackRhs(PackedSideBlock* dst, const MatrixMapType& src) {
  ScopedProfilingLabel label("pack RHS");
  static const SideMapOrder kSideMapOrder =
      MatrixMapType::kOrder == MapOrder::ColMajor ? SideMapOrder::WidthMajor
                                                  : SideMapOrder::DepthMajor;
  typedef typename MatrixMapType::Scalar Scalar;
  typedef SideMap<Scalar, kSideMapOrder> SideMapType;
  SideMapType src_side_map(src.data(), src.cols(), src.rows(), src.stride());
  typedef PackSideBlockImpl<RhsBitDepth<BitDepth>, SideMapType, PackedSideBlock>
      ImplType;
  ImplType impl(dst, src_side_map);
  impl.PackL2();
}

}  // namespace gemmlowp

#ifdef GEMMLOWP_NEON
#include "pack_neon.h"
#endif

#endif  // GEMMLOWP_INTERNAL_PACK_H_
