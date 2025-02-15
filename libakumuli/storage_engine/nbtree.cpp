/**
 * Copyright (c) 2016 Eugene Lazin <4lazin@gmail.com>
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
 *
 */

// C++
#include <iostream>  // For debug print fn.
#include <algorithm>
#include <vector>
#include <sstream>
#include <stack>
#include <array>

// App
#include "nbtree.h"
#include "akumuli_version.h"
#include "status_util.h"
#include "log_iface.h"
#include "operators/scan.h"
#include "operators/aggregate.h"


namespace Akumuli {
namespace StorageEngine {

std::ostream& operator << (std::ostream& out, NBTreeBlockType blocktype) {
    if (blocktype == NBTreeBlockType::LEAF) {
        out << "Leaf";
    } else {
        out << "Superblock";
    }
    return out;
}

static const SubtreeRef INIT_SUBTREE_REF = {
    0,
    //! Series Id
    0,
    //! First element's timestamp
    0,
    //! Last element's timestamp
    0,
    //! Object addr in blockstore
    EMPTY_ADDR,
    //! Smalles value
    std::numeric_limits<double>::max(),
    //! Registration time of the smallest value
    std::numeric_limits<aku_Timestamp>::max(),
    //! Largest value
    std::numeric_limits<double>::lowest(),
    //! Registration time of the largest value
    std::numeric_limits<aku_Timestamp>::lowest(),
    //! Summ of all elements in subtree
    .0,
    //! First value in subtree
    .0,
    //! Last value in subtree
    .0,
    //! Block type
    NBTreeBlockType::LEAF,
    //! Node level in the tree
    0,
    //! Payload size (real)
    0,
    //! Node version
    AKUMULI_VERSION,
    //! Fan out index of the element (current)
    0,
    //! Checksum of the block (not used for links to child nodes)
    0
};

static std::string to_string(const SubtreeRef& ref) {
    std::stringstream fmt;
    fmt                     << "{"
        << ref.count        << ", "
        << ref.id           << ", "
        << ref.begin        << ", "
        << ref.end          << ", "
        << ref.addr         << ", "
        << ref.min          << ", "
        << ref.min_time     << ", "
        << ref.max          << ", "
        << ref.max_time     << ", "
        << ref.sum          << ", "
        << ref.first        << ", "
        << ref.last         << ", "
        << ref.type         << ", "
        << ref.level        << ", "
        << ref.payload_size << ", "
        << ref.version      << ", "
        << ref.fanout_index << ", "
        << ref.checksum     << "}";
    return fmt.str();
}


static std::tuple<aku_Status, std::unique_ptr<IOVecBlock>> read_and_check(std::shared_ptr<BlockStore> bstore, LogicAddr curr) {
    aku_Status status;
    std::unique_ptr<IOVecBlock> block;
    std::tie(status, block) = bstore->read_iovec_block(curr);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, std::move(block));
    }
    if (block->get_size(0) == AKU_BLOCK_SIZE) {
        // This check only makes sense when reading data back. In this case IOVecBlock will
        // contain one large component.
        u8 const* data = block->get_cdata(0);
        SubtreeRef const* subtree = block->get_cheader<SubtreeRef>();
        u32 crc = bstore->checksum(data + sizeof(SubtreeRef), subtree->payload_size);
        if (crc != subtree->checksum) {
            std::stringstream fmt;
            fmt << "Invalid checksum (addr: " << curr << ", level: " << subtree->level << ")";
            Logger::msg(AKU_LOG_ERROR, fmt.str());
            status = AKU_EBAD_DATA;
        }
    }
    return std::make_tuple(status, std::move(block));
}


//! Read block from blockstoroe with all the checks. Panic on error!
static std::unique_ptr<IOVecBlock> read_iovec_block_from_bstore(std::shared_ptr<BlockStore> bstore, LogicAddr curr) {
    aku_Status status;
    std::unique_ptr<IOVecBlock> block;
    std::tie(status, block) = bstore->read_iovec_block(curr);
    if (status != AKU_SUCCESS) {
        Logger::msg(AKU_LOG_ERROR, "Can't read block @" + std::to_string(curr) + ", error: " + StatusUtil::str(status));
        AKU_PANIC("Can't read block - " + StatusUtil::str(status));
    }
    // Check consistency (works with both inner and leaf nodes).
    if (block->get_size(0) == AKU_BLOCK_SIZE) {
        // This check only makes sense when reading data back. In this case IOVecBlock will
        // contain one large component.
        u8 const* data = block->get_cdata(0);
        SubtreeRef const* subtree = block->get_cheader<SubtreeRef>();
        u32 crc = bstore->checksum(data + sizeof(SubtreeRef), subtree->payload_size);
        if (crc != subtree->checksum) {
            std::stringstream fmt;
            fmt << "Invalid checksum (addr: " << curr << ", level: " << subtree->level << ")";
            AKU_PANIC(fmt.str());
        }
    }
    return block;
}

//! Initialize object from leaf node
aku_Status init_subtree_from_leaf(const IOVecLeaf& leaf, SubtreeRef& out) {
    if (leaf.nelements() == 0) {
        return AKU_EBAD_ARG;
    }
    SubtreeRef const* meta = leaf.get_leafmeta();
    out = *meta;
    out.payload_size = 0;
    out.checksum = 0;
    out.addr = EMPTY_ADDR;  // Leaf metadta stores address of the previous node!
    out.type = NBTreeBlockType::LEAF;
    return AKU_SUCCESS;
}

aku_Status init_subtree_from_subtree(const IOVecSuperblock& node, SubtreeRef& backref) {
    std::vector<SubtreeRef> refs;
    aku_Status status = node.read_all(&refs);
    if (status != AKU_SUCCESS) {
        return status;
    }
    backref.begin = refs.front().begin;
    backref.end = refs.back().end;
    backref.first = refs.front().first;
    backref.last = refs.back().last;
    backref.count = 0;
    backref.sum = 0;

    double min = std::numeric_limits<double>::max();
    double max = std::numeric_limits<double>::lowest();
    aku_Timestamp mints = 0;
    aku_Timestamp maxts = 0;
    for (const SubtreeRef& sref: refs) {
        backref.count += sref.count;
        backref.sum   += sref.sum;
        if (min > sref.min) {
            min = sref.min;
            mints = sref.min_time;
        }
        if (max < sref.max) {
            max = sref.max;
            maxts = sref.max_time;
        }
    }
    backref.min = min;
    backref.max = max;
    backref.min_time = mints;
    backref.max_time = maxts;

    // Node level information
    backref.id = node.get_id();
    backref.level = node.get_level();
    backref.type = NBTreeBlockType::INNER;
    backref.version = AKUMULI_VERSION;
    backref.fanout_index = node.get_fanout();
    backref.payload_size = 0;
    return AKU_SUCCESS;
}


/** QueryOperator implementation for leaf node.
  * This is very basic. All node's data is copied to
  * the internal buffer by c-tor.
  */
struct NBTreeLeafIterator : RealValuedOperator {

    //! Starting timestamp
    aku_Timestamp              begin_;
    //! Final timestamp
    aku_Timestamp              end_;
    //! Timestamps
    std::vector<aku_Timestamp> tsbuf_;
    //! Values
    std::vector<double>        xsbuf_;
    //! Range begin
    ssize_t                    from_;
    //! Range end
    ssize_t                    to_;
    //! Status of the iterator initialization process
    aku_Status                 status_;
    //! Padding
    u32 pad_;

    NBTreeLeafIterator(aku_Status status)
        : begin_()
        , end_()
        , from_()
        , to_()
        , status_(status)
    {
    }

    template<class LeafT>
    NBTreeLeafIterator(aku_Timestamp begin, aku_Timestamp end, LeafT const& node, bool delay_init=false)
        : begin_(begin)
        , end_(end)
        , from_()
        , to_()
        , status_(AKU_ENO_DATA)
    {
        if (!delay_init) {
            init(node);
        }
    }

    template<class LeafT>
    void init(LeafT const& node) {
        aku_Timestamp min = std::min(begin_, end_);
        aku_Timestamp max = std::max(begin_, end_);
        aku_Timestamp nb, ne;
        std::tie(nb, ne) = node.get_timestamps();
        if (max < nb || ne < min) {
            status_ = AKU_ENO_DATA;
            return;
        }
        status_ = node.read_all(&tsbuf_, &xsbuf_);
        if (status_ == AKU_SUCCESS) {
            if (begin_ < end_) {
                // FWD direction
                auto it_begin = std::lower_bound(tsbuf_.begin(), tsbuf_.end(), begin_);
                if (it_begin != tsbuf_.end()) {
                    from_ = std::distance(tsbuf_.begin(), it_begin);
                } else {
                    from_ = 0;
                    assert(tsbuf_.front() > begin_);
                }
                auto it_end = std::lower_bound(tsbuf_.begin(), tsbuf_.end(), end_);
                to_ = std::distance(tsbuf_.begin(), it_end);
            } else {
                // BWD direction
                auto it_begin = std::upper_bound(tsbuf_.begin(), tsbuf_.end(), begin_);
                from_ = std::distance(it_begin, tsbuf_.end());

                auto it_end = std::upper_bound(tsbuf_.begin(), tsbuf_.end(), end_);
                to_ = std::distance(it_end, tsbuf_.end());
                std::reverse(tsbuf_.begin(), tsbuf_.end());
                std::reverse(xsbuf_.begin(), xsbuf_.end());
            }
        }
    }

    size_t get_size() const {
        assert(to_ >= from_);
        return static_cast<size_t>(to_ - from_);
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, double *destval, size_t size);
    virtual Direction get_direction();
};


std::tuple<aku_Status, size_t> NBTreeLeafIterator::read(aku_Timestamp *destts, double *destval, size_t size) {
    ssize_t sz = static_cast<ssize_t>(size);
    if (status_ != AKU_SUCCESS) {
        return std::make_tuple(status_, 0);
    }
    ssize_t toread = to_ - from_;
    if (toread > sz) {
        toread = sz;
    }
    if (toread == 0) {
        return std::make_tuple(AKU_ENO_DATA, 0);
    }
    auto begin = from_;
    ssize_t end = from_ + toread;
    std::copy(tsbuf_.begin() + begin, tsbuf_.begin() + end, destts);
    std::copy(xsbuf_.begin() + begin, xsbuf_.begin() + end, destval);
    from_ += toread;
    return std::make_tuple(AKU_SUCCESS, toread);
}

RealValuedOperator::Direction NBTreeLeafIterator::get_direction() {
    if (begin_ < end_) {
        return Direction::FORWARD;
    }
    return Direction::BACKWARD;
}


// ////////////////////// //
// class NBTreeLeafFilter //
// ////////////////////// //

/** Filtering operator for the leaf node.
  * Returns all data-points that match the ValueFilter
  */
struct NBTreeLeafFilter : RealValuedOperator {

    //! Starting timestamp
    aku_Timestamp              begin_;
    //! Final timestamp
    aku_Timestamp              end_;
    //! Timestamps
    std::vector<aku_Timestamp> tsbuf_;
    //! Values
    std::vector<double>        xsbuf_;
    //! Status of the iterator initialization process
    aku_Status                 status_;
    //! ValueFilter
    ValueFilter                filter_;
    //! Read cursor position
    size_t                     pos_;

    NBTreeLeafFilter(aku_Status status)
        : begin_()
        , end_()
        , status_(status)
        , pos_()
    {
    }

    template<class LeafT>
    NBTreeLeafFilter(aku_Timestamp begin,
                     aku_Timestamp end,
                     const ValueFilter& filter,
                     const LeafT& node,
                     bool delay_init=false)
        : begin_(begin)
        , end_(end)
        , status_(AKU_ENO_DATA)
        , filter_(filter)
        , pos_()
    {
        if (!delay_init) {
            init(node);
        }
    }

    template<class LeafT>
    void init(LeafT const& node) {
        aku_Timestamp min = std::min(begin_, end_);
        aku_Timestamp max = std::max(begin_, end_);
        aku_Timestamp nb, ne;
        std::tie(nb, ne) = node.get_timestamps();
        if (max < nb || ne < min) {
            status_ = AKU_ENO_DATA;
            return;
        }
        std::vector<aku_Timestamp> tss;
        std::vector<double>        xss;
        status_ = node.read_all(&tss, &xss);
        ssize_t from = 0, to = 0;
        if (status_ == AKU_SUCCESS) {
            if (begin_ < end_) {
                // FWD direction
                auto it_begin = std::lower_bound(tss.begin(), tss.end(), begin_);
                if (it_begin != tss.end()) {
                    from = std::distance(tss.begin(), it_begin);
                } else {
                    from = 0;
                    assert(tss.front() > begin_);
                }

                auto it_end = std::lower_bound(tss.begin(), tss.end(), end_);
                to = std::distance(tss.begin(), it_end);

                for (ssize_t ix = from; ix < to; ix++){
                    if (filter_.match(xss[ix])) {
                        tsbuf_.push_back(tss[ix]);
                        xsbuf_.push_back(xss[ix]);
                    }
                }
            } else {
                // BWD direction
                auto it_begin = std::lower_bound(tss.begin(), tss.end(), begin_);
                if (it_begin != tss.end()) {
                    from = std::distance(tss.begin(), it_begin);
                }
                else {
                    from = tss.size() - 1;
                }

                auto it_end = std::upper_bound(tss.begin(), tss.end(), end_);
                to = std::distance(tss.begin(), it_end);

                for (ssize_t ix = from; ix >= to; ix--){
                    if (filter_.match(xss[ix])) {
                        tsbuf_.push_back(tss[ix]);
                        xsbuf_.push_back(xss[ix]);
                    }
                }
            }
        }
    }

    size_t get_size() const {
        return static_cast<size_t>(tsbuf_.size());
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, double *destval, size_t size);
    virtual Direction get_direction();
};


std::tuple<aku_Status, size_t> NBTreeLeafFilter::read(aku_Timestamp *destts, double *destval, size_t size) {
    ssize_t sz = static_cast<ssize_t>(size);
    if (status_ != AKU_SUCCESS) {
        return std::make_tuple(status_, 0);
    }
    ssize_t toread = tsbuf_.size() - pos_;
    if (toread > sz) {
        toread = sz;
    }
    if (toread == 0) {
        return std::make_tuple(AKU_ENO_DATA, 0);
    }
    auto begin = pos_;
    ssize_t end = pos_ + toread;
    std::copy(tsbuf_.begin() + begin, tsbuf_.begin() + end, destts);
    std::copy(xsbuf_.begin() + begin, xsbuf_.begin() + end, destval);
    pos_ += toread;
    return std::make_tuple(AKU_SUCCESS, toread);
}

RealValuedOperator::Direction NBTreeLeafFilter::get_direction() {
    if (begin_ < end_) {
        return Direction::FORWARD;
    }
    return Direction::BACKWARD;
}


// ///////////////////////// //
//    Superblock Iterator    //
// ///////////////////////// //

//! Return true if referenced subtree in [begin, end) range.
//! @note Begin should be less then end.
static bool subtree_in_range(SubtreeRef const& ref, aku_Timestamp begin, aku_Timestamp end) {
    if (ref.end < begin || end < ref.begin) {
        return false;
    }
    return true;
}

template<class TVal>
struct NBTreeSBlockIteratorBase : SeriesOperator<TVal> {
    //! Starting timestamp
    aku_Timestamp              begin_;
    //! Final timestamp
    aku_Timestamp              end_;
    //! Address of the current superblock
    LogicAddr addr_;
    //! Blockstore
    std::shared_ptr<BlockStore> bstore_;

    // FSM
    std::vector<SubtreeRef> refs_;
    std::unique_ptr<SeriesOperator<TVal>> iter_;
    u32 fsm_pos_;
    i32 refs_pos_;

    typedef std::unique_ptr<SeriesOperator<TVal>> TIter;
    typedef typename SeriesOperator<TVal>::Direction Direction;

    NBTreeSBlockIteratorBase(std::shared_ptr<BlockStore> bstore, LogicAddr addr, aku_Timestamp begin, aku_Timestamp end)
        : begin_(begin)
        , end_(end)
        , addr_(addr)
        , bstore_(bstore)
        , fsm_pos_(0)
        , refs_pos_(0)
    {
    }

    template<class SuperblockT>
    NBTreeSBlockIteratorBase(std::shared_ptr<BlockStore> bstore, SuperblockT const& sblock, aku_Timestamp begin, aku_Timestamp end)
        : begin_(begin)
        , end_(end)
        , addr_(EMPTY_ADDR)
        , bstore_(bstore)
        , fsm_pos_(1)  // FSM will bypass `init` step.
        , refs_pos_(0)
    {
        aku_Status status = sblock.read_all(&refs_);
        if (status != AKU_SUCCESS) {
            // `read` call should fail with AKU_ENO_DATA error.
            refs_pos_ = begin_ < end_ ? static_cast<i32>(refs_.size()) : -1;
        } else {
            refs_pos_ = begin_ < end_ ? 0 : static_cast<i32>(refs_.size()) - 1;
        }
    }

