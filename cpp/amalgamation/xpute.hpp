// MIT License
//
// Copyright (c) 2025 canplane
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// xpute.hpp
//
// GENERATED from cpp/xpute/ by `make -C cpp amalgamation` (cpp/amalgamation.awk) — do not edit.
//
// xpute's C++ library in one header: every header of cpp/xpute/, each after
// the ones it includes. Compile xpute.cpp, beside it, once into the program.

#pragma once

// cpp/xpute/kit/status/verbose_abort.hpp

// A failed check in libc++'s headers is a broken invariant, so it is a bug on
// every target, reported with the check's own message (bug.cpp
// `xpute_verbose_abort`). libc++'s headers reach it
// through `_LIBCPP_VERBOSE_ABORT`, a macro a program may define before the
// first of them, so this header includes none: the tree's files are compiled
// with it included first (Makefile), and the amalgamation's header opens with
// it. libc++'s compiled library calls its own handler instead, which only a
// module replaces (bug.cpp).

extern "C" [[noreturn]] void xpute_verbose_abort(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));

#define _LIBCPP_VERBOSE_ABORT(...) ::xpute_verbose_abort(__VA_ARGS__)

// cpp/xpute/kit/abi/word.hpp

// These are bit fields in 32- and 64-bit words. A field is `nbit` bits wide and
// starts at bit `shamt`; the caller keeps it inside the word. `field_mask` also
// takes a field as wide as the whole word, which a plain shift cannot express.
// A 64-bit word is also two 32-bit ones, its low word first (`split64`,
// `join64`), as a stream of 32-bit words carries it.

#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace xpute {

// The two 32-bit words of `v`, its low word first.
constexpr std::array<std::uint32_t, 2> split64(std::uint64_t v) noexcept {
    return {static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(v >> 32)};
}

// The 64-bit word whose low and high 32-bit words are `lo` and `hi`.
constexpr std::uint64_t join64(std::uint32_t lo, std::uint32_t hi) noexcept {
    return std::uint64_t{hi} << 32 | lo;
}

template <class W>
concept Word = std::same_as<W, std::uint32_t> || std::same_as<W, std::uint64_t>;

template <Word W> constexpr W bit(unsigned shamt) noexcept {
    return W{1} << shamt;
}

template <Word W> constexpr W field_mask(unsigned nbit, unsigned shamt) noexcept {
    constexpr unsigned bits = std::numeric_limits<W>::digits;
    W field = nbit >= bits ? std::numeric_limits<W>::max() : (W{1} << nbit) - 1;
    return shamt >= bits ? W{0} : W(field << shamt);
}

template <Word W> constexpr W field_get(W x, unsigned shamt, W mask) noexcept {
    return x >> shamt & mask;
}

template <Word W> constexpr W field_set(W x, unsigned shamt, W mask, W v) noexcept {
    return W((x & ~W(mask << shamt)) | W((v & mask) << shamt));
}

template <Word W> constexpr std::make_signed_t<W> as_int_n(unsigned nbit, W x) noexcept {
    constexpr unsigned bits = std::numeric_limits<W>::digits;
    return std::make_signed_t<W>(W(x << (bits - nbit))) >> (bits - nbit);
}

} // namespace xpute

// cpp/xpute/kit/alloc/buddy_tree.hpp

// Copyright 2018 Evan Wallace
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// This is the guest's page allocator, and `slab.hpp` serves anything smaller
// than a page from it. It is a port of evanw/buddy-malloc that follows the C
// line for line, and departs from it in two places. `update_max_ptr` asks the
// `Range` for memory where the C calls `brk`. A block carries no 8-byte header
// with its size, so every caller passes the size it asked for back to `free`.

#include <cstddef>

namespace xpute {

// The width in bits that a range's size and the buddy's exponents fit in; the
// range's base alone is a full pointer.
inline constexpr std::size_t ADDR_BITS = 32;

// The memory this allocator manages. A block is `2^MIN_LOG2` bytes at the
// smallest and `2^MAX_LOG2` at the largest. `MAX_LOG2` bounds the address
// space the tree can describe, not the memory that exists: `end` is where the
// real memory stops.
template <class R>
concept Range = requires {
    { R::MIN_LOG2 } -> std::convertible_to<std::size_t>;
    { R::MAX_LOG2 } -> std::convertible_to<std::size_t>;
    { R::base() } -> std::convertible_to<std::uintptr_t>;
    { R::end() } -> std::convertible_to<std::uintptr_t>;
};

template <Range R> class BuddyMalloc {
    static constexpr std::size_t BUCKET_CNT = R::MAX_LOG2 - R::MIN_LOG2 + 1;
    // The split bits take `2^(BUCKET_CNT - 1) / 8` bytes, which is none at
    // all below four buckets. Past `ADDR_BITS + 1` buckets, one for each
    // exponent from 0 through `ADDR_BITS`, the tree would describe more than
    // a range can address.
    static_assert(BUCKET_CNT >= 4 && BUCKET_CNT <= ADDR_BITS + 1, "a buddy tree's bucket count");

    struct List {
        List *prev;
        List *next;
    };
    static_assert((std::size_t{1} << R::MIN_LOG2) >= sizeof(List), "a free block holds its list links");

  public:
    BuddyMalloc() noexcept = default;
    // A `BuddyMalloc` must not move once it has handed out a block: each
    // bucket's head is the sentinel of its own circular list, and the free
    // blocks point at it.
    BuddyMalloc(const BuddyMalloc &) = delete;
    BuddyMalloc &operator=(const BuddyMalloc &) = delete;

    // The most bytes of the range the allocator has ever reached into, not
    // the bytes live now.
    std::size_t peak() const noexcept { return max_ptr_ - base_ptr_; }
    std::size_t live() const noexcept { return live_; }
    std::size_t range_sz() const noexcept { return end_ptr_ - base_ptr_; }
    // Changes exactly when a request refused before might now be served, so
    // a caller can wait for it to change before it retries.
    std::uint32_t releases() const noexcept { return releases_; }
    // Where the host reads `base_ptr_`, `max_ptr_`, `end_ptr_` and `live_`,
    // then `releases_` and `allocs_` as 32-bit words, in place and in that
    // order. The order of the fields below is part of the contract with the
    // host.
    const std::uintptr_t *stat() const noexcept { return &base_ptr_; }
    std::size_t allocs() const noexcept { return allocs_; }

    std::size_t largest_free() const noexcept {
        if (base_ptr_ == 0) return 0;
        for (std::size_t b = bucket_limit_; b < BUCKET_CNT; b++) {
            if (buckets_[b].next != &buckets_[b]) return std::size_t{1} << (R::MAX_LOG2 - b);
        }
        return 0;
    }

    // Returns null when no block can be had. A block's offset from the
    // range's base is a multiple of its own size, so a caller can find a
    // block's start from any address inside it by masking the offset.
    void *malloc(std::size_t request) noexcept {
        // The first call sets the tree up as one free node of the smallest
        // size; more of the range is taken only as larger blocks are asked
        // for.
        if (base_ptr_ == 0) {
            base_ptr_ = R::base();
            max_ptr_ = base_ptr_;
            end_ptr_ = R::end();
            bucket_limit_ = BUCKET_CNT - 1;
            update_max_ptr(base_ptr_ + sizeof(List));
            list_init(BUCKET_CNT - 1);
            list_push(BUCKET_CNT - 1, reinterpret_cast<List *>(base_ptr_));
        }

        if (request > std::size_t{1} << R::MAX_LOG2) return nullptr;

        std::size_t original_bucket = bucket_for_request(request);
        std::size_t bucket = original_bucket;

        // The loop looks for a free block at least as large as needed, moving
        // to larger buckets when there is no exact match; a larger block is
        // split below.
        for (;;) {
            if (!lower_bucket_limit(bucket)) return nullptr;

            List *ptr = list_pop(bucket);
            if (ptr == nullptr) {
                // When this is not the root's bucket, the search goes on to
                // the next larger one; at bucket 0 there is no larger one, and
                // the request fails.
                if (bucket != bucket_limit_ || bucket == 0) {
                    if (bucket == 0) return nullptr;
                    bucket--;
                    continue;
                }
                // This bucket is the root's and the root is in use. Growing
                // the tree one level puts the root's new right sibling on
                // this bucket's free list.
                if (!lower_bucket_limit(bucket - 1)) return nullptr;
                ptr = list_pop(bucket);
            }

            // The block's memory is reserved before anything is written to
            // it; when the range has no room, the block goes back and the
            // request fails.
            std::size_t sz = std::size_t{1} << (R::MAX_LOG2 - bucket);
            std::size_t need_sz = bucket < original_bucket ? sz / 2 + sizeof(List) : sz;
            if (!update_max_ptr(reinterpret_cast<std::uintptr_t>(ptr) + need_sz)) {
                list_push(bucket, ptr);
                return nullptr;
            }

            // A parent's bit is the XOR of its children's UNUSED flags, so
            // taking this block flips its parent's bit. The grandparent's bit
            // stays: this block's buddy is USED, so the parent was never
            // UNUSED.
            std::size_t i = node_for_ptr(ptr, bucket);
            if (i != 0) flip_parent_is_split(i);

            // A block larger than needed is split down to size. Each step
            // keeps the left half and puts the right half on its bucket's free
            // list.
            while (bucket < original_bucket) {
                i = i * 2 + 1;
                bucket++;
                flip_parent_is_split(i);
                list_push(bucket, ptr_for_node(i + 1, bucket));
            }

            live_ += std::size_t{1} << (R::MAX_LOG2 - original_bucket);
            allocs_++;
            return ptr;
        }
    }

    void free(void *at, std::size_t request) noexcept {
        if (at == nullptr) return;
        List *ptr = static_cast<List *>(at);
        std::size_t bucket = bucket_for_request(request);
        live_ -= std::size_t{1} << (R::MAX_LOG2 - bucket);
        releases_++;
        std::size_t i = node_for_ptr(ptr, bucket);

        // The freed block becomes UNUSED, and while its buddy is UNUSED too the
        // two merge into their parent, up toward the root.
        while (i != 0) {
            flip_parent_is_split(i);
            // After the flip, the parent's bit says SPLIT exactly when the
            // buddy is still USED, and the two cannot merge. The tree's
            // current root has no buddy to merge with.
            if (parent_is_split(i) || bucket == bucket_limit_) break;
            list_remove(ptr_for_node(((i - 1) ^ 1) + 1, bucket));
            i = (i - 1) / 2;
            bucket--;
        }

        // The block goes on the back of its bucket's list, and `malloc` takes
        // from the back, so a free followed by a malloc of the same size hands
        // back the same address.
        list_push(bucket, ptr_for_node(i, bucket));
    }

  private:
    // This stands in for the C's `brk`: the reached memory grows up to the
    // range's end and is refused past it.
    bool update_max_ptr(std::uintptr_t new_val) noexcept {
        if (new_val > max_ptr_) {
            if (new_val > end_ptr_) return false;
            max_ptr_ = new_val;
        }
        return true;
    }

    // A bucket's first node is `(1 << bucket) - 1`, so `idx + 1` is taken
    // before the subtraction, which then never goes below zero.
    List *ptr_for_node(std::size_t idx, std::size_t bucket) const noexcept {
        return reinterpret_cast<List *>(base_ptr_ + ((idx + 1 - (std::size_t{1} << bucket)) << (R::MAX_LOG2 - bucket)));
    }

    std::size_t node_for_ptr(List *ptr, std::size_t bucket) const noexcept {
        return ((reinterpret_cast<std::uintptr_t>(ptr) - base_ptr_) >> (R::MAX_LOG2 - bucket)) + (std::size_t{1} << bucket) - 1;
    }

    bool parent_is_split(std::size_t idx) const noexcept {
        const std::size_t parent = (idx - 1) / 2;
        return (node_is_split_[parent / 8] >> (parent % 8) & 1) == 1;
    }

    void flip_parent_is_split(std::size_t idx) noexcept {
        const std::size_t parent = (idx - 1) / 2;
        node_is_split_[parent / 8] ^= static_cast<std::uint8_t>(1 << (parent % 8));
    }

    std::size_t bucket_for_request(std::size_t request) const noexcept {
        std::size_t bucket = BUCKET_CNT - 1;
        std::size_t sz = std::size_t{1} << R::MIN_LOG2;
        while (sz < request) {
            bucket--;
            sz *= 2;
        }
        return bucket;
    }

    void list_init(std::size_t bucket) noexcept {
        buckets_[bucket].prev = &buckets_[bucket];
        buckets_[bucket].next = &buckets_[bucket];
    }

    void list_push(std::size_t bucket, List *entry) noexcept {
        List *list = &buckets_[bucket];
        List *prev = list->prev;
        entry->prev = prev;
        entry->next = list;
        prev->next = entry;
        list->prev = entry;
    }

    static void list_remove(List *entry) noexcept {
        entry->prev->next = entry->next;
        entry->next->prev = entry->prev;
    }

    List *list_pop(std::size_t bucket) noexcept {
        List *list = &buckets_[bucket];
        List *back = list->prev;
        if (back == list) return nullptr;
        list_remove(back);
        return back;
    }

    // Doubles the tree until its root is at `bucket`. The root is at
    // `bucket_limit_`, and each doubling lowers it by one. Returns false when
    // the range has no room for the links of a new free block.
    bool lower_bucket_limit(std::size_t bucket) noexcept {
        while (bucket < bucket_limit_) {
            std::size_t root = node_for_ptr(reinterpret_cast<List *>(base_ptr_), bucket_limit_);
            // The root is UNUSED, so the whole tree is free and the new,
            // larger root becomes one free block.
            if (!parent_is_split(root)) {
                list_remove(reinterpret_cast<List *>(base_ptr_));
                bucket_limit_--;
                list_init(bucket_limit_);
                list_push(bucket_limit_, reinterpret_cast<List *>(base_ptr_));
                continue;
            }
            // The root is in use, so the new root is SPLIT: its left child is
            // the old root and its right child a new free block, whose links
            // are reserved before they are written.
            List *right_child = ptr_for_node(root + 1, bucket_limit_);
            if (!update_max_ptr(reinterpret_cast<std::uintptr_t>(right_child) + sizeof(List))) return false;
            list_push(bucket_limit_, right_child);
            bucket_limit_--;
            list_init(bucket_limit_);
            // The bit in the new root's parent is flipped, so that a further
            // doubling sees the new root as in use.
            root = (root - 1) / 2;
            if (root != 0) flip_parent_is_split(root);
        }
        return true;
    }

    std::array<List, BUCKET_CNT> buckets_{};
    std::size_t bucket_limit_ = 0;
    // A tree laid out in an array: node `i` has its parent at `(i - 1) / 2`,
    // its children at `2i + 1` and `2i + 2`, and its buddy at
    // `((i - 1) ^ 1) + 1`. Each parent keeps one bit, the XOR of its two
    // children's UNUSED flags. A node's state (UNUSED, SPLIT or USED) is not
    // stored; the walks tell it from these bits.
    std::array<std::uint8_t, (std::size_t{1} << (BUCKET_CNT - 1)) / 8> node_is_split_{};
    std::uintptr_t base_ptr_ = 0;
    std::uintptr_t max_ptr_ = 0;
    std::uintptr_t end_ptr_ = 0;
    std::size_t live_ = 0;
    std::uint32_t releases_ = 0;
    std::uint32_t allocs_ = 0;
};

} // namespace xpute

// cpp/xpute/kit/alloc/slab.hpp

// This is a slab allocator of size classes for requests smaller than a page,
// over the buddy allocator of buddy_tree.hpp, whose smallest block is a page.
// Each class borrows a run of pages from the buddy and carves it into slots
// as SLUB does: the free slots form a list threaded through themselves, and
// the run's header sits in its leading slots. A run goes back to the buddy
// when its last slot is freed. A request of a page or more goes straight to
// the buddy.
//
// A slot finds its run by masking its offset from the range's base, which
// works because the buddy aligns every block to its own size from the base.
// Masking the address would not: the base is aligned only to a page, and a run
// can be larger than a page.

#include <algorithm>
#include <bit>

namespace xpute {

template <Range R> class SlabMalloc {
    // The smallest class is sixteen bytes: below that, a class costs more in
    // run headers than it saves.
    static constexpr std::size_t MIN_CLASS_LOG2 = 4;
    // A run holds at least sixteen slots, so its header takes a small share of
    // the run.
    static constexpr std::size_t MIN_SLOTS_LOG2 = 4;
    static constexpr std::size_t CLASS_CNT = R::MIN_LOG2 - MIN_CLASS_LOG2;

    struct Run {
        Run *prev;
        Run *next;
        std::uint8_t *free;
        std::uint32_t inuse;
    };

    static_assert((std::size_t{1} << MIN_CLASS_LOG2) >= sizeof(void *), "a free slot holds the address of the next");

    static constexpr std::size_t class_log2(std::size_t c) noexcept { return MIN_CLASS_LOG2 + c; }

    static constexpr std::size_t run_log2(std::size_t c) noexcept { return std::max(class_log2(c) + MIN_SLOTS_LOG2, std::size_t{R::MIN_LOG2}); }

    static constexpr std::size_t head_slots(std::size_t c) noexcept { return (sizeof(Run) + (std::size_t{1} << class_log2(c)) - 1) >> class_log2(c); }

    static constexpr std::size_t slots(std::size_t c) noexcept { return std::size_t{1} << (run_log2(c) - class_log2(c)); }

    static constexpr bool every_run_has_a_slot() noexcept {
        for (std::size_t c = 0; c < CLASS_CNT; c++) {
            if (head_slots(c) >= slots(c)) return false;
        }
        return true;
    }
    static_assert(every_run_has_a_slot(), "a run has a slot to hand out past the ones its header takes");

  public:
    SlabMalloc() noexcept = default;
    SlabMalloc(const SlabMalloc &) = delete;
    SlabMalloc &operator=(const SlabMalloc &) = delete;

    BuddyMalloc<R> &pages() noexcept { return buddy_; }
    const BuddyMalloc<R> &pages() const noexcept { return buddy_; }
    const std::uintptr_t *stat() const noexcept { return buddy_.stat(); }

    std::size_t peak() const noexcept { return buddy_.peak(); }
    std::size_t live() const noexcept { return buddy_.live(); }
    std::size_t range_sz() const noexcept { return buddy_.range_sz(); }
    std::uint32_t releases() const noexcept { return buddy_.releases(); }
    std::size_t largest_free() const noexcept { return buddy_.largest_free(); }
    std::size_t allocs() const noexcept { return allocs_; }
    std::uint32_t refusals() const noexcept { return refusals_; }
    std::size_t run_live() const noexcept { return run_live_; }
    std::size_t slot_live() const noexcept { return slot_live_; }

    void *alloc(std::size_t bytes, std::size_t align) noexcept {
        std::size_t c;
        if (!class_for(bytes, align, c)) {
            // A page or more goes to the buddy. The range's base is aligned
            // only to a page, so no block is sure of a larger alignment.
            void *at = align > std::size_t{1} << R::MIN_LOG2 ? nullptr : buddy_.malloc(bytes);
            if (at == nullptr) refusals_++;
            else allocs_++;
            return at;
        }
        Run *run = partial_[c];
        if (run == nullptr) {
            run = open_run(c);
            if (run == nullptr) {
                refusals_++;
                return nullptr;
            }
        }
        std::uint8_t *slot = run->free;
        run->free = next_of(slot);
        run->inuse++;
        if (run->free == nullptr) drop_partial(c, run);
        slot_live_ += std::size_t{1} << class_log2(c);
        allocs_++;
        return slot;
    }

    void dealloc(void *at, std::size_t bytes, std::size_t align) noexcept {
        std::size_t c;
        if (!class_for(bytes, align, c)) {
            buddy_.free(at, bytes);
            return;
        }
        std::size_t run_sz = std::size_t{1} << run_log2(c);
        std::uintptr_t base = R::base();
        Run *run = reinterpret_cast<Run *>(base + ((reinterpret_cast<std::uintptr_t>(at) - base) & ~(run_sz - 1)));
        std::uint8_t *slot = static_cast<std::uint8_t *>(at);
        bool was_full = run->free == nullptr;
        set_next(slot, run->free);
        run->free = slot;
        run->inuse--;
        slot_live_ -= std::size_t{1} << class_log2(c);
        if (run->inuse == 0) {
            // The run's last slot is free, so the run goes back to the buddy,
            // where any class can have its pages, rather than staying with
            // this class.
            if (!was_full) drop_partial(c, run);
            run_live_ -= run_sz;
            buddy_.free(run, run_sz);
        } else if (was_full) {
            push_partial(c, run);
        }
    }

    void *malloc(std::size_t bytes) noexcept { return alloc(bytes, 8); }
    void free(void *at, std::size_t bytes) noexcept { dealloc(at, bytes, 8); }

  private:
    // `class_for` gives the class for a request, or false when the buddy
    // serves it. A slot is aligned to its own size, since it sits at a
    // multiple of its class from a run base aligned to the run's size, so a
    // request's alignment is met by raising its size to the alignment.
    static bool class_for(std::size_t bytes, std::size_t align, std::size_t &c) noexcept {
        std::size_t need = std::max({bytes, align, std::size_t{1}});
        if (need >= std::size_t{1} << R::MIN_LOG2) return false;
        std::size_t log2 = std::max<std::size_t>(std::countr_zero(std::bit_ceil(need)), MIN_CLASS_LOG2);
        c = log2 - MIN_CLASS_LOG2;
        return true;
    }

    static std::uint8_t *next_of(std::uint8_t *slot) noexcept { return *reinterpret_cast<std::uint8_t **>(slot); }
    static void set_next(std::uint8_t *slot, std::uint8_t *next) noexcept { *reinterpret_cast<std::uint8_t **>(slot) = next; }

    Run *open_run(std::size_t c) noexcept {
        std::size_t run_sz = std::size_t{1} << run_log2(c);
        auto *at = static_cast<std::uint8_t *>(buddy_.malloc(run_sz));
        if (at == nullptr) return nullptr;
        std::size_t class_sz = std::size_t{1} << class_log2(c);
        std::uint8_t *next = nullptr;
        // The free list is built from the last slot back, so the first slots
        // handed out have the lowest addresses.
        for (std::size_t i = slots(c); i > head_slots(c);) {
            i--;
            set_next(at + i * class_sz, next);
            next = at + i * class_sz;
        }
        run_live_ += run_sz;
        Run *run = reinterpret_cast<Run *>(at);
        run->free = next;
        run->inuse = 0;
        push_partial(c, run);
        return run;
    }

    void push_partial(std::size_t c, Run *run) noexcept {
        Run *head = partial_[c];
        run->prev = nullptr;
        run->next = head;
        if (head != nullptr) head->prev = run;
        partial_[c] = run;
    }

    void drop_partial(std::size_t c, Run *run) noexcept {
        if (run->prev == nullptr) partial_[c] = run->next;
        else run->prev->next = run->next;
        if (run->next != nullptr) run->next->prev = run->prev;
    }

    BuddyMalloc<R> buddy_;
    std::array<Run *, CLASS_CNT> partial_{};
    std::size_t allocs_ = 0;
    std::uint32_t refusals_ = 0;
    std::size_t run_live_ = 0;
    std::size_t slot_live_ = 0;
};

} // namespace xpute