    aku_Status init() {
        aku_Status status;
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore_, addr_);
        if (status != AKU_SUCCESS) {
            return status;
        }
        IOVecSuperblock current(std::move(block));
        status = current.read_all(&refs_);
        refs_pos_ = begin_ < end_ ? 0 : static_cast<i32>(refs_.size()) - 1;
        return status;
    }

    //! Create leaf iterator (used by `get_next_iter` template method).
    virtual std::tuple<aku_Status, TIter> make_leaf_iterator(const SubtreeRef &ref) = 0;

    //! Create superblock iterator (used by `get_next_iter` template method).
    virtual std::tuple<aku_Status, TIter> make_superblock_iterator(const SubtreeRef &ref) = 0;

    //! This is a template method, aggregator should derive from this object and
    //! override make_*_iterator virtual methods to customize iterator's behavior.
    std::tuple<aku_Status, TIter> get_next_iter() {
        auto min = std::min(begin_, end_);
        auto max = std::max(begin_, end_);

        TIter empty;
        SubtreeRef ref = INIT_SUBTREE_REF;
        if (get_direction() == Direction::FORWARD) {
            if (refs_pos_ == static_cast<i32>(refs_.size())) {
                // Done
                return std::make_tuple(AKU_ENO_DATA, std::move(empty));
            }
            ref = refs_.at(static_cast<size_t>(refs_pos_));
            refs_pos_++;
        } else {
            if (refs_pos_ < 0) {
                // Done
                return std::make_tuple(AKU_ENO_DATA, std::move(empty));
            }
            ref = refs_.at(static_cast<size_t>(refs_pos_));
            refs_pos_--;
        }
        std::tuple<aku_Status, TIter> result;
        if (!bstore_->exists(ref.addr)) {
            return std::make_tuple(AKU_EUNAVAILABLE, std::move(empty));
        }
        if (!subtree_in_range(ref, min, max)) {
            // Subtree not in [begin_, end_) range. Proceed to next.
            result = std::make_tuple(AKU_ENOT_FOUND, std::move(empty));
        } else if (ref.type == NBTreeBlockType::LEAF) {
            result = std::move(make_leaf_iterator(ref));
        } else {
            result = std::move(make_superblock_iterator(ref));
        }
        return std::move(result);
    }

    //! Iteration implementation. Can be customized in derived classes.
    std::tuple<aku_Status, size_t> iter(aku_Timestamp *destts, TVal *destval, size_t size) {
        // Main loop, draw data from iterator till out array become empty.
        size_t out_size = 0;
        aku_Status status = AKU_ENO_DATA;
        while(out_size < size) {
            if (!iter_) {
                // initialize `iter_`
                std::tie(status, iter_) = get_next_iter();
                if (status == AKU_ENOT_FOUND || status == AKU_EUNAVAILABLE) {
                    // Subtree exists but doesn't contains values from begin-end timerange or
                    // entire subtree was deleted
                    Logger::msg(AKU_LOG_TRACE, "Can't open next iterator because " + StatusUtil::str(status));
                    continue;
                } else if (status != AKU_SUCCESS) {
                    // We're out of iterators and should stop.
                    break;
                }
            }
            size_t sz;
            std::tie(status, sz) = iter_->read(destts + out_size, destval + out_size, size - out_size);
            out_size += sz;
            if (status == AKU_ENO_DATA ||
               (status == AKU_EUNAVAILABLE && get_direction() == Direction::FORWARD)) {
                // Move to next iterator.
                iter_.reset();
            } else if (status != AKU_SUCCESS) {
                // Unexpected error, can't proceed.
                break;
            }
        }
        return std::make_tuple(status, out_size);
    }

    virtual Direction get_direction() {
        if (begin_ < end_) {
            return Direction::FORWARD;
        }
        return Direction::BACKWARD;
    }
};


// ////////////////////////// //
// class NBTreeSBlockIterator //
// ////////////////////////// //

struct NBTreeSBlockIterator : NBTreeSBlockIteratorBase<double> {

    NBTreeSBlockIterator(std::shared_ptr<BlockStore> bstore, LogicAddr addr, aku_Timestamp begin, aku_Timestamp end)
        : NBTreeSBlockIteratorBase<double>(bstore, addr, begin, end)
    {
    }

    template<class SuperblockT>
    NBTreeSBlockIterator(std::shared_ptr<BlockStore> bstore, SuperblockT const& sblock, aku_Timestamp begin, aku_Timestamp end)
        : NBTreeSBlockIteratorBase<double>(bstore, sblock, begin, end)
    {
    }

    //! Create leaf iterator (used by `get_next_iter` template method).
    virtual std::tuple<aku_Status, TIter> make_leaf_iterator(const SubtreeRef &ref) {
        assert(ref.type == NBTreeBlockType::LEAF);
        aku_Status status;
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore_, ref.addr);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, std::unique_ptr<RealValuedOperator>());
        }
        auto blockref = block->get_cheader<SubtreeRef>();
        assert(blockref->type == ref.type);
        AKU_UNUSED(blockref);
        IOVecLeaf leaf(std::move(block));
        std::unique_ptr<RealValuedOperator> result;
        result.reset(new NBTreeLeafIterator(begin_, end_, leaf));
        return std::make_tuple(AKU_SUCCESS, std::move(result));
    }

    //! Create superblock iterator (used by `get_next_iter` template method).
    virtual std::tuple<aku_Status, TIter> make_superblock_iterator(const SubtreeRef &ref) {
        TIter result;
        result.reset(new NBTreeSBlockIterator(bstore_, ref.addr, begin_, end_));
        return std::make_tuple(AKU_SUCCESS, std::move(result));
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, double *destval, size_t size) {
        if (!fsm_pos_ ) {
            aku_Status status = AKU_SUCCESS;
            status = init();
            if (status != AKU_SUCCESS) {
                return std::make_pair(status, 0ul);
            }
            fsm_pos_++;
        }
        return iter(destts, destval, size);
    }
};


// /////////////////// //
// class EmptyIterator //
// /////////////////// //

struct EmptyIterator : RealValuedOperator {

    //! Starting timestamp
    aku_Timestamp              begin_;
    //! Final timestamp
    aku_Timestamp              end_;

    EmptyIterator(aku_Timestamp begin, aku_Timestamp end)
        : begin_(begin)
        , end_(end)
    {
    }

    size_t get_size() const {
        return 0;
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, double *destval, size_t size) {
        return std::make_tuple(AKU_ENO_DATA, 0);
    }

    virtual Direction get_direction() {
        if (begin_ < end_) {
            return Direction::FORWARD;
        }
        return Direction::BACKWARD;
    }
};

// //////////////////////// //
// class NBTreeSBlockFilter //
// //////////////////////// //


struct NBTreeSBlockFilter : NBTreeSBlockIteratorBase<double> {

    ValueFilter filter_;

    NBTreeSBlockFilter(std::shared_ptr<BlockStore> bstore,
                       LogicAddr addr,
                       aku_Timestamp begin,
                       aku_Timestamp end,
                       const ValueFilter& filter)
        : NBTreeSBlockIteratorBase<double>(bstore, addr, begin, end)
        , filter_(filter)
    {
    }

    template<class SuperblockT>
    NBTreeSBlockFilter(std::shared_ptr<BlockStore> bstore,
                       SuperblockT const& sblock,
                       aku_Timestamp begin,
                       aku_Timestamp end,
                       const ValueFilter& filter)
        : NBTreeSBlockIteratorBase<double>(bstore, sblock, begin, end)
        , filter_(filter)
    {
    }

    //! Create leaf iterator (used by `get_next_iter` template method).
    virtual std::tuple<aku_Status, TIter> make_leaf_iterator(const SubtreeRef &ref) {
        assert(ref.type == NBTreeBlockType::LEAF);
        aku_Status status;
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore_, ref.addr);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, std::unique_ptr<RealValuedOperator>());
        }
        auto blockref = block->get_cheader<SubtreeRef>();
        assert(blockref->type == ref.type);
        std::unique_ptr<RealValuedOperator> result;
        switch (filter_.get_overlap(*blockref)) {
        case RangeOverlap::FULL_OVERLAP: {
            // Return normal leaf iterator because it's faster
            IOVecLeaf leaf(std::move(block));
            result.reset(new NBTreeLeafIterator(begin_, end_, leaf));
            break;
        }
        case RangeOverlap::PARTIAL_OVERLAP: {
            // Return filtering leaf operator
            IOVecLeaf leaf(std::move(block));
            result.reset(new NBTreeLeafFilter(begin_, end_, filter_, leaf));
            break;
        }
        case RangeOverlap::NO_OVERLAP: {
            // There is no data that can pass the filter so just return an empty iterator
            result.reset(new EmptyIterator(begin_, end_));
            break;
        }
        };
        return std::make_tuple(AKU_SUCCESS, std::move(result));
    }

    //! Create superblock iterator (used by `get_next_iter` template method).
    virtual std::tuple<aku_Status, TIter> make_superblock_iterator(const SubtreeRef &ref) {
        auto overlap = filter_.get_overlap(ref);
        TIter result;
        switch(overlap) {
        case RangeOverlap::FULL_OVERLAP:
            // Return normal superblock iterator
            result.reset(new NBTreeSBlockIterator(bstore_, ref.addr, begin_, end_));
            break;
        case RangeOverlap::PARTIAL_OVERLAP:
            // Return filter
            result.reset(new NBTreeSBlockFilter(bstore_, ref.addr, begin_, end_, filter_));
            break;
        case RangeOverlap::NO_OVERLAP:
            // Return dummy
            result.reset(new EmptyIterator(begin_, end_));
            break;
        }
        return std::make_tuple(AKU_SUCCESS, std::move(result));
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, double *destval, size_t size) {
        if (!fsm_pos_ ) {
            aku_Status status = AKU_SUCCESS;
            status = init();
            if (status != AKU_SUCCESS) {
                return std::make_pair(status, 0ul);
            }
            fsm_pos_++;
        }
        return iter(destts, destval, size);
    }
};

// //////////////////// //
// NBTreeLeafAggregator //
// //////////////////// //

class NBTreeLeafAggregator : public AggregateOperator {
    NBTreeLeafIterator iter_;
    bool enable_cached_metadata_;
    SubtreeRef metacache_;
public:
    template<class LeafT>
    NBTreeLeafAggregator(aku_Timestamp begin, aku_Timestamp end, LeafT const& node)
        : iter_(begin, end, node, true)
        , enable_cached_metadata_(false)
        , metacache_(INIT_SUBTREE_REF)
    {
        aku_Timestamp nodemin, nodemax, min, max;
        std::tie(nodemin, nodemax) = node.get_timestamps();
        min = std::min(begin, end);
        max = std::max(begin, end);
        if (min <= nodemin && nodemax < max) {
            // Leaf totally inside the search range, we can use metadata.
            metacache_ = *node.get_leafmeta();
            enable_cached_metadata_ = true;
        } else {
            // Otherwise we need to compute aggregate from subset of leaf's values.
            iter_.init(node);
        }
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, AggregationResult *destxs, size_t size);
    virtual Direction get_direction();
};

NBTreeLeafAggregator::Direction NBTreeLeafAggregator::get_direction() {
    return iter_.get_direction() == NBTreeLeafIterator::Direction::FORWARD ? Direction::FORWARD : Direction::BACKWARD;
}

std::tuple<aku_Status, size_t> NBTreeLeafAggregator::read(aku_Timestamp *destts, AggregationResult *destxs, size_t size) {
    aku_Timestamp outts = 0;
    AggregationResult outval = INIT_AGGRES;
    if (size == 0) {
        return std::make_tuple(AKU_EBAD_ARG, 0);
    }
    if (enable_cached_metadata_) {
        // Fast path. Use metadata to compute results.
        outval.copy_from(metacache_);
        outts = metacache_.begin;
        enable_cached_metadata_ = false;
        // next call to `read` should return AKU_ENO_DATA
    } else {
        if (!iter_.get_size()) {
            return std::make_tuple(AKU_ENO_DATA, 0);
        }
        size_t size_hint = iter_.get_size();
        std::vector<double> xs(size_hint, .0);
        std::vector<aku_Timestamp> ts(size_hint, 0);
        aku_Status status;
        size_t out_size;
        std::tie(status, out_size) = iter_.read(ts.data(), xs.data(), size_hint);
        if (status != AKU_SUCCESS) {
            return std::tie(status, out_size);
        }
        if (out_size == 0) {
            return std::make_tuple(AKU_ENO_DATA, 0);
        }
        assert(out_size == size_hint);
        bool inverted = iter_.get_direction() == NBTreeLeafIterator::Direction::BACKWARD;
        outval.do_the_math(ts.data(), xs.data(), out_size, inverted);
        outts = ts.front();  // INVARIANT: ts.size() is gt 0, destts(xs) size is gt 0
    }
    destts[0] = outts;
    destxs[0] = outval;
    return std::make_tuple(AKU_SUCCESS, 1);
}


// ////////////////////// //
// NBTreeSBlockAggregator //
// ////////////////////// //

/** Aggregator that returns precomputed value.
  * Value should be set in c-tor.
  */
class ValueAggregator : public AggregateOperator {
    aku_Timestamp ts_;
    AggregationResult value_;
    Direction dir_;
    bool used_;
public:
    ValueAggregator(aku_Timestamp ts, AggregationResult value, Direction dir)
        : ts_(ts)
        , value_(value)
        , dir_(dir)
        , used_(false)
    {
    }

    ValueAggregator()
        : ts_()
        , value_()
        , dir_()
        , used_(true)
    {}

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, AggregationResult *destval, size_t size) override;
    virtual Direction get_direction() override;
};

std::tuple<aku_Status, size_t> ValueAggregator::read(aku_Timestamp *destts, AggregationResult *destval, size_t size) {
    if (size == 0) {
        return std::make_pair(AKU_EBAD_ARG, 0);
    }
    if (used_) {
        return std::make_pair(AKU_ENO_DATA, 0);
    }
    used_ = true;
    destval[0] = value_;
    destts[0] = ts_;
    return std::make_pair(AKU_SUCCESS, 1);
}

ValueAggregator::Direction ValueAggregator::get_direction() {
    return dir_;
}

/** Superblock aggregator (iterator that computes different aggregates e.g. min/max/avg/sum).
  * Uses metadata stored in superblocks in some cases.
  */
class NBTreeSBlockAggregator : public NBTreeSBlockIteratorBase<AggregationResult> {

    bool leftmost_leaf_found_;

public:
    template<class SuperblockT>
    NBTreeSBlockAggregator(std::shared_ptr<BlockStore> bstore,
                           SuperblockT const& sblock,
                           aku_Timestamp begin,
                           aku_Timestamp end)
        : NBTreeSBlockIteratorBase<AggregationResult>(bstore, sblock, begin, end)
        , leftmost_leaf_found_(false)
    {
    }

    NBTreeSBlockAggregator(std::shared_ptr<BlockStore> bstore,
                           LogicAddr addr,
                           aku_Timestamp begin,
                           aku_Timestamp end)
        : NBTreeSBlockIteratorBase<AggregationResult>(bstore, addr, begin, end)
        , leftmost_leaf_found_(false)
    {
    }
    virtual std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> make_leaf_iterator(const SubtreeRef &ref) override;
    virtual std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> make_superblock_iterator(const SubtreeRef &ref) override;
    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, AggregationResult *destval, size_t size) override;
};

std::tuple<aku_Status, size_t> NBTreeSBlockAggregator::read(aku_Timestamp *destts, AggregationResult *destval, size_t size) {
    if (size == 0) {
        return std::make_pair(AKU_EBAD_ARG, 0ul);
    }
    if (!fsm_pos_ ) {
        aku_Status status = AKU_SUCCESS;
        status = init();
        if (status != AKU_SUCCESS) {
            return std::make_pair(status, 0ul);
        }
        fsm_pos_++;
    }
    size_t SZBUF = 1024;
    std::vector<AggregationResult> xss(SZBUF, INIT_AGGRES);
    std::vector<aku_Timestamp> tss(SZBUF, 0);
    aku_Timestamp outts = 0;
    AggregationResult outxs = INIT_AGGRES;
    ssize_t outsz = 0;
    aku_Status status;
    int nagg = 0;
    while(true) {
        std::tie(status, outsz) = iter(tss.data(), xss.data(), SZBUF);
        if ((status == AKU_SUCCESS || status == AKU_ENO_DATA) && outsz != 0) {
            outts = tss[static_cast<size_t>(outsz)];
            outxs = std::accumulate(xss.begin(), xss.begin() + outsz, outxs,
                        [&](AggregationResult lhs, AggregationResult rhs) {
                            lhs.combine(rhs);
                            return lhs;
                        });
            size = 1;
            nagg++;
        } else if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
            size = 0;
            break;
        } else if (outsz == 0) {
            if (nagg) {
                destval[0] = outxs;
                destts[0] = outts;
                size = 1;
            } else {
                size = 0;
            }
            break;
        }
    }
    return std::make_tuple(status, size);
}

std::tuple<aku_Status, std::unique_ptr<AggregateOperator> > NBTreeSBlockAggregator::make_leaf_iterator(SubtreeRef const& ref) {
    if (!bstore_->exists(ref.addr)) {
        TIter empty;
        return std::make_tuple(AKU_EUNAVAILABLE, std::move(empty));
    }
    aku_Status status;
    std::unique_ptr<IOVecBlock> block;
    std::tie(status, block) = read_and_check(bstore_, ref.addr);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, std::unique_ptr<AggregateOperator>());
    }
    leftmost_leaf_found_ = true;
    IOVecLeaf leaf(std::move(block));
    std::unique_ptr<AggregateOperator> result;
    result.reset(new NBTreeLeafAggregator(begin_, end_, leaf));
    return std::make_tuple(AKU_SUCCESS, std::move(result));
}

std::tuple<aku_Status, std::unique_ptr<AggregateOperator> > NBTreeSBlockAggregator::make_superblock_iterator(SubtreeRef const& ref) {
    if (!bstore_->exists(ref.addr)) {
        TIter empty;
        return std::make_tuple(AKU_EUNAVAILABLE, std::move(empty));
    }
    aku_Timestamp min = std::min(begin_, end_);
    aku_Timestamp max = std::max(begin_, end_);
    std::unique_ptr<AggregateOperator> result;
    if (leftmost_leaf_found_ && (min <= ref.begin && ref.end < max)) {
        // We don't need to go to lower level, value from subtree ref can be used instead.
        auto agg = INIT_AGGRES;
        agg.copy_from(ref);
        result.reset(new ValueAggregator(ref.end, agg, get_direction()));
    } else {
        result.reset(new NBTreeSBlockAggregator(bstore_, ref.addr, begin_, end_));
    }
    return std::make_tuple(AKU_SUCCESS, std::move(result));
}


// ///////////////////////// //
// NBTreeLeafGroupAggregator //
// ///////////////////////// //

class NBTreeLeafGroupAggregator : public AggregateOperator {
    NBTreeLeafIterator iter_;
    bool enable_cached_metadata_;
    SubtreeRef metacache_;
    aku_Timestamp begin_;
    aku_Timestamp end_;
    aku_Timestamp step_;
public:
    template<class LeafT>
    NBTreeLeafGroupAggregator(aku_Timestamp begin, aku_Timestamp end, u64 step, LeafT const& node)
        : iter_(begin, end, node, true)
        , enable_cached_metadata_(false)
        , metacache_(INIT_SUBTREE_REF)
        , begin_(begin)
        , end_(end)
        , step_(step)
    {
        aku_Timestamp nodemin, nodemax;
        std::tie(nodemin, nodemax) = node.get_timestamps();
        if (begin < end) {
            auto a = (nodemin - begin) / step;
            auto b = (nodemax - begin) / step;
            if (a == b && nodemin >= begin && nodemax < end) {
                // Leaf totally inside one step range, we can use metadata.
                metacache_ = *node.get_leafmeta();
                enable_cached_metadata_ = true;
            } else {
                // Otherwise we need to compute aggregate from subset of leaf's values.
                iter_.init(node);
            }
        } else {
            auto a = (begin - nodemin) / step;
            auto b = (begin - nodemax) / step;
            if (a == b && nodemax <= begin && nodemin > end) {
                // Leaf totally inside one step range, we can use metadata.
                metacache_ = *node.get_leafmeta();
                enable_cached_metadata_ = true;
            } else {
                // Otherwise we need to compute aggregate from subset of leaf's values.
                iter_.init(node);
            }
        }
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, AggregationResult *destxs, size_t size);
    virtual Direction get_direction();
};

NBTreeLeafGroupAggregator::Direction NBTreeLeafGroupAggregator::get_direction() {
    return iter_.get_direction() == NBTreeLeafIterator::Direction::FORWARD ? Direction::FORWARD : Direction::BACKWARD;
}

std::tuple<aku_Status, size_t> NBTreeLeafGroupAggregator::read(aku_Timestamp *destts, AggregationResult *destxs, size_t size) {
    size_t outix = 0;
    if (size == 0) {
        return std::make_tuple(AKU_EBAD_ARG, 0);
    }
    if (enable_cached_metadata_) {
        if (metacache_.count == 0) {
            return std::make_tuple(AKU_ENO_DATA, 0);
        }
        // Fast path. Use metadata to compute results.
        destts[0] = metacache_.begin;
        destxs[0].copy_from(metacache_);
        auto delta = destxs[0]._end - destxs[0]._begin;
        if (delta > step_) {
            assert(delta <= step_);
        }
        enable_cached_metadata_ = false;  // next call to `read` should return AKU_ENO_DATA
        return std::make_tuple(AKU_SUCCESS, 1);
    } else {
        if (!iter_.get_size()) {
            // Second call to read will lead here if fast path have been taken on first call.
            return std::make_tuple(AKU_ENO_DATA, 0);
        }
        size_t size_hint = std::min(iter_.get_size(), size);
        std::vector<double> xs(size_hint, .0);
        std::vector<aku_Timestamp> ts(size_hint, 0);
        aku_Status status;
        size_t out_size;
        std::tie(status, out_size) = iter_.read(ts.data(), xs.data(), size_hint);
        if (status != AKU_SUCCESS) {
            return std::tie(status, out_size);
        }
        if (out_size == 0) {
            return std::make_tuple(AKU_ENO_DATA, 0);
        }
        assert(out_size == size_hint);
        int valcnt = 0;
        AggregationResult outval = INIT_AGGRES;
        const bool forward = begin_ < end_;
        u64 bin = 0;
        for (size_t ix = 0; ix < out_size; ix++) {
            aku_Timestamp normts = forward ? ts[ix] - begin_
                                           : begin_ - ts[ix];
            if (valcnt == 0) {
                bin = normts / step_;
            } else if (normts / step_ != bin) {
                bin = normts / step_;
                destxs[outix] = outval;
                destts[outix] = outval._begin;
                outix++;
                outval = INIT_AGGRES;
            }
            valcnt++;
            outval.add(ts[ix], xs[ix], forward);
            // Check invariant
            auto delta = outval._end - outval._begin;
            if (delta > step_) {
                assert(delta <= step_);
            }
        }
        if (outval.cnt > 0) {
            destxs[outix] = outval;
            destts[outix] = outval._begin;
            outix++;
        }
    }
    assert(outix <= size);
    return std::make_tuple(AKU_SUCCESS, outix);
}


//

/** Superblock aggregator (iterator that computes different aggregates e.g. min/max/avg/sum).
  * Uses metadata stored in superblocks in some cases.
  */
class NBTreeSBlockGroupAggregator : public NBTreeSBlockIteratorBase<AggregationResult> {
    typedef std::vector<AggregationResult> ReadBuffer;
    u64 step_;
    ReadBuffer rdbuf_;
    u32 rdpos_;
    bool done_;
    enum {
        RDBUF_SIZE = 0x100
    };
public:
    template<class SuperblockT>
    NBTreeSBlockGroupAggregator(std::shared_ptr<BlockStore> bstore,
                                SuperblockT const& sblock,
                                aku_Timestamp begin,
                                aku_Timestamp end,
                                u64 step)
        : NBTreeSBlockIteratorBase<AggregationResult>(bstore, sblock, begin, end)
        , step_(step)
        , rdpos_(0)
        , done_(false)
    {
    }

    NBTreeSBlockGroupAggregator(std::shared_ptr<BlockStore> bstore,
                                LogicAddr addr,
                                aku_Timestamp begin,
                                aku_Timestamp end,
                                u64 step)
        : NBTreeSBlockIteratorBase<AggregationResult>(bstore, addr, begin, end)
        , step_(step)
        , rdpos_(0)
        , done_(false)
    {
    }

    //! Return true if `rdbuf_` is not empty and have some data to read.
    bool can_read() const {
        return rdpos_ < rdbuf_.size();
    }

    //! Return number of elements in rdbuf_ available for reading
    u32 elements_in_rdbuf() const {
        return static_cast<u32>(rdbuf_.size()) - rdpos_;  // Safe to cast because rdbuf_.size() <= RDBUF_SIZE
    }

    /**
     * @brief Copy as much elements as possible to the dest arrays.
     * @param desttx timestamps array
     * @param destxs values array
     * @param size size of both arrays
     * @return number of elements copied
     */
    std::tuple<aku_Status, size_t> copy_to(aku_Timestamp* desttx, AggregationResult* destxs, size_t size) {
        aku_Status status = AKU_SUCCESS;
        size_t copied = 0;
        while (status == AKU_SUCCESS && size > 0) {
            size_t n = elements_in_rdbuf();
            if (!done_) {
                if (n < 2) {
                    status = refill_read_buffer();
                    if (status == AKU_ENO_DATA && can_read()) {
                        status = AKU_SUCCESS;
                    }
                    continue;
                }
                // We can copy last element of the rdbuf_ to the output only if all
                // iterators were consumed! Otherwise invariant will be broken.
                n--;
            } else {
                if (n == 0) {
                    status = AKU_ENO_DATA;
                    break;
                }
            }
            //
            // Copy elements
            auto tocopy = std::min(n, size);
            for (size_t i = 0; i < tocopy; i++) {
                auto const& bottom = rdbuf_.at(rdpos_);
                rdpos_++;
                *desttx++ = bottom._begin;
                *destxs++ = bottom;
                size--;
            }
            copied += tocopy;
        }
        return std::make_tuple(status, copied);
    }

    /**
     * @brief Refils read buffer.
     * @return AKU_SUCCESS on success, AKU_ENO_DATA if there is no more data to read, error code on error
     */
    aku_Status refill_read_buffer() {
        aku_Status status = AKU_ENO_DATA;
        u32 pos_ = 0;

        if (!rdbuf_.empty()) {
            auto tail = rdbuf_.back();  // the last element should be saved because it is possible that
                                        // it's not full (part of the range contained in first iterator
                                        // and another part in second iterator or even in more than one
                                        // iterators).
            rdbuf_.clear();
            rdbuf_.resize(RDBUF_SIZE, INIT_AGGRES);
            rdpos_ = 0;
            rdbuf_.at(0) = tail;
            pos_ = 1;
        } else {
            rdbuf_.clear();
            rdbuf_.resize(RDBUF_SIZE, INIT_AGGRES);
            rdpos_ = 0;
        }

        while(true) {
            if (!iter_) {
                std::tie(status, iter_) = get_next_iter();
                if (status == AKU_ENOT_FOUND || status == AKU_EUNAVAILABLE) {
                    // Subtree exists but doesn't contains values from begin-end timerange or
                    // entire subtree was deleted
                    Logger::msg(AKU_LOG_TRACE, "Can't open next iterator because " + StatusUtil::str(status));
                    continue;
                } else if (status != AKU_SUCCESS) {
                    // We're out of iterators and should stop.
                    done_ = true;
                    break;
                }
            }
            size_t size = rdbuf_.size() - pos_;
            if (size == 0) {
                break;
            }
            std::array<AggregationResult, RDBUF_SIZE> outxs;
            std::array<aku_Timestamp, RDBUF_SIZE>           outts;
            u32 outsz;
            std::tie(status, outsz) = iter_->read(outts.data(), outxs.data(), size);
            if (outsz != 0) {
                if (pos_ > 0) {
                    auto const& last  = rdbuf_.at(pos_ - 1);
                    auto const& first = outxs.front();
                    aku_Timestamp lastts = begin_ < end_ ? last._begin - begin_
                                                         : begin_ - last._begin;
                    aku_Timestamp firstts = begin_ < end_ ? first._begin - begin_
                                                          : begin_ - first._begin;
                    auto lastbin = lastts / step_;
                    auto firstbin = firstts / step_;

                    if (lastbin == firstbin) {
                        pos_--;
                    }
                }
                for (size_t ix = 0; ix < outsz; ix++) {
                    rdbuf_.at(pos_).combine(outxs.at(ix));
                    const auto newdelta = rdbuf_.at(pos_)._end - rdbuf_.at(pos_)._begin;
                    if (newdelta > step_) {
                        assert(newdelta <= step_);
                    }
                    pos_++;
                }
            }
            if (status == AKU_ENO_DATA) {
                iter_.reset();
                continue;
            }
            if (status != AKU_SUCCESS) {
                size = 0;
                break;
            }
        }
        rdbuf_.resize(pos_);
        return status;
    }

    virtual std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> make_leaf_iterator(const SubtreeRef &ref) override;
    virtual std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> make_superblock_iterator(const SubtreeRef &ref) override;
    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, AggregationResult *destval, size_t size) override;
};

std::tuple<aku_Status, size_t> NBTreeSBlockGroupAggregator::read(aku_Timestamp *destts,
                                                                 AggregationResult *destval,
                                                                 size_t size)
{
    if (size == 0) {
        return std::make_pair(AKU_EBAD_ARG, 0ul);
    }
    if (!fsm_pos_ ) {
        aku_Status status = init();
        if (status != AKU_SUCCESS) {
            return std::make_pair(status, 0ul);
        }
        fsm_pos_++;
    }
    return copy_to(destts, destval, size);
}

std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> NBTreeSBlockGroupAggregator::make_leaf_iterator(SubtreeRef const& ref) {
    aku_Status status;
    std::unique_ptr<IOVecBlock> block;
    std::tie(status, block) = read_and_check(bstore_, ref.addr);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, std::unique_ptr<AggregateOperator>());
    }
    IOVecLeaf leaf(std::move(block));
    std::unique_ptr<AggregateOperator> result;
    result.reset(new NBTreeLeafGroupAggregator(begin_, end_, step_, leaf));
    return std::make_tuple(AKU_SUCCESS, std::move(result));
}

std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> NBTreeSBlockGroupAggregator::make_superblock_iterator(SubtreeRef const& ref) {
    std::unique_ptr<AggregateOperator> result;
    bool inner = false;
    if (get_direction() == Direction::FORWARD) {
        auto const query_boundary = (end_ - begin_) / step_;
        auto const start_bucket = (ref.begin - begin_) / step_;
        auto const stop_bucket = (ref.end - begin_) / step_;
        if (start_bucket == stop_bucket && stop_bucket != query_boundary) {
            inner = true;
        }
    } else {
        auto const query_boundary = (begin_ - end_) / step_;
        auto const start_bucket = (begin_ - ref.end) / step_;
        auto const stop_bucket = (begin_ - ref.begin) / step_;
        if (start_bucket == stop_bucket && stop_bucket != query_boundary) {
            inner = true;
        }
    }
    if (inner) {
        // We don't need to go to lower level, value from subtree ref can be used instead.
        auto agg = INIT_AGGRES;
        agg.copy_from(ref);
        result.reset(new ValueAggregator(ref.end, agg, get_direction()));
    } else {
        result.reset(new NBTreeSBlockGroupAggregator(bstore_, ref.addr, begin_, end_, step_));
    }
    return std::make_tuple(AKU_SUCCESS, std::move(result));
}

// ///////////////////// //
// NBTreeLeafGroupFilter //
// ///////////////////// //

class NBTreeGroupAggregateFilter : public AggregateOperator {
    AggregateFilter filter_;
    std::unique_ptr<AggregateOperator> iter_;
public:
    NBTreeGroupAggregateFilter(const AggregateFilter& filter, std::unique_ptr<AggregateOperator>&& iter)
        : filter_(filter)
        , iter_(std::move(iter))
    {
    }

    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, AggregationResult *destval, size_t size) {
        // copy data to the buffer
        size_t i = 0;
        while (i < size) {
            AggregationResult agg;
            aku_Timestamp ts;
            size_t outsz;
            aku_Status status;
            std::tie(status, outsz) = iter_->read(&ts, &agg, 1);
            if (status == AKU_SUCCESS || status == AKU_ENO_DATA) {
                if (filter_.match(agg)) {
                    destts[i] = ts;
                    destval[i] = agg;
                    i++;
                }
                if (status == AKU_ENO_DATA || outsz == 0) {
                    // Stop iteration
                    break;
                }
            } else {
                // Error
                return std::make_tuple(status, 0);
            }
        }
        return std::make_tuple(AKU_SUCCESS, i);
    }

    virtual Direction get_direction() {
        return iter_->get_direction();
    }
};

// //////////////////////////// //
// NBTreeSBlockCandlesticksIter //
// //////////////////////////// //

class NBTreeSBlockCandlesticsIter : public NBTreeSBlockIteratorBase<AggregationResult> {
    NBTreeCandlestickHint hint_;
public:
    template<class SuperblockT>
    NBTreeSBlockCandlesticsIter(std::shared_ptr<BlockStore> bstore,
                                SuperblockT const& sblock,
                                aku_Timestamp begin,
                                aku_Timestamp end,
                                NBTreeCandlestickHint hint)
        : NBTreeSBlockIteratorBase<AggregationResult>(bstore, sblock, begin, end)
        , hint_(hint)
    {
    }

    NBTreeSBlockCandlesticsIter(std::shared_ptr<BlockStore> bstore,
                                LogicAddr addr,
                                aku_Timestamp begin,
                                aku_Timestamp end,
                                NBTreeCandlestickHint hint)
        : NBTreeSBlockIteratorBase<AggregationResult>(bstore, addr, begin, end)
        , hint_(hint)
    {
    }
    virtual std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> make_leaf_iterator(const SubtreeRef &ref) override;
    virtual std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> make_superblock_iterator(const SubtreeRef &ref) override;
    virtual std::tuple<aku_Status, size_t> read(aku_Timestamp *destts, AggregationResult *destval, size_t size) override;
};


std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> NBTreeSBlockCandlesticsIter::make_leaf_iterator(const SubtreeRef &ref) {
    auto agg = INIT_AGGRES;
    agg.copy_from(ref);
    std::unique_ptr<AggregateOperator> result;
    result.reset(new ValueAggregator(ref.end, agg, get_direction()));
    return std::make_tuple(AKU_SUCCESS, std::move(result));
}

std::tuple<aku_Status, std::unique_ptr<AggregateOperator>> NBTreeSBlockCandlesticsIter::make_superblock_iterator(const SubtreeRef &ref) {
    aku_Timestamp min = std::min(begin_, end_);
    aku_Timestamp max = std::max(begin_, end_);
    aku_Timestamp delta = max - min;
    std::unique_ptr<AggregateOperator> result;
    if (min < ref.begin && ref.end < max && hint_.min_delta > delta) {
        // We don't need to go to lower level, value from subtree ref can be used instead.
        auto agg = INIT_AGGRES;
        agg.copy_from(ref);
        result.reset(new ValueAggregator(ref.end, agg, get_direction()));
    } else {
        result.reset(new NBTreeSBlockCandlesticsIter(bstore_, ref.addr, begin_, end_, hint_));
    }
    return std::make_tuple(AKU_SUCCESS, std::move(result));

}

std::tuple<aku_Status, size_t> NBTreeSBlockCandlesticsIter::read(aku_Timestamp *destts, AggregationResult *destval, size_t size) {
    if (!fsm_pos_ ) {
        aku_Status status = AKU_SUCCESS;
        status = init();
        if (status != AKU_SUCCESS) {
            return std::make_pair(status, 0ul);
        }
        fsm_pos_++;
    }
    return iter(destts, destval, size);
}


// ///////// //
// IOVecLeaf //

IOVecLeaf::IOVecLeaf(aku_ParamId id, LogicAddr prev, u16 fanout_index)
    : prev_(prev)
    , block_(new IOVecBlock())
    , writer_(block_.get())
    , fanout_index_(fanout_index)
{
    // Check that invariant holds.
    SubtreeRef* subtree = block_->allocate<SubtreeRef>();
    if (subtree == nullptr) {
        AKU_PANIC("Can't allocate space in IOVecBlock");
    }
    subtree->addr = prev;
    subtree->level = 0;  // Leaf node
    subtree->type = NBTreeBlockType::LEAF;
    subtree->id = id;
    subtree->version = AKUMULI_VERSION;
    subtree->payload_size = 0;
    subtree->fanout_index = fanout_index;
    // values that should be updated by insert
    subtree->begin = std::numeric_limits<aku_Timestamp>::max();
    subtree->end = 0;
    subtree->count = 0;
    subtree->min = std::numeric_limits<double>::max();
    subtree->max = std::numeric_limits<double>::lowest();
    subtree->sum = 0;
    subtree->min_time = std::numeric_limits<aku_Timestamp>::max();
    subtree->max_time = std::numeric_limits<aku_Timestamp>::lowest();
    subtree->first = .0;
    subtree->last = .0;

    // Initialize the writer
    writer_.init(id);
}


IOVecLeaf::IOVecLeaf(std::shared_ptr<BlockStore> bstore, LogicAddr curr)
    : IOVecLeaf(read_iovec_block_from_bstore(bstore, curr))
{
}