// cpp/xpute/kit/status/errno.spec.hpp
//
// GENERATED from kit/spec/errno.json5 (xpute-gen data.rs)
//
// POSIX/Linux-aligned negative errno

#include <optional>
#include <string_view>

namespace xpute {

// What an errno says happened: the canonical status code of gRPC and Abseil
// (absl::StatusCode), by its number there, each errno's as Abseil's
// ErrnoToStatusCode maps it, save that ENOTRECOVERABLE is internal, and
// EBADMSG and EILSEQ data_loss. It says nothing of whether the error is
// fatal: a broken invariant is a bug, which stops the program.
enum class Kind : std::uint8_t {
    ok = 0,
    cancelled = 1,
    unknown = 2,
    invalid_argument = 3,
    deadline_exceeded = 4,
    not_found = 5,
    already_exists = 6,
    permission_denied = 7,
    resource_exhausted = 8,
    failed_precondition = 9,
    out_of_range = 11,
    unimplemented = 12,
    internal = 13,
    unavailable = 14,
    data_loss = 15,
};

// The member `val` is, or none for a number no member has.
constexpr std::optional<Kind> of_Kind(std::uint8_t val) noexcept {
    switch (val) {
    case 0: return Kind::ok;
    case 1: return Kind::cancelled;
    case 2: return Kind::unknown;
    case 3: return Kind::invalid_argument;
    case 4: return Kind::deadline_exceeded;
    case 5: return Kind::not_found;
    case 6: return Kind::already_exists;
    case 7: return Kind::permission_denied;
    case 8: return Kind::resource_exhausted;
    case 9: return Kind::failed_precondition;
    case 11: return Kind::out_of_range;
    case 12: return Kind::unimplemented;
    case 13: return Kind::internal;
    case 14: return Kind::unavailable;
    case 15: return Kind::data_loss;
    }
    return std::nullopt;
}

enum class Errno : std::int32_t {
    ok = 0,
    eperm = -1,
    enoent = -2,
    esrch = -3,
    eintr = -4,
    eio = -5,
    enxio = -6,
    e2big = -7,
    enoexec = -8,
    ebadf = -9,
    echild = -10,
    eagain = -11,
    enomem = -12,
    eacces = -13,
    efault = -14,
    ebusy = -16,
    eexist = -17,
    exdev = -18,
    enodev = -19,
    enotdir = -20,
    eisdir = -21,
    einval = -22,
    enfile = -23,
    emfile = -24,
    enotty = -25,
    efbig = -27,
    enospc = -28,
    espipe = -29,
    erofs = -30,
    emlink = -31,
    epipe = -32,
    edom = -33,
    erange = -34,
    enosys = -38,
    eproto = -71,
    ebadmsg = -74,
    eoverflow = -75,
    emsgsize = -90,
    enotsup = -95,
    eilseq = -84,
    eaddrinuse = -98,
    eaddrnotavail = -99,
    enetdown = -100,
    enetunreach = -101,
    enetreset = -102,
    econnaborted = -103,
    econnreset = -104,
    enobufs = -105,
    eisconn = -106,
    enotconn = -107,
    eshutdown = -108,
    etoomanyrefs = -109,
    etimedout = -110,
    econnrefused = -111,
    ehostdown = -112,
    ehostunreach = -113,
    ealready = -114,
    einprogress = -115,
    ecanceled = -125,
    enotrecoverable = -131,
};

// The member `val` is, or none for a number no member has.
constexpr std::optional<Errno> of_Errno(std::int32_t val) noexcept {
    switch (val) {
    case 0: return Errno::ok;
    case -1: return Errno::eperm;
    case -2: return Errno::enoent;
    case -3: return Errno::esrch;
    case -4: return Errno::eintr;
    case -5: return Errno::eio;
    case -6: return Errno::enxio;
    case -7: return Errno::e2big;
    case -8: return Errno::enoexec;
    case -9: return Errno::ebadf;
    case -10: return Errno::echild;
    case -11: return Errno::eagain;
    case -12: return Errno::enomem;
    case -13: return Errno::eacces;
    case -14: return Errno::efault;
    case -16: return Errno::ebusy;
    case -17: return Errno::eexist;
    case -18: return Errno::exdev;
    case -19: return Errno::enodev;
    case -20: return Errno::enotdir;
    case -21: return Errno::eisdir;
    case -22: return Errno::einval;
    case -23: return Errno::enfile;
    case -24: return Errno::emfile;
    case -25: return Errno::enotty;
    case -27: return Errno::efbig;
    case -28: return Errno::enospc;
    case -29: return Errno::espipe;
    case -30: return Errno::erofs;
    case -31: return Errno::emlink;
    case -32: return Errno::epipe;
    case -33: return Errno::edom;
    case -34: return Errno::erange;
    case -38: return Errno::enosys;
    case -71: return Errno::eproto;
    case -74: return Errno::ebadmsg;
    case -75: return Errno::eoverflow;
    case -90: return Errno::emsgsize;
    case -95: return Errno::enotsup;
    case -84: return Errno::eilseq;
    case -98: return Errno::eaddrinuse;
    case -99: return Errno::eaddrnotavail;
    case -100: return Errno::enetdown;
    case -101: return Errno::enetunreach;
    case -102: return Errno::enetreset;
    case -103: return Errno::econnaborted;
    case -104: return Errno::econnreset;
    case -105: return Errno::enobufs;
    case -106: return Errno::eisconn;
    case -107: return Errno::enotconn;
    case -108: return Errno::eshutdown;
    case -109: return Errno::etoomanyrefs;
    case -110: return Errno::etimedout;
    case -111: return Errno::econnrefused;
    case -112: return Errno::ehostdown;
    case -113: return Errno::ehostunreach;
    case -114: return Errno::ealready;
    case -115: return Errno::einprogress;
    case -125: return Errno::ecanceled;
    case -131: return Errno::enotrecoverable;
    }
    return std::nullopt;
}

// The member's `kind`.
constexpr Kind kind_Errno(Errno val) noexcept {
    switch (val) {
    case Errno::ok: return Kind::ok;
    case Errno::eperm: return Kind::permission_denied;
    case Errno::enoent: return Kind::not_found;
    case Errno::esrch: return Kind::not_found;
    case Errno::eintr: return Kind::unavailable;
    case Errno::eio: return Kind::unknown;
    case Errno::enxio: return Kind::not_found;
    case Errno::e2big: return Kind::invalid_argument;
    case Errno::enoexec: return Kind::unknown;
    case Errno::ebadf: return Kind::failed_precondition;
    case Errno::echild: return Kind::failed_precondition;
    case Errno::eagain: return Kind::unavailable;
    case Errno::enomem: return Kind::resource_exhausted;
    case Errno::eacces: return Kind::permission_denied;
    case Errno::efault: return Kind::invalid_argument;
    case Errno::ebusy: return Kind::failed_precondition;
    case Errno::eexist: return Kind::already_exists;
    case Errno::exdev: return Kind::unimplemented;
    case Errno::enodev: return Kind::not_found;
    case Errno::enotdir: return Kind::failed_precondition;
    case Errno::eisdir: return Kind::failed_precondition;
    case Errno::einval: return Kind::invalid_argument;
    case Errno::enfile: return Kind::resource_exhausted;
    case Errno::emfile: return Kind::resource_exhausted;
    case Errno::enotty: return Kind::invalid_argument;
    case Errno::efbig: return Kind::out_of_range;
    case Errno::enospc: return Kind::resource_exhausted;
    case Errno::espipe: return Kind::invalid_argument;
    case Errno::erofs: return Kind::permission_denied;
    case Errno::emlink: return Kind::resource_exhausted;
    case Errno::epipe: return Kind::failed_precondition;
    case Errno::edom: return Kind::invalid_argument;
    case Errno::erange: return Kind::out_of_range;
    case Errno::enosys: return Kind::unimplemented;
    case Errno::eproto: return Kind::unknown;
    case Errno::ebadmsg: return Kind::data_loss;
    case Errno::eoverflow: return Kind::out_of_range;
    case Errno::emsgsize: return Kind::unknown;
    case Errno::enotsup: return Kind::unimplemented;
    case Errno::eilseq: return Kind::data_loss;
    case Errno::eaddrinuse: return Kind::failed_precondition;
    case Errno::eaddrnotavail: return Kind::already_exists;
    case Errno::enetdown: return Kind::unavailable;
    case Errno::enetunreach: return Kind::unavailable;
    case Errno::enetreset: return Kind::unavailable;
    case Errno::econnaborted: return Kind::unavailable;
    case Errno::econnreset: return Kind::unavailable;
    case Errno::enobufs: return Kind::resource_exhausted;
    case Errno::eisconn: return Kind::failed_precondition;
    case Errno::enotconn: return Kind::failed_precondition;
    case Errno::eshutdown: return Kind::failed_precondition;
    case Errno::etoomanyrefs: return Kind::unknown;
    case Errno::etimedout: return Kind::deadline_exceeded;
    case Errno::econnrefused: return Kind::unavailable;
    case Errno::ehostdown: return Kind::unavailable;
    case Errno::ehostunreach: return Kind::unavailable;
    case Errno::ealready: return Kind::already_exists;
    case Errno::einprogress: return Kind::unknown;
    case Errno::ecanceled: return Kind::cancelled;
    case Errno::enotrecoverable: return Kind::internal;
    }
    __builtin_unreachable();
}

// The member's `text`.
constexpr std::string_view text_Errno(Errno val) noexcept {
    switch (val) {
    case Errno::ok: return "Success";
    case Errno::eperm: return "Operation not permitted";
    case Errno::enoent: return "No such file or directory / entry not found";
    case Errno::esrch: return "No such process";
    case Errno::eintr: return "Interrupted system call";
    case Errno::eio: return "I/O error";
    case Errno::enxio: return "No such device or address";
    case Errno::e2big: return "Argument list too long / object too large for fixed local cap";
    case Errno::enoexec: return "Exec format error";
    case Errno::ebadf: return "Bad file descriptor / bad handle";
    case Errno::echild: return "No child processes";
    case Errno::eagain: return "Try again / Resource temporarily unavailable";
    case Errno::enomem: return "Out of memory";
    case Errno::eacces: return "Permission denied";
    case Errno::efault: return "Bad address";
    case Errno::ebusy: return "Device or resource busy";
    case Errno::eexist: return "File exists / duplicate bind";
    case Errno::exdev: return "Cross-device link";
    case Errno::enodev: return "No such device";
    case Errno::enotdir: return "Not a directory";
    case Errno::eisdir: return "Is a directory";
    case Errno::einval: return "Invalid argument";
    case Errno::enfile: return "File table overflow (system-wide)";
    case Errno::emfile: return "Too many open files (process)";
    case Errno::enotty: return "Not a tty";
    case Errno::efbig: return "File too large / payload too large";
    case Errno::enospc: return "No space left on device / local mailbox full";
    case Errno::espipe: return "Illegal seek";
    case Errno::erofs: return "Read-only file system";
    case Errno::emlink: return "Too many links";
    case Errno::epipe: return "Broken pipe";
    case Errno::edom: return "Math argument out of domain";
    case Errno::erange: return "Math result not representable";
    case Errno::enosys: return "Function not implemented";
    case Errno::eproto: return "Protocol error";
    case Errno::ebadmsg: return "Bad message (protocol/frame/payload invalid)";
    case Errno::eoverflow: return "Value too large for target type / overflow";
    case Errno::emsgsize: return "Message too long / packet too large";
    case Errno::enotsup: return "Operation not supported";
    case Errno::eilseq: return "Illegal byte sequence (decode/encoding failure)";
    case Errno::eaddrinuse: return "Address already in use";
    case Errno::eaddrnotavail: return "Cannot assign requested address";
    case Errno::enetdown: return "Network is down";
    case Errno::enetunreach: return "Network is unreachable";
    case Errno::enetreset: return "Network dropped connection because of reset";
    case Errno::econnaborted: return "Software caused connection abort";
    case Errno::econnreset: return "Connection reset by peer";
    case Errno::enobufs: return "No buffer space available";
    case Errno::eisconn: return "Transport endpoint is already connected";
    case Errno::enotconn: return "Transport endpoint is not connected";
    case Errno::eshutdown: return "Cannot send after transport endpoint shutdown";
    case Errno::etoomanyrefs: return "Too many references";
    case Errno::etimedout: return "Connection timed out";
    case Errno::econnrefused: return "Connection refused";
    case Errno::ehostdown: return "Host is down";
    case Errno::ehostunreach: return "No route to host";
    case Errno::ealready: return "Operation already in progress";
    case Errno::einprogress: return "Operation now in progress";
    case Errno::ecanceled: return "Operation canceled";
    case Errno::enotrecoverable: return "State not recoverable: an invariant the code stands on does not hold";
    }
    __builtin_unreachable();
}

} // namespace xpute

// cpp/xpute/kit/status/error.hpp

// An error is its errno, the place in the source where it was made, and what
// a person reads of it beyond the errno, where its maker says more; what
// happened is the errno's kind, and across the link it is its errno alone. A
// fault from outside the program is returned as a `Result`; a broken
// invariant is fatal and goes through `bug.hpp`.

#include <cstring>
#include <memory>
#include <source_location>
#include <utility>

namespace xpute {

struct Error {
    Errno errnum;
    std::source_location at;
    // What a person reads beyond the errno, NUL-terminated, made once and
    // never grown, and shared by an error's copies, as an error is passed on
    // by value; null where the maker says nothing more.
    std::shared_ptr<const char[]> msg;

    constexpr Kind kind() const noexcept { return kind_Errno(errnum); }
};

inline Error make_error(Errno errnum, std::string_view msg = {},
                        std::source_location at = std::source_location::current()) noexcept {
    std::shared_ptr<char[]> m;
    if (!msg.empty()) {
        m = std::make_shared<char[]>(msg.size() + 1);
        std::memcpy(m.get(), msg.data(), msg.size());
    }
    return Error{errnum, at, std::move(m)};
}

template <class T> class [[nodiscard]] Result {
  public:
    Result(T val) noexcept : val_(std::move(val)) {}
    Result(Error err) noexcept : err_(std::move(err)) {}

    bool ok() const noexcept { return val_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    // Asking a failed result for its value is the caller's bug, and is not
    // checked.
    T &value() noexcept { return *val_; }
    const T &value() const noexcept { return *val_; }
    T *operator->() noexcept { return &*val_; }
    T &operator*() noexcept { return *val_; }

    const Error &error() const noexcept { return err_; }

  private:
    std::optional<T> val_;
    Error err_{Errno::ok, {}, {}};
};

template <> class [[nodiscard]] Result<void> {
  public:
    Result() noexcept = default;
    Result(Error err) noexcept : err_(std::move(err)), failed_(true) {}

    bool ok() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return ok(); }
    const Error &error() const noexcept { return err_; }

  private:
    Error err_{Errno::ok, {}, {}};
    bool failed_ = false;
};

} // namespace xpute

// cpp/xpute/kit/status/bug.hpp

// A broken invariant stops the program. A bug is an errno, its place in the
// source, at most two numbers and an optional message. It is handed to the
// report that `set_report` installed, and then the program stops: a
// WebAssembly module traps, and a native build returns from the innermost
// `caught` it runs in, as Rust's panic unwinds to its `catch_unwind`, or
// aborts where it runs in none. The message is a static string, never one
// built at the bug, as `std::bad_alloc::what()` is: a full heap is itself a
// bug, so reporting one allocates nothing, and what varies goes in the two
// numbers. A failed check of libc++'s own is told with libc++'s message,
// written into static storage at the stop (bug.cpp `abort_bug`), as a Rust
// panic `bug` did not raise is told with its own.

#ifndef __wasm__
#include <setjmp.h>
#endif

namespace xpute {

using Report = void (*)(Errno errnum, const std::source_location &at, std::uint64_t a, std::uint64_t b, const char *msg);

// Installs the function a bug is reported to. Only the first call takes
// effect.
void set_report(Report report) noexcept;

// `msg` is what a person reads of the bug, or null.
[[noreturn]] void bug(Errno errnum, std::uint64_t a = 0, std::uint64_t b = 0, const char *msg = nullptr, std::source_location at = std::source_location::current()) noexcept;

inline void ensure(bool cond, Errno errnum, std::uint64_t a = 0, std::uint64_t b = 0, std::source_location at = std::source_location::current()) noexcept {
    if (!cond) bug(errnum, a, b, nullptr, at);
}

template <class T> T or_bug(std::optional<T> v, Errno errnum, std::source_location at = std::source_location::current()) noexcept {
    if (!v) bug(errnum, 0, 0, nullptr, at);
    return *v;
}

template <class T> T or_bug(Result<T> v, Errno errnum, std::source_location at = std::source_location::current()) noexcept {
    ensure(v.ok(), errnum, 0, 0, at);
    return std::move(*v);
}

#ifndef __wasm__
namespace bug_detail {
// The innermost `caught` on this thread, which a bug returns to.
sigjmp_buf *&catcher() noexcept;
} // namespace bug_detail

// Runs `f` and answers what it answers, or `trap` where a bug stopped it. A
// bug leaves by `siglongjmp`, so what `f` held is never destroyed: the
// program it belongs to is over. The signal mask is neither saved nor
// restored, so an entry costs no system call.
template <class T, class F> T caught(T trap, F &&f) noexcept {
    sigjmp_buf here;
    sigjmp_buf *outer = std::exchange(bug_detail::catcher(), &here);
    if (sigsetjmp(here, 0) != 0) {
        bug_detail::catcher() = outer;
        return trap;
    }
    T v = f();
    bug_detail::catcher() = outer;
    return v;
}
#endif

} // namespace xpute

// cpp/xpute/kit/alloc/heap.hpp

// A heap over the range of memory a program gives it (`Range`), as
// `xpute-kit`'s `alloc/heap.rs` is Rust's: the slab of size classes over the
// buddy allocator of pages (slab.hpp), as a `std::pmr::memory_resource`,
// which a program picks for what it allocates, as its default resource or a
// container's. A request the range cannot hold is a bug: a memory resource
// answers none with nothing. A program's default resource is every thread's,
// and the slab is one thread's, so where the target runs threads each call
// takes it under one lock (`Lock`): one flag, spun on and then yielded on, as
// Rust's heap takes it. A WebAssembly target without the `atomics` feature
// runs no threads, and takes none.

#include <memory_resource>

#if !defined(__wasm__) || defined(__wasm_atomics__)
#include <atomic>
#include <thread>
#endif

namespace xpute {

template <Range R> class HeapMalloc final : public std::pmr::memory_resource {
  public:
    HeapMalloc() noexcept = default;
    HeapMalloc(const HeapMalloc &) = delete;
    HeapMalloc &operator=(const HeapMalloc &) = delete;

    // The slab, whose counters the program reads.
    const SlabMalloc<R> &slab() const noexcept { return slab_; }

  private:
#if !defined(__wasm__) || defined(__wasm_atomics__)
    // The slab's lock, on a target that runs threads: whether a thread holds
    // the slab.
    class Lock {
      public:
        void take() noexcept {
            // How many times a thread spins on the lock before it gives its
            // time back to the scheduler on each try instead.
            constexpr unsigned SPIN_CNT = 100;
            for (unsigned spins = 0; locked_.test_and_set(std::memory_order_acquire);) {
                if (spins < SPIN_CNT) spins++;
                else std::this_thread::yield();
            }
        }

        void give() noexcept { locked_.clear(std::memory_order_release); }

      private:
        std::atomic_flag locked_;
    };
#else
    // The slab's lock, on a target that runs no threads: none, the slab held
    // by the one thread there is.
    class Lock {
      public:
        void take() noexcept {}
        void give() noexcept {}
    };
#endif

    void *do_allocate(std::size_t n, std::size_t align) override {
        lock_.take();
        void *at = slab_.alloc(n, align);
        lock_.give();
        ensure(at != nullptr, Errno::enomem, n, align);
        return at;
    }

    void do_deallocate(void *at, std::size_t n, std::size_t align) override {
        lock_.take();
        slab_.dealloc(at, n, align);
        lock_.give();
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }

    SlabMalloc<R> slab_;
    Lock lock_;
};

} // namespace xpute

// cpp/xpute/kit/codec/encoding.hpp

// Hex and unpadded base64url, in the forms the other languages write. An
// output span that is too short is a bug; a text that does not decode is an
// EBADMSG error.

#include <span>

namespace xpute {

constexpr std::size_t hex_len(std::size_t bytes) noexcept {
    return bytes * 2;
}

// `out` must hold at least `hex_len(in.size())` characters.
std::size_t hex_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept;

// Accepts hex digits in either case.
Result<std::size_t> hex_decode(std::string_view in, std::span<std::uint8_t> out) noexcept;

constexpr std::size_t base64url_len(std::size_t bytes) noexcept {
    return bytes / 3 * 4 + (bytes % 3 == 0 ? 0 : bytes % 3 + 1);
}

// `out` must hold at least `base64url_len(in.size())` characters.
std::size_t base64url_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept;

// Refuses with EBADMSG a character outside the alphabet, padding, and any bit
// set past the last byte, so that each byte string has one encoding.
Result<std::size_t> base64url_decode(std::string_view in, std::span<std::uint8_t> out) noexcept;

// Reads the UTF-8 character at `i`. `len` is its length in bytes when it is
// well formed. Otherwise `len` is 0 and `bad` is the length of the maximal
// ill-formed prefix there, at least 1.
struct Utf8Step {
    std::size_t len, bad;
};
constexpr Utf8Step utf8_step(std::string_view s, std::size_t i) noexcept {
    auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char b = byte(i);
    std::size_t len = b < 0x80 ? 1 : (b >= 0xc2 && b < 0xe0) ? 2 : (b >= 0xe0 && b < 0xf0) ? 3 : (b >= 0xf0 && b < 0xf5) ? 4 : 0;
    // The allowed range of the second byte depends on the first, which keeps
    // overlong forms and surrogates out.
    std::size_t good = len == 0 ? 0 : 1;
    for (std::size_t k = 1; k < len && i + k < s.size(); k++) {
        unsigned char c = byte(i + k);
        unsigned char lo = 0x80, hi = 0xbf;
        if (k == 1 && b == 0xe0) lo = 0xa0;
        if (k == 1 && b == 0xed) hi = 0x9f;
        if (k == 1 && b == 0xf0) lo = 0x90;
        if (k == 1 && b == 0xf4) hi = 0x8f;
        if (c < lo || c > hi) break;
        good++;
    }
    if (len != 0 && good == len) return {len, 0};
    return {0, good == 0 ? 1 : good};
}

constexpr bool is_utf8(std::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size();) {
        Utf8Step c = utf8_step(s, i);
        if (c.len == 0) return false;
        i += c.len;
    }
    return true;
}