IOVecLeaf::IOVecLeaf(std::unique_ptr<IOVecBlock> block)
    : prev_(EMPTY_ADDR)
    , block_(std::move(block))
{
    const SubtreeRef* subtree = block_->get_cheader<SubtreeRef>();
    prev_ = subtree->addr;
    fanout_index_ = subtree->fanout_index;
}

static std::unique_ptr<IOVecBlock> clone(const std::unique_ptr<IOVecBlock>& block) {
    std::unique_ptr<IOVecBlock> res(new IOVecBlock());
    res->copy_from(*block);
    return res;
}

static aku_ParamId getid(std::unique_ptr<IOVecBlock> const& block) {
    auto ptr = block->get_header<SubtreeRef>();
    return ptr->id;
}

IOVecLeaf::IOVecLeaf(std::unique_ptr<IOVecBlock> block, IOVecLeaf::CloneTag)
    : prev_(EMPTY_ADDR)
    , block_(clone(block))
    , writer_(block_.get())
{
    writer_.init(getid(block));
    // Re-insert the data
    IOVecBlockReader<IOVecBlock> reader(block.get(), static_cast<u32>(sizeof(SubtreeRef)));
    size_t sz = reader.nelements();
    for (size_t ix = 0; ix < sz; ix++) {
        aku_Status status;
        aku_Timestamp ts;
        double value;
        std::tie(status, ts, value) = reader.next();
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Leaf node clone error, can't read the previous node (some data will be lost)");
            assert(false);
            return;
        }
        status = writer_.put(ts, value);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Leaf node clone error, can't write to the new node (some data will be lost)");
            assert(false);
            return;
        }
    }

    const SubtreeRef* subtree = block_->get_cheader<SubtreeRef>();
    prev_ = subtree->addr;
    fanout_index_ = subtree->fanout_index;
}

size_t IOVecLeaf::_get_uncommitted_size() const {
    return static_cast<size_t>(writer_.get_write_index());
}

size_t IOVecLeaf::bytes_used() const {
    size_t res = 0;
    for (int i = 0; i < IOVecBlock::NCOMPONENTS; i++) {
        res += block_->get_size(i);
    }
    return res;
}

SubtreeRef const* IOVecLeaf::get_leafmeta() const {
    return block_->get_cheader<SubtreeRef>();
}

size_t IOVecLeaf::nelements() const {
    SubtreeRef const* subtree = block_->get_cheader<SubtreeRef>();
    return subtree->count;
}

u16 IOVecLeaf::get_fanout() const {
    return fanout_index_;
}

aku_ParamId IOVecLeaf::get_id() const {
    SubtreeRef const* subtree = block_->get_cheader<SubtreeRef>();
    return subtree->id;
}

std::tuple<aku_Timestamp, aku_Timestamp> IOVecLeaf::get_timestamps() const {
    SubtreeRef const* subtree = block_->get_cheader<SubtreeRef>();
    return std::make_tuple(subtree->begin, subtree->end);
}

void IOVecLeaf::set_prev_addr(LogicAddr addr) {
    prev_ = addr;
    SubtreeRef* subtree = block_->get_header<SubtreeRef>();
    subtree->addr = addr;
}

void IOVecLeaf::set_node_fanout(u16 fanout) {
    assert(fanout <= AKU_NBTREE_FANOUT);
    fanout_index_ = fanout;
    SubtreeRef* subtree = block_->get_header<SubtreeRef>();
    subtree->fanout_index = fanout;
}

LogicAddr IOVecLeaf::get_addr() const {
    return block_->get_addr();
}

LogicAddr IOVecLeaf::get_prev_addr() const {
    // Should be set correctly no metter how IOVecLeaf was created.
    return prev_;
}


aku_Status IOVecLeaf::read_all(std::vector<aku_Timestamp>* timestamps,
                               std::vector<double>* values) const
{
    int windex = writer_.get_write_index();
    IOVecBlockReader<IOVecBlock> reader(block_.get(), static_cast<u32>(sizeof(SubtreeRef)));
    size_t sz = reader.nelements();
    timestamps->reserve(sz);
    values->reserve(sz);
    for (size_t ix = 0; ix < sz; ix++) {
        aku_Status status;
        aku_Timestamp ts;
        double value;
        std::tie(status, ts, value) = reader.next();
        if (status != AKU_SUCCESS) {
            return status;
        }
        timestamps->push_back(ts);
        values->push_back(value);
    }
    // Read tail elements from `writer_`
    if (windex != 0) {
        writer_.read_tail_elements(timestamps, values);
    }
    return AKU_SUCCESS;
}

aku_Status IOVecLeaf::append(aku_Timestamp ts, double value) {
    aku_Status status = writer_.put(ts, value);
    if (status == AKU_SUCCESS) {
        SubtreeRef* subtree = block_->get_header<SubtreeRef>();
        subtree->end = ts;
        subtree->last = value;
        if (subtree->count == 0) {
            subtree->begin = ts;
            subtree->first = value;
        }
        subtree->count++;
        subtree->sum += value;
        if (subtree->max < value) {
            subtree->max = value;
            subtree->max_time = ts;
        }
        if (subtree->min > value) {
            subtree->min = value;
            subtree->min_time = ts;
        }
    }
    return status;
}

std::tuple<aku_Status, LogicAddr> IOVecLeaf::commit(std::shared_ptr<BlockStore> bstore) {
    assert(nelements() != 0);
    u16 size = static_cast<u16>(writer_.commit()) - sizeof(SubtreeRef);
    assert(size);
    SubtreeRef* subtree = block_->get_header<SubtreeRef>();
    subtree->payload_size = size;
    if (prev_ != EMPTY_ADDR && fanout_index_ > 0) {
        subtree->addr = prev_;
    } else {
        // addr = EMPTY indicates that there is
        // no link to previous node.
        subtree->addr  = EMPTY_ADDR;
        // Invariant: fanout index should be 0 in this case.
    }
    subtree->version = AKUMULI_VERSION;
    subtree->level = 0;
    subtree->type  = NBTreeBlockType::LEAF;
    subtree->fanout_index = fanout_index_;
    // Compute checksum
    subtree->checksum = bstore->checksum(*block_, sizeof(SubtreeRef), size);
    return bstore->append_block(*block_);
}


std::unique_ptr<RealValuedOperator> IOVecLeaf::range(aku_Timestamp begin, aku_Timestamp end) const {
    std::unique_ptr<RealValuedOperator> it;
    it.reset(new NBTreeLeafIterator(begin, end, *this));
    return it;
}

std::unique_ptr<RealValuedOperator> IOVecLeaf::filter(aku_Timestamp begin,
                                                      aku_Timestamp end,
                                                      const ValueFilter& filter) const
{
    std::unique_ptr<RealValuedOperator> it;
    it.reset(new NBTreeLeafFilter(begin, end, filter, *this));
    return it;
}

std::unique_ptr<AggregateOperator> IOVecLeaf::aggregate(aku_Timestamp begin, aku_Timestamp end) const {
    std::unique_ptr<AggregateOperator> it;
    it.reset(new NBTreeLeafAggregator(begin, end, *this));
    return it;
}

std::unique_ptr<AggregateOperator> IOVecLeaf::candlesticks(aku_Timestamp begin, aku_Timestamp end, NBTreeCandlestickHint hint) const {
    AKU_UNUSED(hint);
    auto agg = INIT_AGGRES;
    const SubtreeRef* subtree = block_->get_cheader<SubtreeRef>();
    agg.copy_from(*subtree);
    std::unique_ptr<AggregateOperator> result;
    AggregateOperator::Direction dir = begin < end ? AggregateOperator::Direction::FORWARD : AggregateOperator::Direction::BACKWARD;
    result.reset(new ValueAggregator(subtree->end, agg, dir));
    return result;
}

std::unique_ptr<AggregateOperator> IOVecLeaf::group_aggregate(aku_Timestamp begin, aku_Timestamp end, u64 step) const {
    std::unique_ptr<AggregateOperator> it;
    it.reset(new NBTreeLeafGroupAggregator(begin, end, step, *this));
    return it;
}

std::tuple<aku_Status, LogicAddr> IOVecLeaf::split_into(std::shared_ptr<BlockStore> bstore,
                                                         aku_Timestamp pivot,
                                                         bool preserve_backrefs,
                                                         u16 *fanout_index,
                                                         SuperblockAppender *top_level)
{
    /* When the method is called from IOVecSuperblock::split method, the
     * top_level node will be provided. Otherwise it will be null.
     */
    aku_Status status;
    std::vector<double> xss;
    std::vector<aku_Timestamp> tss;
    status = read_all(&tss, &xss);
    if (status != AKU_SUCCESS || tss.size() == 0) {
        return std::make_tuple(status, EMPTY_ADDR);
    }
    // Make new superblock with two leafs
    // Left hand side leaf node
    u32 ixbase = 0;
    IOVecLeaf lhs(get_id(), preserve_backrefs ? prev_ : EMPTY_ADDR, *fanout_index);
    for (u32 i = 0; i < tss.size(); i++) {
        if (tss[i] < pivot) {
            status = lhs.append(tss[i], xss[i]);
            if (status != AKU_SUCCESS) {
                return std::make_tuple(status, EMPTY_ADDR);
            }
        } else {
            ixbase = i;
            break;
        }
    }
    SubtreeRef lhs_ref;
    if (ixbase == 0) {
        // Special case, the lhs node is empty
        lhs_ref.addr = EMPTY_ADDR;
    } else {
        LogicAddr lhs_addr;
        std::tie(status, lhs_addr) = lhs.commit(bstore);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY_ADDR);
        }
        status = init_subtree_from_leaf(lhs, lhs_ref);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY_ADDR);
        }
        lhs_ref.addr = lhs_addr;
        (*fanout_index)++;
    }
    // Right hand side leaf node, it can't be empty in any case
    // because the leaf node is not empty.
    auto prev = lhs_ref.addr == EMPTY_ADDR ? prev_ : lhs_ref.addr;
    IOVecLeaf rhs(get_id(), prev, *fanout_index);
    for (u32 i = ixbase; i < tss.size(); i++) {
        status = rhs.append(tss[i], xss[i]);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY_ADDR);
        }
    }
    SubtreeRef rhs_ref;
    if (ixbase == tss.size()) {
        // Special case, rhs is empty
        rhs_ref.addr = EMPTY_ADDR;
    } else {
        LogicAddr rhs_addr;
        std::tie(status, rhs_addr) = rhs.commit(bstore);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY_ADDR);
        }
        status = init_subtree_from_leaf(rhs, rhs_ref);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY_ADDR);
        }
        rhs_ref.addr = rhs_addr;
        (*fanout_index)++;
    }
    // Superblock
    if (lhs_ref.addr != EMPTY_ADDR) {
        status = top_level->append(lhs_ref);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY_ADDR);
        }
    }
    if (rhs_ref.addr != EMPTY_ADDR) {
        status = top_level->append(rhs_ref);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY_ADDR);
        }
    }
    return std::make_tuple(AKU_SUCCESS, EMPTY_ADDR);
}

std::tuple<aku_Status, LogicAddr> IOVecLeaf::split(std::shared_ptr<BlockStore> bstore,
                                                    aku_Timestamp pivot,
                                                    bool preserve_backrefs)
{
    // New superblock
    IOVecSuperblock sblock(get_id(), preserve_backrefs ? get_prev_addr() : EMPTY_ADDR, get_fanout(), 0);
    aku_Status status;
    LogicAddr  addr;
    u16 fanout = 0;
    std::tie(status, addr) = split_into(bstore, pivot, false, &fanout, &sblock);
    if (status != AKU_SUCCESS || sblock.nelements() == 0) {
        return std::make_tuple(status, EMPTY_ADDR);
    }
    std::tie(status, addr) = sblock.commit(bstore);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, EMPTY_ADDR);
    }
    return std::make_tuple(AKU_SUCCESS, addr);
}


// /////////////// //
// IOVecSuperblock //
// /////////////// //

IOVecSuperblock::IOVecSuperblock(aku_ParamId id, LogicAddr prev, u16 fanout, u16 lvl)
    : block_(new IOVecBlock())
    , id_(id)
    , write_pos_(0)
    , fanout_index_(fanout)
    , level_(lvl)
    , prev_(prev)
    , immutable_(false)
{
    SubtreeRef ref = {};
    ref.type = NBTreeBlockType::INNER;
    block_->append_chunk(&ref, sizeof(ref));
    assert(prev_ != 0);
}

IOVecSuperblock::IOVecSuperblock(std::unique_ptr<IOVecBlock> block)
    : block_(std::move(block))
    , immutable_(true)
{
    // Use zero-copy here.
    SubtreeRef const* ref = block_->get_cheader<SubtreeRef>();
    assert(ref->type == NBTreeBlockType::INNER);
    id_ = ref->id;
    fanout_index_ = ref->fanout_index;
    prev_ = ref->addr;
    write_pos_ = ref->payload_size;
    level_ = ref->level;
    assert(prev_ != 0);
}

IOVecSuperblock::IOVecSuperblock(LogicAddr addr, std::shared_ptr<BlockStore> bstore)
    : IOVecSuperblock(read_iovec_block_from_bstore(bstore, addr))
{
}

IOVecSuperblock::IOVecSuperblock(LogicAddr addr, std::shared_ptr<BlockStore> bstore, bool remove_last)
    : block_(new IOVecBlock())
    , immutable_(false)
{
    std::unique_ptr<IOVecBlock> block = read_iovec_block_from_bstore(bstore, addr);
    SubtreeRef const* ref = block->get_cheader<SubtreeRef>();
    assert(ref->type == NBTreeBlockType::INNER);
    id_ = ref->id;
    fanout_index_ = ref->fanout_index;
    prev_ = ref->addr;
    level_ = ref->level;
    write_pos_ = ref->payload_size;
    if (remove_last && write_pos_ != 0) {
        write_pos_--;
    }
    assert(prev_ != 0);
    // We can't use zero-copy here because `block` belongs to other node.
    block_->copy_from(*block);

    // Shrink block size if possible to save memory
    int bytes_used = static_cast<int>((write_pos_ + 1) * sizeof(SubtreeRef));
    block_->set_write_pos_and_shrink(bytes_used);
}

SubtreeRef const* IOVecSuperblock::get_sblockmeta() const {
    SubtreeRef const* pref = block_->get_cheader<SubtreeRef>();
    return pref;
}

size_t IOVecSuperblock::nelements() const {
    return write_pos_;
}

u16 IOVecSuperblock::get_level() const {
    return level_;
}

u16 IOVecSuperblock::get_fanout() const {
    return fanout_index_;
}

aku_ParamId IOVecSuperblock::get_id() const {
    return id_;
}

LogicAddr IOVecSuperblock::get_prev_addr() const {
    return prev_;
}

void IOVecSuperblock::set_prev_addr(LogicAddr addr) {
    assert(addr != 0);
    prev_ = addr;
    block_->get_header<SubtreeRef>()->addr = addr;
}

void IOVecSuperblock::set_node_fanout(u16 newfanout) {
    assert(newfanout <= AKU_NBTREE_FANOUT);
    fanout_index_ = newfanout;
    block_->get_header<SubtreeRef>()->fanout_index = newfanout;
}

LogicAddr IOVecSuperblock::get_addr() const {
    return block_->get_addr();
}

aku_Status IOVecSuperblock::append(const SubtreeRef &p) {
    if (is_full()) {
        return AKU_EOVERFLOW;
    }
    if (immutable_) {
        return AKU_EBAD_DATA;
    }
    // Write data into buffer
    u32 bytes_written = block_->append_chunk(&p, sizeof(SubtreeRef));
    if (bytes_written == 0) {
        return AKU_ENO_MEM;
    }
    SubtreeRef* pref = reinterpret_cast<SubtreeRef*>(block_->get_data(0));
    if (write_pos_ == 0) {
        pref->begin = p.begin;
    }
    pref->end = p.end;
    write_pos_++;
    return AKU_SUCCESS;
}

std::tuple<aku_Status, LogicAddr> IOVecSuperblock::commit(std::shared_ptr<BlockStore> bstore) {
    assert(nelements() != 0);
    if (immutable_) {
        return std::make_tuple(AKU_EBAD_DATA, EMPTY_ADDR);
    }
    SubtreeRef* backref = block_->get_header<SubtreeRef>();
    auto status = init_subtree_from_subtree(*this, *backref);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, EMPTY_ADDR);
    }
    backref->addr = prev_;
    backref->payload_size = static_cast<u16>(write_pos_);
    assert(backref->payload_size + sizeof(SubtreeRef) < AKU_BLOCK_SIZE);
    backref->fanout_index = fanout_index_;
    backref->id = id_;
    backref->level = level_;
    backref->type  = NBTreeBlockType::INNER;
    backref->version = AKUMULI_VERSION;
    // add checksum
    backref->checksum = bstore->checksum(block_->get_cdata(0) + sizeof(SubtreeRef), backref->payload_size);
    return bstore->append_block(*block_);
}

bool IOVecSuperblock::is_full() const {
    return write_pos_ >= AKU_NBTREE_FANOUT;
}

aku_Status IOVecSuperblock::read_all(std::vector<SubtreeRef>* refs) const {
    for(u32 ix = 0u; ix < write_pos_; ix++) {
        SubtreeRef item;
        u32 res = block_->read_chunk(&item, sizeof(SubtreeRef) * (ix + 1), sizeof(SubtreeRef));
        if (res == 0) {
            return AKU_EBAD_DATA;
        }
        refs->push_back(item);
    }
    return AKU_SUCCESS;
}

bool IOVecSuperblock::top(SubtreeRef* outref) const {
    if (write_pos_ == 0) {
        return false;
    }
    SubtreeRef item;
    u32 offset = sizeof(item) * write_pos_;
    u32 res = block_->read_chunk(&item, offset, sizeof(item));
    if (res == 0) {
        return false;
    }
    *outref = item;
    return true;
}

bool IOVecSuperblock::top(LogicAddr* outaddr) const {
    SubtreeRef child;
    if (top(&child)) {
        *outaddr = child.addr;
        return true;
    }
    return false;
}

std::tuple<aku_Timestamp, aku_Timestamp> IOVecSuperblock::get_timestamps() const {
    SubtreeRef const* pref = block_->get_cheader<SubtreeRef>();
    return std::tie(pref->begin, pref->end);
}

std::unique_ptr<RealValuedOperator> IOVecSuperblock::search(aku_Timestamp begin,
                                                         aku_Timestamp end,
                                                         std::shared_ptr<BlockStore> bstore) const
{
    std::unique_ptr<RealValuedOperator> result;
    result.reset(new NBTreeSBlockIterator(bstore, *this, begin, end));
    return result;
}

std::unique_ptr<RealValuedOperator> IOVecSuperblock::filter(aku_Timestamp begin,
                                                             aku_Timestamp end,
                                                             const ValueFilter& filter,
                                                             std::shared_ptr<BlockStore> bstore) const
{
    std::unique_ptr<RealValuedOperator> result;
    result.reset(new NBTreeSBlockFilter(bstore, *this, begin, end, filter));
    return result;
}

std::unique_ptr<AggregateOperator> IOVecSuperblock::aggregate(aku_Timestamp begin,
                                                            aku_Timestamp end,
                                                            std::shared_ptr<BlockStore> bstore) const
{
    std::unique_ptr<AggregateOperator> result;
    result.reset(new NBTreeSBlockAggregator(bstore, *this, begin, end));
    return result;
}

std::unique_ptr<AggregateOperator> IOVecSuperblock::candlesticks(aku_Timestamp begin, aku_Timestamp end,
                                                                 std::shared_ptr<BlockStore> bstore,
                                                                 NBTreeCandlestickHint hint) const
{
    std::unique_ptr<AggregateOperator> result;
    result.reset(new NBTreeSBlockCandlesticsIter(bstore, *this, begin, end, hint));
    return result;
}

std::unique_ptr<AggregateOperator> IOVecSuperblock::group_aggregate(aku_Timestamp begin,
                                                                    aku_Timestamp end,
                                                                    u64 step,
                                                                    std::shared_ptr<BlockStore> bstore) const
{
    std::unique_ptr<AggregateOperator> result;
    result.reset(new NBTreeSBlockGroupAggregator(bstore, *this, begin, end, step));
    return result;
}

std::tuple<aku_Status, LogicAddr> IOVecSuperblock::split_into(std::shared_ptr<BlockStore> bstore,
                                                                          aku_Timestamp pivot,
                                                                          bool preserve_horizontal_links,
                                                                          SuperblockAppender *root)
{
    // for each node in BFS order:
    //      if pivot is inside the node:
    //          node.split() <-- recursive call
    //      else if top_level_node and node is on the right from pivot:
    //          node.clone().fix_horizontal_link()
    std::vector<SubtreeRef> refs;
    aku_Status status = read_all(&refs);
    if (status != AKU_SUCCESS || refs.empty()) {
        return std::make_tuple(status, EMPTY_ADDR);
    }
    for (u32 i = 0; i < refs.size(); i++) {
        if (refs[i].begin <= pivot && pivot <= refs[i].end) {
            // Do split the node
            LogicAddr new_ith_child_addr = EMPTY_ADDR;
            u16 current_fanout = 0;
            // Clone current node
            for (u32 j = 0; j < i; j++) {
                root->append(refs[j]);
                current_fanout++;
            }
            std::unique_ptr<IOVecBlock> block;
            std::tie(status, block) = read_and_check(bstore, refs[i].addr);
            if (status != AKU_SUCCESS) {
                return std::make_tuple(status, EMPTY_ADDR);
            }
            auto refsi = block->get_cheader<SubtreeRef>();
            assert(refsi->count == refs[i].count);
            assert(refsi->type  == refs[i].type);
            assert(refsi->begin == refs[i].begin);
            AKU_UNUSED(refsi);
            if (refs[i].type == NBTreeBlockType::INNER) {
                IOVecSuperblock sblock(std::move(block));
                LogicAddr ignored;
                std::tie(status, new_ith_child_addr, ignored) = sblock.split(bstore, pivot, false);
                if (status != AKU_SUCCESS) {
                    return std::make_tuple(status, EMPTY_ADDR);
                }
            } else {
                IOVecLeaf oldleaf(std::move(block));
                if ((refs.size() - AKU_NBTREE_FANOUT) > 1) {
                    // Split in-place
                    std::tie(status, new_ith_child_addr) = oldleaf.split_into(bstore, pivot, preserve_horizontal_links, &current_fanout, root);
                    if (status != AKU_SUCCESS) {
                        return std::make_tuple(status, EMPTY_ADDR);
                    }
                } else {
                    // Create new level in the tree
                    std::tie(status, new_ith_child_addr) = oldleaf.split(bstore, pivot, preserve_horizontal_links);
                    if (status != AKU_SUCCESS) {
                        return std::make_tuple(status, EMPTY_ADDR);
                    }
                }
            }
            if (new_ith_child_addr != EMPTY_ADDR) {
                SubtreeRef newref;
                auto block = read_iovec_block_from_bstore(bstore, new_ith_child_addr);
                IOVecSuperblock child(std::move(block));
                status = init_subtree_from_subtree(child, newref);
                if (status != AKU_SUCCESS) {
                    return std::make_tuple(status, EMPTY_ADDR);
                }
                newref.addr = new_ith_child_addr;
                root->append(newref);
                current_fanout++;
            }
            LogicAddr last_child_addr;
            if (!root->top(&last_child_addr)) {
                AKU_PANIC("Attempt to split an empty node");
            }
            if (preserve_horizontal_links) {
                // Fix backrefs on the right from the pivot
                // Move from left to right and clone the blocks fixing
                // the back references.
                for (u32 j = i+1; j < refs.size(); j++) {
                    if (refs[j].type == NBTreeBlockType::INNER) {
                        IOVecSuperblock cloned_child(refs[j].addr, bstore, false);
                        cloned_child.set_prev_addr(last_child_addr);
                        cloned_child.set_node_fanout(current_fanout);
                        current_fanout++;
                        std::tie(status, last_child_addr) = cloned_child.commit(bstore);
                        if (status != AKU_SUCCESS) {
                            return std::make_tuple(status, EMPTY_ADDR);
                        }
                        SubtreeRef backref;
                        status = init_subtree_from_subtree(cloned_child, backref);
                        if (status != AKU_SUCCESS) {
                            return std::make_tuple(status, EMPTY_ADDR);
                        }
                        backref.addr = last_child_addr;
                        status = root->append(backref);
                        if (status != AKU_SUCCESS) {
                            return std::make_tuple(status, EMPTY_ADDR);
                        }
                    } else {
                        std::unique_ptr<IOVecBlock> child_block;
                        std::tie(status, child_block) = read_and_check(bstore, refs[j].addr);
                        IOVecLeaf cloned_child(std::move(child_block), IOVecLeaf::CloneTag());
                        cloned_child.set_prev_addr(last_child_addr);
                        cloned_child.set_node_fanout(current_fanout);
                        current_fanout++;
                        std::tie(status, last_child_addr) = cloned_child.commit(bstore);
                        if (status != AKU_SUCCESS) {
                            return std::make_tuple(status, EMPTY_ADDR);
                        }
                        SubtreeRef backref;
                        status = init_subtree_from_leaf(cloned_child, backref);
                        if (status != AKU_SUCCESS) {
                            return std::make_tuple(status, EMPTY_ADDR);
                        }
                        backref.addr = last_child_addr;
                        status = root->append(backref);
                        if (status != AKU_SUCCESS) {
                            return std::make_tuple(status, EMPTY_ADDR);
                        }
                    }
                }
            } else {
                for (u32 j = i+1; j < refs.size(); j++) {
                    root->append(refs[j]);
                }
            }
            return std::tie(status, last_child_addr);
        }
    }
    // The pivot point is not found
    return std::make_tuple(AKU_ENOT_FOUND, EMPTY_ADDR);
}

std::tuple<aku_Status, LogicAddr, LogicAddr> IOVecSuperblock::split(std::shared_ptr<BlockStore> bstore,
                                                                     aku_Timestamp pivot,
                                                                     bool preserve_horizontal_links)
{
    aku_Status status;
    LogicAddr last_child;
    IOVecSuperblock new_sblock(id_, prev_, get_fanout(), level_);
    std::tie(status, last_child) = split_into(bstore, pivot, preserve_horizontal_links, &new_sblock);
    if (status != AKU_SUCCESS || new_sblock.nelements() == 0) {
        return std::make_tuple(status, EMPTY_ADDR, EMPTY_ADDR);
    }
    LogicAddr newaddr = EMPTY_ADDR;
    std::tie(status, newaddr) = new_sblock.commit(bstore);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, EMPTY_ADDR, EMPTY_ADDR);
    }
    return std::tie(status, newaddr, last_child);
}

// //////////////////////// //
//        NBTreeExtent      //
// //////////////////////// //


//! Represents extent made of one memory resident leaf node
struct NBTreeLeafExtent : NBTreeExtent {
    std::shared_ptr<BlockStore> bstore_;
    std::weak_ptr<NBTreeExtentsList> roots_;
    aku_ParamId id_;
    LogicAddr last_;
    std::shared_ptr<IOVecLeaf> leaf_;
    u16 fanout_index_;
    // padding
    u16 pad0_;
    u32 pad1_;

    NBTreeLeafExtent(std::shared_ptr<BlockStore> bstore,
                     std::shared_ptr<NBTreeExtentsList> roots,
                     aku_ParamId id,
                     LogicAddr last)
        : bstore_(bstore)
        , roots_(roots)
        , id_(id)
        , last_(last)
        , fanout_index_(0)
        , pad0_{}
        , pad1_{}
    {
        if (last_ != EMPTY_ADDR) {
            // Load previous node and calculate fanout.
            aku_Status status;
            std::unique_ptr<IOVecBlock> block;
            std::tie(status, block) = read_and_check(bstore_, last_);
            if (status == AKU_EUNAVAILABLE) {
                // Can't read previous node (retention)
                fanout_index_ = 0;
                last_ = EMPTY_ADDR;
            } else if (status != AKU_SUCCESS) {
                Logger::msg(AKU_LOG_ERROR, "Can't read block @" + std::to_string(last_) + ", error: " + StatusUtil::str(status));
                AKU_PANIC("Invalid argument, " + StatusUtil::str(status));
            } else {
                auto psubtree = block->get_cheader<SubtreeRef>();
                fanout_index_ = psubtree->fanout_index + 1;
                if (fanout_index_ == AKU_NBTREE_FANOUT) {
                    fanout_index_ = 0;
                    last_ = EMPTY_ADDR;
                }
            }
        }
        reset_leaf();
    }

    size_t bytes_used() const {
        return leaf_->bytes_used();
    }

    virtual ExtentStatus status() const override {
        // Leaf extent can be new and empty or new and filled with data
        if (leaf_->nelements() == 0) {
            return ExtentStatus::NEW;
        }
        return ExtentStatus::OK;
    }

    virtual aku_Status update_prev_addr(LogicAddr addr) override {
        if (leaf_->get_addr() == EMPTY_ADDR) {
            leaf_->set_prev_addr(addr);
            return AKU_SUCCESS;
        }
        // This can happen due to concurrent access
        return AKU_EACCESS;
    }

    virtual aku_Status update_fanout_index(u16 fanout_index) override {
        if (leaf_->get_addr() == EMPTY_ADDR) {
            leaf_->set_node_fanout(fanout_index);
            fanout_index_ = fanout_index;
            return AKU_SUCCESS;
        }
        // This can happen due to concurrent access
        return AKU_EACCESS;
    }

    aku_Status get_prev_subtreeref(SubtreeRef &payload) {
        aku_Status status = AKU_SUCCESS;
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore_, last_);
        if (status != AKU_SUCCESS) {
            return status;
        }
        IOVecLeaf leaf(std::move(block));
        status = init_subtree_from_leaf(leaf, payload);
        payload.addr = last_;
        return status;
    }

    u16 get_current_fanout_index() const {
        return leaf_->get_fanout();
    }

    void reset_leaf() {
        leaf_.reset(new IOVecLeaf(id_, last_, fanout_index_));
    }

    virtual std::tuple<bool, LogicAddr> append(aku_Timestamp ts, double value) override;
    virtual std::tuple<bool, LogicAddr> append(const SubtreeRef &pl) override;
    virtual std::tuple<bool, LogicAddr> commit(bool final) override;
    virtual std::unique_ptr<RealValuedOperator> search(aku_Timestamp begin, aku_Timestamp end) const override;
    virtual std::unique_ptr<RealValuedOperator> filter(aku_Timestamp begin,
                                                       aku_Timestamp end,
                                                       const ValueFilter& filter) const override;
    virtual std::unique_ptr<AggregateOperator> aggregate(aku_Timestamp begin, aku_Timestamp end) const override;
    virtual std::unique_ptr<AggregateOperator> candlesticks(aku_Timestamp begin, aku_Timestamp end, NBTreeCandlestickHint hint) const override;
    virtual std::unique_ptr<AggregateOperator> group_aggregate(aku_Timestamp begin, aku_Timestamp end, u64 step) const override;
    virtual bool is_dirty() const override;
    virtual void debug_dump(std::ostream& stream, int base_indent, std::function<std::string(aku_Timestamp)> tsformat, u32 mask) const override;
    virtual std::tuple<bool, LogicAddr> split(aku_Timestamp pivot) override;
};


static void dump_subtree_ref(std::ostream& stream,
                             SubtreeRef const* ref,
                             LogicAddr prev_addr,
                             int base_indent,
                             LogicAddr self_addr,
                             std::function<std::string(aku_Timestamp)> tsformat,
                             u32 mask=0xFFFFFFFF)
{
    auto tag = [base_indent](const char* tag_name) {
        std::stringstream str;
        for (int i = 0; i < base_indent; i++) {
            str << '\t';
        }
        str << '<' << tag_name << '>';
        return str.str();
    };
    auto afmt = [](LogicAddr addr) {
        if (addr == EMPTY_ADDR) {
            return std::string();
        }
        return std::to_string(addr);
    };
    if (mask&1) {
        if (ref->type == NBTreeBlockType::LEAF) {
            stream << tag("type")     << "Leaf"                       << "</type>\n";
        } else {
            stream << tag("type")     << "Superblock"                 << "</type>\n";
        }
    }
    if (mask & 2) {
        stream << tag("addr")         << afmt(self_addr)              << "</addr>\n";
    }
    if (mask & 4) {
        stream << tag("id")           << ref->id                      << "</id>\n";
    }
    if (mask & 8) {
        stream << tag("prev_addr")    << afmt(prev_addr)              << "</prev_addr>\n";
    }
    if (mask & 16) {
        stream << tag("begin")        << tsformat(ref->begin)         << "</begin>\n";
    }
    if (mask & 32) {
        stream << tag("end")          << tsformat(ref->end)           << "</end>\n";
    }
    if (mask & 64) {
        stream << tag("count")        << ref->count                   << "</count>\n";
    }
    if (mask & 128) {
        stream << tag("min")          << ref->min                     << "</min>\n";
    }
    if (mask & 0x100) {
        stream << tag("min_time")     << tsformat(ref->min_time)      << "</min_time>\n";
    }
    if (mask & 0x200) {
        stream << tag("max")          << ref->max                     << "</max>\n";
    }
    if (mask & 0x400) {
        stream << tag("max_time")     << tsformat(ref->max_time)      << "</max_time>\n";
    }
    if (mask & 0x800) {
        stream << tag("sum")          << ref->sum                     << "</sum>\n";
    }
    if (mask & 0x1000) {
        stream << tag("first")        << ref->first                   << "</first>\n";
    }
    if (mask & 0x2000) {
        stream << tag("last")         << ref->last                    << "</last>\n";
    }
    if (mask & 0x4000) {
        stream << tag("version")      << ref->version                 << "</version>\n";
    }
    if (mask & 0x8000) {
        stream << tag("level")        << ref->level                   << "</level>\n";
    }
    if (mask & 0x10000) {
        stream << tag("type")         << ref->type                    << "</level>\n";
    }
    if (mask & 0x20000) {
        stream << tag("payload_size") << ref->payload_size            << "</payload_size>\n";
    }
    if (mask & 0x40000) {
        stream << tag("fanout_index") << ref->fanout_index            << "</fanout_index>\n";
    }
    if (mask & 0x80000) {
        stream << tag("checksum")     << ref->checksum                << "</checksum>\n";
    }
}


void NBTreeLeafExtent::debug_dump(std::ostream& stream, int base_indent, std::function<std::string(aku_Timestamp)> tsformat, u32 mask) const {
    SubtreeRef const* ref = leaf_->get_leafmeta();
    stream << std::string(static_cast<size_t>(base_indent), '\t') <<  "<node>\n";
    dump_subtree_ref(stream, ref, leaf_->get_prev_addr(), base_indent + 1, leaf_->get_addr(), tsformat, mask);
    stream << std::string(static_cast<size_t>(base_indent), '\t') << "</node>\n";
}

std::tuple<bool, LogicAddr> NBTreeLeafExtent::append(SubtreeRef const&) {
    Logger::msg(AKU_LOG_ERROR, "Attempt to insert ref into a leaf node, id=" + std::to_string(id_)
                + ", fanout=" + std::to_string(fanout_index_) + ", last=" + std::to_string(last_));
    AKU_PANIC("Can't append subtree to leaf node");
}

std::tuple<bool, LogicAddr> NBTreeLeafExtent::append(aku_Timestamp ts, double value) {
    // Invariant: leaf_ should be initialized, if leaf_ is full
    // and pushed to block-store, reset_leaf should be called
    aku_Status status = leaf_->append(ts, value);
    if (status == AKU_EOVERFLOW) {
        LogicAddr addr;
        bool parent_saved;
        // Commit full node
        std::tie(parent_saved, addr) = commit(false);
        // Stack overflow here means that there is a logic error in
        // the program that results in IOVecLeaf::append always
        // returning AKU_EOVERFLOW.
        append(ts, value);
        return std::make_tuple(parent_saved, addr);
    }
    return std::make_tuple(false, EMPTY_ADDR);
}