// Hands `put` the text with each maximal ill-formed prefix replaced by one
// U+FFFD, as Rust's `String::from_utf8_lossy` does.
template <class Put> void utf8_lossy(std::string_view s, Put &&put) {
    static constexpr std::string_view FFFD = "\xef\xbf\xbd";
    std::size_t plain = 0, i = 0;
    while (i < s.size()) {
        Utf8Step c = utf8_step(s, i);
        if (c.len != 0) {
            i += c.len;
            continue;
        }
        put(s.substr(plain, i - plain));
        put(FFFD);
        i += c.bad;
        plain = i;
    }
    put(s.substr(plain));
}

} // namespace xpute

// cpp/xpute/kit/codec/hash.hpp

// This is 64-bit FNV-1a. Every language and the spec generator compute the
// same hash of the same bytes.

namespace xpute {

inline constexpr std::uint64_t FNV64_OFF = 0xcbf29ce484222325u;
inline constexpr std::uint64_t FNV64_PRIME = 0x100000001b3u;

constexpr std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t h = FNV64_OFF;
    for (std::uint8_t b : bytes) {
        h ^= b;
        h *= FNV64_PRIME;
    }
    return h;
}

constexpr std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = FNV64_OFF;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= FNV64_PRIME;
    }
    return h;
}

} // namespace xpute

// cpp/xpute/kit/codec/json.hpp

// A JSON reader that allocates nothing. `read_json` checks a text against
// JSON's grammar once; the values it hands out are then read in place, and no
// tree is built. `quoted` writes a string as a JSON string.

namespace xpute {

namespace json_detail {

std::size_t skip_ws(std::string_view s, std::size_t at) noexcept;
void push_utf8(std::uint32_t c, char *out, std::size_t &n) noexcept;

} // namespace json_detail

enum class JsonType : std::uint8_t { null, boolean, number, string, array, object };

// The characters of a `JsonStr`, its escapes decoded. A `\u` escape of a lone
// surrogate reads as U+FFFD.
class JsonCharIter {
  public:
    explicit JsonCharIter(std::string_view rest) noexcept : rest_(rest) {}
    std::optional<char32_t> next() noexcept;

  private:
    std::string_view rest_;
};

// A JSON string as it stands in the text, without its quotes. Its escapes are
// decoded only as its characters are read.
class JsonStr {
  public:
    explicit JsonStr(std::string_view raw) noexcept : raw_(raw) {}
    // This answers the string as it stands, or none when it holds an escape
    // and must be decoded.
    std::optional<std::string_view> as_plain() const noexcept;
    JsonCharIter chars() const noexcept { return JsonCharIter(raw_); }
    bool eq_str(std::string_view s) const noexcept;
    template <class S> void decode_into(S &out) const {
        JsonCharIter c = chars();
        while (std::optional<char32_t> ch = c.next()) {
            char buf[4];
            std::size_t n = 0;
            json_detail::push_utf8(*ch, buf, n);
            out.append(buf, n);
        }
    }
    std::string_view raw() const noexcept { return raw_; }

  private:
    std::string_view raw_;
};

class JsonItemIter;
class JsonFieldIter;

class JsonValue {
  public:
    JsonType type_of() const noexcept;
    bool is_null() const noexcept { return type_of() == JsonType::null; }
    std::optional<bool> as_bool() const noexcept;
    std::optional<double> as_f64() const noexcept;
    std::optional<JsonStr> as_str() const noexcept;
    JsonItemIter items() const noexcept;
    // Every field in the text's order; a key that appears twice is yielded
    // twice.
    JsonFieldIter fields() const noexcept;
    // The last field with that key, which is the one the runtime's
    // `JSON.parse` keeps.
    std::optional<JsonValue> get(std::string_view key) const noexcept;
    std::optional<JsonValue> at(std::size_t idx) const noexcept;
    std::string_view text() const noexcept { return s_; }

  private:
    friend std::optional<JsonValue> read_json(std::string_view) noexcept;
    friend class JsonItemIter;
    friend class JsonFieldIter;
    explicit JsonValue(std::string_view s) noexcept : s_(s) {}
    std::string_view s_;
};

class JsonItemIter {
  public:
    std::optional<JsonValue> next() noexcept;

  private:
    friend class JsonValue;
    JsonItemIter(std::string_view s, std::size_t at) noexcept : s_(s), at_(at) {}
    std::string_view s_;
    std::size_t at_;
};

class JsonFieldIter {
  public:
    std::optional<std::pair<JsonStr, JsonValue>> next() noexcept;

  private:
    friend class JsonValue;
    JsonFieldIter(std::string_view s, std::size_t at) noexcept : s_(s), at_(at) {}
    std::string_view s_;
    std::size_t at_;
};

// The one value the text holds, or none when the text is not exactly one JSON
// value with only whitespace around it.
std::optional<JsonValue> read_json(std::string_view text) noexcept;

// Writes `s` as a JSON string, as `JSON.stringify` writes one: a control
// character other than \n, \r and \t becomes `\u00XX`.
template <class Put> void quoted(std::string_view s, Put &&put) {
    static constexpr char HEX[] = "0123456789abcdef";
    put(std::string_view("\""));
    std::size_t plain = 0;
    for (std::size_t i = 0; i < s.size(); i++) {
        auto c = static_cast<unsigned char>(s[i]);
        std::string_view esc;
        char u[6];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c >= 0x20) continue;
            u[0] = '\\', u[1] = 'u', u[2] = '0', u[3] = '0', u[4] = HEX[c >> 4], u[5] = HEX[c & 15];
            esc = std::string_view(u, 6);
        }
        put(s.substr(plain, i - plain));
        put(esc);
        plain = i + 1;
    }
    put(s.substr(plain));
    put(std::string_view("\""));
}

} // namespace xpute

// cpp/xpute/kit/collection/arena.hpp

// An arena is a pool of slots that are constructed once and reused. Slots are
// handed out in order and taken back by rewinding to an earlier length.
// `truncate` destroys nothing: a slot handed out again is in whatever state
// its last user left it, with its buffers still allocated, and the caller must
// reinitialize it. `Alloc` has no default, because the default would be the
// system's heap; the caller names the memory it means.

#include <vector>

namespace xpute {

template <class T, class Alloc> class ArenaGuard;

struct ArenaConfig {
    std::uint32_t init_cap;
    std::uint32_t max_cap;
};

template <class T, class Alloc> class Arena {
  public:
    static Result<Arena> make(ArenaConfig opts, const Alloc &alloc = Alloc()) {
        Arena arena(opts.max_cap, alloc);
        if (Result<void> r = arena.grow(std::min(opts.init_cap, opts.max_cap)); !r) return r.error();
        return arena;
    }

    std::uint32_t cap() const noexcept { return cap_; }
    std::uint32_t len() const noexcept { return len_; }
    bool is_empty() const noexcept { return len_ == 0; }
    std::uint32_t max_cap() const noexcept { return max_cap_; }

    T &get(std::uint32_t idx) noexcept {
        ensure(idx < mem_.size(), Errno::enotrecoverable, idx, mem_.size());
        return mem_[idx];
    }
    std::span<T> mem() noexcept { return mem_; }
    void set_max_cap(std::uint32_t cap) noexcept { max_cap_ = cap; }

    Result<T *> alloc() {
        if (len_ >= cap_) {
            if (Result<void> r = grow(len_ + 1); !r) return r.error();
        }
        return &mem_[len_++];
    }

    Result<void> reserve(std::uint32_t cnt) {
        std::uint32_t req = len_ > std::numeric_limits<std::uint32_t>::max() - cnt ? std::numeric_limits<std::uint32_t>::max() : len_ + cnt;
        return grow(req);
    }

    // Rewinds to `new_len` slots. A `new_len` above the current length is a
    // bug, not a way to grow.
    void truncate(std::uint32_t new_len) noexcept {
        ensure(new_len <= len_, Errno::einval, new_len, len_);
        len_ = new_len;
    }

    // Remembers the current length and rewinds to it when the guard is
    // destroyed.
    ArenaGuard<T, Alloc> scope() noexcept;

  private:
    Arena(std::uint32_t max_cap, const Alloc &alloc) : mem_(alloc), max_cap_(max_cap) {}

    // A request that saturates, as `reserve` computes it, is refused at
    // `max_cap` rather than wrapping below it. The capacity doubles but stops
    // at `max_cap`, so a request is refused only when it does not fit under
    // `max_cap`.
    Result<void> grow(std::uint32_t req) {
        if (req <= cap_) return {};
        if (req > max_cap_) return make_error(Errno::eoverflow);
        std::uint32_t new_cap = std::clamp(cap_ > max_cap_ / 2 ? max_cap_ : cap_ * 2, req, max_cap_);
        mem_.resize(new_cap);
        cap_ = new_cap;
        return {};
    }

    std::vector<T, Alloc> mem_;
    std::uint32_t max_cap_;
    std::uint32_t cap_ = 0;
    std::uint32_t len_ = 0;
};

template <class T, class Alloc> class ArenaGuard {
  public:
    explicit ArenaGuard(Arena<T, Alloc> &arena) noexcept : arena_(arena), mark_(arena.len()) {}
    ~ArenaGuard() { arena_.truncate(mark_); }
    ArenaGuard(const ArenaGuard &) = delete;
    ArenaGuard &operator=(const ArenaGuard &) = delete;
    Arena<T, Alloc> &operator*() noexcept { return arena_; }
    Arena<T, Alloc> *operator->() noexcept { return &arena_; }

  private:
    Arena<T, Alloc> &arena_;
    std::uint32_t mark_;
};

template <class T, class Alloc> ArenaGuard<T, Alloc> Arena<T, Alloc>::scope() noexcept {
    return ArenaGuard<T, Alloc>(*this);
}

} // namespace xpute

// cpp/xpute/kit/collection/deque.hpp

// A deque is a double-ended queue in a ring over a buffer the caller owns. It
// allocates nothing and never grows: a push onto a full deque is a bug. `head`
// and `len` alone say which slots are live.

namespace xpute {

template <class T> struct Deque {
    std::span<T> buf;
    std::uint32_t head = 0;
    std::uint32_t len = 0;

    std::uint32_t cap() const noexcept { return static_cast<std::uint32_t>(buf.size()); }
    bool is_empty() const noexcept { return len == 0; }
    bool is_full() const noexcept { return len == cap(); }

    const T *front() const noexcept { return len == 0 ? nullptr : &buf[head]; }
    const T *back() const noexcept { return len == 0 ? nullptr : &buf[(head + len - 1) % cap()]; }

    void push_back(T v) noexcept {
        ensure(!is_full(), Errno::enospc, len);
        buf[(head + len) % cap()] = std::move(v);
        len++;
    }

    void push_front(T v) noexcept {
        ensure(!is_full(), Errno::enospc, len);
        head = (head + cap() - 1) % cap();
        buf[head] = std::move(v);
        len++;
    }

    std::optional<T> pop_front() noexcept {
        if (len == 0) return std::nullopt;
        T v = std::exchange(buf[head], T{});
        head = (head + 1) % cap();
        len--;
        return v;
    }

    std::optional<T> pop_back() noexcept {
        if (len == 0) return std::nullopt;
        len--;
        return std::exchange(buf[(head + len) % cap()], T{});
    }

    void clear() noexcept {
        for (std::size_t i = 0; i < len; i++) buf[(head + i) % cap()] = T{};
        head = 0;
        len = 0;
    }
};

} // namespace xpute

// cpp/xpute/kit/collection/heap.hpp

// This is a binary heap over a vector the caller owns. Keys must be totally
// ordered: a key that compares with nothing, as a NaN does, breaks the heap's
// order.

namespace xpute {

template <class K, class T> struct Element {
    K key;
    T val;
    bool operator==(const Element &) const = default;
};

// `MIN` puts the smallest key first.
template <class Vec, bool MIN> class Heap {
  public:
    using E = typename Vec::value_type;

    explicit Heap(Vec &a) noexcept : a_(a) {}

    const E *top() const noexcept { return a_.empty() ? nullptr : &a_[0]; }

    // Sifts the element at `root` down within the first `sz` elements. Both
    // subtrees of `root` must already be heaps.
    static void heapify(std::span<E> a, std::size_t root, std::size_t sz) noexcept {
        std::size_t i = root;
        E e = a[i];
        std::size_t child = (i << 1) + 1;
        while (child < sz) {
            std::size_t r = child + 1;
            if (r < sz && ahead(a[r].key, a[child].key)) child = r;
            if (!ahead(a[child].key, e.key)) break;
            a[i] = a[child];
            i = child;
            child = (i << 1) + 1;
        }
        a[i] = e;
    }

    static void build_of(std::span<E> a) noexcept {
        for (std::size_t i = a.size() >> 1; i-- > 0;) heapify(a, i, a.size());
    }

    void build() noexcept { build_of(a_); }

    void push(E e) {
        std::size_t i = a_.size();
        a_.push_back(e);
        while (i > 0) {
            std::size_t p = (i - 1) >> 1;
            if (!ahead(e.key, a_[p].key)) break;
            a_[i] = a_[p];
            i = p;
        }
        a_[i] = e;
    }

    std::optional<E> pop() noexcept {
        if (a_.empty()) return std::nullopt;
        E out = a_[0];
        a_[0] = a_.back();
        a_.pop_back();
        if (!a_.empty()) heapify(a_, 0, a_.size());
        return out;
    }

    // Replaces the top element and restores the order. The heap must not be
    // empty.
    void replace_root(E e) noexcept {
        a_[0] = e;
        heapify(a_, 0, a_.size());
    }

  private:
    static bool ahead(const auto &x, const auto &y) noexcept {
        if constexpr (MIN) return x < y;
        else return y < x;
    }

    Vec &a_;
};

template <class Vec> using MinHeap = Heap<Vec, true>;
template <class Vec> using MaxHeap = Heap<Vec, false>;

// Sorts by key, ascending and in place, in O(n log n). The sort is not stable.
template <class K, class T> void heapsort(std::span<Element<K, T>> a) noexcept {
    using H = Heap<std::span<Element<K, T>>, false>;
    std::size_t n = a.size();
    if (n <= 1) return;
    H::build_of(a);
    for (std::size_t end = n - 1; end > 0; end--) {
        std::swap(a[0], a[end]);
        H::heapify(a, 0, end);
    }
}

} // namespace xpute

// cpp/xpute/kit/collection/list.hpp

// A list holds at most `N` values, inline. It never grows: a push past `N` is a
// bug (ENOSPC). Its first `len` slots hold values, and the rest are
// uninitialized.

namespace xpute {

template <class T, std::size_t N> class List {
  public:
    constexpr List() noexcept {}
    List(const List &) = delete;
    List &operator=(const List &) = delete;

    // Moving a list moves its values over and leaves `o` empty.
    List(List &&o) noexcept { take(o); }
    List &operator=(List &&o) noexcept {
        if (this != &o) {
            clear();
            take(o);
        }
        return *this;
    }

    ~List() { clear(); }

    std::size_t len() const noexcept { return len_; }
    bool is_empty() const noexcept { return len_ == 0; }

    void push(T val) noexcept {
        ensure(len_ < N, Errno::enospc, N);
        std::construct_at(slot(len_), std::move(val));
        len_++;
    }

    std::span<const T> as_slice() const noexcept { return {slot(0), len_}; }
    std::span<T> as_mut_slice() noexcept { return {slot(0), len_}; }

    // Indexing and iteration reach the live values, as a Rust `List` derefs to
    // its slice.
    T &operator[](std::size_t at) noexcept { return as_mut_slice()[at]; }
    const T &operator[](std::size_t at) const noexcept { return as_slice()[at]; }
    T *begin() noexcept { return slot(0); }
    T *end() noexcept { return slot(len_); }
    const T *begin() const noexcept { return slot(0); }
    const T *end() const noexcept { return slot(len_); }

    // Takes out the value at `at` and moves every value after it down one
    // slot, so the order is kept.
    T remove(std::size_t at) noexcept {
        ensure(at < len_, Errno::einval, at);
        T val = std::move(*slot(at));
        std::destroy_at(slot(at));
        for (std::size_t i = at; i + 1 < len_; i++) move_down(i + 1, i);
        len_--;
        return val;
    }

    // Destroys every value `keep` refuses and keeps the order of the rest, in
    // one pass.
    template <class Keep> void retain(Keep &&keep) noexcept {
        std::size_t live = 0;
        for (std::size_t at = 0; at < len_; at++) {
            // Every slot from `live` up to `at` is already empty: its value
            // was either refused and destroyed, or moved down.
            if (keep(std::as_const(*slot(at)))) {
                if (live != at) move_down(at, live);
                live++;
            } else {
                std::destroy_at(slot(at));
            }
        }
        len_ = live;
    }

    void clear() noexcept {
        std::destroy(slot(0), slot(len_));
        len_ = 0;
    }

  private:
    T *slot(std::size_t at) noexcept { return reinterpret_cast<T *>(bytes_) + at; }
    const T *slot(std::size_t at) const noexcept { return reinterpret_cast<const T *>(bytes_) + at; }

    // Moves the value in the live slot `from` into the empty slot `to`, and
    // leaves `from` empty.
    void move_down(std::size_t from, std::size_t to) noexcept {
        std::construct_at(slot(to), std::move(*slot(from)));
        std::destroy_at(slot(from));
    }

    void take(List &o) noexcept {
        for (std::size_t i = 0; i < o.len_; i++) std::construct_at(slot(i), std::move(*o.slot(i)));
        len_ = o.len_;
        o.clear();
    }

    // Raw storage: `push` constructs a value in a slot, and whatever takes the
    // value out destroys it. There is room for one value even when `N` is 0,
    // because C++ has no empty array.
    alignas(T) std::byte bytes_[std::max<std::size_t>(N, 1) * sizeof(T)];
    std::size_t len_ = 0;
};

} // namespace xpute

// cpp/xpute/kit/math/rng.32.hpp

// The 32-bit family is the counterpart of rng.64.hpp in `u32` arithmetic alone:
// WebGPU has no 64-bit integer, so a shader can compute these same numbers. The
// family holds Murmur3's finalizer, a seed derived through it, and a stream
// that applies it to a Weyl sequence.

namespace xpute {

inline constexpr std::uint32_t SEED_SALT_32 = 0x9e3779b9u;

constexpr std::uint32_t mix32(std::uint32_t x) noexcept {
    std::uint32_t z = x + SEED_SALT_32;
    z = (z ^ z >> 16) * 0x85ebca6bu;
    z = (z ^ z >> 13) * 0xc2b2ae35u;
    return z ^ z >> 16;
}

constexpr std::uint32_t seed_mix32(std::uint32_t base, std::uint32_t tag) noexcept {
    return mix32(base ^ tag);
}

class SplitMix32 {
  public:
    constexpr explicit SplitMix32(std::uint32_t seed) noexcept : x_(seed) {}

    constexpr std::uint32_t next_u32() noexcept {
        std::uint32_t z = mix32(x_);
        x_ += SEED_SALT_32;
        return z;
    }

  private:
    std::uint32_t x_;
};

} // namespace xpute

// cpp/xpute/kit/math/rng.64.hpp

// The 64-bit family holds splitmix64's finalizer, a seed derived through it,
// and splitmix64's stream, drawn as `u32` from the high 32 bits of each word.

namespace xpute {

inline constexpr std::uint64_t SEED_SALT = 0x9e3779b97f4a7c15u;

constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    std::uint64_t z = x + SEED_SALT;
    z = (z ^ z >> 30) * 0xbf58476d1ce4e5b9u;
    z = (z ^ z >> 27) * 0x94d049bb133111ebu;
    return z ^ z >> 31;
}

constexpr std::uint64_t seed_mix64(std::uint64_t base, std::uint64_t tag) noexcept {
    return mix64(base ^ tag);
}

class SplitMix64 {
  public:
    constexpr explicit SplitMix64(std::uint64_t seed) noexcept : x_(seed) {}

    std::uint32_t next_u32() noexcept {
        x_ += SEED_SALT;
        std::uint64_t z = x_;
        z = (z ^ z >> 30) * 0xbf58476d1ce4e5b9u;
        z = (z ^ z >> 27) * 0x94d049bb133111ebu;
        z ^= z >> 31;
        return static_cast<std::uint32_t>(z >> 32);
    }

  private:
    std::uint64_t x_;
};

} // namespace xpute

// cpp/xpute/kit/math/rng.hpp

// These seeds and streams are reproducible, and not for anything that must
// resist prediction. There are two families, a file each: a 64-bit one built
// on splitmix64 (rng.64.hpp), and a 32-bit one built on Murmur3's finalizer
// (rng.32.hpp), which a GPU shader can compute as well.

namespace xpute {

// Maps a `u32` onto [0, 1), uniformly.
constexpr double u32_to_unit(std::uint32_t x) noexcept {
    return static_cast<double>(x) / 4294967296.0;
}

} // namespace xpute

// cpp/xpute/kit/math/scalar.hpp

// These are scalar functions of a float or a double. Each is computed in plain
// IEEE arithmetic, with no fused operations, so its bits match the Rust kit's.

// Rust never fuses a multiply and an add, so neither may this code. Clang is
// held to that by this pragma as well as by the build's flags; GCC ignores
// the pragma and relies on the flag alone.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#include <cmath>