//! Forcibly commit changes, even if current page is not full
std::tuple<bool, LogicAddr> NBTreeLeafExtent::commit(bool final) {
    // Invariant: after call to this method data from `leaf_` should
    // endup in block store, upper level root node should be updated
    // and `leaf_` variable should be reset.
    // Otherwise: panic should be triggered.

    LogicAddr addr;
    aku_Status status;
    std::tie(status, addr) = leaf_->commit(bstore_);
    if (status != AKU_SUCCESS) {
        Logger::msg(AKU_LOG_ERROR, "Can't write leaf-node to block-store, " + StatusUtil::str(status)
                    + ", id=" + std::to_string(id_) + ", fanout=" + std::to_string(fanout_index_)
                    + ", last=" + std::to_string(last_));
        AKU_PANIC("Can't write leaf-node to block-store, " + StatusUtil::str(status));
    }
    // Gather stats and send them to upper-level node
    SubtreeRef payload = INIT_SUBTREE_REF;
    status = init_subtree_from_leaf(*leaf_, payload);
    if (status != AKU_SUCCESS) {
        // This shouldn't happen because leaf node can't be
        // empty just after overflow.
        Logger::msg(AKU_LOG_ERROR, "Can summarize leaf-node - " + StatusUtil::str(status)
                    + ", id=" + std::to_string(id_) + ", fanout=" + std::to_string(fanout_index_)
                    + ", last=" + std::to_string(last_) + ", payload=" + to_string(payload));
        AKU_PANIC("Can summarize leaf-node - " + StatusUtil::str(status));
    }
    payload.addr = addr;
    bool parent_saved = false;
    auto roots_collection = roots_.lock();
    size_t next_level = payload.level + 1;
    if (roots_collection) {
        if (!final || roots_collection->_get_roots().size() > next_level) {
            parent_saved = roots_collection->append(payload);
        }
    } else {
        // Invariant broken.
        // Roots collection was destroyed before write process
        // stops.
        Logger::msg(AKU_LOG_ERROR, "Roots collection destroyed"
                    ", id=" + std::to_string(id_) + ", fanout=" + std::to_string(fanout_index_)
                    + ", last=" + std::to_string(last_) + ", payload=" + to_string(payload));
        AKU_PANIC("Roots collection destroyed");
    }
    fanout_index_++;
    last_ = addr;
    if (fanout_index_ == AKU_NBTREE_FANOUT) {
        fanout_index_ = 0;
        last_ = EMPTY_ADDR;
    }
    reset_leaf();
    // NOTE: we should reset current extent's rescue point because parent node was saved and
    // already has a link to current extent (e.g. leaf node was saved and new leaf
    // address was added to level 1 node, level 1 node becomes full and was written to disk).
    // If we won't do this - we will read the same information twice during crash recovery process.
    return std::make_tuple(parent_saved, addr);
}

std::unique_ptr<RealValuedOperator> NBTreeLeafExtent::search(aku_Timestamp begin, aku_Timestamp end) const {
    return leaf_->range(begin, end);
}

std::unique_ptr<RealValuedOperator> NBTreeLeafExtent::filter(aku_Timestamp begin,
                                                             aku_Timestamp end,
                                                             const ValueFilter& filter) const
{
    return leaf_->filter(begin, end, filter);
}

std::unique_ptr<AggregateOperator> NBTreeLeafExtent::aggregate(aku_Timestamp begin, aku_Timestamp end) const {
    return leaf_->aggregate(begin, end);
}

std::unique_ptr<AggregateOperator> NBTreeLeafExtent::candlesticks(aku_Timestamp begin, aku_Timestamp end, NBTreeCandlestickHint hint) const {
    return leaf_->candlesticks(begin, end, hint);
}

std::unique_ptr<AggregateOperator> NBTreeLeafExtent::group_aggregate(aku_Timestamp begin, aku_Timestamp end, u64 step) const {
    return leaf_->group_aggregate(begin, end, step);
}

bool NBTreeLeafExtent::is_dirty() const {
    if (leaf_) {
        return leaf_->nelements() != 0;
    }
    return false;
}

std::tuple<bool, LogicAddr> NBTreeLeafExtent::split(aku_Timestamp pivot) {
    aku_Status status;
    LogicAddr addr;
    std::tie(status, addr) = leaf_->split(bstore_, pivot, true);
    if (status != AKU_SUCCESS || addr == EMPTY_ADDR) {
        return std::make_tuple(false, EMPTY_ADDR);
    }
    auto block = read_iovec_block_from_bstore(bstore_, addr);
    IOVecSuperblock sblock(std::move(block));
    // Gather stats and send them to upper-level node
    SubtreeRef payload = INIT_SUBTREE_REF;
    status = init_subtree_from_subtree(sblock, payload);
    if (status != AKU_SUCCESS) {
        // This shouldn't happen because sblock can't be empty, it contains
        // two or one child element.
        Logger::msg(AKU_LOG_ERROR, "Can summarize leaf-node - " + StatusUtil::str(status)
                    + ", id=" + std::to_string(id_) + ", fanout=" + std::to_string(fanout_index_)
                    + ", last=" + std::to_string(last_));
        AKU_PANIC("Can summarize leaf-node - " + StatusUtil::str(status));
    }
    payload.addr = addr;
    bool parent_saved = false;
    auto roots_collection = roots_.lock();
    if (roots_collection) {
        parent_saved = roots_collection->append(payload);
    } else {
        // Invariant broken.
        // Roots collection was destroyed before write process
        // stops.
        Logger::msg(AKU_LOG_ERROR, "Roots collection destroyed"
                    ", id=" + std::to_string(id_) + ", fanout=" + std::to_string(fanout_index_)
                    + ", last=" + std::to_string(last_));
        AKU_PANIC("Roots collection destroyed");
    }
    fanout_index_++;
    last_ = addr;
    if (fanout_index_ == AKU_NBTREE_FANOUT) {
        fanout_index_ = 0;
        last_ = EMPTY_ADDR;
    }
    reset_leaf();
    return std::make_tuple(parent_saved, addr);
}


// ////////////////////// //
//   NBTreeSBlockExtent   //
// ////////////////////// //

struct NBTreeSBlockExtent : NBTreeExtent {
    std::shared_ptr<BlockStore> bstore_;
    std::weak_ptr<NBTreeExtentsList> roots_;
    std::unique_ptr<IOVecSuperblock> curr_;
    aku_ParamId id_;
    LogicAddr last_;
    u16 fanout_index_;
    u16 level_;
    // padding
    u32 killed_;

    NBTreeSBlockExtent(std::shared_ptr<BlockStore> bstore,
                       std::shared_ptr<NBTreeExtentsList> roots,
                       aku_ParamId id,
                       LogicAddr addr,
                       u16 level)
        : bstore_(bstore)
        , roots_(roots)
        , id_(id)
        , last_(EMPTY_ADDR)
        , fanout_index_(0)
        , level_(level)
        , killed_(0)
    {
        if (addr != EMPTY_ADDR) {
            // `addr` is not empty. Node should be restored from
            // block-store.
            aku_Status status;
            std::unique_ptr<IOVecBlock> block;
            std::tie(status, block) = read_and_check(bstore_, addr);
            if (status  == AKU_EUNAVAILABLE) {
                addr = EMPTY_ADDR;
                killed_ = 1;
            } else if (status != AKU_SUCCESS) {
                Logger::msg(AKU_LOG_ERROR, "Can't read @" + std::to_string(addr) + ", error: " + StatusUtil::str(status));
                AKU_PANIC("Invalid argument, " + StatusUtil::str(status));
            } else {
                auto psubtree = block->get_cheader<SubtreeRef>();
                fanout_index_ = psubtree->fanout_index + 1;
                if (fanout_index_ == AKU_NBTREE_FANOUT) {
                    fanout_index_ = 0;
                    last_ = EMPTY_ADDR;
                }
                last_ = psubtree->addr;
            }
        }
        if (addr != EMPTY_ADDR) {
            // CoW constructor should be used here.
            curr_.reset(new IOVecSuperblock(addr, bstore_, false));
        } else {
            // `addr` is not set. Node should be created from scratch.
            curr_.reset(new IOVecSuperblock(id, EMPTY_ADDR, 0, level));
        }
    }

    ExtentStatus status() const override {
        if (killed_) {
            return ExtentStatus::KILLED_BY_RETENTION;
        } else if (curr_->nelements() == 0) {
            // Node is new
            return ExtentStatus::NEW;
        }
        // Node is filled with data or created using CoW constructor
        return ExtentStatus::OK;
    }

    virtual aku_Status update_prev_addr(LogicAddr addr) override {
        if (curr_->get_addr() == EMPTY_ADDR) {
            curr_->set_prev_addr(addr);
            return AKU_SUCCESS;
        }
        return AKU_EACCESS;
    }

    virtual aku_Status update_fanout_index(u16 fanout_index) override {
        if (curr_->get_addr() == EMPTY_ADDR) {
            curr_->set_node_fanout(fanout_index);
            fanout_index_ = fanout_index;
            return AKU_SUCCESS;
        }
        return AKU_EACCESS;
    }

    void reset_subtree() {
        curr_.reset(new IOVecSuperblock(id_, last_, fanout_index_, level_));
    }

    u16 get_fanout_index() const {
        return fanout_index_;
    }

    u16 get_level() const {
        return level_;
    }

    LogicAddr get_prev_addr() const {
        return curr_->get_prev_addr();
    }

    virtual std::tuple<bool, LogicAddr> append(aku_Timestamp ts, double value) override;
    virtual std::tuple<bool, LogicAddr> append(const SubtreeRef &pl) override;
    virtual std::tuple<bool, LogicAddr> commit(bool final) override;
    virtual std::unique_ptr<RealValuedOperator> search(aku_Timestamp begin, aku_Timestamp end) const override;
    virtual std::unique_ptr<RealValuedOperator> filter(aku_Timestamp begin,
                                                       aku_Timestamp end,
                                                       const ValueFilter& filter) const override;
    virtual std::unique_ptr<AggregateOperator> aggregate(aku_Timestamp begin, aku_Timestamp end) const override;
    virtual std::unique_ptr<AggregateOperator> candlesticks(aku_Timestamp begin, aku_Timestamp end, NBTreeCandlestickHint hint) const override;
    virtual std::unique_ptr<AggregateOperator> group_aggregate(aku_Timestamp begin, aku_Timestamp end, u64 step) const override;
    virtual bool is_dirty() const override;
    virtual void debug_dump(std::ostream& stream, int base_indent, std::function<std::string(aku_Timestamp)> tsformat, u32 mask) const override;
    virtual std::tuple<bool, LogicAddr> split(aku_Timestamp pivot) override;
};

void NBTreeSBlockExtent::debug_dump(std::ostream& stream, int base_indent, std::function<std::string(aku_Timestamp)> tsformat, u32 mask) const {
    SubtreeRef const* ref = curr_->get_sblockmeta();
    stream << std::string(static_cast<size_t>(base_indent), '\t') <<  "<node>\n";
    dump_subtree_ref(stream, ref, curr_->get_prev_addr(), base_indent + 1, curr_->get_addr(), tsformat, mask);

    std::vector<SubtreeRef> refs;
    aku_Status status = curr_->read_all(&refs);
    if (status != AKU_SUCCESS) {
        Logger::msg(AKU_LOG_ERROR, std::string("Can't read data ") + StatusUtil::str(status));
        return;
    }

    if (refs.empty()) {
        stream << std::string(static_cast<size_t>(base_indent), '\t') <<  "</node>\n";
        return;
    }

    // Traversal control
    enum class Action {
        DUMP_NODE,
        OPEN_NODE,
        CLOSE_NODE,
        OPEN_CHILDREN,
        CLOSE_CHILDREN,
    };

    typedef std::tuple<LogicAddr, Action, int> StackItem;

    std::stack<StackItem> stack;

    stack.push(std::make_tuple(0, Action::CLOSE_NODE, base_indent));
    stack.push(std::make_tuple(0, Action::CLOSE_CHILDREN, base_indent + 1));
    for (auto const& ref: refs) {
        LogicAddr addr = ref.addr;
        stack.push(std::make_tuple(0, Action::CLOSE_NODE, base_indent + 2));
        stack.push(std::make_tuple(addr, Action::DUMP_NODE, base_indent + 3));
        stack.push(std::make_tuple(0, Action::OPEN_NODE, base_indent + 2));
    }
    stack.push(std::make_tuple(0, Action::OPEN_CHILDREN, base_indent + 1));

    // Tree traversal (depth first)
    while(!stack.empty()) {
        LogicAddr addr;
        Action action;
        int indent;
        std::tie(addr, action, indent) = stack.top();
        stack.pop();

        auto tag = [indent](const char* tag_name, const char* tag_opener = "<") {
            return std::string(static_cast<size_t>(indent), '\t') + tag_opener + tag_name + ">";
        };

        switch(action) {
        case Action::DUMP_NODE: {
            aku_Status status;
            std::unique_ptr<IOVecBlock> block;
            std::tie(status, block) = bstore_->read_iovec_block(addr);
            if (status != AKU_SUCCESS) {
                stream << tag("addr") << addr << "</addr>\n";
                stream << tag("fail") << StatusUtil::c_str(status) << "</fail>" << std::endl;
                continue;
            }
            auto subtreeref = block->get_cheader<SubtreeRef>();
            if (subtreeref->type == NBTreeBlockType::LEAF) {
                // leaf node
                IOVecLeaf leaf(std::move(block));
                SubtreeRef const* ref = leaf.get_leafmeta();
                dump_subtree_ref(stream, ref, leaf.get_prev_addr(), indent, leaf.get_addr(), tsformat, mask);
            } else {
                // superblock
                IOVecSuperblock sblock(std::move(block));
                SubtreeRef const* ref = sblock.get_sblockmeta();
                dump_subtree_ref(stream, ref, sblock.get_prev_addr(), indent, sblock.get_addr(), tsformat, mask);
                std::vector<SubtreeRef> children;
                status = sblock.read_all(&children);
                if (status != AKU_SUCCESS) {
                    AKU_PANIC("Can't read superblock");
                }
                stack.push(std::make_tuple(0, Action::CLOSE_CHILDREN, indent));
                for(const SubtreeRef& sref: children) {
                    stack.push(std::make_tuple(0, Action::CLOSE_NODE, indent + 1));
                    stack.push(std::make_tuple(sref.addr, Action::DUMP_NODE, indent + 2));
                    stack.push(std::make_tuple(0, Action::OPEN_NODE, indent + 1));
                }
                stack.push(std::make_tuple(0, Action::OPEN_CHILDREN, indent));
            }
        }
        break;
        case Action::OPEN_NODE:
            stream << tag("node") << std::endl;
        break;
        case Action::CLOSE_NODE:
            stream << tag("node", "</") << std::endl;
        break;
        case Action::OPEN_CHILDREN:
            stream << tag("children") << std::endl;
        break;
        case Action::CLOSE_CHILDREN:
            stream << tag("children", "</") << std::endl;
        break;
        };
    }
}

std::tuple<bool, LogicAddr> NBTreeSBlockExtent::append(aku_Timestamp, double) {
    AKU_PANIC("Data should be added to the root 0");
}

std::tuple<bool, LogicAddr> NBTreeSBlockExtent::append(SubtreeRef const& pl) {
    auto status = curr_->append(pl);
    if (status == AKU_EOVERFLOW) {
        LogicAddr addr;
        bool parent_saved;
        std::tie(parent_saved, addr) = commit(false);
        append(pl);
        return std::make_tuple(parent_saved, addr);
    }
    return std::make_tuple(false, EMPTY_ADDR);
}

std::tuple<bool, LogicAddr> NBTreeSBlockExtent::commit(bool final) {
    // Invariant: after call to this method data from `curr_` should
    // endup in block store, upper level root node should be updated
    // and `curr_` variable should be reset.
    // Otherwise: panic should be triggered.

    LogicAddr addr;
    aku_Status status;
    std::tie(status, addr) = curr_->commit(bstore_);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("Can't write superblock to block-store, " + StatusUtil::str(status));
    }
    // Gather stats and send them to upper-level node
    SubtreeRef payload = INIT_SUBTREE_REF;
    status = init_subtree_from_subtree(*curr_, payload);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("Can summarize current node - " + StatusUtil::str(status));
    }
    payload.addr = addr;
    bool parent_saved = false;
    auto roots_collection = roots_.lock();
    size_t next_level = payload.level + 1;
    if (roots_collection) {
        if (!final || roots_collection->_get_roots().size() > next_level) {
            // We shouldn't create new root if `commit` called from `close` method.
            parent_saved = roots_collection->append(payload);
        }
    } else {
        // Invariant broken.
        // Roots collection was destroyed before write process
        // stops.
        AKU_PANIC("Roots collection destroyed");
    }
    fanout_index_++;
    last_ = addr;
    if (fanout_index_ == AKU_NBTREE_FANOUT) {
        fanout_index_ = 0;
        last_ = EMPTY_ADDR;
    }
    reset_subtree();
    // NOTE: we should reset current extent's rescue point because parent node was saved and
    // parent node already has a link to this extent.
    return std::make_tuple(parent_saved, addr);
}

std::unique_ptr<RealValuedOperator> NBTreeSBlockExtent::search(aku_Timestamp begin, aku_Timestamp end) const {
    return curr_->search(begin, end, bstore_);
}

std::unique_ptr<RealValuedOperator> NBTreeSBlockExtent::filter(aku_Timestamp begin,
                                                               aku_Timestamp end,
                                                               const ValueFilter& filter) const
{
    return curr_->filter(begin, end, filter, bstore_);
}

std::unique_ptr<AggregateOperator> NBTreeSBlockExtent::aggregate(aku_Timestamp begin, aku_Timestamp end) const {
    return curr_->aggregate(begin, end, bstore_);
}

std::unique_ptr<AggregateOperator> NBTreeSBlockExtent::candlesticks(aku_Timestamp begin, aku_Timestamp end, NBTreeCandlestickHint hint) const {
    return curr_->candlesticks(begin, end, bstore_, hint);
}

std::unique_ptr<AggregateOperator> NBTreeSBlockExtent::group_aggregate(aku_Timestamp begin, aku_Timestamp end, u64 step) const {
    return curr_->group_aggregate(begin, end, step, bstore_);
}

bool NBTreeSBlockExtent::is_dirty() const {
    if (curr_) {
        return curr_->nelements() != 0;
    }
    return false;
}

std::tuple<bool, LogicAddr> NBTreeSBlockExtent::split(aku_Timestamp pivot) {
    const auto empty_res = std::make_tuple(false, EMPTY_ADDR);
    aku_Status status;
    std::unique_ptr<IOVecSuperblock> clone;
    clone.reset(new IOVecSuperblock(id_, curr_->get_prev_addr(), curr_->get_fanout(), curr_->get_level()));
    LogicAddr last_child_addr;
    std::tie(status, last_child_addr) = curr_->split_into(bstore_, pivot, true, clone.get());
    // The addr variable should be empty, because we're using the clone
    if (status != AKU_SUCCESS) {
        return empty_res;
    }
    curr_.swap(clone);
    return std::make_tuple(false, last_child_addr);
}


template<class SuperblockT>
static void check_superblock_consistency(std::shared_ptr<BlockStore> bstore,
                                         SuperblockT const* sblock,
                                         u16 required_level,
                                         bool check_backrefs) {
    // For each child.
    std::vector<SubtreeRef> refs;
    aku_Status status = sblock->read_all(&refs);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("IOVecSuperblock.read_all failed, exit code: " + StatusUtil::str(status));
    }
    std::vector<LogicAddr> nodes2follow;
    // Check nodes.
    size_t nelements = sblock->nelements();
    int nerrors = 0;
    for (size_t i = 0; i < nelements; i++) {
        if (check_backrefs) {
            // require refs[i].fanout_index == i.
            auto fanout = refs[i].fanout_index;
            if (fanout != i) {
                std::string error_message = "Faulty superblock found, expected fanout_index = "
                                          + std::to_string(i) + " actual = "
                                          + std::to_string(fanout);
                Logger::msg(AKU_LOG_ERROR, error_message);
                nerrors++;
            }
            if (refs[i].level != required_level) {
                std::string error_message = "Faulty superblock found, expected level = "
                                          + std::to_string(required_level) + " actual level = "
                                          + std::to_string(refs[i].level);
                Logger::msg(AKU_LOG_ERROR, error_message);
                nerrors++;
            }
        }
        // Try to read block and check stats
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore, refs[i].addr);
        if (status == AKU_EUNAVAILABLE) {
            // block was deleted due to retention.
            Logger::msg(AKU_LOG_INFO, "Block " + std::to_string(refs[i].addr));
        } else if (status == AKU_SUCCESS) {
            SubtreeRef out = INIT_SUBTREE_REF;
            const SubtreeRef* iref = block->get_cheader<SubtreeRef>();
            if (iref->type == NBTreeBlockType::LEAF) {
                IOVecLeaf leaf(std::move(block));
                status = init_subtree_from_leaf(leaf, out);
                if (status != AKU_SUCCESS) {
                    AKU_PANIC("Can't summarize leaf node at " + std::to_string(refs[i].addr) + " error: "
                                                              + StatusUtil::str(status));
                }
            } else {
                IOVecSuperblock superblock(std::move(block));
                status = init_subtree_from_subtree(superblock, out);
                if (status != AKU_SUCCESS) {
                    AKU_PANIC("Can't summarize inner node at " + std::to_string(refs[i].addr) + " error: "
                                                               + StatusUtil::str(status));
                }
            }
            // Compare metadata refs
            std::stringstream fmt;
            int nbadfields = 0;
            if (refs[i].begin != out.begin) {
                fmt << ".begin " << refs[i].begin << " != " << out.begin << "; ";
                nbadfields++;
            }
            if (refs[i].end != out.end) {
                fmt << ".end " << refs[i].end << " != " << out.end << "; ";
                nbadfields++;
            }
            if (refs[i].count != out.count) {
                fmt << ".count " << refs[i].count << " != " << out.count << "; ";
                nbadfields++;
            }
            if (refs[i].id != out.id) {
                fmt << ".id " << refs[i].id << " != " << out.id << "; ";
                nbadfields++;
            }
            if (!same_value(refs[i].max, out.max)) {
                fmt << ".max " << refs[i].max << " != " << out.max << "; ";
                nbadfields++;
            }
            if (!same_value(refs[i].min, out.min)) {
                fmt << ".min " << refs[i].min << " != " << out.min << "; ";
                nbadfields++;
            }
            if (!same_value(refs[i].sum, out.sum)) {
                fmt << ".sum " << refs[i].sum << " != " << out.sum << "; ";
                nbadfields++;
            }
            if (refs[i].version != out.version) {
                fmt << ".version " << refs[i].version << " != " << out.version << "; ";
                nbadfields++;
            }
            if (nbadfields) {
                Logger::msg(AKU_LOG_ERROR, "Inner node contains bad values: " + fmt.str());
                nerrors++;
            } else {
                nodes2follow.push_back(refs[i].addr);
            }
        } else {
            // Some other error occured.
            AKU_PANIC("Can't read node from block-store: " + StatusUtil::str(status));
        }
    }
    if (nerrors) {
        AKU_PANIC("Invalid structure at " + std::to_string(required_level) + " examine log for more details.");
    }

    // Recur
    for (auto addr: nodes2follow) {
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore, addr);
        const SubtreeRef* iref = block->get_cheader<SubtreeRef>();
        if (iref->type == NBTreeBlockType::INNER) {
            IOVecSuperblock child(addr, bstore);
            // We need to check backrefs only on top level that is used for crash recovery.
            // In all other levels backreferences is not used for anything.
            check_superblock_consistency(bstore, &child, required_level == 0 ? 0 : required_level - 1, false);
        }
    }
}


void NBTreeExtent::check_extent(NBTreeExtent const* extent, std::shared_ptr<BlockStore> bstore, size_t level) {
    if (level == 0) {
        // Leaf node
        return;
    }
    auto subtree = dynamic_cast<NBTreeSBlockExtent const*>(extent);
    if (subtree) {
        // Complex extent.
        auto const* curr = subtree->curr_.get();
        check_superblock_consistency(bstore, curr, static_cast<u16>(level - 1), true);
    }
}

// ///////////////////// //
//   NBTreeExtentsList   //
// ///////////////////// //


NBTreeExtentsList::NBTreeExtentsList(aku_ParamId id, std::vector<LogicAddr> addresses, std::shared_ptr<BlockStore> bstore)
    : bstore_(bstore)
    , id_(id)
    , last_(0ull)
    , rescue_points_(std::move(addresses))
    , initialized_(false)
    , write_count_(0ul)
#ifdef AKU_ENABLE_MUTATION_TESTING
    , rd_()
    , rand_gen_(rd_())
    , dist_(0, 1000)
    , threshold_(1)
#endif
{
    if (rescue_points_.size() >= std::numeric_limits<u16>::max()) {
        AKU_PANIC("Tree depth is too large");
    }
}

std::tuple<size_t, size_t> NBTreeExtentsList::bytes_used() const {
    size_t c1 = 0, c2 = 0;
    if (!extents_.empty()) {
        auto leaf = dynamic_cast<NBTreeLeafExtent const*>(extents_.front().get());
        if (leaf != nullptr) {
            c1 = leaf->bytes_used();
        }
        if (extents_.size() > 1) {
            c2 = 0x1000 * (extents_.size() - 1);
        }
    }
    return std::make_tuple(c1, c2);
}

void NBTreeExtentsList::force_init() {
    UniqueLock lock(lock_);
    if (!initialized_) {
        init();
    }
}

size_t NBTreeExtentsList::_get_uncommitted_size() const {
    if (!extents_.empty()) {
        auto leaf = dynamic_cast<NBTreeLeafExtent const*>(extents_.front().get());
        if (leaf == nullptr) {
            // Small check to make coverity scan happy
            AKU_PANIC("Bad extent at level 0, leaf node expected");
        }
        return leaf->leaf_->_get_uncommitted_size();
    }
    return 0;
}

bool NBTreeExtentsList::is_initialized() const {
    SharedLock lock(lock_);
    return initialized_;
}

std::vector<NBTreeExtent const*> NBTreeExtentsList::get_extents() const {
    // NOTE: no lock here because we're returning extents and this breaks
    //       all thread safety but this is doesn't matter because this method
    //       should be used only for testing in single-threaded setting.
    std::vector<NBTreeExtent const*> result;
    for (auto const& ptr: extents_) {
        result.push_back(ptr.get());
    }
    return result;
}

#ifdef AKU_ENABLE_MUTATION_TESTING
u32 NBTreeExtentsList::chose_random_node() {
    std::uniform_int_distribution<u32> rext(0, static_cast<u32>(extents_.size()-1));
    u32 ixnode = rext(rand_gen_);
    return ixnode;
}
#endif

std::tuple<aku_Status, AggregationResult> NBTreeExtentsList::get_aggregates(u32 ixnode) const {
    auto it = extents_.at(ixnode)->aggregate(0, AKU_MAX_TIMESTAMP);
    aku_Timestamp ts;
    AggregationResult dest;
    size_t outsz;
    aku_Status status;
    std::tie(status, outsz) = it->read(&ts, &dest, 1);
    if (outsz == 0) {
        Logger::msg(AKU_LOG_ERROR, "Can't split the node: no data returned from aggregate query");
        return std::make_tuple(AKU_ENOT_FOUND, dest);
    }
    if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
        Logger::msg(AKU_LOG_ERROR, "Can't split the node: " + StatusUtil::str(status));
    }
    return std::make_tuple(AKU_SUCCESS, dest);
}

#ifdef AKU_ENABLE_MUTATION_TESTING
LogicAddr NBTreeExtentsList::split_random_node(u32 ixnode) {
    AggregationResult dest;
    aku_Status status;
    std::tie(status, dest) = get_aggregates(ixnode);
    if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
        return EMPTY_ADDR;
    }
    aku_Timestamp begin = dest._begin;
    aku_Timestamp end   = dest._end;

    // Chose the pivot point
    std::uniform_int_distribution<aku_Timestamp> rsplit(begin, end);
    aku_Timestamp pivot = rsplit(rand_gen_);
    LogicAddr addr;
    bool parent_saved;
    if (extents_.at(ixnode)->is_dirty()) {
        std::tie(parent_saved, addr) = extents_.at(ixnode)->split(pivot);
        return addr;
    }
    return EMPTY_ADDR;
}
#endif

void NBTreeExtentsList::check_rescue_points(u32 i) const {
    if (i == 0) {
        return;
    }
    // we can use i-1 value to restore the i'th
    LogicAddr addr = rescue_points_.at(i-1);

    auto aggit = extents_.at(i)->aggregate(AKU_MIN_TIMESTAMP, AKU_MAX_TIMESTAMP);
    aku_Timestamp ts;
    AggregationResult res;
    size_t sz;
    aku_Status status;
    std::tie(status, sz) = aggit->read(&ts, &res, 1);
    if (sz == 0 || (status != AKU_SUCCESS && status != AKU_ENO_DATA)) {
        // Failed check
        assert(false);
    }

    IOVecSuperblock sblock(id_, EMPTY_ADDR, 0, 0);
    std::vector<SubtreeRef> refs;
    while(addr != EMPTY_ADDR) {
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore_, addr);
        if (status == AKU_EUNAVAILABLE) {
            // Block removed due to retention. Can't actually check anything.
            return;
        }
        const SubtreeRef* ref = block->get_cheader<SubtreeRef>();
        SubtreeRef tmp = *ref;
        tmp.addr = addr;
        refs.push_back(tmp);
        addr = ref->addr;
    }
    for(auto it = refs.rbegin(); it < refs.rend(); it++) {
        status = sblock.append(*it);
        assert(status == AKU_SUCCESS);
    }
    aggit = sblock.aggregate(AKU_MIN_TIMESTAMP, AKU_MAX_TIMESTAMP, bstore_);
    AggregationResult newres;
    std::tie(status, sz) = aggit->read(&ts, &newres, 1);
    if (sz == 0 || (status != AKU_SUCCESS && status != AKU_ENO_DATA)) {
        // Failed check
        assert(false);
    }
    assert(res._begin   == newres._begin);
    assert(res._end     == newres._end);
    assert(res.cnt      == newres.cnt);
    assert(res.first    == newres.first);
    assert(res.last     == newres.last);
    assert(res.max      == newres.max);
    assert(res.min      == newres.min);
    assert(res.maxts    == newres.maxts);
    assert(res.mints    == newres.mints);
}

std::tuple<aku_Status, LogicAddr> NBTreeExtentsList::_split(aku_Timestamp pivot) {
    aku_Status status;
    LogicAddr paddr = EMPTY_ADDR;
    size_t extent_index = extents_.size();
    // Find the extent that contains the pivot
    for (size_t i = 0; i < extents_.size(); i++) {
        auto it = extents_.at(i)->aggregate(AKU_MIN_TIMESTAMP, AKU_MAX_TIMESTAMP);
        AggregationResult res;
        size_t outsz;
        aku_Timestamp ts;
        std::tie(status, outsz) = it->read(&ts, &res, 1);
        if (status == AKU_SUCCESS) {
            if (res._begin <= pivot && pivot < res._end) {
                extent_index = i;
            }
            break;
        } else if (status == AKU_ENO_DATA || status == AKU_EUNAVAILABLE) {
            continue;
        }
        return std::make_tuple(status, paddr);
    }
    if (extent_index == extents_.size()) {
        return std::make_tuple(AKU_ENOT_FOUND, paddr);
    }
    bool parent_saved = false;
    std::tie(parent_saved, paddr) = extents_.at(extent_index)->split(pivot);
    if (paddr != EMPTY_ADDR) {
        std::unique_ptr<IOVecBlock> rblock;
        std::tie(status, rblock) = read_and_check(bstore_, paddr);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Can't read @" + std::to_string(paddr) + ", error: " + StatusUtil::str(status));
            AKU_PANIC("Can't read back the data");
        }
        // extent_index and the level of the node can mismatch
        auto pnode = rblock->get_cheader<SubtreeRef>();
        if (rescue_points_.size() > pnode->level) {
            rescue_points_.at(pnode->level) = paddr;
        } else {
            rescue_points_.push_back(paddr);
        }
        if (extent_index > 0) {
            u16 prev_fanout = 0;
            LogicAddr prev_addr = EMPTY_ADDR;
            if (pnode->fanout_index < AKU_NBTREE_MAX_FANOUT_INDEX) {
                prev_fanout = pnode->fanout_index + 1;
                prev_addr   = paddr;
            }
            auto prev_extent = extent_index - 1;
            status = extents_.at(prev_extent)->update_prev_addr(prev_addr);
            if (status != AKU_SUCCESS) {
                AKU_PANIC("Invalid access pattern in split method");
            }
            status = extents_.at(prev_extent)->update_fanout_index(prev_fanout);
            if (status != AKU_SUCCESS) {
                AKU_PANIC("Can't update fanout index of the node");
            }
        }
    }
    return std::make_tuple(status, paddr);
}

NBTreeAppendResult NBTreeExtentsList::append(aku_Timestamp ts, double value, bool allow_duplicate_timestamps) {
    UniqueLock lock(lock_);  // NOTE: NBTreeExtentsList::append(subtree) can be called from here
                             //       recursively (maybe even many times).
    if (!initialized_) {
        init();
    }
    if (allow_duplicate_timestamps ? ts < last_ : ts <= last_) {
        return NBTreeAppendResult::FAIL_LATE_WRITE;
    }
    last_ = ts;
    write_count_++;
    if (extents_.size() == 0) {
        // create first leaf node
        std::unique_ptr<NBTreeExtent> leaf;
        leaf.reset(new NBTreeLeafExtent(bstore_, shared_from_this(), id_, EMPTY_ADDR));
        extents_.push_back(std::move(leaf));
        rescue_points_.push_back(EMPTY_ADDR);
    }
    auto result = NBTreeAppendResult::OK;
    bool parent_saved = false;
    LogicAddr addr = EMPTY_ADDR;
    std::tie(parent_saved, addr) = extents_.front()->append(ts, value);
    if (addr != EMPTY_ADDR) {
        // We need to clear the rescue point since the address is already
        // persisted.
        //addr = parent_saved ? EMPTY_ADDR : addr;
        if (rescue_points_.size() > 0) {
            rescue_points_.at(0) = addr;
        } else {
            rescue_points_.push_back(addr);
        }
        result = NBTreeAppendResult::OK_FLUSH_NEEDED;
    }
    return result;
}

bool NBTreeExtentsList::append(const SubtreeRef &pl) {
    // NOTE: this method should be called by extents which
    //       is called by another `append` overload recursively
    //       and lock will be held already so no lock here!
    u16 lvl = static_cast<u16>(pl.level + 1);
    NBTreeExtent* root = nullptr;
    if (extents_.size() > lvl) {
        // Fast path
        root = extents_[lvl].get();
    } else if (extents_.size() == lvl) {
        std::unique_ptr<NBTreeExtent> p;
        p.reset(new NBTreeSBlockExtent(bstore_, shared_from_this(),
                                       id_,
                                       EMPTY_ADDR,
                                       lvl));
        root = p.get();
        extents_.push_back(std::move(p));
        rescue_points_.push_back(EMPTY_ADDR);
    } else {
        Logger::msg(AKU_LOG_ERROR, std::to_string(id_) + " Invalid node level - " + std::to_string(lvl));
        AKU_PANIC("Invalid node level");
    }
    bool parent_saved = false;
    LogicAddr addr = EMPTY_ADDR;
    write_count_++;
    std::tie(parent_saved, addr) = root->append(pl);
    if (addr != EMPTY_ADDR) {
        // NOTE: `addr != EMPTY_ADDR` means that something was saved to disk (current node or parent node).
        //addr = parent_saved ? EMPTY_ADDR : addr;
        if (rescue_points_.size() > lvl) {
            rescue_points_.at(lvl) = addr;
        } else if (rescue_points_.size() == lvl) {
            rescue_points_.push_back(addr);
        } else {
            // INVARIANT: order of commits - leaf node committed first, then inner node at level 1,
            // then level 2 and so on. Address of the inner node (or root node) should be greater then addresses
            // of all its children.
            AKU_PANIC("Out of order commit!");
        }
        return true;
    }
    return false;
}