namespace xpute {

inline constexpr double EPS = 1e-5;

// These are the float types these functions take: double, and float for a
// value that is stored as one.
template <class T>
concept Float = std::is_floating_point_v<T>;

// Equal within `eps`, by default `EPS`. The tolerance is absolute near zero
// and relative to the larger magnitude beyond.
template <Float T> inline bool approx_eq(T a, T b, T eps = static_cast<T>(EPS)) noexcept {
    if (a == b) return true;
    T diff = std::fabs(a - b);
    return diff < eps || diff < eps * std::fmax(std::fabs(a), std::fabs(b));
}

template <Float T> inline bool near(T a, T b, T tol) noexcept {
    return std::fabs(a - b) < tol;
}

// Rounds as JavaScript's `Math.round` does: half rounds up, so -2.5 becomes
// -2.
template <Float T> inline T round_half_up(T x) noexcept {
    return x - std::trunc(x) == T(-0.5) ? std::ceil(x) : std::round(x);
}

// Rounds as C's and Rust's `round` do: half rounds away from zero, so -2.5
// becomes -3.
template <Float T> inline T round_half_away(T x) noexcept {
    return std::round(x);
}

// Rounds as IEEE 754's default and WGSL's `round` do: half rounds to even, so
// 2.5 becomes 2 and -2.5 becomes -2.
template <Float T> inline T round_half_even(T x) noexcept {
    // `x - floor(x)` is exact for every value, and 0 once `x` is too large to
    // have a fraction, so a tie is detected exactly in any rounding mode.
    T f = std::floor(x), d = x - f;
    T r = d < T(0.5) ? f : d > T(0.5) ? f + 1 : std::fmod(f, T(2)) == 0 ? f : f + 1;
    // A zero result keeps the sign of `x`, so -0.5 rounds to -0, as in Rust's
    // `round_ties_even`.
    return r == 0 ? std::copysign(T(0), x) : r;
}

// The kit's one rounding, half to even as WGSL's `round`, so the CPU and a
// shader land on the same integer. `std::round` rounds half away from zero
// instead.
template <Float T> inline T round(T x) noexcept {
    return round_half_even(x);
}

// Converts as Rust's `as` does: toward zero, saturating at the type's bounds,
// and NaN as 0. A plain `static_cast` is undefined outside the type's range.
template <class T> constexpr T saturate_as(double v) noexcept {
    if (v != v) return 0;
    if (v <= static_cast<double>(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
    if (v >= static_cast<double>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
    return static_cast<T>(v);
}

template <Float T> inline T lerp(T a, T b, T t) noexcept {
    return a + (b - a) * t;
}

template <Float T> inline T inv_lerp(T x, T min, T max) noexcept {
    return (x - min) / (max - min);
}

template <Float T> inline T remap(T x, T in_min, T in_max, T out_min, T out_max) noexcept {
    return lerp(out_min, out_max, inv_lerp(x, in_min, in_max));
}

template <Float T> inline T clamp(T x, T min, T max) noexcept {
    return x < min ? min : x > max ? max : x;
}

template <Float T> inline T wrap(T x, T min, T max) noexcept {
    T range = max - min;
    return std::fmod(std::fmod(x - min, range) + range, range) + min;
}

template <Float T> inline T floor_to_step(T x, T step) noexcept {
    return std::floor(x / step) * step;
}

template <Float T> inline T round_to_step(T x, T step) noexcept {
    return round(x / step) * step;
}

template <Float T> inline T ceil_to_step(T x, T step) noexcept {
    return std::ceil(x / step) * step;
}

} // namespace xpute

// cpp/xpute/kit/wire/tlv.hpp

// TLV, a sequence of values read once, front to back, with no table and no
// offset. Each value is a tag byte and its little-endian payload, and the tag
// `end` closes the sequence. A tag fixes its payload's size, except `str` and
// `bytes`, whose payload starts with its length as a u32; a reader cannot skip
// a tag it does not know, so an unknown tag is EBADMSG.
//
// The reader tells how the sequence ended (`Reader::end`): closed by the tag
// `end`, or unclosed, where the input ran out between two values, which the
// caller may or may not accept. Input that runs out inside a value is
// EBADMSG, and the values read before it stand.
//
// Equal values give equal bytes only under a schema that fixes their types: 1
// may be written as a u8, a u32 or an f64.

#include <variant>

namespace xpute::tlv {

enum class Tag : std::uint8_t {
    end = 0x00,
    boolean = 0x01,
    u8 = 0x02,
    i8 = 0x03,
    u16 = 0x04,
    i16 = 0x05,
    u32 = 0x06,
    i32 = 0x07,
    u64 = 0x08,
    i64 = 0x09,
    f32 = 0x0a,
    f64 = 0x0b,
    str = 0x10,
    bytes = 0x20,
};

// `decode` puts U+FFFD in place of each sequence that is not UTF-8, as the
// Rust and TypeScript readers do.
struct Str {
    std::string_view text;
    bool operator==(const Str &) const = default;
    template <class Put> void decode(Put &&put) const { utf8_lossy(text, std::forward<Put>(put)); }
};

struct Blob {
    std::span<const std::uint8_t> data;
    bool operator==(const Blob &o) const { return std::equal(data.begin(), data.end(), o.data.begin(), o.data.end()); }
};

enum class End : std::uint8_t {
    closed,
    unclosed,
};

using Value = std::variant<bool, std::uint8_t, std::int8_t, std::uint16_t, std::int16_t, std::uint32_t, std::int32_t, std::uint64_t, std::int64_t, float, double, Str, Blob>;

template <class Buf> class Writer {
  public:
    explicit Writer(Buf &buf) noexcept : buf_(buf) {}

    const Buf &data() const noexcept { return buf_; }

    // Appends the tag `end`, once. A write after it is EBADMSG.
    const Buf &finish() {
        if (!sealed_) {
            buf_.push_back(static_cast<std::uint8_t>(Tag::end));
            sealed_ = true;
        }
        return buf_;
    }

    Result<void> write(const Value &v) {
        if (sealed_) return make_error(Errno::ebadmsg);
        std::visit([this](const auto &x) { this->put(x); }, v);
        return {};
    }

    Result<void> write_all(std::span<const Value> vals) {
        for (const Value &v : vals) {
            if (Result<void> r = write(v); !r) return r;
        }
        return {};
    }

  private:
    void tag(Tag t) { buf_.push_back(static_cast<std::uint8_t>(t)); }

    template <class T> void le(T x) {
        auto raw = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(x);
        if constexpr (std::endian::native == std::endian::big) std::reverse(raw.begin(), raw.end());
        buf_.insert(buf_.end(), raw.begin(), raw.end());
    }

    void put(bool x) {
        tag(Tag::boolean);
        buf_.push_back(x ? 1 : 0);
    }
    void put(std::uint8_t x) { tag(Tag::u8), le(x); }
    void put(std::int8_t x) { tag(Tag::i8), le(x); }
    void put(std::uint16_t x) { tag(Tag::u16), le(x); }
    void put(std::int16_t x) { tag(Tag::i16), le(x); }
    void put(std::uint32_t x) { tag(Tag::u32), le(x); }
    void put(std::int32_t x) { tag(Tag::i32), le(x); }
    void put(std::uint64_t x) { tag(Tag::u64), le(x); }
    void put(std::int64_t x) { tag(Tag::i64), le(x); }
    void put(float x) { tag(Tag::f32), le(x); }
    void put(double x) { tag(Tag::f64), le(x); }
    void put(const Str &s) {
        tag(Tag::str);
        le(static_cast<std::uint32_t>(s.text.size()));
        buf_.insert(buf_.end(), s.text.begin(), s.text.end());
    }
    void put(const Blob &b) {
        tag(Tag::bytes);
        le(static_cast<std::uint32_t>(b.data.size()));
        buf_.insert(buf_.end(), b.data.begin(), b.data.end());
    }

    Buf &buf_;
    bool sealed_ = false;
};

class Reader {
  public:
    explicit Reader(std::span<const std::uint8_t> buf) noexcept : buf_(buf) {}

    // None at the tag `end` or at the input's end, which `end()` tells apart.
    Result<std::optional<Value>> next() noexcept;

    // How the sequence ended, once the reader has reached its end; none while
    // values remain, and after a fault.
    std::optional<End> end() const noexcept { return end_; }

    template <class Vec> Result<void> read_all(Vec &out) {
        for (;;) {
            Result<std::optional<Value>> v = next();
            if (!v) return v.error();
            if (!*v) return {};
            out.push_back(**v);
        }
    }

  private:
    // `n` is 64-bit so that a length off the wire near 2^32 cannot wrap the
    // sum.
    bool has(std::uint64_t n) const noexcept { return off_ + n <= buf_.size(); }

    template <class T> T le() noexcept {
        std::array<std::uint8_t, sizeof(T)> raw;
        std::memcpy(raw.data(), buf_.data() + off_, sizeof(T));
        if constexpr (std::endian::native == std::endian::big) std::reverse(raw.begin(), raw.end());
        off_ += sizeof(T);
        return std::bit_cast<T>(raw);
    }

    std::span<const std::uint8_t> buf_;
    std::size_t off_ = 0;
    std::optional<End> end_;
};

} // namespace xpute::tlv

// cpp/xpute/kit/wire/xtp/spec.hpp

// XTP, the Xpute Tree Packet, is a tree of values in one buffer, read in place.
// This header holds its layout, its type byte, and the helpers that read and
// write a packet's words. The format is described in
// kit/ts/src/wire/xtp/README.md, and the golden records under
// kit/spec/golden/wire/xtp/ hold the C++, Rust and TypeScript sides to the same
// bytes.
//
// A packet is a 16-byte header and one root node, all little-endian. A len,
// an offset or a size takes an 8-byte slot, the value in its low half; the
// high half is reserved and zero, or holds a type.
//
//   header  word 0  [MAGIC 32 | reserved 32]
//           word 1  [root payload size 32 | type 8 | reserved 24]
//   branch  [child count 32 | reserved 32], then a descriptor for each child:
//           [offset from the branch's start 32 | type 8 | reserved 24]
//
// A child's offset counts from its branch's start, so a subtree can be copied
// into another packet unchanged. An absent node keeps its type, with a size or
// an offset of 0. A present child starts past its branch's table, before the
// next present sibling, and inside its parent; a reader refuses a packet that
// breaks this.

namespace xpute::xtp {

using NodeType = std::uint8_t;

// A type byte with its top bit set names a leaf:
//
//   7 | 6 | 5 | 4 3 | 2 1 0
//   L | R | S |  C  |   A
//
// L is set for a leaf, and R is reserved. S is set for a sequence and clear
// for a scalar. C is the class: unsigned, signed, float, or non-numeric. A is
// the argument: for a number, log2 of its width in bytes; for a non-numeric
// leaf, which one it is (bool or bitset, str, strs). A type byte with the top
// bit clear is a special type: nil, branch or graft.
inline constexpr NodeType BIT_LEAF = 0x80;
inline constexpr NodeType BIT_SEQ = 0x20;
inline constexpr NodeType CLASS_MASK = 0x18;
inline constexpr NodeType CLASS_U = 0x00;
inline constexpr NodeType CLASS_I = 0x08;
inline constexpr NodeType CLASS_F = 0x10;
inline constexpr NodeType CLASS_R = 0x18;
inline constexpr NodeType ARG_MASK = 0x07;

constexpr NodeType int_type(std::uint32_t log2_width, bool sign) noexcept {
    return BIT_LEAF | (sign ? CLASS_I : CLASS_U) | (log2_width & ARG_MASK);
}
constexpr NodeType float_type(std::uint32_t log2_width) noexcept {
    return BIT_LEAF | CLASS_F | (log2_width & ARG_MASK);
}
constexpr NodeType nonnum_type(std::uint32_t x) noexcept {
    return BIT_LEAF | CLASS_R | (x & ARG_MASK);
}

enum class ScalarType : NodeType {
    u8 = int_type(0, false),
    i8 = int_type(0, true),
    u16 = int_type(1, false),
    i16 = int_type(1, true),
    u32 = int_type(2, false),
    i32 = int_type(2, true),
    u64 = int_type(3, false),
    i64 = int_type(3, true),
    f32 = float_type(2),
    f64 = float_type(3),
    boolean = nonnum_type(0),
};

enum class SequenceType : NodeType {
    u8_array = BIT_SEQ | int_type(0, false),
    i8_array = BIT_SEQ | int_type(0, true),
    u16_array = BIT_SEQ | int_type(1, false),
    i16_array = BIT_SEQ | int_type(1, true),
    u32_array = BIT_SEQ | int_type(2, false),
    i32_array = BIT_SEQ | int_type(2, true),
    u64_array = BIT_SEQ | int_type(3, false),
    i64_array = BIT_SEQ | int_type(3, true),
    f32_array = BIT_SEQ | float_type(2),
    f64_array = BIT_SEQ | float_type(3),
    bitset = BIT_SEQ | nonnum_type(0),
    // UTF-8 bytes followed by a NUL, which `len` does not count.
    str = BIT_SEQ | nonnum_type(1),
    // Many strings in one leaf, so that a string costs no node of its own:
    // `len` + 1 u32 offsets, each no less than the one before, then the UTF-8
    // they index. String `i` lies between offsets `i` and `i + 1`.
    strs = BIT_SEQ | nonnum_type(2),
};

enum class SpecialType : NodeType {
    // The untyped null. An absent node is not nil: it keeps its own type.
    nil = 0x00,
    branch = 0x20,
    // Never on the wire: a graft is written as its packet's root, under that
    // root's type.
    graft = 0x40,
};

template <class E> constexpr NodeType type_of(E t) noexcept {
    return static_cast<NodeType>(t);
}

constexpr bool type_is_leaf(NodeType t) noexcept {
    return (t & BIT_LEAF) != 0;
}
constexpr bool type_is_special(NodeType t) noexcept {
    return !type_is_leaf(t);
}
constexpr bool type_is_scalar(NodeType t) noexcept {
    return type_is_leaf(t) && (t & BIT_SEQ) == 0;
}
constexpr bool type_is_seq(NodeType t) noexcept {
    return type_is_leaf(t) && (t & BIT_SEQ) != 0;
}
constexpr bool type_is_nonnum(NodeType t) noexcept {
    return type_is_leaf(t) && (t & CLASS_MASK) == CLASS_R;
}
constexpr bool type_is_float(NodeType t) noexcept {
    return type_is_leaf(t) && (t & CLASS_MASK) == CLASS_F;
}
constexpr bool type_is_signed(NodeType t) noexcept {
    return type_is_leaf(t) && (t & CLASS_MASK) == CLASS_I;
}
constexpr std::uint32_t type_arg_of(NodeType t) noexcept {
    return t & ARG_MASK;
}

template <class T> inline constexpr SequenceType ARRAY_OF = [] {
    if constexpr (std::is_same_v<T, std::uint8_t>) return SequenceType::u8_array;
    else if constexpr (std::is_same_v<T, std::int8_t>) return SequenceType::i8_array;
    else if constexpr (std::is_same_v<T, std::uint16_t>) return SequenceType::u16_array;
    else if constexpr (std::is_same_v<T, std::int16_t>) return SequenceType::i16_array;
    else if constexpr (std::is_same_v<T, std::uint32_t>) return SequenceType::u32_array;
    else if constexpr (std::is_same_v<T, std::int32_t>) return SequenceType::i32_array;
    else if constexpr (std::is_same_v<T, std::uint64_t>) return SequenceType::u64_array;
    else if constexpr (std::is_same_v<T, std::int64_t>) return SequenceType::i64_array;
    else if constexpr (std::is_same_v<T, float>) return SequenceType::f32_array;
    else {
        static_assert(std::is_same_v<T, double>, "no array of this element on the wire");
        return SequenceType::f64_array;
    }
}();

// "XTP\0" read as a little-endian u32.
inline constexpr std::uint32_t MAGIC = 0x00505458u;
inline constexpr std::uint32_t WORD_SZ = 8;
inline constexpr std::uint32_t HDR_SZ = 2 * WORD_SZ;
inline constexpr std::uint32_t MAX_PKT_SZ = 0x7fffffffu;
inline constexpr std::uint32_t RESERVED = 0;
inline constexpr std::uint32_t DESC_TYPE_MASK = 0xffu;

// A little-endian `T` at `p`, which need not be aligned.
template <class T> T load(const std::uint8_t *p) noexcept {
    std::array<std::uint8_t, sizeof(T)> raw;
    std::memcpy(raw.data(), p, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) std::reverse(raw.begin(), raw.end());
    return std::bit_cast<T>(raw);
}

template <class T> void store(std::uint8_t *p, T x) noexcept {
    auto raw = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(x);
    if constexpr (std::endian::native == std::endian::big) std::reverse(raw.begin(), raw.end());
    std::memcpy(p, raw.data(), sizeof(T));
}

inline void set_word(std::span<std::uint8_t> buf, std::uint32_t off, std::uint32_t lo, std::uint32_t hi) noexcept {
    store(buf.data() + off, lo);
    store(buf.data() + off + 4, hi);
}
inline std::array<std::uint32_t, 2> get_word(std::span<const std::uint8_t> buf, std::uint32_t off) noexcept {
    return {load<std::uint32_t>(buf.data() + off), load<std::uint32_t>(buf.data() + off + 4)};
}

// Whether a type byte may appear on the wire: nil, branch, or a leaf with no
// reserved bit set and an argument its class defines. A graft may not.
constexpr bool type_is_known(NodeType t) noexcept {
    if (t == type_of(SpecialType::nil) || t == type_of(SpecialType::branch)) return true;
    if (!type_is_leaf(t) || (t & ~(BIT_LEAF | BIT_SEQ | CLASS_MASK | ARG_MASK)) != 0) return false;
    std::uint32_t arg = type_arg_of(t);
    switch (t & CLASS_MASK) {
    case CLASS_F: return arg == 2 || arg == 3;
    case CLASS_R: return arg <= (type_is_seq(t) ? 2u : 0u);
    default: return arg <= 3;
    }
}

// Reads a slot that holds a len, a size or the magic. A high half that is not
// zero makes the packet malformed (EBADMSG).
inline Result<std::uint32_t> get_len(std::span<const std::uint8_t> buf, std::uint32_t off) noexcept {
    auto [lo, hi] = get_word(buf, off);
    if (hi != RESERVED) return make_error(Errno::ebadmsg);
    return lo;
}

// Reads a descriptor, a branch's entry for a child or the header's root word:
// the offset or size, and the type. A type the wire cannot carry, or a
// reserved bit set, makes the packet malformed (EBADMSG).
struct Desc {
    std::uint32_t off;
    NodeType type;
};
inline Result<Desc> get_desc(std::span<const std::uint8_t> buf, std::uint32_t off) noexcept {
    auto [lo, desc] = get_word(buf, off);
    auto type = static_cast<NodeType>(desc & DESC_TYPE_MASK);
    if (desc != type || !type_is_known(type)) return make_error(Errno::ebadmsg);
    return Desc{lo, type};
}

// `unit` must be a power of two no larger than `WORD_SZ`, or EINVAL.
inline Result<std::uint32_t> align(std::uint32_t nbyte, std::uint32_t unit) noexcept {
    if (unit == 0 || (unit & (unit - 1)) != 0 || unit > WORD_SZ) return make_error(Errno::einval);
    std::uint32_t mask = unit - 1;
    if (nbyte > UINT32_MAX - mask) return make_error(Errno::eoverflow);
    return (nbyte + mask) & ~mask;
}

// A sequence or a branch starts on a word, so its len slot is aligned.
inline Result<std::uint32_t> align_sz(NodeType t) noexcept {
    if (t == type_of(SpecialType::branch) || type_is_seq(t)) return WORD_SZ;
    switch (static_cast<ScalarType>(t)) {
    case ScalarType::u8:
    case ScalarType::i8:
    case ScalarType::boolean:
        return 1u;
    case ScalarType::u16:
    case ScalarType::i16:
        return 2u;
    case ScalarType::u32:
    case ScalarType::i32:
    case ScalarType::f32:
        return 4u;
    case ScalarType::u64:
    case ScalarType::i64:
    case ScalarType::f64:
        return 8u;
    }
    return make_error(Errno::ebadmsg);
}

// One element's size. A strs's element is a u32 offset, and a bitset's is a
// byte, since its bits are given a byte each before they are packed.
constexpr std::uint32_t elem_sz(SequenceType t) noexcept {
    if (t == SequenceType::strs) return 4;
    return type_is_nonnum(type_of(t)) ? 1 : 1u << type_arg_of(type_of(t));
}

} // namespace xpute::xtp

// cpp/xpute/kit/wire/xtp/cursor.hpp

// This is the read side of XTP: cursors over a packet. What a read returns
// points into the packet's bytes, so the packet must outlive it. The packet
// need not be aligned in memory, since every number is copied out of it, but
// each node must be aligned within the packet.

#include <string>

namespace xpute::xtp {

struct Null {
    bool operator==(const Null &) const = default;
};

template <class T> class ArrayView {
  public:
    ArrayView(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(bytes_.size() / sizeof(T)); }
    T operator[](std::uint32_t i) const noexcept { return load<T>(bytes_.data() + i * sizeof(T)); }

  private:
    std::span<const std::uint8_t> bytes_;
};

struct Array {
    SequenceType type;
    std::span<const std::uint8_t> bytes;

    template <class T> ArrayView<T> elements() const noexcept { return bytes; }
};

// A bitset read in place: bit `i` is bit `i % 8` of byte `i / 8`.
struct Bitset {
    std::span<const std::uint8_t> packed;
    std::uint32_t len;

    bool operator[](std::uint32_t i) const noexcept { return (packed[i >> 3] >> (i & 7)) & 1; }
};

struct StrList {
    std::span<const std::uint8_t> offs;
    std::span<const std::uint8_t> blob;
    std::uint32_t len;

    std::uint32_t size() const noexcept { return len; }
    bool empty() const noexcept { return len == 0; }

    std::optional<std::string_view> get(std::uint32_t i) const noexcept {
        if (i >= len) return std::nullopt;
        std::uint32_t a = load<std::uint32_t>(offs.data() + 4 * i), b = load<std::uint32_t>(offs.data() + 4 * (i + 1));
        return std::string_view(reinterpret_cast<const char *>(blob.data() + a), b - a);
    }
    std::string_view operator[](std::uint32_t i) const noexcept {
        std::optional<std::string_view> s = get(i);
        ensure(s.has_value(), Errno::enotrecoverable, i, len);
        return *s;
    }

    class iterator {
      public:
        std::string_view operator*() const noexcept { return (*s_)[i_]; }
        iterator &operator++() noexcept { return ++i_, *this; }
        bool operator==(const iterator &o) const noexcept { return i_ == o.i_; }

      private:
        friend struct StrList;
        iterator(const StrList *s, std::uint32_t i) noexcept : s_(s), i_(i) {}
        const StrList *s_;
        std::uint32_t i_;
    };
    iterator begin() const noexcept { return iterator(this, 0); }
    iterator end() const noexcept { return iterator(this, len); }
};

class BranchCursor;
template <class Alloc> struct DeepValue;

using Value = std::variant<Null, bool, std::uint8_t, std::int8_t, std::uint16_t, std::int16_t, std::uint32_t, std::int32_t, std::uint64_t, std::int64_t, float,
                           double, std::string_view, Array, Bitset, StrList, BranchCursor>;

class Cursor {
  public:
    NodeType type() const noexcept { return type_; }
    bool is_null() const noexcept { return base_ == 0; }
    bool is_branch() const noexcept { return !is_null() && type_ == type_of(SpecialType::branch); }

    std::optional<Cursor> opt() const noexcept { return is_null() ? std::nullopt : std::optional(*this); }

    Result<BranchCursor> as_branch() const noexcept;

    Result<Value> get() const noexcept;

    template <class T> Result<ArrayView<T>> get_array() const noexcept {
        if (is_null() || type_ != type_of(ARRAY_OF<T>)) return make_error(Errno::ebadmsg);
        Result<Array> a = array(sizeof(T));
        if (!a) return a.error();
        return ArrayView<T>(a->bytes);
    }

    Result<std::uint64_t> get_u64() const noexcept;
    Result<bool> get_bool() const noexcept;
    Result<std::string_view> get_str_ref() const noexcept;
    Result<StrList> get_strs() const noexcept;
    template <class Alloc = std::allocator<std::byte>> Result<DeepValue<Alloc>> get_deep(const Alloc &alloc = Alloc()) const;

    // Any number type a double holds exactly. A 64-bit integer is refused
    // (EBADMSG), even when its value would fit.
    Result<double> get_number() const noexcept;

  private:
    friend class TreeReader;
    friend class BranchCursor;

    Cursor(std::span<const std::uint8_t> pkt, std::uint32_t base, NodeType type, std::uint32_t len = 0) noexcept : pkt_(pkt), base_(base), type_(type), len_(len) {}

    static Result<Cursor> at(std::span<const std::uint8_t> pkt, std::uint32_t base, NodeType type) noexcept;

    template <class T> Result<T> get_as() const noexcept;
    template <class T> Result<Value> scalar() const noexcept;

    Result<Array> array(std::uint32_t elem) const noexcept;
    Result<Bitset> bitset() const noexcept;
    Result<std::string_view> str() const noexcept;
    Result<StrList> strs() const noexcept;

    std::span<const std::uint8_t> pkt_;
    // 0 for an absent node, since no node can start inside the header.
    std::uint32_t base_;
    NodeType type_;
    std::uint32_t len_;
};

class BranchCursor {
  public:
    std::uint32_t base() const noexcept { return node_.base_; }
    std::uint32_t len() const noexcept { return node_.len_; }
    bool is_branch() const noexcept { return true; }
    Result<BranchCursor> as_branch() const noexcept { return *this; }
    std::optional<BranchCursor> opt() const noexcept { return *this; }

    Result<Value> get_at(std::uint32_t idx) const noexcept;
    template <class Vec> Result<void> get(Vec &out) const;

    class iterator {
      public:
        Result<Cursor> operator*() const noexcept { return b_->at(i_); }
        iterator &operator++() noexcept { return ++i_, *this; }
        bool operator==(const iterator &o) const noexcept { return i_ == o.i_; }

      private:
        friend class BranchCursor;
        iterator(const BranchCursor *b, std::uint32_t i) noexcept : b_(b), i_(i) {}
        const BranchCursor *b_;
        std::uint32_t i_;
    };
    iterator begin() const noexcept { return iterator(this, 0); }
    iterator end() const noexcept { return iterator(this, len()); }

    Result<Cursor> at(std::uint32_t idx) const noexcept;
    Result<BranchCursor> at_branch(std::uint32_t idx) const noexcept;

    bool operator==(const BranchCursor &o) const noexcept { return node_.pkt_.data() == o.node_.pkt_.data() && base() == o.base(); }

  private:
    friend class Cursor;
    explicit BranchCursor(Cursor node) noexcept : node_(node) {}

    // Where child `idx`'s extent ends: at the next present sibling's start, or
    // else at this branch's end, where its packet slice ends.
    std::uint32_t end_of(std::uint32_t idx) const noexcept;

    Cursor node_;
};

class TreeReader {
  public:
    static Result<TreeReader> make(std::span<const std::uint8_t> pkt) noexcept;

    Cursor read() const noexcept { return root_; }
    Result<BranchCursor> read_branch() const noexcept { return root_.as_branch(); }

  private:
    explicit TreeReader(Cursor root) noexcept : root_(root) {}

    Cursor root_;
};

template <class Alloc> struct DeepValue {
    using List = std::vector<DeepValue, typename std::allocator_traits<Alloc>::template rebind_alloc<DeepValue>>;
    std::variant<Value, List> v;
};

template <class Alloc> Result<DeepValue<Alloc>> Cursor::get_deep(const Alloc &alloc) const {
    if (!is_branch()) {
        Result<Value> v = get();
        if (!v) return v.error();
        return DeepValue<Alloc>{*v};
    }
    Result<BranchCursor> b = as_branch();
    if (!b) return b.error();
    typename DeepValue<Alloc>::List list(alloc);
    list.reserve(b->len());
    for (Result<Cursor> c : *b) {
        if (!c) return c.error();
        Result<DeepValue<Alloc>> d = c->get_deep(alloc);
        if (!d) return d.error();
        list.push_back(std::move(*d));
    }
    return DeepValue<Alloc>{std::move(list)};
}

template <class Vec> Result<void> BranchCursor::get(Vec &out) const {
    for (Result<Cursor> c : *this) {
        if (!c) return c.error();
        Result<Value> v = c->get();
        if (!v) return v.error();
        out.push_back(*v);
    }
    return {};
}

} // namespace xpute::xtp

// cpp/xpute/kit/wire/xtp/encode.hpp

// Writes a packet in place, into bytes the caller holds, without building a
// tree and without allocating. It is the one writer of the layout: `encode`
// (view.hpp) walks its tree onto it, so both give the same bytes. A node that
// does not fit does not fail its call: the writer goes on counting entries,
// and `finish` refuses the packet (EOVERFLOW). In every language, a string
// fits exactly when its UTF-8 and the NUL after it do.

namespace xpute::xtp {

class PacketWriter {
  public:
    // A writer for a packet whose root is any one node. The packet may take
    // the whole buffer, up to `MAX_PKT_SZ` bytes.
    explicit PacketWriter(std::span<std::uint8_t> buf) noexcept;

    PacketWriter &nil() noexcept { return entry(std::nullopt, type_of(SpecialType::nil)); }

    // An empty optional writes a typed null, an absent node that keeps its
    // type.
    PacketWriter &u8(std::optional<std::uint8_t> v) noexcept { return scalar(ScalarType::u8, v); }
    PacketWriter &i8(std::optional<std::int8_t> v) noexcept { return scalar(ScalarType::i8, v); }
    PacketWriter &u16(std::optional<std::uint16_t> v) noexcept { return scalar(ScalarType::u16, v); }
    PacketWriter &i16(std::optional<std::int16_t> v) noexcept { return scalar(ScalarType::i16, v); }
    PacketWriter &u32(std::optional<std::uint32_t> v) noexcept { return scalar(ScalarType::u32, v); }
    PacketWriter &i32(std::optional<std::int32_t> v) noexcept { return scalar(ScalarType::i32, v); }
    PacketWriter &u64(std::optional<std::uint64_t> v) noexcept { return scalar(ScalarType::u64, v); }
    PacketWriter &i64(std::optional<std::int64_t> v) noexcept { return scalar(ScalarType::i64, v); }
    PacketWriter &f32(std::optional<float> v) noexcept { return scalar(ScalarType::f32, v); }
    PacketWriter &f64(std::optional<double> v) noexcept { return scalar(ScalarType::f64, v); }
    PacketWriter &boolean(std::optional<bool> v) noexcept {
        return scalar(ScalarType::boolean, v ? std::optional<std::uint8_t>(*v ? 1 : 0) : std::nullopt);
    }

    // A packed array of `T`, an empty optional a typed null.
    template <class T> PacketWriter &array(std::optional<std::span<const T>> a) noexcept {
        if (!a) return entry(std::nullopt, type_of(ARRAY_OF<T>));
        return array_with<T>(static_cast<std::uint32_t>(a->size()), [&a](std::uint32_t i) { return (*a)[i]; });
    }

    template <class T, class F> PacketWriter &array_with(std::uint32_t len, F &&val) noexcept {
        std::optional<std::uint32_t> at = seq(len, std::uint64_t{len} * sizeof(T));
        if (at) {
            for (std::uint32_t i = 0; i < len; i++) store<T>(buf_.data() + *at + WORD_SZ + i * sizeof(T), val(i));
        }
        return entry(at, type_of(ARRAY_OF<T>));
    }

    // `bits` holds one byte for each bit, nonzero for set. They are packed
    // eight to a byte, the first in the lowest bit.
    PacketWriter &bitset(std::optional<std::span<const std::uint8_t>> bits) noexcept;

    PacketWriter &str(std::optional<std::string_view> s) noexcept;

    PacketWriter &strs(std::optional<std::span<const std::string_view>> items) noexcept;

    // Writing a number of children other than `cnt` is a bug.
    template <class F> PacketWriter &branch(std::uint32_t cnt, F &&fill) noexcept {
        std::optional<std::uint32_t> at = branch_open(cnt);
        Table outer = table_;
        table_ = Table{.base = at.value_or(0), .cnt = cnt, .written = 0, .root = false};
        fill(*this);
        ensure(table_.written == table_.cnt, Errno::enotrecoverable, table_.cnt, table_.written);
        table_ = outer;
        return entry(branch_close(at), type_of(SpecialType::branch));
    }

    PacketWriter &no_branch() noexcept { return entry(std::nullopt, type_of(SpecialType::branch)); }

    // Copies the root of the encoded packet `pkt` in as this entry, aligned as
    // its root's type requires. A malformed `pkt` makes `finish` refuse the
    // packet (EBADMSG).
    PacketWriter &graft(std::span<const std::uint8_t> pkt) noexcept;

    // Writes the header and returns the packet's length, header included and
    // padded to a word, or the first error that spoiled the packet.
    Result<std::uint32_t> finish() noexcept;

  private:
    // Writes a str whose bytes `write` puts into the room after the len slot.
    // `write` returns how many bytes it wrote, or nothing when they did not
    // fit, which spoils the packet.
    template <class F> PacketWriter &str_with(F &&write) noexcept {
        std::optional<std::uint32_t> at;
        if (std::optional<std::uint32_t> room = str_room()) {
            std::uint32_t payload = str_payload();
            std::optional<std::size_t> n = write(std::span<char>(reinterpret_cast<char *>(buf_.data() + payload), *room));
            at = n && *n <= *room ? str_commit(static_cast<std::uint32_t>(*n)) : spoil(Errno::eoverflow);
        }
        return entry(at, type_of(SequenceType::str));
    }

    // The child table the writer's next entries go in. The root is treated as
    // a branch of one child whose entry is the header's root word: `root`
    // marks it, and its entry sets the root's type instead of writing a
    // descriptor.
    struct Table {
        std::uint32_t base;
        std::uint32_t cnt;
        std::uint32_t written;
        bool root;
    };

    std::optional<std::uint32_t> spoil(Errno errnum) noexcept;
    std::optional<std::uint32_t> reserve(std::uint32_t unit, std::uint64_t bytes) noexcept;
    PacketWriter &entry(std::optional<std::uint32_t> at, NodeType type) noexcept;

    template <class T> PacketWriter &scalar(ScalarType type, std::optional<T> v) noexcept {
        std::optional<std::uint32_t> at;
        if (v) {
            at = reserve(sizeof(T), sizeof(T));
            if (at) store<T>(buf_.data() + *at, *v);
        }
        return entry(at, type_of(type));
    }

    std::optional<std::uint32_t> seq(std::uint32_t len, std::uint64_t bytes) noexcept;

    std::uint32_t str_payload() const noexcept;
    std::optional<std::uint32_t> str_room() noexcept;
    std::optional<std::uint32_t> str_commit(std::uint32_t len) noexcept;

    std::optional<std::uint32_t> branch_open(std::uint32_t cnt) noexcept;
    std::optional<std::uint32_t> branch_close(std::optional<std::uint32_t> at) noexcept;

    std::span<std::uint8_t> buf_;
    Table table_{.base = 0, .cnt = 1, .written = 0, .root = true};
    NodeType root_type_ = type_of(SpecialType::nil);
    std::uint32_t lim_ = HDR_SZ;
    // This is the first error met; later ones are not kept.
    std::optional<Error> err_;
};

} // namespace xpute::xtp

// cpp/xpute/kit/wire/xtp/view.hpp

// This is the write side of XTP as a tree: nodes built in memory, which
// `encode` then lays out as a packet. A node borrows what it carries (an
// array, a string, a graft's packet), so the caller must keep them alive
// until the tree is encoded.

namespace xpute::xtp {

enum class NodeKind : std::uint8_t { nil, scalar, array, bitset, str, strs, branch, graft };

template <class Alloc = std::allocator<std::byte>> struct Node {
    using NodeList = std::vector<Node, typename std::allocator_traits<Alloc>::template rebind_alloc<Node>>;

    NodeKind kind = NodeKind::nil;
    NodeType type = type_of(SpecialType::nil);
    // False for a typed null, an absent node that keeps its type.
    bool present = true;
    // A scalar's value, its bytes copied into the start of this word.
    std::uint64_t bits = 0;
    std::span<const std::uint8_t> bytes;
    std::string_view str;
    std::span<const std::string_view> strs;
    NodeList children;

    explicit Node(const Alloc &alloc = Alloc()) : children(alloc) {}
};

namespace view_detail {

template <class Alloc, class T> Node<Alloc> scalar(ScalarType type, std::optional<T> v, const Alloc &alloc) {
    Node<Alloc> n(alloc);
    n.kind = NodeKind::scalar;
    n.type = type_of(type);
    n.present = v.has_value();
    if (v) std::memcpy(&n.bits, &*v, sizeof(T));
    return n;
}

template <class Alloc, class T> Node<Alloc> array(std::optional<std::span<const T>> a, const Alloc &alloc) {
    Node<Alloc> n(alloc);
    n.kind = NodeKind::array;
    n.type = type_of(ARRAY_OF<T>);
    n.present = a.has_value();
    if (a) n.bytes = std::as_bytes(*a).empty() ? std::span<const std::uint8_t>() : std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(a->data()), a->size_bytes());
    return n;
}

} // namespace view_detail

template <class Alloc> class BranchView;

template <class Alloc = std::allocator<std::byte>> struct ViewValue {
    struct Null {};
    struct Array {
        SequenceType type;
        std::span<const std::uint8_t> bytes;
    };
    std::variant<Null, bool, std::uint8_t, std::int8_t, std::uint16_t, std::int16_t, std::uint32_t, std::int32_t, std::uint64_t, std::int64_t, float, double,
                 std::string_view, Array, std::span<const ViewValue>, const Node<Alloc> *>
        v;
};

// On a tree, `Self::write` replaces the root; on a branch, it appends a child.
template <class Self, class Alloc> class NodeView {
  public:
    Self &nil() { return self().write(Node<Alloc>(alloc())); }

    Self &u8(std::optional<std::uint8_t> v) { return self().write(view_detail::scalar(ScalarType::u8, v, alloc())); }
    Self &i8(std::optional<std::int8_t> v) { return self().write(view_detail::scalar(ScalarType::i8, v, alloc())); }
    Self &u16(std::optional<std::uint16_t> v) { return self().write(view_detail::scalar(ScalarType::u16, v, alloc())); }
    Self &i16(std::optional<std::int16_t> v) { return self().write(view_detail::scalar(ScalarType::i16, v, alloc())); }
    Self &u32(std::optional<std::uint32_t> v) { return self().write(view_detail::scalar(ScalarType::u32, v, alloc())); }
    Self &i32(std::optional<std::int32_t> v) { return self().write(view_detail::scalar(ScalarType::i32, v, alloc())); }
    Self &u64(std::optional<std::uint64_t> v) { return self().write(view_detail::scalar(ScalarType::u64, v, alloc())); }
    Self &i64(std::optional<std::int64_t> v) { return self().write(view_detail::scalar(ScalarType::i64, v, alloc())); }
    Self &f32(std::optional<float> v) { return self().write(view_detail::scalar(ScalarType::f32, v, alloc())); }
    Self &f64(std::optional<double> v) { return self().write(view_detail::scalar(ScalarType::f64, v, alloc())); }
    Self &boolean(std::optional<bool> v) {
        return self().write(view_detail::scalar(ScalarType::boolean, v ? std::optional<std::uint8_t>(*v ? 1 : 0) : std::nullopt, alloc()));
    }

    Self &u8_array(std::optional<std::span<const std::uint8_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &i8_array(std::optional<std::span<const std::int8_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &u16_array(std::optional<std::span<const std::uint16_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &i16_array(std::optional<std::span<const std::int16_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &u32_array(std::optional<std::span<const std::uint32_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &i32_array(std::optional<std::span<const std::int32_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &u64_array(std::optional<std::span<const std::uint64_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &i64_array(std::optional<std::span<const std::int64_t>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &f32_array(std::optional<std::span<const float>> a) { return self().write(view_detail::array(a, alloc())); }
    Self &f64_array(std::optional<std::span<const double>> a) { return self().write(view_detail::array(a, alloc())); }

    // `bits` holds one byte for each bit, nonzero for set.
    Self &bitset(std::optional<std::span<const std::uint8_t>> bits) {
        Node<Alloc> n(alloc());
        n.kind = NodeKind::bitset;
        n.type = type_of(SequenceType::bitset);
        n.present = bits.has_value();
        if (bits) n.bytes = *bits;
        return self().write(std::move(n));
    }

    Self &str(std::optional<std::string_view> s) {
        Node<Alloc> n(alloc());
        n.kind = NodeKind::str;
        n.type = type_of(SequenceType::str);
        n.present = s.has_value();
        if (s) n.str = *s;
        return self().write(std::move(n));
    }

    Self &strs(std::optional<std::span<const std::string_view>> items) {
        Node<Alloc> n(alloc());
        n.kind = NodeKind::strs;
        n.type = type_of(SequenceType::strs);
        n.present = items.has_value();
        if (items) n.strs = *items;
        return self().write(std::move(n));
    }

    template <class F> Self &branch(F &&fill) {
        BranchView<Alloc> b(alloc());
        fill(b);
        return self().write(std::move(b.node()));
    }
    // An absent branch, which keeps its type.
    Self &no_branch() {
        Node<Alloc> n(alloc());
        n.kind = NodeKind::branch;
        n.type = type_of(SpecialType::branch);
        n.present = false;
        return self().write(std::move(n));
    }

    // `pkt` must be an encoded packet. It is checked when the tree is encoded,
    // not here.
    Self &graft(std::span<const std::uint8_t> pkt) {
        Node<Alloc> n(alloc());
        n.kind = NodeKind::graft;
        n.type = type_of(SpecialType::graft);
        n.bytes = pkt;
        return self().write(std::move(n));
    }

  protected:
    Result<Node<Alloc>> lower(const ViewValue<Alloc> &val) {
        using V = ViewValue<Alloc>;
        const Alloc &a = alloc();
        return std::visit(
            [&](const auto &x) -> Result<Node<Alloc>> {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, typename V::Null>) return Node<Alloc>(a);
                else if constexpr (std::is_same_v<T, bool>) return view_detail::scalar(ScalarType::boolean, std::optional<std::uint8_t>(x ? 1 : 0), a);
                else if constexpr (std::is_same_v<T, std::uint8_t>) return view_detail::scalar(ScalarType::u8, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::int8_t>) return view_detail::scalar(ScalarType::i8, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::uint16_t>) return view_detail::scalar(ScalarType::u16, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::int16_t>) return view_detail::scalar(ScalarType::i16, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::uint32_t>) return view_detail::scalar(ScalarType::u32, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::int32_t>) return view_detail::scalar(ScalarType::i32, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::uint64_t>) return view_detail::scalar(ScalarType::u64, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::int64_t>) return view_detail::scalar(ScalarType::i64, std::optional(x), a);
                else if constexpr (std::is_same_v<T, float>) return view_detail::scalar(ScalarType::f32, std::optional(x), a);
                else if constexpr (std::is_same_v<T, double>) return view_detail::scalar(ScalarType::f64, std::optional(x), a);
                else if constexpr (std::is_same_v<T, std::string_view>) {
                    Node<Alloc> n(a);
                    n.kind = NodeKind::str;
                    n.type = type_of(SequenceType::str);
                    n.str = x;
                    return n;
                } else if constexpr (std::is_same_v<T, typename V::Array>) {
                    // The arrays the Rust encoder accepts: numbers in whole
                    // elements, or a bitset; anything else is EBADMSG.
                    bool bitset = x.type == SequenceType::bitset;
                    if (!bitset && (type_is_nonnum(type_of(x.type)) || x.bytes.size() % elem_sz(x.type) != 0)) return make_error(Errno::ebadmsg);
                    Node<Alloc> n(a);
                    n.kind = x.type == SequenceType::bitset ? NodeKind::bitset : NodeKind::array;
                    n.type = type_of(x.type);
                    n.bytes = x.bytes;
                    return n;
                } else if constexpr (std::is_same_v<T, std::span<const V>>) {
                    BranchView<Alloc> b(a);
                    for (const V &child : x) {
                        Result<Node<Alloc>> c = b.lower(child);
                        if (!c) return c.error();
                        b.write(std::move(*c));
                    }
                    return std::move(b.node());
                } else {
                    return *x;
                }
            },
            val.v);
    }

  private:
    Self &self() { return static_cast<Self &>(*this); }
    const Alloc &alloc() { return static_cast<Self &>(*this).allocator(); }
};

template <class Alloc = std::allocator<std::byte>> class TreeView : public NodeView<TreeView<Alloc>, Alloc> {
  public:
    explicit TreeView(const Alloc &alloc = Alloc()) : alloc_(alloc), node_(alloc) {}

    TreeView &write(Node<Alloc> node) {
        node_ = std::move(node);
        return *this;
    }

    Result<TreeView *> set(const ViewValue<Alloc> &val) {
        Result<Node<Alloc>> n = this->lower(val);
        if (!n) return n.error();
        write(std::move(*n));
        return this;
    }

    const Node<Alloc> &node() const noexcept { return node_; }
    const Alloc &allocator() const noexcept { return alloc_; }

  private:
    Alloc alloc_;
    Node<Alloc> node_;
};

template <class Alloc = std::allocator<std::byte>> class BranchView : public NodeView<BranchView<Alloc>, Alloc> {
  public:
    explicit BranchView(const Alloc &alloc = Alloc()) : alloc_(alloc), node_(alloc) {
        node_.kind = NodeKind::branch;
        node_.type = type_of(SpecialType::branch);
    }

    BranchView &write(Node<Alloc> node) {
        node_.children.push_back(std::move(node));
        return *this;
    }

    Result<BranchView *> set(std::span<const ViewValue<Alloc>> vals) {
        node_.children.clear();
        for (const ViewValue<Alloc> &val : vals) {
            Result<BranchView *> r = put(val);
            if (!r) return r;
        }
        return this;
    }

    Result<BranchView *> put(const ViewValue<Alloc> &val) {
        Result<Node<Alloc>> n = this->lower(val);
        if (!n) return n.error();
        write(std::move(*n));
        return this;
    }

    template <class V> BranchView &subtree(const V &view) { return write(view.node()); }

    Node<Alloc> &node() noexcept { return node_; }
    const Node<Alloc> &node() const noexcept { return node_; }
    const Alloc &allocator() const noexcept { return alloc_; }

  private:
    template <class, class> friend class NodeView;
    Alloc alloc_;
    Node<Alloc> node_;
};

struct TreeEncoderConfig {
    // The size of the first buffer tried; it doubles each time the packet
    // does not fit.
    std::uint32_t init_cap;
    std::optional<std::uint32_t> max_cap;
};

namespace view_detail {

template <class T> T load(const std::uint8_t *p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <class T, class Alloc> void array_into(PacketWriter &w, const Node<Alloc> &n) {
    if (!n.present) {
        w.template array_with<T>(0, [](std::uint32_t) { return T{}; });
        return;
    }
    auto len = static_cast<std::uint32_t>(n.bytes.size() / sizeof(T));
    w.template array_with<T>(len, [&n](std::uint32_t i) { return load<T>(n.bytes.data() + i * sizeof(T)); });
}

template <class Alloc> void walk(PacketWriter &w, const Node<Alloc> &n);

template <class Alloc> void scalar_into(PacketWriter &w, const Node<Alloc> &n) {
    auto bits = [&]<class T>() { return n.present ? std::optional<T>(load<T>(reinterpret_cast<const std::uint8_t *>(&n.bits))) : std::nullopt; };
    switch (static_cast<ScalarType>(n.type)) {
    case ScalarType::u8: w.u8(bits.template operator()<std::uint8_t>()); break;
    case ScalarType::i8: w.i8(bits.template operator()<std::int8_t>()); break;
    case ScalarType::u16: w.u16(bits.template operator()<std::uint16_t>()); break;
    case ScalarType::i16: w.i16(bits.template operator()<std::int16_t>()); break;
    case ScalarType::u32: w.u32(bits.template operator()<std::uint32_t>()); break;
    case ScalarType::i32: w.i32(bits.template operator()<std::int32_t>()); break;
    case ScalarType::u64: w.u64(bits.template operator()<std::uint64_t>()); break;
    case ScalarType::i64: w.i64(bits.template operator()<std::int64_t>()); break;
    case ScalarType::f32: w.f32(bits.template operator()<float>()); break;
    case ScalarType::f64: w.f64(bits.template operator()<double>()); break;
    case ScalarType::boolean: {
        std::optional<std::uint8_t> b = bits.template operator()<std::uint8_t>();
        w.boolean(b ? std::optional<bool>(*b != 0) : std::nullopt);
        break;
    }
    }
}

template <class Alloc> void walk(PacketWriter &w, const Node<Alloc> &n) {
    switch (n.kind) {
    case NodeKind::nil: w.nil(); return;
    case NodeKind::scalar: scalar_into(w, n); return;
    case NodeKind::array:
        if (!n.present) {
            switch (static_cast<SequenceType>(n.type)) {
            case SequenceType::u8_array: w.template array<std::uint8_t>(std::nullopt); return;
            case SequenceType::i8_array: w.template array<std::int8_t>(std::nullopt); return;
            case SequenceType::u16_array: w.template array<std::uint16_t>(std::nullopt); return;
            case SequenceType::i16_array: w.template array<std::int16_t>(std::nullopt); return;
            case SequenceType::u32_array: w.template array<std::uint32_t>(std::nullopt); return;
            case SequenceType::i32_array: w.template array<std::int32_t>(std::nullopt); return;
            case SequenceType::u64_array: w.template array<std::uint64_t>(std::nullopt); return;
            case SequenceType::i64_array: w.template array<std::int64_t>(std::nullopt); return;
            case SequenceType::f32_array: w.template array<float>(std::nullopt); return;
            default: w.template array<double>(std::nullopt); return;
            }
        }
        switch (static_cast<SequenceType>(n.type)) {
        case SequenceType::u8_array: array_into<std::uint8_t>(w, n); return;
        case SequenceType::i8_array: array_into<std::int8_t>(w, n); return;
        case SequenceType::u16_array: array_into<std::uint16_t>(w, n); return;
        case SequenceType::i16_array: array_into<std::int16_t>(w, n); return;
        case SequenceType::u32_array: array_into<std::uint32_t>(w, n); return;
        case SequenceType::i32_array: array_into<std::int32_t>(w, n); return;
        case SequenceType::u64_array: array_into<std::uint64_t>(w, n); return;
        case SequenceType::i64_array: array_into<std::int64_t>(w, n); return;
        case SequenceType::f32_array: array_into<float>(w, n); return;
        default: array_into<double>(w, n); return;
        }
    case NodeKind::bitset: w.bitset(n.present ? std::optional(n.bytes) : std::nullopt); return;
    case NodeKind::str: w.str(n.present ? std::optional(n.str) : std::nullopt); return;
    case NodeKind::strs: w.strs(n.present ? std::optional(n.strs) : std::nullopt); return;
    case NodeKind::branch:
        if (!n.present) {
            w.no_branch();
            return;
        }
        w.branch(static_cast<std::uint32_t>(n.children.size()), [&n](PacketWriter &b) {
            for (const Node<Alloc> &c : n.children) walk(b, c);
        });
        return;
    case NodeKind::graft: w.graft(n.bytes); return;
    }
}

} // namespace view_detail

// This encodes a tree by walking it onto `PacketWriter`. The first buffer has
// `init_cap` bytes; each time the packet overflows it, the tree is walked
// again into one twice as large, up to `max_cap`. A `max_cap` below `HDR_SZ`
// or above `MAX_PKT_SZ` is EINVAL; a packet larger than `max_cap` is
// EOVERFLOW.
template <class Alloc, class View>
Result<std::vector<std::uint8_t, typename std::allocator_traits<Alloc>::template rebind_alloc<std::uint8_t>>> encode(const View &view, TreeEncoderConfig opts = {},
                                                                                                                     const Alloc &alloc = Alloc()) {
    using Buffer = std::vector<std::uint8_t, typename std::allocator_traits<Alloc>::template rebind_alloc<std::uint8_t>>;
    std::uint32_t max_cap = opts.max_cap.value_or(MAX_PKT_SZ);
    if (max_cap < HDR_SZ || max_cap > MAX_PKT_SZ) return make_error(Errno::einval);
    std::uint32_t cap = std::max(HDR_SZ, std::min(opts.init_cap, max_cap));
    Buffer buf(alloc);
    for (;;) {
        buf.assign(cap, 0);
        PacketWriter w(std::span<std::uint8_t>(buf.data(), buf.size()));
        view_detail::walk(w, view.node());
        Result<std::uint32_t> n = w.finish();
        if (n) {
            buf.resize(*n);
            return buf;
        }
        if (n.error().errnum != Errno::eoverflow || cap == max_cap) return n.error();
        cap = cap > max_cap / 2 ? max_cap : cap * 2;
    }
}

} // namespace xpute::xtp

// cpp/xpute/link/cabi.hpp

// A packet's values in the Component Model's Canonical ABI layout, as
// xpute-link's `cabi.rs` lays them: each at its type's alignment,
// little-endian, a record's fields in order, a variant its discriminant and
// then its payload. A string's or a list's bytes lie past the packet's fixed
// part, and their pointer is an offset from the packet's start. A packet past
// the bytes it is given spoils, and `Lower::finish` answers EMSGSIZE. A packet
// that does not hold what its type says spoils its reading, whose reads answer
// zero from then on, and `Lift::finish` answers EBADMSG.

namespace xpute::link {

static_assert(std::endian::native == std::endian::little, "a packet's numbers are copied as the machine holds them");

// A number a list holds, whose bytes, every one set, are its value
// little-endian: a list of them crosses as its bytes, in one copy.
template <class T>
concept Number = (std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>;

// A value one put or get carries: a number, or a bool as one byte.
template <class T>
concept Scalar = Number<T> || std::same_as<T, bool>;

// Writes a packet into the bytes it is given: its fixed part first
// (`start`), then the bytes of its strings and lists past it. A writer
// nothing was written by answers 0, no packet.
class Lower {
  public:
    // A writer over `buf`, nothing written there yet.
    explicit Lower(std::span<std::uint8_t> buf) noexcept : buf_(buf), end_(0), overflow_(false) {}

    // A packet of a fixed part `fixed_sz` long, zeroed, in `buf`.
    Lower(std::span<std::uint8_t> buf, std::size_t fixed_sz) noexcept : Lower(buf) { start(fixed_sz); }

    // Starts the packet with a fixed part `fixed_sz` long, zeroed.
    void start(std::size_t fixed_sz) noexcept {
        overflow_ = fixed_sz > buf_.size();
        if (!overflow_) std::memset(buf_.data(), 0, fixed_sz);
        end_ = fixed_sz;
    }

    template <Scalar T> void put(std::size_t at, T v) noexcept {
        if (overflow_ || at + sizeof v > buf_.size()) {
            overflow_ = true;
            return;
        }
        if constexpr (std::same_as<T, bool>) buf_[at] = v ? 1 : 0;
        else std::memcpy(buf_.data() + at, &v, sizeof v);
    }

    // Takes `len` elements of `sz` bytes at `align` past what is written,
    // zeroed, and puts their offset and count at `at`: a list's or a
    // string's pointer and length. It answers where they begin, or none
    // where they run past the bytes it is given.
    std::optional<std::size_t> list(std::size_t at, std::size_t len, std::size_t sz, std::size_t align) noexcept {
        std::size_t base = (end_ + align - 1) / align * align;
        std::size_t end = base + len * sz;
        if (overflow_ || end > buf_.size()) {
            overflow_ = true;
            return std::nullopt;
        }
        std::memset(buf_.data() + end_, 0, end - end_);
        end_ = end;
        put(at, static_cast<std::uint32_t>(base));
        put(at + 4, static_cast<std::uint32_t>(len));
        return base;
    }

    // Puts the numbers `v` past what is written, at their alignment, and
    // their offset and count at `at`.
    template <Number T> void arr(std::size_t at, std::span<const T> v) noexcept {
        auto base = list(at, v.size(), sizeof(T), sizeof(T));
        if (base && !v.empty()) std::memcpy(buf_.data() + *base, v.data(), v.size_bytes());
    }

    void str(std::size_t at, std::string_view s) noexcept {
        arr<std::uint8_t>(at, {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()});
    }

    // The packet's length, or EMSGSIZE where it is past the bytes it is given.
    Result<std::size_t> finish() const noexcept {
        if (overflow_) return make_error(Errno::emsgsize);
        return end_;
    }

  private:
    std::span<std::uint8_t> buf_;
    std::size_t end_;
    bool overflow_;
};

// Reads a packet's values where its type says they lie. What lies past the
// packet, a misaligned list, a string not UTF-8 or a discriminant that names
// no case spoils the reading.
class Lift {
  public:
    explicit Lift(std::span<const std::uint8_t> pkt) noexcept : pkt_(pkt) {}

    // The packet's bytes, or none for no packet.
    std::optional<std::span<const std::uint8_t>> pkt() const noexcept {
        if (pkt_.empty()) return std::nullopt;
        return pkt_;
    }

    // A bool reads 0 as false and any other byte as true, as the ABI lifts
    // it.
    template <Scalar T> T get(std::size_t at) noexcept {
        T v{};
        if (malformed_ || at + sizeof v > pkt_.size()) {
            malformed_ = true;
            return v;
        }
        if constexpr (std::same_as<T, bool>) v = pkt_[at] != 0;
        else std::memcpy(&v, pkt_.data() + at, sizeof v);
        return v;
    }

    // A discriminant of `n` cases at `at`, its width by `n`.
    std::uint32_t case_(std::size_t at, std::size_t n) noexcept {
        std::uint32_t d = n <= 256 ? get<std::uint8_t>(at) : n <= 65536 ? get<std::uint16_t>(at) : get<std::uint32_t>(at);
        if (d < n) return d;
        malformed_ = true;
        return 0;
    }

    // The elements of `sz` bytes at `align` whose offset and count lie at
    // `at`: where they begin, and how many.
    std::pair<std::size_t, std::size_t> list(std::size_t at, std::size_t sz, std::size_t align) noexcept {
        std::size_t base = get<std::uint32_t>(at);
        std::uint64_t len = get<std::uint32_t>(at + 4);
        if (malformed_ || base % align != 0 || base > pkt_.size() || len * sz > pkt_.size() - base) {
            malformed_ = true;
            return {0, 0};
        }
        return {base, static_cast<std::size_t>(len)};
    }

    // A list of numbers at `at`, copied out.
    template <Number T> std::vector<T> arr(std::size_t at) noexcept {
        auto [base, len] = list(at, sizeof(T), sizeof(T));
        std::vector<T> v(len);
        if (len != 0) std::memcpy(v.data(), pkt_.data() + base, len * sizeof(T));
        return v;
    }

    std::string_view str(std::size_t at) noexcept {
        auto [base, len] = list(at, 1, 1);
        std::string_view s(reinterpret_cast<const char *>(pkt_.data() + base), len);
        if (is_utf8(s)) return s;
        malformed_ = true;
        return {};
    }

    // Nothing, or EBADMSG where the packet did not hold what was read.
    Result<void> finish() const noexcept {
        if (malformed_) return make_error(Errno::ebadmsg);
        return {};
    }

  private:
    std::span<const std::uint8_t> pkt_;
    bool malformed_ = false;
};

} // namespace xpute::link

// cpp/xpute/link/abi/types.hpp
//
// GENERATED from link/wit (xpute-link-gen wit.rs)

namespace xpute::abi::link::types {

namespace _rt {
using ::xpute::link::Lift;
using ::xpute::link::Lower;
} // namespace _rt

struct Token {
    struct Int {
        std::int32_t val;
    };
    struct Str {
        std::string val;
    };

    std::variant<Int, Str> variant;

    static constexpr std::size_t SZ = 12;
    static constexpr std::size_t ALIGN = 4;

    void store(_rt::Lower &w, std::size_t at) const noexcept {
        switch (this->variant.index()) {
        case 0: w.put<std::uint8_t>(at, 0); w.put<std::int32_t>(at + 4, std::get<0>(this->variant).val); break;
        case 1: w.put<std::uint8_t>(at, 1); w.str(at + 4, std::get<1>(this->variant).val); break;
        }
    }

    static Token load(_rt::Lift &r, std::size_t at) noexcept {
        switch (r.case_(at, 2)) {
        case 0: return Token{Int{r.get<std::int32_t>(at + 4)}};
        case 1: return Token{Str{std::string(r.str(at + 4))}};
        }
        return {};
    }
};

struct Lent {
    std::uint64_t buf;
    std::uint32_t nbyte;

    static constexpr std::size_t SZ = 16;
    static constexpr std::size_t ALIGN = 8;

    void store(_rt::Lower &w, std::size_t at) const noexcept {
        w.put<std::uint64_t>(at, this->buf);
        w.put<std::uint32_t>(at + 8, this->nbyte);
    }

    static Lent load(_rt::Lift &r, std::size_t at) noexcept {
        return Lent{
            .buf = r.get<std::uint64_t>(at),
            .nbyte = r.get<std::uint32_t>(at + 8),
        };
    }
};

} // namespace xpute::abi::link::types

// cpp/xpute/link/frame.spec.hpp
//
// GENERATED from link/spec/frame.json5 (xpute-gen data.rs)
//
// A frame is one method called across the link, four 32-bit words; TAG, CMD, RESULT and
// PKT are their indices, little-endian. The tag is the request, unique among the sender's
// pending ones, and a later answer carries it back. The command is the RES bit, the handle
// the callee issued above METHOD_BITS, and the method's place among its object's below
// them. An answer carries 0 or a negative errno, its value in its packet. The method's args, a packet in the Canonical ABI's layout,
// lie right past the words, and PKT says how long they are (link/wit/types.wit). ROOT
// is the handle every side implements, OPEN, CANCEL and OPEN_AT its methods, DROP the method
// every handle has, NAMED marks an interface's handle, SLOT_BITS splits an issued handle
// into its slot and its generation, and TRAP and TRAP_AT are a native guest's stop.

namespace xpute::link::frame {

// The request, which a later answer carries back.
inline constexpr std::uint32_t TAG = 0u;

// RES, the handle and the method.
inline constexpr std::uint32_t CMD = 1u;

// On an answer, 0 or a negative errno: the call refused, by the link or by the method as a caller's bug (EINVAL), or a method's failure where its WIT has no result; on a request, 0.
inline constexpr std::uint32_t RESULT = 2u;

// The length in bytes of the frame's packet, which lies right past its words; 0 when it has none.
inline constexpr std::uint32_t PKT = 3u;

// The frame's length in words.
inline constexpr std::uint32_t WORDS = 4u;

// The bit of the command that marks an answer.
inline constexpr std::uint32_t RES = 2147483648u;

// The bits of the command below the handle, which hold the method.
inline constexpr std::uint32_t METHOD_BITS = 8u;

// The root's handle, which every side implements.
inline constexpr std::uint32_t ROOT = 0u;

// The root's method that opens an object by its URI.
inline constexpr std::uint32_t OPEN = 0u;

// The root's method that cancels a call the sender has pending, named by its tag, the one
// arg. A call the callee can stop it stops, and answers ECANCELED; any other runs on. Either
// way it settles by its own answer, and what it lent stays lent until then.
inline constexpr std::uint32_t CANCEL = 1u;

// The root's method that opens an object under a handle the sender has issued for it:
// the handle, which of the receiver's mounted openers opens it, by its place among them, and
// the URI. A runtime that stands in for a UI on another thread answers the guest's open at
// once, issuing the handle, and carries the open to the UI's end as this. It takes no
// answer.
inline constexpr std::uint32_t OPEN_AT = 2u;

// The method every handle has, which lets it go.
inline constexpr std::uint32_t DROP = 255u;

// The handle's top bit, which marks it named: an interface's, its bits below the hash of
// the interface's name the generator writes (xpute-link-gen wit.rs). An interface with
// functions of its own is mounted under it with no open; one of resources alone names its
// openers by it. An issued handle has it clear.
inline constexpr std::uint32_t NAMED = 4194304u;

// The bits of a handle that name its slot, below its generation.
inline constexpr std::uint32_t SLOT_BITS = 20u;

// What a native guest's export answers where the guest stopped, caught at the export so nothing leaves into its caller: as a module's trap. The guest is over, and its host enters it no more.
inline constexpr std::int32_t TRAP = -2147483648;

// What a native guest's link_init answers in its layout's place where the guest stopped, as TRAP is for its other exports: an odd address, where no layout lies.
inline constexpr std::uint64_t TRAP_AT = 4294967295ull;

// The words of the link's layout, which a guest's link_init lays and answers the address
// of: the inbox's address, a u64 in two words, its low word first, then a frame's cap, then a
// word unused, so the inbox, which follows the layout's words, lies on 8 bytes. The outbox
// follows the inbox.
namespace LAYOUT {

inline constexpr std::size_t INBOX_AT = 0u;

inline constexpr std::size_t CAP_AT = 2u;

inline constexpr std::size_t PAD_AT = 3u;

inline constexpr std::size_t WORDS = 4u;
} // namespace LAYOUT

} // namespace xpute::link::frame

// cpp/xpute/link/mem.hpp

// The guest's memory, which every address on the link names, a u64 on every
// platform (link/wit/types.wit `lent`), as xpute-link's `mem.rs` reads it: a
// module's offset into its own memory, which begins at 0, so its offset is its
// pointer, or a native guest's pointer, which a host linked into the same
// process reads as its own. An address past what the platform's pointer holds
// is a bug.

namespace xpute::link::mem {

using Lent = ::xpute::abi::link::types::Lent;

// The `len` bytes at `at`.
std::span<std::uint8_t> bytes(std::uint64_t at, std::uint32_t len) noexcept;

namespace detail {
// The address of `p`.
std::uint64_t addr(const void *p) noexcept;
} // namespace detail

// The address of the value `v`, such as a word the guest lends its host to
// store into.
template <class T> std::uint64_t addr(const T &v) noexcept {
    return detail::addr(&v);
}

// The range `s` as a lent buffer, where it lies and how many bytes it takes
// (link/wit/types.wit `lent`). An empty range has no address, and lends
// (0, 0), in a module and natively alike; one past 4 GiB is a bug.
template <class T> Lent lent(std::span<const T> s) noexcept {
    if (s.empty()) return Lent{.buf = 0, .nbyte = 0};
    ensure(s.size_bytes() <= UINT32_MAX, Errno::efault);
    return Lent{.buf = addr(s.front()), .nbyte = static_cast<std::uint32_t>(s.size_bytes())};
}

// The little-endian word at `at`, aligned or not.
std::uint32_t word(std::uint64_t at) noexcept;

// Writes the little-endian word `v` at `at`, aligned or not.
void set_word(std::uint64_t at, std::uint32_t v) noexcept;

// Loads the aligned word at `at` with acquire: a word that hands memory
// between the guest and its host, such as a fence the guest reads while the
// host answers it, which both sides touch only atomically, so whatever the
// other side wrote before it stored the word is seen.
std::uint32_t load_acquire(std::uint64_t at) noexcept;

// Stores `v` into such a word with release, so whatever this side wrote
// before is seen by the side that loads it.
void store_release(std::uint64_t at, std::uint32_t v) noexcept;

} // namespace xpute::link::mem

// cpp/xpute/link/frame.hpp

// A frame's words are laid out by `link/spec/frame.json5`, as xpute-link's
// `frame.rs` lays them: the request's tag, its command, the result, and the
// packet's length. The command is the `RES` bit, the handle the callee issued
// and the method's place among its object's. The packet, the method's args or
// its answer's value in the Canonical ABI's layout (`cabi.hpp`), lies right
// past the words. The words are little-endian, as both forms' memory is.

namespace xpute::link {

// The size of a frame's words, its header before its packet.
inline constexpr std::uint32_t HDR_SZ = frame::WORDS * 4;

// `NAMED` is a handle's top bit.
static_assert(frame::NAMED == frame::RES >> frame::METHOD_BITS >> 1);

struct Frame {
    std::uint32_t tag;
    // An answer, to the receiver's request `tag`.
    bool res;
    std::uint32_t handle;
    std::uint32_t method;
    std::int32_t result;
    std::uint32_t pkt_len;

    // The frame laid at `at`.
    static Frame at(std::uint64_t at) noexcept {
        auto w = [at](std::uint32_t k) { return mem::word(at + 4 * std::uint64_t{k}); };
        std::uint32_t cmd = w(frame::CMD);
        return Frame{
            .tag = w(frame::TAG),
            .res = (cmd & frame::RES) != 0,
            .handle = (cmd & ~frame::RES) >> frame::METHOD_BITS,
            .method = cmd & ((1u << frame::METHOD_BITS) - 1),
            .result = static_cast<std::int32_t>(w(frame::RESULT)),
            .pkt_len = w(frame::PKT),
        };
    }

    // Lays the frame's words at `at`.
    void lay(std::uint64_t at) const noexcept {
        auto set = [at](std::uint32_t k, std::uint32_t v) { mem::set_word(at + 4 * std::uint64_t{k}, v); };
        set(frame::TAG, tag);
        set(frame::CMD, (res ? frame::RES : 0) | handle << frame::METHOD_BITS | method);
        set(frame::RESULT, static_cast<std::uint32_t>(result));
        set(frame::PKT, pkt_len);
    }

    // The packet of the frame of `cap` bytes laid at `at`, none where it has
    // none; a packet past the cap is EBADMSG.
    Result<std::optional<std::span<const std::uint8_t>>> pkt(std::uint64_t at, std::uint32_t cap) const noexcept {
        if (pkt_len == 0) return std::optional<std::span<const std::uint8_t>>();
        if (std::uint64_t{HDR_SZ} + pkt_len > cap) return make_error(Errno::ebadmsg);
        return std::optional<std::span<const std::uint8_t>>(mem::bytes(at + HDR_SZ, pkt_len));
    }
};

} // namespace xpute::link

// cpp/xpute/link/resource.hpp

// A handle names an object on the side that implements it, which issued it
// (`link.hpp`), as xpute-link's `resource.rs` issues it. As the component
// model's table does, a handle's slot is used again once its object is
// dropped, so that a side issues handles for as long as it runs, the slots
// dropped first used first, so that each comes back as late as it can; the
// slot is the handle's low `SLOT_BITS`, from 1, the root being 0. Above it a
// generation, which a slot's drop advances, keeps a frame late for a dropped
// object from reaching the one in its slot now, until the slot has been used
// `GENERATION_MASK` + 1 times. More live objects than the slots hold is a
// bug.

namespace xpute::link {

using Handle = std::uint32_t;

// The objects a side implements, by handle: slot `k` at `k - 1`, and the queue
// of the slots dropped, through their `next`, from `head_` to `tail_`.
template <class T> class Table {
  public:
    // Issues a handle, for `put` to fill; `remove` gives it back unfilled.
    Handle issue() noexcept {
        std::uint32_t k;
        if (head_ != 0) {
            k = head_;
            head_ = std::exchange(slots_[k - 1].next, ISSUED);
            if (head_ == 0) tail_ = 0;
        } else {
            slots_.push_back(Slot{0, std::nullopt, ISSUED});
            k = static_cast<std::uint32_t>(slots_.size());
            ensure(k <= SLOT_MASK, Errno::emfile, k);
        }
        return k | slots_[k - 1].generation << frame::SLOT_BITS;
    }

    // Puts `v` under `h`: a handle this table issued, or one the other end
    // issued, as a UI's end holds what the runtime opens on it under the
    // guest's end's handles. The slot `h` names is then issued at `h`'s
    // generation, taken out of the queue if it was free, it and the slots
    // before it made free where there were none; one issued at another
    // generation is a bug.
    void put(Handle h, T v) noexcept {
        std::uint32_t k = h & SLOT_MASK;
        ensure(k != 0, Errno::ebadf, h);
        while (slots_.size() < k) {
            slots_.push_back(Slot{0, std::nullopt, ISSUED});
            remove(static_cast<std::uint32_t>(slots_.size()));
        }
        Slot &s = slots_[k - 1];
        if (s.next != ISSUED) {
            std::uint32_t prev = 0;
            for (std::uint32_t c = head_; c != k; c = slots_[c - 1].next) prev = c;
            if (prev == 0) head_ = s.next;
            else slots_[prev - 1].next = s.next;
            if (tail_ == k) tail_ = prev;
            s.next = ISSUED;
            s.generation = h >> frame::SLOT_BITS;
        }
        ensure(s.generation == h >> frame::SLOT_BITS, Errno::ebadf, h);
        s.val = std::move(v);
    }

    // Puts `v` at a handle issued for it, and answers the handle.
    Handle add(T v) noexcept {
        Handle h = issue();
        put(h, std::move(v));
        return h;
    }

    T *get(Handle h) noexcept {
        Slot *s = slot(h);
        return s != nullptr && s->val ? &*s->val : nullptr;
    }

    // Drops `h`: its object, if it holds one, and its slot, last in the
    // queue, for a later handle of the next generation. A slot that is free
    // already is no handle's.
    std::optional<T> remove(Handle h) noexcept {
        Slot *s = slot(h);
        if (s == nullptr || s->next != ISSUED) return std::nullopt;
        std::optional<T> v = std::exchange(s->val, std::nullopt);
        s->generation = (s->generation + 1) & GENERATION_MASK;
        s->next = 0;
        std::uint32_t k = h & SLOT_MASK;
        if (tail_ == 0) head_ = k;
        else slots_[tail_ - 1].next = k;
        tail_ = k;
        return v;
    }

  private:
    static constexpr std::uint32_t SLOT_MASK = (1u << frame::SLOT_BITS) - 1;
    // The largest generation an issued handle holds above its slot, below
    // `NAMED`.
    static constexpr std::uint32_t GENERATION_MASK = (frame::NAMED - 1) >> frame::SLOT_BITS;
    // What an issued slot's `next` holds, which no free slot's does.
    static constexpr std::uint32_t ISSUED = UINT32_MAX;

    // A slot: its generation, its object while it holds one, and the next
    // free slot while it is free, 0 for none; `ISSUED` while it is issued.
    struct Slot {
        std::uint32_t generation;
        std::optional<T> val;
        std::uint32_t next;
    };

    // The slot `h` names, where `h` is of its generation.
    Slot *slot(Handle h) noexcept {
        std::uint32_t k = h & SLOT_MASK;
        if (k == 0 || k > slots_.size()) return nullptr;
        Slot &s = slots_[k - 1];
        return s.generation == h >> frame::SLOT_BITS ? &s : nullptr;
    }

    std::vector<Slot> slots_;
    std::uint32_t head_ = 0;
    std::uint32_t tail_ = 0;
};

} // namespace xpute::link

// cpp/xpute/link/link.hpp

// The link (`link/wit/types.wit`), as xpute-link's `link.rs` runs it.
// Whatever is called is an object, called by its methods under a handle: an
// interface's named handle, the hash of its name (`frame.json5` `NAMED`), or
// a resource's, named by a URI, a tuple of tokens, and opened by the root's
// `open` to a handle the implementing side issues. A frame is one method
// called: its tag the request, unique among the sender's pending ones, its
// command the handle, the method and a `RES` bit (`frame.hpp`), its packet
// the args in the Canonical ABI's layout (`cabi.hpp`). A frame crosses one
// way through one function, `link_out` from the guest and `link_in` into it,
// and each crossing returns at once what the method came to: its result, or
// EINPROGRESS for one that answers later, by a frame marked `RES` with the
// request's tag. So each side has the same ways:
//
// - `call` takes what the crossing returns, for a method that always answers
//   at once, and stops as a bug on EINPROGRESS;
// - `send` hands it to its answer, and an `async func` is sent;
// - `mount_named` mounts an interface's implementation under its named
//   handle, and `mount` puts the opener of an interface's resources the side
//   implements under a URI's leading tokens, the longest matching winning;
//   `open` and `drop` reach the root's open and a handle's drop on the other
//   side;
// - `cancel` cancels a request sent and pending (the root's `CANCEL`), and
//   `on_cancel` puts what runs if a request the side answers is canceled.
//
// The link holds these on both ends:
//
// - One guest entry at a time: no `link_in` starts before another returns,
//   and the outside never enters the guest nested.
// - An answer is never run inside `send`, and a result at once and a later
//   one take the same path. On the guest, the answers run at the end of the
//   entry, once its handler has returned, just before `link_in` returns. An
//   entry ends when no answer is due and the program's end has no work left:
//   those made due while answers run, or by the end's work, run in it too.
// - A frame's packet is the receiver's for the crossing alone, and memory its
//   args lend by (at, len) is the callee's until the answer: the caller keeps
//   it, as the answer it owns.
// - The callee answers in the frame the request came in: it lays it again,
//   marked RES, with its result and its return data as the packet. A call's
//   return data is readable until the next crossing; a send's answer gets a
//   copy, which it owns.
//
// C++ is written for the guest alone, so a packet is always one its side
// writes: none crosses here already written, as one the UI sent does on a
// host.

#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>

namespace xpute::link {

using Token = ::xpute::abi::link::types::Token;
using Lent = ::xpute::abi::link::types::Lent;

using Tag = std::uint32_t;
using Method = std::uint32_t;

using frame::CANCEL;
using frame::DROP;
using frame::NAMED;
using frame::OPEN;
using frame::ROOT;
using frame::TRAP;
using frame::TRAP_AT;

// What an async method lends: a vector of plain values, whose bytes the
// other side may write, which lie in one run of the guest's memory and stay
// put while it is moved, and so while the answer owns it
// (`link/wit/types.wit` `lent`).
template <class L>
concept Lend = requires(L &l) {
    []<class T, class A>(std::vector<T, A> &)
        requires std::is_trivially_copyable_v<T>
    {}(l);
    std::data(l);
};

// `l` as a lent buffer: where it lies in the guest's memory, and how many
// bytes it takes.
template <class L> Lent lent(L &l) noexcept {
    return mem::lent(std::span<const std::remove_reference_t<decltype(*std::data(l))>>(std::data(l), std::size(l)));
}

// A packet a side writes: its fixed part's size, and what writes it
// (`Lower`).
struct Packet {
    std::size_t fixed_sz;
    std::function<void(Lower &)> write;
};

// A method's args. None is no packet.
using Args = std::optional<Packet>;

// A later answer's return data, written as its frame is laid. None is no
// packet.
using Data = std::optional<Packet>;

// An object a side implements: given a method, the request's tag, for a later
// answer (`task_return`), its args (`args`, empty for none) and the writer of
// its answer's value (`ret`), it answers its result, a status (link.ts
// `LinkObject`). The reader and the writer are over the frame the request
// came in, so a method that answers a value writes it once its args are
// read, starting the packet (`Lower::start`), and one that writes none
// answers no packet.
class Object {
  public:
    virtual ~Object() = default;
    virtual std::int32_t call(Method m, Tag tag, Lift args, Lower &ret) = 0;
};

// What hears a sent method's answer: its result, and a copy of its return
// data, which it owns.
using Answer = std::function<void(std::int32_t, std::optional<std::vector<std::uint8_t>>)>;

// What answers an async method's request once the method has its value, and
// where the method says how it stops if its caller cancels it (`CANCEL`).
template <class T> class Return {
  public:
    // The answer to request `tag` on the side whose `on_cancel` is `on`.
    Return(Tag tag, void (*on)(Tag, std::function<void()>), std::function<void(Result<T>)> answer) noexcept
        : tag_(tag), on_(on), answer_(std::move(answer)) {}

    // Answers the request with `v`.
    void answer(Result<T> v) const { answer_(std::move(v)); }

    // Runs `stop` if the caller cancels the request before its answer; the
    // method then answers ECANCELED once it has stopped. A method that
    // cannot stop says nothing, runs on and answers as it would.
    void on_cancel(std::function<void()> stop) const { on_(tag_, std::move(stop)); }

  private:
    Tag tag_;
    void (*on_)(Tag, std::function<void()>);
    std::function<void(Result<T>)> answer_;
};

// What opens the object a URI names, given the handle it is issued.
using Opener = std::function<Result<std::unique_ptr<Object>>(std::span<const Token>, Handle)>;

// The link's layout: the guest's inbox and a frame's cap. The outbox lies
// right past the inbox.
struct Layout {
    std::uint64_t inbox;
    std::uint32_t cap;

    // The layout `init` wrote at `at` (frame.json5 `LAYOUT`).
    static Layout at(std::uint64_t at) noexcept {
        auto w = [at](std::size_t k) { return mem::word(at + 4 * k); };
        return Layout{join64(w(frame::LAYOUT::INBOX_AT), w(frame::LAYOUT::INBOX_AT + 1)), w(frame::LAYOUT::CAP_AT)};
    }

    // Writes the layout at `at`, whose words are the caller's.
    void lay(std::uint64_t at) const noexcept {
        auto set = [at](std::size_t k, std::uint32_t v) { mem::set_word(at + 4 * k, v); };
        auto [lo, hi] = split64(inbox);
        set(frame::LAYOUT::INBOX_AT, lo);
        set(frame::LAYOUT::INBOX_AT + 1, hi);
        set(frame::LAYOUT::CAP_AT, cap);
    }
};

// One side of the link: what both sides do, once. A side lays its frames in
// its own box and crosses by its own function: the guest lays in its outbox
// and crosses by `link_out`. It takes the other side's frame where that side
// laid it (`recv`), and runs the answers due when its owner says the work at
// hand is done (`run_due`): the guest as an entry ends. A later answer this
// side gives while it is crossing is written then and laid with the answers
// due, as xpute-link `link.rs` `Side` holds it.
class Side {
  public:
    Side(std::uint64_t (*out_box)(Layout), std::int32_t (*cross)(std::uint64_t)) noexcept : out_box_(out_box), cross_(cross) {}

    void set_layout(Layout l) noexcept { layout_ = l; }

    Layout layout() const noexcept { return or_bug(layout_, Errno::enotrecoverable); }

    // Whether this side is crossing, its control on the other side.
    bool crossing() const noexcept { return crossing_; }

    // Runs `f`, a crossing of this side's own that is no frame, as crossing.
    template <class F> auto crossed(F &&f) {
        bool was = std::exchange(crossing_, true);
        if constexpr (std::is_void_v<decltype(f())>) {
            f();
            crossing_ = was;
        } else {
            auto v = f();
            crossing_ = was;
            return v;
        }
    }

    // Puts `open` as the opener of the URIs that start with `prefix`, whose
    // objects are the resources of the interface of the named handle `h`.
    void mount(Handle h, std::vector<Token> prefix, Opener open);

    // Mounts `o` under its interface's named handle `h`, which the other
    // side calls with no open.
    void mount_named(Handle h, std::unique_ptr<Object> o);

    // Adds `o` under a handle issued for it, which a method answers for
    // the other side to call `o` by: a resource a function returns.
    Handle add(std::unique_ptr<Object> o) { return objects_.add(std::move(o)); }

    // Opens the object `uri` names on the other side, and answers its
    // handle.
    Result<Handle> open(std::span<const Token> uri);

    // Lets the object `h` names on the other side go.
    void drop(Handle h);

    // Calls `m` of `h`, which answers at once: its result, and its return
    // data, readable until the next crossing.
    std::pair<std::int32_t, std::optional<std::span<const std::uint8_t>>> call(Handle h, Method m, const Args &args);

    // Sends `m` of `h`, its answer pending from before the crossing, so a
    // later answer given inside it finds its request, and answers the
    // request's tag.
    Tag send(Handle h, Method m, const Args &args, Answer answer);

    // Cancels the request `t` this side sent, while its answer is pending;
    // its answer still comes, and is heard as any other (`CANCEL`).
    void cancel(Tag t);

    // Puts `stop` as how the request `t` this side answers stops if it is
    // canceled (`Return::on_cancel`).
    void on_cancel(Tag t, std::function<void()> stop);

    // Answers the request `t`, whose method answered EINPROGRESS, with
    // `result` and its return data, as the component model's `task.return`.
    // A later answer is found by its tag alone. While this side is crossing,
    // its packet is written now and laid after the work at hand (`run_due`);
    // a packet past the cap is answered EMSGSIZE with no data, as one laid at
    // once is.
    void task_return(Tag t, std::int32_t result, const Data &data);

    // The frame the other side laid at `at`: a later answer, made due, or a
    // method, run by its object, its answer laid in the frame it came in.
    std::int32_t recv(std::uint64_t at);

    // Lays the later answers given while crossing, then runs the answers
    // due, those made due while they run too.
    void run_due();

    // Whether an answer is due.
    bool has_due() const noexcept { return !due_.empty() || !completions_.empty(); }

  private:
    // An answer due: what hears it, its result and its return data.
    struct Due {
        Answer answer;
        std::int32_t result;
        std::optional<std::vector<std::uint8_t>> data;
    };

    // A later answer given while this side is crossing: its request, result
    // and packet, written.
    struct Completion {
        Tag tag;
        std::int32_t result;
        std::optional<std::vector<std::uint8_t>> pkt;
    };

    std::int32_t run(Handle h, Method m, Tag tag, Lift args, Lower &ret);
    Result<Handle> opened(Lift &args);
    Tag tag() noexcept;
    Result<void> lay(std::uint64_t at, Frame words, const Args &pkt);
    std::pair<std::int32_t, std::optional<std::span<const std::uint8_t>>> out(Tag tag, Handle h, Method m, bool res, std::int32_t result, const Args &pkt);

    std::optional<Layout> layout_;
    // The box this side lays its frames in.
    std::uint64_t (*out_box_)(Layout);
    // Crosses with the frame laid at `at`, and answers what the method came
    // to.
    std::int32_t (*cross_)(std::uint64_t);
    // Whether this side is crossing: control is on the other side.
    bool crossing_ = false;
    // Where the next request's tag is sought.
    Tag next_ = 0;
    // The openers, each with the named handle of the interface whose
    // resources it opens, by the URI's leading tokens.
    std::vector<std::tuple<Handle, std::vector<Token>, Opener>> mounts_;
    // The objects this side implements, by the handles it issued.
    Table<std::unique_ptr<Object>> objects_;
    // The interfaces this side implements, by their named handles (`NAMED`).
    std::map<Handle, std::unique_ptr<Object>> named_;
    // The answers of the sent requests that answer later, by tag.
    std::map<Tag, Answer> pending_;
    // How each request this side answers stops if its caller cancels it, by
    // tag, until it answers.
    std::map<Tag, std::function<void()>> cancels_;
    // The answers due, run after the work at hand.
    std::deque<Due> due_;
    // The later answers given while crossing, laid after the work at hand.
    std::deque<Completion> completions_;
};

// The URI of names alone, as a resource's (package, resource).
std::vector<Token> uri(std::initializer_list<std::string_view> names);

// Writes `uri` at `at` as a `list<token>`, as `ROOT` `OPEN`'s args hold it
// (`types.wit`).
void store_uri(Lower &w, std::size_t at, std::span<const Token> uri);

// The URI the `list<token>` at `at` holds (`store_uri`); a read past the
// packet is `r`'s to report (`Lift::finish`).
std::vector<Token> load_uri(Lift &r, std::size_t at);

// A method's result, its status: answered, or the errno it failed with (a
// refusal, or a method's errno where its WIT has no `result`). Its value, if
// it has one, is in its return data.
Result<void> answer(std::int32_t result) noexcept;

// A method's result and the value `load` reads from its return data `data`
// (`Lift`), refused EBADMSG where it has none or does not hold the value.
template <class D, class F>
auto answer(std::int32_t result, const std::optional<D> &data, F &&load) -> Result<decltype(load(std::declval<Lift &>()))> {
    if (Result<void> a = answer(result); !a) return a.error();
    if (!data) return make_error(Errno::ebadmsg);
    Lift r{std::span<const std::uint8_t>(*data)};
    auto v = load(r);
    if (Result<void> f = r.finish(); !f) return f.error();
    return v;
}

} // namespace xpute::link

// cpp/xpute/link/wait.hpp

// A sent method as an awaitable, over a side's `send`, with no executor of
// its own, as xpute-link's `wait.rs` is a future. The method is sent when the
// wait is made. Its answer keeps the result, the return data and what the
// args lend, and resumes the coroutine that awaits it; the wait hands all
// three back. Destroying the wait before its answer cancels the call
// (`frame.json5` `CANCEL`), as the component model's dropped subtask is
// canceled; what the args lend stays alive until the answer, which still
// comes and owns it (`link/wit/types.wit` `lent`). The wait owns its slot
// until it is destroyed with the answer still to come; the answer owns it
// from then on, and frees it as it comes.
//
// A `Task` is the coroutine that awaits: what C++ needs to write an async
// function, which Rust's `async` gives. It runs as it is made, up to its
// first wait, so a call it makes is sent then, and it holds its frame: a task
// destroyed before it ends destroys the waits it holds, which cancels their
// calls. Awaiting a task resumes the awaiting coroutine once it ends.

#include <coroutine>

namespace xpute::link {

// What a sent method comes to: its result, its return data and what its args
// lent.
template <class L> using Outcome = std::tuple<std::int32_t, std::optional<std::vector<std::uint8_t>>, L>;

// A side's send, which answers the request's tag (`guest/link.hpp` `send`).
using Send = Tag (*)(Handle, Method, const Args &, Answer);

template <class L> class [[nodiscard]] Wait {
  public:
    // Sends `m` of `h` by `by`, a side's send, with `args`, now; `lent` is
    // what the args lend by (at, len), kept until the answer. `cancel` is the
    // side's, which a destruction before the answer calls.
    Wait(Send by, void (*cancel)(Tag), Handle h, Method m, const Args &args, L lent)
        : slot_(new Slot(std::move(lent))), cancel_(cancel) {
        tag_ = by(h, m, args, [slot = slot_](std::int32_t result, std::optional<std::vector<std::uint8_t>> data) {
            if (slot->orphan) {
                delete slot;
                return;
            }
            slot->answer.emplace(result, std::move(data));
            if (std::coroutine_handle<> w = std::exchange(slot->continuation, {})) w.resume();
        });
    }

    Wait(Wait &&o) noexcept : slot_(std::exchange(o.slot_, nullptr)), tag_(o.tag_), cancel_(o.cancel_) {}
    Wait &operator=(Wait &&) = delete;
    Wait(const Wait &) = delete;

    ~Wait() {
        if (slot_ == nullptr) return;
        // Once the answer has come, a destruction cancels nothing.
        if (slot_->answer) {
            delete slot_;
            return;
        }
        slot_->orphan = true;
        cancel_(tag_);
    }

    bool await_ready() const noexcept { return slot_->answer.has_value(); }
    void await_suspend(std::coroutine_handle<> h) noexcept { slot_->continuation = h; }
    Outcome<L> await_resume() {
        auto [result, data] = std::move(*slot_->answer);
        return Outcome<L>(result, std::move(data), std::move(slot_->lent));
    }

  private:
    // Where the answer leaves its result and return data, beside what the
    // args lent, and the coroutine that awaits it.
    struct Slot {
        explicit Slot(L lent) : lent(std::move(lent)) {}
        L lent;
        std::optional<std::pair<std::int32_t, std::optional<std::vector<std::uint8_t>>>> answer;
        std::coroutine_handle<> continuation;
        // Whether the wait is gone, and the answer owns the slot.
        bool orphan = false;
    };

    Slot *slot_;
    Tag tag_ = 0;
    void (*cancel_)(Tag);
};

template <class T> class Task;

namespace task_detail {

struct PromiseBase {
    // The coroutine that awaits the task, resumed as it ends.
    std::coroutine_handle<> continuation;

    std::suspend_never initial_suspend() const noexcept { return {}; }

    struct Final {
        bool await_ready() const noexcept { return false; }
        template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept {
            if (std::coroutine_handle<> a = h.promise().continuation) return a;
            return std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };
    Final final_suspend() const noexcept { return {}; }

    // Nothing is thrown where exceptions are off.
    void unhandled_exception() const noexcept { bug(Errno::enotrecoverable); }
};

template <class T> struct Promise : PromiseBase {
    std::optional<T> val;
    Task<T> get_return_object() noexcept;
    void return_value(T v) noexcept { val.emplace(std::move(v)); }
};

template <> struct Promise<void> : PromiseBase {
    Task<void> get_return_object() noexcept;
    void return_void() const noexcept {}
};

} // namespace task_detail

template <class T> class [[nodiscard]] Task {
  public:
    using promise_type = task_detail::Promise<T>;

    explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    Task(Task &&o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task &operator=(Task &&o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    ~Task() {
        if (h_) h_.destroy();
    }

    // Whether the task has ended.
    bool done() const noexcept { return h_.done(); }

    // Its value, once it has ended; asking before is a bug.
    T get() {
        ensure(h_.done(), Errno::ebusy);
        if constexpr (!std::is_void_v<T>) return std::move(*h_.promise().val);
    }

    bool await_ready() const noexcept { return h_.done(); }
    void await_suspend(std::coroutine_handle<> a) noexcept { h_.promise().continuation = a; }
    T await_resume() { return get(); }

  private:
    std::coroutine_handle<promise_type> h_;
};

namespace task_detail {

template <class T> Task<T> Promise<T>::get_return_object() noexcept {
    return Task<T>(std::coroutine_handle<Promise<T>>::from_promise(*this));
}

inline Task<void> Promise<void>::get_return_object() noexcept {
    return Task<void>(std::coroutine_handle<Promise<void>>::from_promise(*this));
}

} // namespace task_detail

} // namespace xpute::link

// cpp/xpute/guest/stop.hpp

// The guest's report of its own failures to the host, by the stop the host
// implements (`wit/stop/stop.wit`), as `xpute-guest`'s `stop.rs` is a Rust
// guest's.

namespace xpute::guest::stop {

// Tells the host a bug in the guest's own code, with its message where it has
// one, by its stop, before the guest stops; `XPUTE_LINK` runs it as the
// guest's first export starts, so that it covers the guest's whole run. A bug outside an entry, in `init` before the
// host can hear a stop, or while the guest is out on `link_out`, is not told
// (`in_guest`): the trap tells it.
void hook() noexcept;

} // namespace xpute::guest::stop

// cpp/xpute/guest/link.hpp

// The guest's end of the link (`xpute/link/link.hpp`), as `xpute-guest`'s
// `link.rs` is: its one side, which lays in its outbox and crosses by
// `link_out`, its three exports (`XPUTE_LINK`), and the entry they run each
// call in. The machine's names are read here as the guest's callers read them.
//
// A program says nothing of its memory here: what the link lays and lends,
// and its layout, it takes from the memory resource
// the program set as its default (`std::pmr::get_default_resource`), and the
// program picks it and its global `operator new`.

namespace xpute::guest {

// Lays the link over frames of `cap` bytes, a multiple of 8 that holds a
// frame's words, and runs nothing of the program; `enter` and `end` are the
// program's start and end of every entry (`boot`, `recv`), `end` answering
// whether it left work. It answers where the link's layout lies: the inbox's
// address and a frame's cap (frame.json5 `LAYOUT`), then the inbox and the
// outbox.
std::uint64_t init(std::uint32_t cap, void (*enter)(), bool (*end)()) noexcept;

// The guest's first entry, once the host has read the layout. Unlike every
// later entry, it starts by `boot`, which opens what the program's `enter`
// may read, and only then by `enter`; it ends as every entry does.
void boot(void (*boot)());

// The frame the host laid in the inbox: a later answer, to its request, or a
// method, to its object, in an entry of its own.
std::int32_t recv();

// Puts `open` as the opener of the URIs that start with `prefix`, whose
// objects are the resources of the interface of the named handle `h`.
void mount(link::Handle h, std::vector<link::Token> prefix, link::Opener open);

// Mounts `o` under its interface's named handle `h`.
void mount_named(link::Handle h, std::unique_ptr<link::Object> o);

// Adds `o` under a handle issued for it, for a method to answer.
link::Handle add(std::unique_ptr<link::Object> o);

// Opens the object `uri` names on the host, and answers its handle.
Result<link::Handle> open(std::span<const link::Token> uri);

// Lets the object `h` names on the host go.
void drop(link::Handle h);

// Calls `m` of `h`, which answers at once: its result, and its return data,
// readable until the next crossing.
std::pair<std::int32_t, std::optional<std::span<const std::uint8_t>>> call(link::Handle h, link::Method m, const link::Args &args);

// Sends `m` of `h`, its answer pending from before the crossing, as the
// host's is, and answers the request's tag.
link::Tag send(link::Handle h, link::Method m, const link::Args &args, link::Answer answer);

// Cancels the request `t`, while its answer is pending
// (`frame.json5` `CANCEL`).
void cancel(link::Tag t);

// Puts `stop` as how the request `t` the guest answers stops if it is
// canceled.
void on_cancel(link::Tag t, std::function<void()> stop);

// Answers the request `t`, whose method answered EINPROGRESS, with `result`
// and its return data. A later answer is found by its tag alone.
void task_return(link::Tag t, std::int32_t result, const link::Data &data);

// Whether control is in the guest's own code: inside an entry, and not out
// on the host through `link_out`. A program's report of its failures takes
// only these.
bool in_guest() noexcept;

} // namespace xpute::guest

// Defines the guest's three exports (`link/wit/types.wit`), over frames of
// `cap_` bytes, with `boot_` mounting what the guest implements and opening
// what it calls, and `enter_` and `end_` starting and ending each entry; from
// its init on, the guest's failures are its own (`stop.hpp` `hook`). A module
// exports them by name; a native guest as C symbols, which catch a bug (kit
// `caught`) and answer a trap (`TRAP`, or `TRAP_AT` for init's layout), as a
// Rust guest's catch its panic. A failed check of libc++'s own still aborts a
// native guest's process, where a module traps.
#ifdef __wasm__
// The module's static initializers, which wasm-ld gathers into one function.
// The guest's first export runs them, before anything of the program, as the
// native loader runs a library's; a module that calls it is a reactor to
// wasm-ld, whose exports it leaves as they are.
extern "C" void __wasm_call_ctors();
#define XPUTE_LINK(cap_, boot_, enter_, end_)                                                                                                      \
    extern "C" __attribute__((export_name("link_init"))) std::uint64_t link_init() {                                                              \
        __wasm_call_ctors();                                                                                                                        \
        ::xpute::guest::stop::hook();                                                                                                               \
        return ::xpute::guest::init((cap_), (enter_), (end_));                                                                                     \
    }                                                                                                                                               \
    extern "C" __attribute__((export_name("link_boot"))) void link_boot() { ::xpute::guest::boot(boot_); }                                        \
    extern "C" __attribute__((export_name("link_in"))) std::int32_t link_in() { return ::xpute::guest::recv(); }
#else
#define XPUTE_LINK(cap_, boot_, enter_, end_)                                                                                                      \
    extern "C" __attribute__((visibility("default"))) std::uint64_t link_init() {                                                                 \
        return ::xpute::caught(::xpute::link::TRAP_AT, [] {                                                                                       \
            ::xpute::guest::stop::hook();                                                                                                           \
            return ::xpute::guest::init((cap_), (enter_), (end_));                                                                                 \
        });                                                                                                                                         \
    }                                                                                                                                               \
    extern "C" __attribute__((visibility("default"))) std::int32_t link_boot() {                                                                  \
        return ::xpute::caught(::xpute::link::TRAP, [] {                                                                                            \
            ::xpute::guest::boot(boot_);                                                                                                            \
            return 0;                                                                                                                               \
        });                                                                                                                                         \
    }                                                                                                                                               \
    extern "C" __attribute__((visibility("default"))) std::int32_t link_in() { return ::xpute::caught(::xpute::link::TRAP, ::xpute::guest::recv); }
#endif

// cpp/xpute/guest/wait.hpp

// A sent method as an awaitable, on the guest's side (xpute/link wait.hpp),
// for the generated bindings' calls, as xpute-guest's wait.rs is.

namespace xpute::guest {

template <class L> link::Wait<L> wait(link::Handle h, link::Method m, const link::Args &args, L lent) {
    return link::Wait<L>(send, cancel, h, m, args, std::move(lent));
}

} // namespace xpute::guest

// cpp/xpute/guest/abi/clocks/monotonic_clock.hpp
//
// GENERATED from wit/clocks (xpute-link-gen wit.rs)

namespace xpute::abi::clocks::monotonic_clock {

namespace _rt {
using ::xpute::Result;
using ::xpute::guest::call;
using ::xpute::guest::wait;
using ::xpute::link::Handle;
using ::xpute::link::Lift;
using ::xpute::link::Lower;
using ::xpute::link::Method;
using ::xpute::link::Packet;
using ::xpute::link::Task;
using ::xpute::link::answer;
} // namespace _rt

// The interface's handle, named by `xpute:clocks/monotonic-clock` (frame.json5 `NAMED`).
inline constexpr _rt::Handle HANDLE = 0x73f703;

// `now`
inline constexpr _rt::Method NOW = 0;
// `wait-for`
inline constexpr _rt::Method WAIT_FOR = 1;

using Mark = std::uint64_t;

inline _rt::Result<std::uint64_t> now() noexcept {
    auto [result_, data_] = _rt::call(HANDLE, NOW, std::nullopt);
    return _rt::answer(result_, data_, [&](_rt::Lift &r_) { return r_.get<std::uint64_t>(0); });
}

inline _rt::Task<_rt::Result<void>> wait_for(std::uint64_t how_long) noexcept {
    auto sent_ = _rt::wait(HANDLE, WAIT_FOR, _rt::Packet{8, [&](_rt::Lower &w_) { w_.put<std::uint64_t>(0, how_long); }}, std::monostate());
    auto [result_, data_, lent_] = co_await std::move(sent_);
    co_return _rt::answer(result_);
}

} // namespace xpute::abi::clocks::monotonic_clock

// cpp/xpute/guest/abi/clocks/system_clock.hpp
//
// GENERATED from wit/clocks (xpute-link-gen wit.rs)

namespace xpute::abi::clocks::system_clock {

namespace _rt {
using ::xpute::Result;
using ::xpute::guest::call;
using ::xpute::link::Handle;
using ::xpute::link::Lift;
using ::xpute::link::Lower;
using ::xpute::link::Method;
using ::xpute::link::answer;
} // namespace _rt

// The interface's handle, named by `xpute:clocks/system-clock` (frame.json5 `NAMED`).
inline constexpr _rt::Handle HANDLE = 0x7dfa61;

// `now`
inline constexpr _rt::Method NOW = 0;

struct Instant {
    std::int64_t seconds;
    std::uint32_t nanoseconds;

    static constexpr std::size_t SZ = 16;
    static constexpr std::size_t ALIGN = 8;

    void store(_rt::Lower &w, std::size_t at) const noexcept {
        w.put<std::int64_t>(at, this->seconds);
        w.put<std::uint32_t>(at + 8, this->nanoseconds);
    }

    static Instant load(_rt::Lift &r, std::size_t at) noexcept {
        return Instant{
            .seconds = r.get<std::int64_t>(at),
            .nanoseconds = r.get<std::uint32_t>(at + 8),
        };
    }
};

inline _rt::Result<::xpute::abi::clocks::system_clock::Instant> now() noexcept {
    auto [result_, data_] = _rt::call(HANDLE, NOW, std::nullopt);
    return _rt::answer(result_, data_, [&](_rt::Lift &r_) { return ::xpute::abi::clocks::system_clock::Instant::load(r_, 0); });
}

} // namespace xpute::abi::clocks::system_clock

// cpp/xpute/guest/abi/clocks/types.hpp
//
// GENERATED from wit/clocks (xpute-link-gen wit.rs)

namespace xpute::abi::clocks::types {

using Duration = std::uint64_t;

} // namespace xpute::abi::clocks::types

// cpp/xpute/guest/abi/fs/types.hpp
//
// GENERATED from wit/fs (xpute-link-gen wit.rs)

namespace xpute::abi::fs::types {

enum class ErrorCode : std::uint32_t {
    // No file is there.
    no_entry = 0,
    // Its backend failed to read or write it.
    io = 1,
    // Its backend keeps nothing written there.
    read_only = 2,
    // Its backend refused access to it.
    access = 3,
    // Its backend holds it in a form it cannot read as bytes.
    bad_message = 4,
    // The network did not reach its server.
    remote_unreachable = 5,
    // Its server holds no such object.
    no_object = 6,
};

// The member `val` is, or none for a number no member has.
constexpr std::optional<ErrorCode> of_ErrorCode(std::uint32_t val) noexcept {
    switch (val) {
    case 0: return ErrorCode::no_entry;
    case 1: return ErrorCode::io;
    case 2: return ErrorCode::read_only;
    case 3: return ErrorCode::access;
    case 4: return ErrorCode::bad_message;
    case 5: return ErrorCode::remote_unreachable;
    case 6: return ErrorCode::no_object;
    }
    return std::nullopt;
}

} // namespace xpute::abi::fs::types

// cpp/xpute/guest/abi/fs/file.hpp
//
// GENERATED from wit/fs (xpute-link-gen wit.rs)

namespace xpute::abi::fs::file {

namespace _rt {
using ::xpute::Result;
using ::xpute::guest::wait;
using ::xpute::link::Handle;
using ::xpute::link::Lend;
using ::xpute::link::Lent;
using ::xpute::link::Lift;
using ::xpute::link::Lower;
using ::xpute::link::Method;
using ::xpute::link::Packet;
using ::xpute::link::Task;
using ::xpute::link::answer;
using ::xpute::link::lent;
} // namespace _rt

// The interface's handle, named by `xpute:fs/file` (frame.json5 `NAMED`).
inline constexpr _rt::Handle HANDLE = 0x5b6743;

// `[method]file.size`
inline constexpr _rt::Method FILE_SIZE = 0;
// `[method]file.read`
inline constexpr _rt::Method FILE_READ = 1;
// `[method]file.write`
inline constexpr _rt::Method FILE_WRITE = 2;
// `[method]file.set-aside`
inline constexpr _rt::Method FILE_SET_ASIDE = 3;
// `[method]file.close`
inline constexpr _rt::Method FILE_CLOSE = 4;

inline _rt::Task<_rt::Result<std::variant<std::uint32_t, ::xpute::abi::fs::types::ErrorCode>>> file_size(_rt::Handle file_) noexcept {
    auto sent_ = _rt::wait(file_, FILE_SIZE, std::nullopt, std::monostate());
    auto [result_, data_, lent_] = co_await std::move(sent_);
    co_return _rt::answer(result_, data_, [&](_rt::Lift &r_) { return (r_.case_(0, 2) == 0 ? std::variant<std::uint32_t, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<0>, r_.get<std::uint32_t>(4)) : std::variant<std::uint32_t, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<1>, static_cast<::xpute::abi::fs::types::ErrorCode>(r_.case_(4, 7)))); });
}

template <_rt::Lend L> inline _rt::Task<std::pair<L, _rt::Result<std::variant<std::uint32_t, ::xpute::abi::fs::types::ErrorCode>>>> file_read(_rt::Handle file_, L buf) noexcept {
    _rt::Lent buf_lent_ = _rt::lent(buf);
    auto sent_ = _rt::wait(file_, FILE_READ, _rt::Packet{16, [&](_rt::Lower &w_) { buf_lent_.store(w_, 0); }}, std::move(buf));
    auto [result_, data_, lent_] = co_await std::move(sent_);
    co_return {std::move(lent_), _rt::answer(result_, data_, [&](_rt::Lift &r_) { return (r_.case_(0, 2) == 0 ? std::variant<std::uint32_t, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<0>, r_.get<std::uint32_t>(4)) : std::variant<std::uint32_t, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<1>, static_cast<::xpute::abi::fs::types::ErrorCode>(r_.case_(4, 7)))); })};
}

template <_rt::Lend L> inline _rt::Task<std::pair<L, _rt::Result<void>>> file_write(_rt::Handle file_, L buf) noexcept {
    _rt::Lent buf_lent_ = _rt::lent(buf);
    auto sent_ = _rt::wait(file_, FILE_WRITE, _rt::Packet{16, [&](_rt::Lower &w_) { buf_lent_.store(w_, 0); }}, std::move(buf));
    auto [result_, data_, lent_] = co_await std::move(sent_);
    co_return {std::move(lent_), _rt::answer(result_)};
}

inline _rt::Task<_rt::Result<std::variant<std::monostate, ::xpute::abi::fs::types::ErrorCode>>> file_set_aside(_rt::Handle file_) noexcept {
    auto sent_ = _rt::wait(file_, FILE_SET_ASIDE, std::nullopt, std::monostate());
    auto [result_, data_, lent_] = co_await std::move(sent_);
    co_return _rt::answer(result_, data_, [&](_rt::Lift &r_) { return (r_.case_(0, 2) == 0 ? std::variant<std::monostate, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<0>, std::monostate()) : std::variant<std::monostate, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<1>, static_cast<::xpute::abi::fs::types::ErrorCode>(r_.case_(1, 7)))); });
}

inline _rt::Task<_rt::Result<std::variant<std::monostate, ::xpute::abi::fs::types::ErrorCode>>> file_close(_rt::Handle file_) noexcept {
    auto sent_ = _rt::wait(file_, FILE_CLOSE, std::nullopt, std::monostate());
    auto [result_, data_, lent_] = co_await std::move(sent_);
    co_return _rt::answer(result_, data_, [&](_rt::Lift &r_) { return (r_.case_(0, 2) == 0 ? std::variant<std::monostate, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<0>, std::monostate()) : std::variant<std::monostate, ::xpute::abi::fs::types::ErrorCode>(std::in_place_index<1>, static_cast<::xpute::abi::fs::types::ErrorCode>(r_.case_(1, 7)))); });
}

} // namespace xpute::abi::fs::file

// cpp/xpute/guest/abi/life.hpp
//
// GENERATED from wit/life (xpute-link-gen wit.rs)

namespace xpute::abi::life {

namespace _rt {
using ::xpute::Errno;
using ::xpute::Result;
using ::xpute::guest::mount_named;
using ::xpute::link::Handle;
using ::xpute::link::Lift;
using ::xpute::link::Lower;
using ::xpute::link::Method;
using ::xpute::link::Object;
using ::xpute::link::Tag;
} // namespace _rt

// The interface's handle, named by `xpute:life/life` (frame.json5 `NAMED`).
inline constexpr _rt::Handle HANDLE = 0x46edb9;

// `hidden`
inline constexpr _rt::Method HIDDEN = 0;
// `shown`
inline constexpr _rt::Method SHOWN = 1;
// `attached`
inline constexpr _rt::Method ATTACHED = 2;

class Guest {
  public:
    virtual ~Guest() = default;
    virtual _rt::Result<void> hidden() = 0;
    virtual _rt::Result<void> shown() = 0;
    virtual _rt::Result<void> attached() = 0;
};

class Object_ final : public _rt::Object {
  public:
    explicit Object_(std::unique_ptr<Guest> implementation) noexcept : implementation_(std::move(implementation)) {}

    std::int32_t call(_rt::Method m_, [[maybe_unused]] _rt::Tag tag_, [[maybe_unused]] _rt::Lift args_, [[maybe_unused]] _rt::Lower &ret_) override {
        switch (m_) {
        case HIDDEN: {
            _rt::Result<void> x_ = implementation_->hidden();
            if (!x_) return static_cast<std::int32_t>(x_.error().errnum);
            return 0;
        }
        case SHOWN: {
            _rt::Result<void> x_ = implementation_->shown();
            if (!x_) return static_cast<std::int32_t>(x_.error().errnum);
            return 0;
        }
        case ATTACHED: {
            _rt::Result<void> x_ = implementation_->attached();
            if (!x_) return static_cast<std::int32_t>(x_.error().errnum);
            return 0;
        }
        default:
            return static_cast<std::int32_t>(_rt::Errno::enosys);
        }
    }

  private:
    std::unique_ptr<Guest> implementation_;
};

// Mounts `implementation` under the interface's handle.
inline void mount(std::unique_ptr<Guest> implementation) {
    _rt::mount_named(HANDLE, std::make_unique<Object_>(std::move(implementation)));
}

} // namespace xpute::abi::life

// cpp/xpute/guest/abi/log.hpp
//
// GENERATED from wit/log (xpute-link-gen wit.rs)

namespace xpute::abi::log {

namespace _rt {
using ::xpute::Result;
using ::xpute::guest::call;
using ::xpute::link::Handle;
using ::xpute::link::Lower;
using ::xpute::link::Method;
using ::xpute::link::Packet;
using ::xpute::link::answer;
} // namespace _rt

// The interface's handle, named by `xpute:log/log` (frame.json5 `NAMED`).
inline constexpr _rt::Handle HANDLE = 0x73da26;

// `log`
inline constexpr _rt::Method LOG = 0;

enum class Level : std::uint32_t {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    critical = 5,
};

// The member `val` is, or none for a number no member has.
constexpr std::optional<Level> of_Level(std::uint32_t val) noexcept {
    switch (val) {
    case 0: return Level::trace;
    case 1: return Level::debug;
    case 2: return Level::info;
    case 3: return Level::warn;
    case 4: return Level::error;
    case 5: return Level::critical;
    }
    return std::nullopt;
}

inline _rt::Result<void> log(::xpute::abi::log::Level level, std::string_view context, std::string_view msg) noexcept {
    auto [result_, data_] = _rt::call(HANDLE, LOG, _rt::Packet{20, [&](_rt::Lower &w_) { w_.put<std::uint8_t>(0, static_cast<std::uint8_t>(level)); w_.str(4, context); w_.str(12, msg); }});
    return _rt::answer(result_);
}

} // namespace xpute::abi::log

// cpp/xpute/guest/abi/stop.hpp
//
// GENERATED from wit/stop (xpute-link-gen wit.rs)

namespace xpute::abi::stop {

namespace _rt {
using ::xpute::Result;
using ::xpute::guest::call;
using ::xpute::link::Handle;
using ::xpute::link::Lower;
using ::xpute::link::Method;
using ::xpute::link::Packet;
using ::xpute::link::answer;
} // namespace _rt

// The interface's handle, named by `xpute:stop/stop` (frame.json5 `NAMED`).
inline constexpr _rt::Handle HANDLE = 0x4eccaa;

// `stopped`
inline constexpr _rt::Method STOPPED = 0;

inline _rt::Result<void> stopped(std::int32_t errnum, std::string_view file, std::uint32_t line, std::uint64_t n1, std::uint64_t n2, std::optional<std::string_view> msg) noexcept {
    auto [result_, data_] = _rt::call(HANDLE, STOPPED, _rt::Packet{48, [&](_rt::Lower &w_) { w_.put<std::int32_t>(0, errnum); w_.str(4, file); w_.put<std::uint32_t>(12, line); w_.put<std::uint64_t>(16, n1); w_.put<std::uint64_t>(24, n2); if (msg) { w_.put<std::uint8_t>(32, 1); w_.str(36, (*msg)); } }});
    return _rt::answer(result_);
}

} // namespace xpute::abi::stop

// cpp/xpute/guest/clock.hpp

// The host's clocks (`wit/clocks/monotonic-clock.wit`,
// `wit/clocks/system-clock.wit`), called in an entry, as `xpute-guest`'s
// `clock.rs` reads them. They count nanoseconds, as wasi:clocks does; the
// program reads them in ms, converted here alone.

namespace xpute::guest::clock {

// The time in ms, which never runs backward; only differences mean anything.
double now() noexcept;

// A mark of the monotonic clock in ms, on `now`'s line, or a duration
// between two in ms: a time the host answers, as a display's frame start,
// or a span of one, as the gap since the frame before.
double ms(std::uint64_t mark) noexcept;

// A time in ms on `now`'s line as a mark of the monotonic clock, rounded
// down, and none before the clock's start (0): a time the program asks the
// host for, as a display's frame start.
std::uint64_t mark(double ms) noexcept;

// `ms` as a duration the monotonic clock waits for, rounded up.
std::uint64_t duration(double ms) noexcept;

// The system clock, in Unix ms.
double system() noexcept;

} // namespace xpute::guest::clock

// cpp/xpute/guest/log.hpp

// The guest's log records, told to its host by the log it implements
// (`wit/log/log.wit`), as `xpute-guest`'s `log.rs` is a Rust guest's: a
// level, a context and a message, each record worth telling and neither an
// error nor a bug.

namespace xpute::guest {

// Tells the host `msg`, at `level`, about `context`. A host that hears
// nothing more, the link trapped among them, has nothing to answer, so the
// answer is not read.
void log(abi::log::Level level, std::string_view context, std::string_view msg) noexcept;

} // namespace xpute::guest