void NBTreeExtentsList::open() {
    // NOTE: lock doesn't needed here because this method will be called by
    //       the `force_init` method that already holds the write lock.
    Logger::msg(AKU_LOG_INFO, std::to_string(id_) + " Trying to open tree, repair status - OK, addr: " +
                              std::to_string(rescue_points_.back()));
    // NOTE: rescue_points_ list should have at least two elements [EMPTY_ADDR, Root].
    // Because of this `addr` is always an inner node.
    if (rescue_points_.size() < 2) {
        // Only one page was saved to disk!
        // Create new root, because now we will create new root (this is the only case
        // when new root will be created during tree-open process).
        u16 root_level = 1;
        std::unique_ptr<NBTreeSBlockExtent> root_extent;
        root_extent.reset(new NBTreeSBlockExtent(bstore_, shared_from_this(), id_, EMPTY_ADDR, root_level));

        // Read old leaf node. Add single element to the root.
        LogicAddr addr = rescue_points_.front();
        std::unique_ptr<IOVecBlock> leaf_block;
        aku_Status status;
        std::tie(status, leaf_block) = read_and_check(bstore_, addr);
        if (status != AKU_SUCCESS) {
            // Tree is old and should be removed, no data was left on the block device.
            Logger::msg(AKU_LOG_INFO, std::to_string(id_) + " Dead tree detected");
            std::unique_ptr<NBTreeExtent> leaf_extent(new NBTreeLeafExtent(bstore_, shared_from_this(), id_, EMPTY_ADDR));
            extents_.push_back(std::move(leaf_extent));
            initialized_ = true;
            return;
        }
        IOVecLeaf leaf(std::move(leaf_block));  // fully loaded leaf
        SubtreeRef sref = INIT_SUBTREE_REF;
        status = init_subtree_from_leaf(leaf, sref);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, std::to_string(id_) + " Can't open tree at: " + std::to_string(addr) +
                        " error: " + StatusUtil::str(status));
            AKU_PANIC("Can't open tree");
        }
        sref.addr = addr;
        root_extent->append(sref);  // this always should return `false` and `EMPTY_ADDR`, no need to check this.

        // Create new empty leaf
        std::unique_ptr<NBTreeExtent> leaf_extent(new NBTreeLeafExtent(bstore_, shared_from_this(), id_, addr));
        extents_.push_back(std::move(leaf_extent));
        extents_.push_back(std::move(root_extent));
        rescue_points_.push_back(EMPTY_ADDR);
    } else {
        // Initialize root node.
        auto root_level = rescue_points_.size() - 1;
        LogicAddr addr = rescue_points_.back();
        std::unique_ptr<NBTreeSBlockExtent> root;
        // CoW should be used here, otherwise tree height will increase after each reopen.
        root.reset(new NBTreeSBlockExtent(bstore_, shared_from_this(), id_, addr, static_cast<u16>(root_level)));

        // Initialize leaf using new leaf node!
        // TODO: leaf_prev = load_prev_leaf_addr(root);
        LogicAddr leaf_prev = EMPTY_ADDR;
        std::unique_ptr<NBTreeExtent> leaf(new NBTreeLeafExtent(bstore_, shared_from_this(), id_, leaf_prev));
        extents_.push_back(std::move(leaf));

        // Initialize inner nodes.
        for (size_t i = 1; i < root_level; i++) {
            // TODO: leaf_prev = load_prev_inner_addr(root, i);
            LogicAddr inner_prev = EMPTY_ADDR;
            std::unique_ptr<NBTreeExtent> inner;
            inner.reset(new NBTreeSBlockExtent(bstore_, shared_from_this(),
                                               id_, inner_prev, static_cast<u16>(i)));
            extents_.push_back(std::move(inner));
        }

        extents_.push_back(std::move(root));
    }
    // Scan extents backwards and remove stalled extents
    int ext2remove = 0;
    for (auto it = extents_.rbegin(); it != extents_.rend(); it++) {
        if ((*it)->status() == NBTreeExtent::ExtentStatus::KILLED_BY_RETENTION) {
            ext2remove++;
        } else {
            break;
        }
    }
    // All extents can't be removed because leaf extent can't return `KILLED_BY_RETENTION` status.
    // If all data was deleted by retention, only one new leaf node will be present in the `extents_`
    // list.
    for(;ext2remove --> 0;) {
        extents_.pop_back();
        rescue_points_.pop_back();
    }

    // Restore `last_`
    if (extents_.size()) {
        auto it = extents_.back()->search(AKU_MAX_TIMESTAMP, 0);
        aku_Timestamp ts;
        double val;
        aku_Status status;
        size_t nread;
        std::tie(status, nread) = it->read(&ts, &val, 1);
        if (status != AKU_SUCCESS) {
            // The tree is empty due to retention so we can use smallest possible
            // timestamp
            Logger::msg(AKU_LOG_TRACE, "Tree " + std::to_string(this->id_) + " is empty due to retention");
            ts = AKU_MIN_TIMESTAMP;
        }
        last_ = ts;
    }
    assert(rescue_points_.size() == extents_.size());
}

static void create_empty_extents(std::shared_ptr<NBTreeExtentsList> self,
                                 std::shared_ptr<BlockStore> bstore,
                                 aku_ParamId id,
                                 size_t nlevels,
                                 std::vector<std::unique_ptr<NBTreeExtent>>* extents)
{
    for (size_t i = 0; i < nlevels; i++) {
        if (i == 0) {
            // Create empty leaf node
            std::unique_ptr<NBTreeLeafExtent> leaf;
            leaf.reset(new NBTreeLeafExtent(bstore, self, id, EMPTY_ADDR));
            extents->push_back(std::move(leaf));
        } else {
            // Create empty inner node
            std::unique_ptr<NBTreeSBlockExtent> inner;
            u16 level = static_cast<u16>(i);
            inner.reset(new NBTreeSBlockExtent(bstore, self, id, EMPTY_ADDR, level));
            extents->push_back(std::move(inner));
        }
    }
}

void NBTreeExtentsList::repair() {
    // NOTE: lock doesn't needed for the same reason as in `open` method.
    Logger::msg(AKU_LOG_INFO, std::to_string(id_) + " Trying to open tree, repair status - REPAIR, addr: " +
                              std::to_string(rescue_points_.back()));
    create_empty_extents(shared_from_this(), bstore_, id_, rescue_points_.size(), &extents_);
    std::stack<LogicAddr> stack;
    // Follow rescue points in backward direction
    for (auto addr: rescue_points_) {
        stack.push(addr);
    }
    std::vector<SubtreeRef> refs;
    refs.reserve(32);
    while (!stack.empty()) {
        LogicAddr curr = stack.top();
        stack.pop();
        if (curr == EMPTY_ADDR) {
            // Insert all nodes in direct order
            for(auto it = refs.rbegin(); it < refs.rend(); it++) {
                append(*it);  // There is no need to check return value.
            }
            refs.clear();
            // Invariant: this part is guaranteed to be called for every level that have
            // the data to recover. First child node of every extent will have `get_prev_addr()`
            // return EMPTY_ADDR. This value will trigger this code.
            continue;
        }
        aku_Status status;
        std::unique_ptr<IOVecBlock> block;
        std::tie(status, block) = read_and_check(bstore_, curr);
        if (status != AKU_SUCCESS) {
            // Stop collecting data and force building of the current extent.
            stack.push(EMPTY_ADDR);
            // The node was deleted because of retention process we should
            continue;  // with the next rescue point which may be newer.
        }
        const SubtreeRef* curr_pref = block->get_cheader<SubtreeRef>();
        if (curr_pref->type == NBTreeBlockType::LEAF) {
            IOVecLeaf leaf(std::move(block));
            SubtreeRef ref = INIT_SUBTREE_REF;
            status = init_subtree_from_leaf(leaf, ref);
            if (status != AKU_SUCCESS) {
                Logger::msg(AKU_LOG_ERROR, std::to_string(id_) + " Can't summarize leaf node at " +
                                           std::to_string(curr) + " error: " +
                                           StatusUtil::str(status));
            }
            ref.addr = curr;
            refs.push_back(ref);
            stack.push(leaf.get_prev_addr());  // get_prev_addr() can return EMPTY_ADDR
        } else {
            IOVecSuperblock sblock(std::move(block));
            SubtreeRef ref = INIT_SUBTREE_REF;
            status = init_subtree_from_subtree(sblock, ref);
            if (status != AKU_SUCCESS) {
                Logger::msg(AKU_LOG_ERROR, std::to_string(id_) + " Can't summarize inner node at " +
                                           std::to_string(curr) + " error: " +
                                           StatusUtil::str(status));
            }
            ref.addr = curr;
            refs.push_back(ref);
            stack.push(sblock.get_prev_addr());  // get_prev_addr() can return EMPTY_ADDR
        }
    }
}

void NBTreeExtentsList::init() {
    initialized_ = true;
    if (rescue_points_.empty() == false) {
        auto rstat = repair_status(rescue_points_);
        // Tree should be opened normally.
        if (rstat == RepairStatus::OK) {
            open();
        }
        // Tree should be restored (crush recovery kicks in here).
        else {
            repair();
        }
    }
}

std::unique_ptr<RealValuedOperator> NBTreeExtentsList::search(aku_Timestamp begin, aku_Timestamp end) const {
    if (!initialized_) {
        const_cast<NBTreeExtentsList*>(this)->force_init();
    }
    SharedLock lock(lock_);
    std::vector<std::unique_ptr<RealValuedOperator>> iterators;
    if (begin < end) {
        for (auto it = extents_.rbegin(); it != extents_.rend(); it++) {
            iterators.push_back((*it)->search(begin, end));
        }
    } else {
        for (auto const& root: extents_) {
            iterators.push_back(root->search(begin, end));
        }
    }
    if (iterators.size() == 1) {
        return std::move(iterators.front());
    }
    std::unique_ptr<RealValuedOperator> concat;
    concat.reset(new ChainOperator(std::move(iterators)));
    return concat;
}

std::unique_ptr<RealValuedOperator> NBTreeExtentsList::filter(aku_Timestamp begin,
                                                              aku_Timestamp end,
                                                              const ValueFilter& filter) const
{
    if (!initialized_) {
        const_cast<NBTreeExtentsList*>(this)->force_init();
    }
    SharedLock lock(lock_);
    std::vector<std::unique_ptr<RealValuedOperator>> iterators;
    if (begin < end) {
        for (auto it = extents_.rbegin(); it != extents_.rend(); it++) {
            iterators.push_back((*it)->filter(begin, end, filter));
        }
    } else {
        for (auto const& root: extents_) {
            iterators.push_back(root->filter(begin, end, filter));
        }
    }
    if (iterators.size() == 1) {
        return std::move(iterators.front());
    }
    std::unique_ptr<RealValuedOperator> concat;
    concat.reset(new ChainOperator(std::move(iterators)));
    return concat;
}

std::unique_ptr<AggregateOperator> NBTreeExtentsList::aggregate(aku_Timestamp begin, aku_Timestamp end) const {
    if (!initialized_) {
        const_cast<NBTreeExtentsList*>(this)->force_init();
    }
    SharedLock lock(lock_);
    std::vector<std::unique_ptr<AggregateOperator>> iterators;
    if (begin < end) {
        for (auto it = extents_.rbegin(); it != extents_.rend(); it++) {
            iterators.push_back((*it)->aggregate(begin, end));
        }
    } else {
        for (auto const& root: extents_) {
            iterators.push_back(root->aggregate(begin, end));
        }
    }
    if (iterators.size() == 1) {
        return std::move(iterators.front());
    }
    std::unique_ptr<AggregateOperator> concat;
    concat.reset(new CombineAggregateOperator(std::move(iterators)));
    return concat;

}

std::unique_ptr<AggregateOperator> NBTreeExtentsList::group_aggregate(aku_Timestamp begin, aku_Timestamp end, aku_Timestamp step) const {
    if (!initialized_) {
        const_cast<NBTreeExtentsList*>(this)->force_init();
    }
    SharedLock lock(lock_);
    std::vector<std::unique_ptr<AggregateOperator>> iterators;
    if (begin < end) {
        for (auto it = extents_.rbegin(); it != extents_.rend(); it++) {
            iterators.push_back((*it)->group_aggregate(begin, end, step));
        }
    } else {
        for (auto const& root: extents_) {
            iterators.push_back(root->group_aggregate(begin, end, step));
        }
    }
    std::unique_ptr<AggregateOperator> concat;
    concat.reset(new CombineGroupAggregateOperator(begin, end, step, std::move(iterators)));
    return concat;
}

std::unique_ptr<AggregateOperator> NBTreeExtentsList::group_aggregate_filter(aku_Timestamp begin,
                                                                             aku_Timestamp end,
                                                                             aku_Timestamp step,
                                                                             const AggregateFilter &filter) const
{
    auto iter = group_aggregate(begin, end, step);
    std::unique_ptr<AggregateOperator> result;
    result.reset(new NBTreeGroupAggregateFilter(filter, std::move(iter)));
    return result;
}

std::unique_ptr<AggregateOperator> NBTreeExtentsList::candlesticks(aku_Timestamp begin, aku_Timestamp end, NBTreeCandlestickHint hint) const {
    if (!initialized_) {
        const_cast<NBTreeExtentsList*>(this)->force_init();
    }
    SharedLock lock(lock_);
    std::vector<std::unique_ptr<AggregateOperator>> iterators;
    if (begin < end) {
        for (auto it = extents_.rbegin(); it != extents_.rend(); it++) {
            iterators.push_back((*it)->candlesticks(begin, end, hint));
        }
    } else {
        for (auto const& root: extents_) {
            iterators.push_back(root->candlesticks(begin, end, hint));
        }
    }
    if (iterators.size() == 1) {
        return std::move(iterators.front());
    }
    std::unique_ptr<AggregateOperator> concat;
    // NOTE: there is no intersections between extents so we can join iterators
    concat.reset(new CombineAggregateOperator(std::move(iterators)));
    return concat;
}


std::vector<LogicAddr> NBTreeExtentsList::close() {
    UniqueLock lock(lock_);
    if (initialized_) {
        if (write_count_) {
            Logger::msg(AKU_LOG_TRACE, std::to_string(id_) + " Going to close the tree.");
            LogicAddr addr = EMPTY_ADDR;
            bool parent_saved = false;
            for(size_t index = 0ul; index < extents_.size(); index++) {
                if (extents_.at(index)->is_dirty()) {
                    std::tie(parent_saved, addr) = extents_.at(index)->commit(true);
                }
            }
            assert(!parent_saved);
            // NOTE: at this point `addr` should contain address of the tree's root.
            std::vector<LogicAddr> result(rescue_points_.size(), EMPTY_ADDR);
            result.back() = addr;
            std::swap(rescue_points_, result);
        } else {
            // Special case, tree was opened but left unmodified
            if (rescue_points_.size() == 2 && rescue_points_.back() == EMPTY_ADDR) {
                rescue_points_.pop_back();
            }
        }
    }
    #ifdef AKU_UNIT_TEST_CONTEXT
    // This code should be executed only from unit-test.
    if (extents_.size() > 1) {
        NBTreeExtent::check_extent(extents_.back().get(), bstore_, extents_.size() - 1);
    }
    #endif
    // This node is not initialized now but can be restored from `rescue_points_` list.
    extents_.clear();
    initialized_ = false;
    // roots should be a list of EMPTY_ADDR values followed by
    // the address of the root node [E, E, E.., rootaddr].
    return rescue_points_;
}

std::vector<LogicAddr> NBTreeExtentsList::get_roots() const {
    SharedLock lock(lock_);
    return rescue_points_;
}

std::vector<LogicAddr> NBTreeExtentsList::_get_roots() const {
    return rescue_points_;
}

NBTreeExtentsList::RepairStatus NBTreeExtentsList::repair_status(std::vector<LogicAddr> const& rescue_points) {
    ssize_t count = static_cast<ssize_t>(rescue_points.size()) -
                    std::count(rescue_points.begin(), rescue_points.end(), EMPTY_ADDR);
    if (count == 1 && rescue_points.back() != EMPTY_ADDR) {
        return RepairStatus::OK;
    }
    return RepairStatus::REPAIR;
}


static NBTreeBlockType _dbg_get_block_type(const std::unique_ptr<IOVecBlock>& block) {
    auto ref = block->get_cheader<SubtreeRef>();
    return ref->level == 0 ? NBTreeBlockType::LEAF : NBTreeBlockType::INNER;
}

void NBTreeExtentsList::debug_print(LogicAddr root, std::shared_ptr<BlockStore> bstore, size_t depth) {
    std::string pad(depth, ' ');
    if (root == EMPTY_ADDR) {
        std::cout << pad << "EMPTY_ADDR" << std::endl;
        return;
    }
    aku_Status status;
    std::unique_ptr<IOVecBlock> block;
    std::tie(status, block) = read_and_check(bstore, root);
    if (status != AKU_SUCCESS) {
        std::cout << pad << "ERROR: Can't read block at " << root << " " << StatusUtil::str(status) << std::endl;
    }
    auto type = _dbg_get_block_type(block);
    if (type == NBTreeBlockType::LEAF) {
        IOVecLeaf leaf(std::move(block));
        std::vector<aku_Timestamp> ts;
        std::vector<double> xs;
        status = leaf.read_all(&ts, &xs);
        if (status != AKU_SUCCESS) {
            std::cout << pad << "ERROR: Can't decompress block at " << root << " " << StatusUtil::str(status) << std::endl;
        }
        std::cout << pad << "Leaf at " << root << " TS: [" << ts.front() << ", " << ts.back() << "]" << std::endl;
        std::cout << pad << "        " << root << " XS: [" << ts.front() << ", " << ts.back() << "]" << std::endl;
    } else {
        IOVecSuperblock inner(root, bstore);
        std::vector<SubtreeRef> refs;
        status = inner.read_all(&refs);
        if (status != AKU_SUCCESS) {
            std::cout << pad << "ERROR: Can't decompress superblock at " << root << " " << StatusUtil::str(status) << std::endl;
        }
        std::cout << pad << "Node at " << root << " TS: [" << refs.front().begin << ", " << refs.back().end << "]" << std::endl;
        for (SubtreeRef ref: refs) {
            std::cout << pad << "- node: " << ref.addr << std::endl;
            std::cout << pad << "- TS: [" << ref.begin << ", " << ref.end << "]" << std::endl;
            std::cout << pad << "- level: " << ref.level << std::endl;
            std::cout << pad << "- fanout index: " << ref.fanout_index << std::endl;
            debug_print(ref.addr, bstore, depth + 4);
        }
    }
}

}}
