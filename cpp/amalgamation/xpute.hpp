// xpute.hpp
//
// GENERATED from cpp/xpute/ by `make -C cpp amalgamation` (cpp/amalgamation.awk) — do not edit.
//
// xpute's C++ library in one header: every header of cpp/xpute/, each after
// the ones it includes. Compile xpute.cpp, beside it, once into the program.

#pragma once

// cpp/xpute/kit/abi/cmd.hpp

// `major << 8 | minor` in the 16 bits a frame carries.

#include <cstdint>
#include <optional>

namespace xpute {

constexpr std::uint32_t cmd(std::uint32_t major, std::uint32_t minor) noexcept {
    return (major & 0xff) << 8 | (minor & 0xff);
}

constexpr std::uint32_t cmd_major(std::uint32_t c) noexcept {
    return c >> 8 & 0xff;
}

constexpr std::uint32_t cmd_minor(std::uint32_t c) noexcept {
    return c & 0xff;
}

// Declared beside the enum by its table, as Rust's `ALL`.
template <class E> struct Members;

// None for a number nobody gave out.
template <class E> constexpr std::optional<E> member_of(std::uint32_t number) noexcept {
    for (E m : Members<E>::ALL) {
        if (static_cast<std::uint32_t>(m) == number) return m;
    }
    return std::nullopt;
}

} // namespace xpute

// cpp/xpute/kit/abi/word.hpp

// Bit fields in either word width. A width of the whole word is handled, which
// the shift alone cannot say; otherwise `nbit` and `shamt` must stay inside it.

#include <concepts>
#include <limits>
#include <type_traits>

namespace xpute {

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

// evanw/buddy-malloc, line for line, with two changes: memory comes from a
// given `Range` instead of `brk`, and `free` is told the size, so there is no
// 8-byte header.

#include <array>
#include <cstddef>

namespace xpute {

// The bits a range's size and the exponents the buddy keeps fit in: a guest's
// memory is 32-bit whatever the machine, though on a native build a range's
// base is a full pointer (xpute-kit alloc.rs `ADDR_BITS`).
inline constexpr std::size_t ADDR_BITS = 32;

// `MAX_LOG2` is the largest block the tree can describe, a bound on the
// address space and not on any memory.
template <class R>
concept Range = requires {
    { R::MIN_LOG2 } -> std::convertible_to<std::size_t>;
    { R::MAX_LOG2 } -> std::convertible_to<std::size_t>;
    { R::base() } -> std::convertible_to<std::uintptr_t>;
    { R::end() } -> std::convertible_to<std::uintptr_t>;
};

template <Range R> class BuddyMalloc {
    static constexpr std::size_t BUCKET_CNT = R::MAX_LOG2 - R::MIN_LOG2 + 1;
    // A tree of fewer than four buckets has no split bit to keep, and one of
    // more than an exponent each, 0 through ADDR_BITS, a bitmap past what a
    // range can address.
    static_assert(BUCKET_CNT >= 4 && BUCKET_CNT <= ADDR_BITS + 1, "a buddy tree's bucket count");

    struct List {
        List *prev;
        List *next;
    };
    static_assert((std::size_t{1} << R::MIN_LOG2) >= sizeof(List), "a free block holds its list links");

  public:
    BuddyMalloc() noexcept = default;
    // Its bucket heads are their own lists' sentinels, so it must not move
    // once a block has been handed out.
    BuddyMalloc(const BuddyMalloc &) = delete;
    BuddyMalloc &operator=(const BuddyMalloc &) = delete;

    // The high-water mark, not what is live.
    std::size_t used() const noexcept { return max_ptr_ - base_ptr_; }
    std::size_t live() const noexcept { return live_; }
    std::size_t range_bytes() const noexcept { return end_ptr_ - base_ptr_; }
    // A refused request can only be served once this has moved.
    std::uint32_t releases() const noexcept { return releases_; }
    // Base, max, end, live, then releases and allocs as half-words, read in
    // place by a host.
    const std::uintptr_t *stat() const noexcept { return &base_ptr_; }
    std::size_t allocs() const noexcept { return allocs_; }

    std::size_t largest_free() const noexcept {
        if (base_ptr_ == 0) return 0;
        for (std::size_t b = bucket_limit_; b < BUCKET_CNT; b++) {
            if (buckets_[b].next != &buckets_[b]) return std::size_t{1} << (R::MAX_LOG2 - b);
        }
        return 0;
    }

    // The block is a power of two, aligned to its own size from the base.
    void *malloc(std::size_t request) noexcept {
        // Initialize on the first call: the tree starts as a single node of
        // the smallest size, and more of the range is taken as needed.
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

        // A bucket with a non-empty free list at least as large as needed,
        // splitting a larger one where there is no exact match.
        for (;;) {
            if (!lower_bucket_limit(bucket)) return nullptr;

            List *ptr = list_pop(bucket);
            if (ptr == nullptr) {
                // Away from the root, or unable to grow the tree further:
                // carry on to the next bucket.
                if (bucket != bucket_limit_ || bucket == 0) {
                    if (bucket == 0) return nullptr;
                    bucket--;
                    continue;
                }
                // Otherwise grow the tree one more level and pop again: the
                // root is known to be used, so this adds a parent above it in
                // the SPLIT state with its right child on this bucket's list.
                if (!lower_bucket_limit(bucket - 1)) return nullptr;
                ptr = list_pop(bucket);
            }

            // Take the address range before going further; where there is no
            // room, put the block back and fail.
            std::size_t sz = std::size_t{1} << (R::MAX_LOG2 - bucket);
            std::size_t bytes_needed = bucket < original_bucket ? sz / 2 + sizeof(List) : sz;
            if (!update_max_ptr(reinterpret_cast<std::uintptr_t>(ptr) + bytes_needed)) {
                list_push(bucket, ptr);
                return nullptr;
            }

            // A node off the free list goes from UNUSED to USED, which flips
            // the parent's "is split" bit. The grandparent never needs the
            // same: the buddy is USED, or the parent would not be split.
            std::size_t i = node_for_ptr(ptr, bucket);
            if (i != 0) flip_parent_is_split(i);

            // A node larger than needed is split down to size, each step
            // moving to the left child, splitting the parent, and putting the
            // right child on its bucket's list.
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

        // Up to the root, flipping USED blocks to UNUSED and merging UNUSED
        // buddies into a single UNUSED parent.
        while (i != 0) {
            flip_parent_is_split(i);
            // A parent that now reads SPLIT means the buddy is USED, so there
            // is nothing to merge with; a root has no buddy.
            if (parent_is_split(i) || bucket == bucket_limit_) break;
            list_remove(ptr_for_node(((i - 1) ^ 1) + 1, bucket));
            i = (i - 1) / 2;
            bucket--;
        }

        // At the back of this bucket's list: `malloc` takes from the back, so
        // a free followed by a malloc of the same size hands back the same
        // address.
        list_push(bucket, ptr_for_node(i, bucket));
    }

  private:
    // The C asks `brk`; here the range says.
    bool update_max_ptr(std::uintptr_t new_val) noexcept {
        if (new_val > max_ptr_) {
            if (new_val > end_ptr_) return false;
            max_ptr_ = new_val;
        }
        return true;
    }

    // The address a node stands for. A bucket's first node is
    // `(1 << bucket) - 1`, so the `+ 1` comes before the subtraction.
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

    // The tree is rooted at the bucket limit; this grows it by doubling until
    // the root lies at `bucket`, each doubling lowering the limit by one.
    bool lower_bucket_limit(std::size_t bucket) noexcept {
        while (bucket < bucket_limit_) {
            std::size_t root = node_for_ptr(reinterpret_cast<List *>(base_ptr_), bucket_limit_);
            // A parent that is not SPLIT means the whole space is free: clear
            // the root's list, raise the tree, and give the widened space to
            // the new root.
            if (!parent_is_split(root)) {
                list_remove(reinterpret_cast<List *>(base_ptr_));
                bucket_limit_--;
                list_init(bucket_limit_);
                list_push(bucket_limit_, reinterpret_cast<List *>(base_ptr_));
                continue;
            }
            // Otherwise the tree is in use: make a parent for the root in the
            // SPLIT state with its right child on the free list, taking the
            // memory for that entry before writing it.
            List *right_child = ptr_for_node(root + 1, bucket_limit_);
            if (!update_max_ptr(reinterpret_cast<std::uintptr_t>(right_child) + sizeof(List))) return false;
            list_push(bucket_limit_, right_child);
            bucket_limit_--;
            list_init(bucket_limit_);
            // The grandparent's SPLIT flag, so lowering the limit again knows
            // the root just added is in use.
            root = (root - 1) / 2;
            if (root != 0) flip_parent_is_split(root);
        }
        return true;
    }

    std::array<List, BUCKET_CNT> buckets_{};
    std::size_t bucket_limit_ = 0;
    // The linearized tree of bits: parent `(i - 1) / 2`, children `i * 2 + 1`
    // and `i * 2 + 2`, sibling `((i - 1) ^ 1) + 1`. Only SPLIT is stored.
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

// Size classes over buddy_tree.hpp, as SLUB: a class with no free slot borrows
// a run from the buddy and returns it when its last slot is freed. A slot finds
// its run by masking its offset, which holds because the buddy aligns a block
// to its own size.

#include <algorithm>
#include <bit>

namespace xpute {

template <Range R> class SlabMalloc {
    // Below 16 bytes a class costs more in run headers than it saves.
    static constexpr std::size_t MIN_CLASS_LOG2 = 4;
    // So a run's header is at most a sixteenth of it.
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

    std::size_t used() const noexcept { return buddy_.used(); }
    std::size_t live() const noexcept { return buddy_.live(); }
    std::size_t range_bytes() const noexcept { return buddy_.range_bytes(); }
    std::uint32_t releases() const noexcept { return buddy_.releases(); }
    std::size_t largest_free() const noexcept { return buddy_.largest_free(); }
    std::size_t allocs() const noexcept { return allocs_; }
    std::uint32_t refusals() const noexcept { return refusals_; }
    std::size_t run_live() const noexcept { return run_live_; }
    std::size_t slot_live() const noexcept { return slot_live_; }

    void *alloc(std::size_t bytes, std::size_t align) noexcept {
        std::size_t c;
        if (!class_for(bytes, align, c)) {
            // A page or more: the buddy's placement satisfies any alignment up
            // to a page and none past one.
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
        std::size_t run_bytes = std::size_t{1} << run_log2(c);
        std::uintptr_t base = R::base();
        Run *run = reinterpret_cast<Run *>(base + ((reinterpret_cast<std::uintptr_t>(at) - base) & ~(run_bytes - 1)));
        std::uint8_t *slot = static_cast<std::uint8_t *>(at);
        bool was_full = run->free == nullptr;
        set_next(slot, run->free);
        run->free = slot;
        run->inuse--;
        slot_live_ -= std::size_t{1} << class_log2(c);
        if (run->inuse == 0) {
            // The last slot: the run goes back, and every class gets the pages
            // rather than this one keeping them.
            if (!was_full) drop_partial(c, run);
            run_live_ -= run_bytes;
            buddy_.free(run, run_bytes);
        } else if (was_full) {
            push_partial(c, run);
        }
    }

    void *malloc(std::size_t bytes) noexcept { return alloc(bytes, 8); }
    void free(void *at, std::size_t bytes) noexcept { dealloc(at, bytes, 8); }

  private:
    // The class a request falls in, or false where it is the buddy's. The
    // alignment is folded into the size: a slot sits at a multiple of its
    // class from a run base that is a multiple of its own size.
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
        std::size_t run_bytes = std::size_t{1} << run_log2(c);
        auto *at = static_cast<std::uint8_t *>(buddy_.malloc(run_bytes));
        if (at == nullptr) return nullptr;
        std::size_t cls = std::size_t{1} << class_log2(c);
        std::uint8_t *next = nullptr;
        // From the back, so a run's first slots out are its lowest addresses.
        for (std::size_t i = slots(c); i > head_slots(c);) {
            i--;
            set_next(at + i * cls, next);
            next = at + i * cls;
        }
        run_live_ += run_bytes;
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
// GENERATED from spec/status/errno.json
//
// POSIX/Linux-aligned negative errno

#include <string_view>
#include <utility>

namespace xpute {

enum class Errno : std::int32_t {
    ok = 0, // Success

    // ---- Generic ----
    eperm = -1, // Operation not permitted
    enoent = -2, // No such file or directory / entry not found
    esrch = -3, // No such process
    eintr = -4, // Interrupted system call
    eio = -5, // I/O error
    enxio = -6, // No such device or address
    e2big = -7, // Argument list too long / object too large for fixed local cap
    enoexec = -8, // Exec format error
    ebadf = -9, // Bad file descriptor / bad handle
    echild = -10, // No child processes
    eagain = -11, // Try again / Resource temporarily unavailable
    enomem = -12, // Out of memory
    eacces = -13, // Permission denied
    efault = -14, // Bad address
    ebusy = -16, // Device or resource busy
    eexist = -17, // File exists / duplicate bind
    exdev = -18, // Cross-device link
    enodev = -19, // No such device
    enotdir = -20, // Not a directory
    eisdir = -21, // Is a directory
    einval = -22, // Invalid argument
    enfile = -23, // File table overflow (system-wide)
    emfile = -24, // Too many open files (process)
    enotty = -25, // Not a tty
    efbig = -27, // File too large / payload too large
    enospc = -28, // No space left on device / local mailbox full
    espipe = -29, // Illegal seek
    erofs = -30, // Read-only file system
    emlink = -31, // Too many links
    epipe = -32, // Broken pipe
    edom = -33, // Math argument out of domain
    erange = -34, // Math result not representable
    enosys = -38, // Function not implemented

    // ---- Protocol / Message / Encoding ----
    eproto = -71, // Protocol error
    ebadmsg = -74, // Bad message (protocol/frame/payload invalid)
    eoverflow = -75, // Value too large for target type / overflow
    emsgsize = -90, // Message too long / packet too large
    enotsup = -95, // Operation not supported
    eilseq = -84, // Illegal byte sequence (decode/encoding failure)

    // ---- Endpoint / Addressing ----
    eaddrinuse = -98, // Address already in use
    eaddrnotavail = -99, // Cannot assign requested address

    // ---- Network / Transport (Linux asm-generic/errno.h aligned) ----
    enetdown = -100, // Network is down
    enetunreach = -101, // Network is unreachable
    enetreset = -102, // Network dropped connection because of reset
    econnaborted = -103, // Software caused connection abort
    econnreset = -104, // Connection reset by peer
    enobufs = -105, // No buffer space available
    eisconn = -106, // Transport endpoint is already connected
    enotconn = -107, // Transport endpoint is not connected
    eshutdown = -108, // Cannot send after transport endpoint shutdown
    etoomanyrefs = -109, // Too many references
    etimedout = -110, // Connection timed out
    econnrefused = -111, // Connection refused
    ehostdown = -112, // Host is down
    ehostunreach = -113, // No route to host
    ealready = -114, // Operation already in progress
    einprogress = -115, // Operation now in progress

    // ---- Lifecycle / Cancellation ----
    ecanceled = -125, // Operation canceled
    enotrecoverable = -131, // State not recoverable: an invariant the code stands on does not hold
};

// An errno's name, as Rust's Display prints an error's.
constexpr std::string_view errno_name(Errno e) noexcept {
    switch (e) {
    case Errno::ok: return "OK";
    case Errno::eperm: return "EPERM";
    case Errno::enoent: return "ENOENT";
    case Errno::esrch: return "ESRCH";
    case Errno::eintr: return "EINTR";
    case Errno::eio: return "EIO";
    case Errno::enxio: return "ENXIO";
    case Errno::e2big: return "E2BIG";
    case Errno::enoexec: return "ENOEXEC";
    case Errno::ebadf: return "EBADF";
    case Errno::echild: return "ECHILD";
    case Errno::eagain: return "EAGAIN";
    case Errno::enomem: return "ENOMEM";
    case Errno::eacces: return "EACCES";
    case Errno::efault: return "EFAULT";
    case Errno::ebusy: return "EBUSY";
    case Errno::eexist: return "EEXIST";
    case Errno::exdev: return "EXDEV";
    case Errno::enodev: return "ENODEV";
    case Errno::enotdir: return "ENOTDIR";
    case Errno::eisdir: return "EISDIR";
    case Errno::einval: return "EINVAL";
    case Errno::enfile: return "ENFILE";
    case Errno::emfile: return "EMFILE";
    case Errno::enotty: return "ENOTTY";
    case Errno::efbig: return "EFBIG";
    case Errno::enospc: return "ENOSPC";
    case Errno::espipe: return "ESPIPE";
    case Errno::erofs: return "EROFS";
    case Errno::emlink: return "EMLINK";
    case Errno::epipe: return "EPIPE";
    case Errno::edom: return "EDOM";
    case Errno::erange: return "ERANGE";
    case Errno::enosys: return "ENOSYS";
    case Errno::eproto: return "EPROTO";
    case Errno::ebadmsg: return "EBADMSG";
    case Errno::eoverflow: return "EOVERFLOW";
    case Errno::emsgsize: return "EMSGSIZE";
    case Errno::enotsup: return "ENOTSUP";
    case Errno::eilseq: return "EILSEQ";
    case Errno::eaddrinuse: return "EADDRINUSE";
    case Errno::eaddrnotavail: return "EADDRNOTAVAIL";
    case Errno::enetdown: return "ENETDOWN";
    case Errno::enetunreach: return "ENETUNREACH";
    case Errno::enetreset: return "ENETRESET";
    case Errno::econnaborted: return "ECONNABORTED";
    case Errno::econnreset: return "ECONNRESET";
    case Errno::enobufs: return "ENOBUFS";
    case Errno::eisconn: return "EISCONN";
    case Errno::enotconn: return "ENOTCONN";
    case Errno::eshutdown: return "ESHUTDOWN";
    case Errno::etoomanyrefs: return "ETOOMANYREFS";
    case Errno::etimedout: return "ETIMEDOUT";
    case Errno::econnrefused: return "ECONNREFUSED";
    case Errno::ehostdown: return "EHOSTDOWN";
    case Errno::ehostunreach: return "EHOSTUNREACH";
    case Errno::ealready: return "EALREADY";
    case Errno::einprogress: return "EINPROGRESS";
    case Errno::ecanceled: return "ECANCELED";
    case Errno::enotrecoverable: return "ENOTRECOVERABLE";
    }
    return "errno";
}

// The member `code` is, none for a number no member has.
constexpr std::optional<Errno> errno_of(std::int32_t code) noexcept {
    switch (code) {
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

// A result as a pair, as Rust's ErrnoResult is: the errno, and the value
// where there is one.
template <class T> using ErrnoResult = std::pair<Errno, std::optional<T>>;

} // namespace xpute

// cpp/xpute/kit/status/error.hpp

// An error is its errno, its class and where it was made. A broken invariant
// is fatal (bug.hpp); a fault from outside is returned as a Result.

#include <source_location>

namespace xpute {

enum class ErrorClass : std::uint8_t {
    invariant,
    fault,
    marshal,
    network,
};

struct Error {
    Errno errnum;
    ErrorClass cls;
    std::source_location at;

    constexpr bool is_fault() const noexcept { return cls != ErrorClass::invariant; }
};

constexpr Error make_error(ErrorClass cls, Errno errnum, std::source_location at = std::source_location::current()) noexcept {
    return Error{errnum, cls, at};
}

constexpr Error marshal_error(Errno errnum, std::source_location at = std::source_location::current()) noexcept {
    return Error{errnum, ErrorClass::marshal, at};
}

template <class T> class [[nodiscard]] Result {
  public:
    Result(T val) noexcept : val_(std::move(val)) {}
    Result(Error err) noexcept : err_(err) {}

    bool ok() const noexcept { return val_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    // Asking where there is none is a bug.
    T &value() noexcept { return *val_; }
    const T &value() const noexcept { return *val_; }
    T *operator->() noexcept { return &*val_; }
    T &operator*() noexcept { return *val_; }

    const Error &error() const noexcept { return err_; }

  private:
    std::optional<T> val_;
    Error err_{Errno::ok, ErrorClass::invariant, {}};
};

template <> class [[nodiscard]] Result<void> {
  public:
    Result() noexcept = default;
    Result(Error err) noexcept : err_(err), failed_(true) {}

    bool ok() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return ok(); }
    const Error &error() const noexcept { return err_; }

  private:
    Error err_{Errno::ok, ErrorClass::invariant, {}};
    bool failed_ = false;
};

} // namespace xpute

// cpp/xpute/kit/codec/encoding.hpp

// Hex and unpadded base64url, the forms the other languages write. A span too
// short is a bug; text that does not decode is returned as EBADMSG.

#include <span>

namespace xpute {

constexpr std::size_t hex_len(std::size_t bytes) noexcept {
    return bytes * 2;
}

// `out` is at least hex_len(in.size()).
std::size_t hex_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept;

// Either case is accepted.
Result<std::size_t> hex_decode(std::string_view in, std::span<std::uint8_t> out) noexcept;

constexpr std::size_t base64url_len(std::size_t bytes) noexcept {
    return bytes / 3 * 4 + (bytes % 3 == 0 ? 0 : bytes % 3 + 1);
}

// `out` is at least base64url_len(in.size()).
std::size_t base64url_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept;

// EBADMSG also for padding or bits set past the last byte.
Result<std::size_t> base64url_decode(std::string_view in, std::span<std::uint8_t> out) noexcept;

// The character at `i`: its bytes when it is one, else 0 and the length of its
// maximal bad prefix, at least 1.
struct Utf8Step {
    std::size_t len, bad;
};
constexpr Utf8Step utf8_step(std::string_view s, std::size_t i) noexcept {
    auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char b = byte(i);
    std::size_t len = b < 0x80 ? 1 : (b >= 0xc2 && b < 0xe0) ? 2 : (b >= 0xe0 && b < 0xf0) ? 3 : (b >= 0xf0 && b < 0xf5) ? 4 : 0;
    // The second byte's range depends on the first, which keeps overlong forms
    // and surrogates out.
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

// A maximal bad prefix is one U+FFFD, as `from_utf8_lossy` does.
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

// FNV-1a, 64 bits, the same word every language and the spec generator compute.

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

// JSON checked once and read where it lies, with nothing allocated.

namespace xpute {

namespace json_detail {

std::size_t skip_ws(std::string_view s, std::size_t at) noexcept;
void push_utf8(std::uint32_t c, char *out, std::size_t &n) noexcept;

} // namespace json_detail

enum class JsonType : std::uint8_t { null, boolean, number, string, array, object };

// A lone surrogate reads as U+FFFD.
class JsonChars {
  public:
    explicit JsonChars(std::string_view rest) noexcept : rest_(rest) {}
    std::optional<char32_t> next() noexcept;

  private:
    std::string_view rest_;
};

// Escapes as written, decoded as they are read.
class JsonStr {
  public:
    explicit JsonStr(std::string_view raw) noexcept : raw_(raw) {}
    // None when it holds an escape.
    std::optional<std::string_view> as_plain() const noexcept;
    JsonChars chars() const noexcept { return JsonChars(raw_); }
    bool eq_str(std::string_view s) const noexcept;
    template <class S> void decode_into(S &out) const {
        JsonChars c = chars();
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

class JsonItems;
class JsonFields;

class JsonValue {
  public:
    JsonType type_of() const noexcept;
    bool is_null() const noexcept { return type_of() == JsonType::null; }
    std::optional<bool> as_bool() const noexcept;
    std::optional<double> as_f64() const noexcept;
    std::optional<JsonStr> as_str() const noexcept;
    JsonItems items() const noexcept;
    // A key given twice is given twice.
    JsonFields fields() const noexcept;
    // The last field of that name, as the host's parse keeps.
    std::optional<JsonValue> get(std::string_view key) const noexcept;
    std::optional<JsonValue> at(std::size_t idx) const noexcept;
    std::string_view text() const noexcept { return s_; }

  private:
    friend std::optional<JsonValue> read_json(std::string_view) noexcept;
    friend class JsonItems;
    friend class JsonFields;
    explicit JsonValue(std::string_view s) noexcept : s_(s) {}
    std::string_view s_;
};

class JsonItems {
  public:
    std::optional<JsonValue> next() noexcept;

  private:
    friend class JsonValue;
    JsonItems(std::string_view s, std::size_t at) noexcept : s_(s), at_(at) {}
    std::string_view s_;
    std::size_t at_;
};

class JsonFields {
  public:
    std::optional<std::pair<JsonStr, JsonValue>> next() noexcept;

  private:
    friend class JsonValue;
    JsonFields(std::string_view s, std::size_t at) noexcept : s_(s), at_(at) {}
    std::string_view s_;
    std::size_t at_;
};

// None where the text holds anything but one value.
std::optional<JsonValue> read_json(std::string_view text) noexcept;

// Control characters other than \n, \r and \t as `\u00XX`, as
// `JSON.stringify` writes them.
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

// cpp/xpute/kit/status/bug.hpp

// A broken invariant, said as an errno, a place and at most two numbers. It is
// fatal: the installed report runs and the program stops.

namespace xpute {

using Report = void (*)(Errno errnum, const std::source_location &at, std::uint64_t a, std::uint64_t b);

// The first installed stands.
void set_report(Report report) noexcept;

[[noreturn]] void bug(Errno errnum, std::uint64_t a = 0, std::uint64_t b = 0, std::source_location at = std::source_location::current()) noexcept;

inline void ensure(bool cond, Errno errnum, std::uint64_t a = 0, std::uint64_t b = 0, std::source_location at = std::source_location::current()) noexcept {
    if (!cond) bug(errnum, a, b, at);
}

template <class T> T or_bug(std::optional<T> v, Errno errnum, std::source_location at = std::source_location::current()) noexcept {
    if (!v) bug(errnum, 0, 0, at);
    return *v;
}

template <class T> T or_bug(Result<T> v, Errno errnum, std::source_location at = std::source_location::current()) noexcept {
    ensure(v.ok(), errnum, 0, 0, at);
    return std::move(*v);
}

} // namespace xpute

// cpp/xpute/kit/collection/arena.hpp

// A pool of constructed slots, bump-allocated and rewound whole. `truncate`
// destroys nothing, so a slot arrives in whatever state its last user left it.
// `Alloc` has no default, since a default would be the system's heap.

#include <vector>

namespace xpute {

template <class T, class Alloc> class ArenaGuard;

struct ArenaOptions {
    std::uint32_t init_cap;
    std::uint32_t max_cap;
};

template <class T, class Alloc> class Arena {
  public:
    static Result<Arena> make(ArenaOptions opts, const Alloc &alloc = Alloc()) {
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

    // Past the cursor is a broken invariant, not a resize request.
    void truncate(std::uint32_t new_len) noexcept {
        ensure(new_len <= len_, Errno::einval, new_len, len_);
        len_ = new_len;
    }

    // Marks here and rewinds to the mark when the guard goes.
    ArenaGuard<T, Alloc> scope() noexcept;

  private:
    Arena(std::uint32_t max_cap, const Alloc &alloc) : mem_(alloc), max_cap_(max_cap) {}

    // Saturates, so one past u32 is refused at max_cap rather than wrapping under it.
    // Doubling stops at the cap, so the cap refuses only what does not fit.
    Result<void> grow(std::uint32_t req) {
        if (req <= cap_) return {};
        if (req > max_cap_) return marshal_error(Errno::eoverflow);
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

// A ring over a buffer the caller owns; it allocates nothing and never grows.

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

    // A full deque is the caller's bug.
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

// A binary heap over an array the caller owns. Keys must be totally ordered:
// a NaN key makes the sift walk off the order.

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

    // Both of the root's subtrees must already hold the heap property.
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

    // The new element's place is opened by moving parents down, so one write
    // lands it rather than a swap per level.
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

    // The heap must not be empty.
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

// Unstable, in place, O(n log n).
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

// A list of at most `N`, held inline: it never grows, and a push past `N` is a
// bug (ENOSPC). For what a program bounds by a constant, where a vector would
// grow to that bound anyway. Its first `len` slots are live.

#include <memory>

namespace xpute {

template <class T, std::size_t N> class List {
  public:
    constexpr List() noexcept {}
    List(const List &) = delete;
    List &operator=(const List &) = delete;

    // The values move over, and `o` is left empty.
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

    // A slice's reads, as a Rust list derefs to its slice.
    T &operator[](std::size_t at) noexcept { return as_mut_slice()[at]; }
    const T &operator[](std::size_t at) const noexcept { return as_slice()[at]; }
    T *begin() noexcept { return slot(0); }
    T *end() noexcept { return slot(len_); }
    const T *begin() const noexcept { return slot(0); }
    const T *end() const noexcept { return slot(len_); }

    // The value at `at`, those after it moved down one: the order kept.
    T remove(std::size_t at) noexcept {
        ensure(at < len_, Errno::einval, at);
        T val = std::move(*slot(at));
        std::destroy_at(slot(at));
        for (std::size_t i = at; i + 1 < len_; i++) move_down(i + 1, i);
        len_--;
        return val;
    }

    // Those `keep` refuses dropped, the order of the rest kept, in one pass.
    template <class Keep> void retain(Keep &&keep) noexcept {
        std::size_t kept = 0;
        for (std::size_t at = 0; at < len_; at++) {
            // A slot below `at` and past `kept` is already empty: refused, or
            // moved down from.
            if (keep(std::as_const(*slot(at)))) {
                if (kept != at) move_down(at, kept);
                kept++;
            } else {
                std::destroy_at(slot(at));
            }
        }
        len_ = kept;
    }

    void clear() noexcept {
        std::destroy(slot(0), slot(len_));
        len_ = 0;
    }

  private:
    T *slot(std::size_t at) noexcept { return reinterpret_cast<T *>(bytes_) + at; }
    const T *slot(std::size_t at) const noexcept { return reinterpret_cast<const T *>(bytes_) + at; }

    // Into the empty slot `to` from the live `from`, which is left empty.
    void move_down(std::size_t from, std::size_t to) noexcept {
        std::construct_at(slot(to), std::move(*slot(from)));
        std::destroy_at(slot(from));
    }

    void take(List &o) noexcept {
        for (std::size_t i = 0; i < o.len_; i++) std::construct_at(slot(i), std::move(*o.slot(i)));
        len_ = o.len_;
        o.clear();
    }

    // Uninitialized; a value is built into a slot by `push` and ended by what
    // takes it out. One byte where `N` is 0, as an array is never empty.
    alignas(T) std::byte bytes_[std::max<std::size_t>(N, 1) * sizeof(T)];
    std::size_t len_ = 0;
};

} // namespace xpute

// cpp/xpute/kit/math/rng.32.hpp

// The 32-bit family, rng.64.hpp's counterpart in u32 alone, gpu-portable since
// WebGPU has no 64-bit integer: Murmur3's finalizer, a seed derived through
// it, and a stream that draws it over a Weyl sequence.

namespace xpute {

inline constexpr std::uint32_t SEED_SALT_32 = 0x9e3779b9u;

constexpr std::uint32_t mix32(std::uint32_t x) noexcept {
    std::uint32_t z = x + SEED_SALT_32;
    z = (z ^ z >> 16) * 0x85ebca6bu;
    z = (z ^ z >> 13) * 0xc2b2ae35u;
    return z ^ z >> 16;
}

constexpr std::uint32_t derive_seed_u32(std::uint32_t base, std::uint32_t tag) noexcept {
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

// The 64-bit family: splitmix64's finalizer, a seed derived through it, and
// splitmix64's stream drawn as u32, each its word's high 32 bits.

namespace xpute {

inline constexpr std::uint64_t SEED_SALT = 0x9e3779b97f4a7c15u;

constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    std::uint64_t z = x + SEED_SALT;
    z = (z ^ z >> 30) * 0xbf58476d1ce4e5b9u;
    z = (z ^ z >> 27) * 0x94d049bb133111ebu;
    return z ^ z >> 31;
}

constexpr std::uint64_t derive_seed_u64(std::uint64_t base, std::uint64_t tag) noexcept {
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
        // The high half, as java.util.random's nextInt and rand_xoshiro's
        // SplitMix64 take a u32 from a 64-bit word.
        return static_cast<std::uint32_t>(z >> 32);
    }

  private:
    std::uint64_t x_;
};

} // namespace xpute

// cpp/xpute/kit/math/rng.hpp

// Reproducible seeds and streams; not for anything that must resist prediction.
// Two families, a file each: 64-bit (splitmix64, rng.64.hpp) and 32-bit
// (Murmur3's finalizer, rng.32.hpp), the latter gpu-portable, since WebGPU has
// no 64-bit integer.

namespace xpute {

/// Uniform in [0, 1).
constexpr double u32_to_unit(std::uint32_t x) noexcept {
    return static_cast<double>(x) / 4294967296.0;
}

} // namespace xpute

// cpp/xpute/kit/math/scalar.hpp

// Functions of one float or double, each exact in IEEE arithmetic so the bits
// match Rust's.

// Rust never fuses a multiply and an add; held here, not only in the flags.
// GCC does not read the pragma, so there the flag alone holds it.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#include <cmath>

namespace xpute {

inline constexpr double EPS = 1e-5;

// What these functions take: double, and float where a value lives as one.
template <class T>
concept Float = std::is_floating_point_v<T>;

// Absolute and relative tolerance, so the allowed error scales with magnitude.
template <Float T> inline bool approx_eq(T a, T b, T eps = static_cast<T>(EPS)) noexcept {
    if (a == b) return true;
    T diff = std::fabs(a - b);
    return diff < eps || diff < eps * std::fmax(std::fabs(a), std::fabs(b));
}

template <Float T> inline bool near(T a, T b, T tol) noexcept {
    return std::fabs(a - b) < tol;
}

// The project rounds to even, as IEEE 754 and WGSL do, so a value the CPU and
// a shader both round lands on one integer.

// -2.5 is -2, as JavaScript's `Math.round`.
template <Float T> inline T round_half_up(T x) noexcept {
    return x - std::trunc(x) == T(-0.5) ? std::ceil(x) : std::round(x);
}

// -2.5 is -3, as C's `round`.
template <Float T> inline T round_half_away(T x) noexcept {
    return std::round(x);
}

// 2.5 is 2, -2.5 is -2.
template <Float T> inline T round_half_even(T x) noexcept {
    // x - floor(x) is exact for every value, and 0 past the mantissa, so the
    // tie is told exactly whatever the rounding mode.
    T f = std::floor(x), d = x - f;
    T r = d < T(0.5) ? f : d > T(0.5) ? f + 1 : std::fmod(f, T(2)) == 0 ? f : f + 1;
    // -0.5 rounds to -0, as Rust's round_ties_even.
    return r == 0 ? std::copysign(T(0), x) : r;
}

template <Float T> inline T round(T x) noexcept {
    return round_half_even(x);
}

// As Rust's `as`: toward zero, saturating, NaN as 0, where a plain cast is
// undefined past the range.
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

// TLV, a forward-only typed value sequence: [tag, payload] … [END]. A reader
// keeps nothing it has passed. Reading stops closed (END), unclosed (input
// ran out at a boundary), or EBADMSG (inside an element or an unknown tag).

#include <cstring>
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

// Invalid UTF-8 decodes to U+FFFD, as the host decodes it.
struct Str {
    std::string_view text;
    bool operator==(const Str &) const = default;
    template <class Put> void decode(Put &&put) const { utf8_lossy(text, std::forward<Put>(put)); }
};

struct Bytes {
    std::span<const std::uint8_t> data;
    bool operator==(const Bytes &o) const { return std::equal(data.begin(), data.end(), o.data.begin(), o.data.end()); }
};

enum class End : std::uint8_t {
    closed,
    unclosed,
};

// In the tags' order.
using Value = std::variant<bool, std::uint8_t, std::int8_t, std::uint16_t, std::int16_t, std::uint32_t, std::int32_t, std::uint64_t, std::int64_t, float, double, Str, Bytes>;

template <class Buf> class Writer {
  public:
    explicit Writer(Buf &buf) noexcept : buf_(buf) {}

    const Buf &data() const noexcept { return buf_; }

    // The writer is not written to after.
    const Buf &finish() {
        if (!sealed_) {
            buf_.push_back(static_cast<std::uint8_t>(Tag::end));
            sealed_ = true;
        }
        return buf_;
    }

    Result<void> write(const Value &v) {
        if (sealed_) return marshal_error(Errno::ebadmsg);
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
    void put(const Bytes &b) {
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

    // None at END or at the input's end, which `end` tells apart.
    Result<std::optional<Value>> next() noexcept;

    // None while values remain, and none after a fault.
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
    // In 64 bits: a length past 2^32 off the wire would otherwise wrap.
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

// XTP, the Xpute Tree Packet: a relocatable packet of one tree; the golden records hold every
// language's encoding to the same bytes.
//
//   word0  [MAGIC 32 | reserved 32]
//   word1  [root payload bytes 32 | type 8 | reserved 24]
//
// A branch is an 8-byte child count, then one word per child (offset in the
// low half, type in the next byte). An absent node keeps its type with a size
// or offset of 0.

namespace xpute::xtp {

using NodeType = std::uint8_t;

// A leaf's type byte, when its top bit is set:
//
//   7 | 6 | 5 | 4 3 | 2 1 0
//   L | R | S |  C  |   A
//
// L leaf, R reserved, S sequence (else scalar), C class (unsigned, signed,
// float, neither), A its argument: log2 of a number's width in bytes.
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
    str = BIT_SEQ | nonnum_type(1),
    // len + 1 offsets into the UTF-8 after them.
    strs = BIT_SEQ | nonnum_type(2),
};

enum class SpecialType : NodeType {
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

// "XTP\0" read little-endian.
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

// Low half first.
inline void set_word(std::span<std::uint8_t> buf, std::uint32_t off, std::uint32_t lo, std::uint32_t hi) noexcept {
    store(buf.data() + off, lo);
    store(buf.data() + off + 4, hi);
}
inline std::array<std::uint32_t, 2> get_word(std::span<const std::uint8_t> buf, std::uint32_t off) noexcept {
    return {load<std::uint32_t>(buf.data() + off), load<std::uint32_t>(buf.data() + off + 4)};
}

// A type the wire carries: the untyped null, a branch, or a leaf of a width its class has.
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

// A read slot whose high half is reserved, zero on the wire: a len, a size, the magic.
inline Result<std::uint32_t> get_len(std::span<const std::uint8_t> buf, std::uint32_t off) noexcept {
    auto [lo, hi] = get_word(buf, off);
    if (hi != RESERVED) return marshal_error(Errno::ebadmsg);
    return lo;
}

// A read child entry or root word: its offset or size, and a known type with the rest reserved.
struct Desc {
    std::uint32_t off;
    NodeType type;
};
inline Result<Desc> get_desc(std::span<const std::uint8_t> buf, std::uint32_t off) noexcept {
    auto [lo, desc] = get_word(buf, off);
    auto type = static_cast<NodeType>(desc & DESC_TYPE_MASK);
    if (desc != type || !type_is_known(type)) return marshal_error(Errno::ebadmsg);
    return Desc{lo, type};
}

// `unit` is a power of two no wider than a word.
inline Result<std::uint32_t> align(std::uint32_t nbyte, std::uint32_t unit) noexcept {
    if (unit == 0 || (unit & (unit - 1)) != 0 || unit > WORD_SZ) return marshal_error(Errno::einval);
    std::uint32_t mask = unit - 1;
    if (nbyte > UINT32_MAX - mask) return marshal_error(Errno::eoverflow);
    return (nbyte + mask) & ~mask;
}

// A sequence or a branch starts on a word so its leading slot is one.
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
    return marshal_error(Errno::ebadmsg);
}

// A string sequence's element is its offsets' word.
constexpr std::uint32_t elem_sz(SequenceType t) noexcept {
    if (t == SequenceType::strs) return 4;
    return type_is_nonnum(type_of(t)) ? 1 : 1u << type_arg_of(type_of(t));
}

} // namespace xpute::xtp

// cpp/xpute/kit/wire/xtp/cursor.hpp

// Cursors over a packet; what a read hands back is a view of the packet's
// bytes, so the packet must outlive it. The packet need not be aligned in
// memory, but its nodes must be aligned within it.

#include <string>

namespace xpute::xtp {

struct Null {
    bool operator==(const Null &) const = default;
};

template <class T> class Elements {
  public:
    Elements(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(bytes_.size() / sizeof(T)); }
    T operator[](std::uint32_t i) const noexcept { return load<T>(bytes_.data() + i * sizeof(T)); }

  private:
    std::span<const std::uint8_t> bytes_;
};

struct Array {
    SequenceType type;
    std::span<const std::uint8_t> bytes;

    template <class T> Elements<T> elements() const noexcept { return bytes; }
};

// Low bit first.
struct Bitset {
    std::span<const std::uint8_t> packed;
    std::uint32_t len;

    bool operator[](std::uint32_t i) const noexcept { return (packed[i >> 3] >> (i & 7)) & 1; }
};

struct Strs {
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
        friend struct Strs;
        iterator(const Strs *s, std::uint32_t i) noexcept : s_(s), i_(i) {}
        const Strs *s_;
        std::uint32_t i_;
    };
    iterator begin() const noexcept { return iterator(this, 0); }
    iterator end() const noexcept { return iterator(this, len); }
};

class BranchCursor;
template <class Alloc> struct DeepValue;

using Value = std::variant<Null, bool, std::uint8_t, std::int8_t, std::uint16_t, std::int16_t, std::uint32_t, std::int32_t, std::uint64_t, std::int64_t, float,
                           double, std::string_view, Array, Bitset, Strs, BranchCursor>;

class Cursor {
  public:
    NodeType type() const noexcept { return type_; }
    bool is_null() const noexcept { return base_ == 0; }
    bool is_branch() const noexcept { return !is_null() && type_ == type_of(SpecialType::branch); }

    std::optional<Cursor> opt() const noexcept { return is_null() ? std::nullopt : std::optional(*this); }

    Result<BranchCursor> as_branch() const noexcept;

    Result<Value> get() const noexcept;

    template <class T> Result<Elements<T>> get_array() const noexcept {
        if (is_null() || type_ != type_of(ARRAY_OF<T>)) return marshal_error(Errno::ebadmsg);
        Result<Array> a = array(sizeof(T));
        if (!a) return a.error();
        return Elements<T>(a->bytes);
    }

    Result<std::uint64_t> get_u64() const noexcept;
    Result<std::int64_t> get_i64() const noexcept;
    Result<bool> get_bool() const noexcept;
    Result<std::string_view> get_str_ref() const noexcept;
    template <class Alloc = std::allocator<char>> Result<std::basic_string<char, std::char_traits<char>, Alloc>> get_str(const Alloc &alloc = Alloc()) const {
        Result<std::string_view> s = get_str_ref();
        if (!s) return s.error();
        return std::basic_string<char, std::char_traits<char>, Alloc>(*s, alloc);
    }
    Result<Strs> get_strs() const noexcept;
    template <class Alloc = std::allocator<std::byte>> Result<DeepValue<Alloc>> get_deep(const Alloc &alloc = Alloc()) const;

    // Not for 64-bit integers, which a double cannot hold exactly.
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
    Result<Strs> strs() const noexcept;

    std::span<const std::uint8_t> pkt_;
    // 0 for an absent node; no node can start there.
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

    // Where child `idx`'s extent ends: the next present sibling's base, or this branch's end.
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

// Writes a packet in place into bytes the caller holds, with no tree and no
// allocation; the bytes match the tree encoder's for the same tree. A string
// fits where its UTF-8 and the NUL after it do, on every side.

namespace xpute::xtp {

class PacketWriter {
  public:
    // The buffer's length is the cap, at most MAX_PKT_SZ.
    explicit PacketWriter(std::span<std::uint8_t> buf) noexcept;

    PacketWriter &nil() noexcept { return entry(std::nullopt, type_of(SpecialType::nil)); }

    // None writes a null of that type.
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

    PacketWriter &u8_array(std::optional<std::span<const std::uint8_t>> a) noexcept { return array(a); }
    PacketWriter &i8_array(std::optional<std::span<const std::int8_t>> a) noexcept { return array(a); }
    PacketWriter &u16_array(std::optional<std::span<const std::uint16_t>> a) noexcept { return array(a); }
    PacketWriter &i16_array(std::optional<std::span<const std::int16_t>> a) noexcept { return array(a); }
    PacketWriter &u32_array(std::optional<std::span<const std::uint32_t>> a) noexcept { return array(a); }
    PacketWriter &i32_array(std::optional<std::span<const std::int32_t>> a) noexcept { return array(a); }
    PacketWriter &u64_array(std::optional<std::span<const std::uint64_t>> a) noexcept { return array(a); }
    PacketWriter &i64_array(std::optional<std::span<const std::int64_t>> a) noexcept { return array(a); }
    PacketWriter &f32_array(std::optional<std::span<const float>> a) noexcept { return array(a); }
    PacketWriter &f64_array(std::optional<std::span<const double>> a) noexcept { return array(a); }

    template <class T, class F> PacketWriter &array_with(std::uint32_t len, F &&val) noexcept {
        std::optional<std::uint32_t> at = seq(len, std::uint64_t{len} * sizeof(T));
        if (at) {
            for (std::uint32_t i = 0; i < len; i++) store<T>(buf_.data() + *at + WORD_SZ + i * sizeof(T), val(i));
        }
        return entry(at, type_of(ARRAY_OF<T>));
    }

    // Packed low bit first.
    PacketWriter &bitset(std::optional<std::span<const std::uint8_t>> bits) noexcept;

    // Followed by a NUL that the length leaves out.
    PacketWriter &str(std::optional<std::string_view> s) noexcept;

    // len + 1 offsets into the UTF-8 after them.
    PacketWriter &strs(std::optional<std::span<const std::string_view>> items) noexcept;

    // `write` gets the room after the length slot and answers the bytes written,
    // or none when they did not fit.
    template <class F> PacketWriter &str_with(F &&write) noexcept {
        std::optional<std::uint32_t> at;
        if (std::optional<std::uint32_t> room = str_room()) {
            std::uint32_t payload = str_payload();
            std::optional<std::size_t> n = write(std::span<char>(reinterpret_cast<char *>(buf_.data() + payload), *room));
            at = n && *n <= *room ? str_commit(static_cast<std::uint32_t>(*n)) : spoil(Errno::eoverflow);
        }
        return entry(at, type_of(SequenceType::str));
    }

    // Writing a different number of children than `count` is a bug.
    template <class F> PacketWriter &branch(std::uint32_t cnt, F &&fill) noexcept {
        std::optional<std::uint32_t> at = branch_open(cnt);
        Frame outer = frame_;
        frame_ = Frame{.base = at.value_or(0), .cnt = cnt, .written = 0, .root = false};
        fill(*this);
        ensure(frame_.written == frame_.cnt, Errno::enotrecoverable, frame_.cnt, frame_.written);
        frame_ = outer;
        return entry(branch_close(at), type_of(SpecialType::branch));
    }

    PacketWriter &no_branch() noexcept { return entry(std::nullopt, type_of(SpecialType::branch)); }

    // Its root's payload copied, placed as its root's type places it.
    PacketWriter &graft(std::span<const std::uint8_t> pkt) noexcept;

    // The length, header included and to the word, or why it was refused.
    Result<std::uint32_t> finish() noexcept;

  private:
    // The root is a branch of one whose entry is the header.
    struct Frame {
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

    template <class T> PacketWriter &array(std::optional<std::span<const T>> a) noexcept {
        if (!a) return entry(std::nullopt, type_of(ARRAY_OF<T>));
        return array_with<T>(static_cast<std::uint32_t>(a->size()), [&a](std::uint32_t i) { return (*a)[i]; });
    }

    std::uint32_t str_payload() const noexcept;
    std::optional<std::uint32_t> str_room() noexcept;
    std::optional<std::uint32_t> str_commit(std::uint32_t len) noexcept;

    std::optional<std::uint32_t> branch_open(std::uint32_t cnt) noexcept;
    std::optional<std::uint32_t> branch_close(std::optional<std::uint32_t> at) noexcept;

    std::span<std::uint8_t> buf_;
    Frame frame_{.base = 0, .cnt = 1, .written = 0, .root = true};
    NodeType root_type_ = type_of(SpecialType::nil);
    std::uint32_t lim_ = HDR_SZ;
    // The first reason only.
    std::optional<Error> err_;
};

} // namespace xpute::xtp

// cpp/xpute/kit/wire/xtp/view.hpp

// The write end as a tree. A node borrows what it carries (an array, a string,
// a graft's packet), so the caller keeps them alive until the tree is encoded.

namespace xpute::xtp {

enum class NodeKind : std::uint8_t { nil, scalar, array, bitset, str, strs, branch, graft };

template <class Alloc = std::allocator<std::byte>> struct Node {
    using Children = std::vector<Node, typename std::allocator_traits<Alloc>::template rebind_alloc<Node>>;

    NodeKind kind = NodeKind::nil;
    NodeType type = type_of(SpecialType::nil);
    // A typed null keeps its type and is absent.
    bool present = true;
    // Little end first.
    std::uint64_t bits = 0;
    std::span<const std::uint8_t> bytes;
    std::string_view str;
    std::span<const std::string_view> strs;
    Children children;

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

// `Self::write` replaces a tree's root or appends a branch's child.
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

    // Bits given a byte each, nonzero for set.
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

    // With no `fill`, an absent branch that keeps its type.
    template <class F> Self &branch(F &&fill) {
        BranchView<Alloc> b(alloc());
        fill(b);
        return self().write(std::move(b.node()));
    }
    Self &no_branch() {
        Node<Alloc> n(alloc());
        n.kind = NodeKind::branch;
        n.type = type_of(SpecialType::branch);
        n.present = false;
        return self().write(std::move(n));
    }

    // Only the construction is checked here; the header is read at encode.
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
                    // A number array of whole elements, or a bitset, as the Rust encoder takes.
                    bool bitset = x.type == SequenceType::bitset;
                    if (!bitset && (type_is_nonnum(type_of(x.type)) || x.bytes.size() % elem_sz(x.type) != 0)) return marshal_error(Errno::ebadmsg);
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

struct TreeEncoderOptions {
    // The first buffer's size; it doubles where the tree overflows it.
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
            case SequenceType::u8_array: w.u8_array(std::nullopt); return;
            case SequenceType::i8_array: w.i8_array(std::nullopt); return;
            case SequenceType::u16_array: w.u16_array(std::nullopt); return;
            case SequenceType::i16_array: w.i16_array(std::nullopt); return;
            case SequenceType::u32_array: w.u32_array(std::nullopt); return;
            case SequenceType::i32_array: w.i32_array(std::nullopt); return;
            case SequenceType::u64_array: w.u64_array(std::nullopt); return;
            case SequenceType::i64_array: w.i64_array(std::nullopt); return;
            case SequenceType::f32_array: w.f32_array(std::nullopt); return;
            default: w.f64_array(std::nullopt); return;
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

// EINVAL for a cap outside a packet's sizes, EOVERFLOW for a tree past the cap.
template <class Alloc, class View>
Result<std::vector<std::uint8_t, typename std::allocator_traits<Alloc>::template rebind_alloc<std::uint8_t>>> encode(const View &view, TreeEncoderOptions opts = {},
                                                                                                                     const Alloc &alloc = Alloc()) {
    using Bytes = std::vector<std::uint8_t, typename std::allocator_traits<Alloc>::template rebind_alloc<std::uint8_t>>;
    std::uint32_t max_cap = opts.max_cap.value_or(MAX_PKT_SZ);
    if (max_cap < HDR_SZ || max_cap > MAX_PKT_SZ) return marshal_error(Errno::einval);
    std::uint32_t cap = std::max(HDR_SZ, std::min(opts.init_cap, max_cap));
    Bytes buf(alloc);
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

// cpp/xpute/guest/abi/number.hpp

// A number a packet carries, read as its field's type. What the host sends is
// checked here, so nothing past the door meets a NaN, an infinity or an
// integer cast into range.

namespace xpute::abi {

// A number an enum does not give out, where the enum has no member it reads as.
constexpr Error unknown(std::source_location at = std::source_location::current()) noexcept {
    return marshal_error(Errno::einval, at);
}

inline Result<double> finite(double n, std::source_location at = std::source_location::current()) noexcept {
    if (!std::isfinite(n)) return unknown(at);
    return n;
}

// A whole number the type holds, never one cast into it.
template <class T> Result<T> integer(double n, std::source_location at = std::source_location::current()) noexcept {
    bool whole = std::isfinite(n) && n == std::trunc(n) && std::fabs(n) <= static_cast<double>(std::uint64_t{1} << std::numeric_limits<double>::digits);
    if (!whole) return unknown(at);
    auto i = static_cast<std::int64_t>(n);
    if (!std::in_range<T>(i)) return unknown(at);
    return static_cast<T>(i);
}

} // namespace xpute::abi

// cpp/xpute/guest/abi/cmd.spec.hpp
//
// GENERATED from spec/abi/cmd.json
//
// xpute's own commands, by number (kit abi/cmd: `major << 8 | minor`), and the packet
// each carries — an XTP branch whose children are its fields, in order. A command with
// no fields carries no packet. The host sends them, port 0 (ipc/frame.json HOST); a
// program numbers its own as it likes, and the sender tells the two apart.
//
// The direction is the ring: `pkt` is what the host submits, `event` what the guest
// raises on the completion ring.

namespace xpute::abi {

// What a command is about.
enum class Major : std::uint32_t {
    NOP = 0x00,
    SYS = 0x01,
};

} // namespace xpute::abi

template <> struct xpute::Members<xpute::abi::Major> {
    static constexpr std::array<xpute::abi::Major, 2> ALL{xpute::abi::Major::NOP, xpute::abi::Major::SYS};
};

namespace xpute::abi {

// The hostcalls' answers (ipc/sys.json).
enum class SysMinor : std::uint32_t {
    OPENED = 0x01,
};

} // namespace xpute::abi

template <> struct xpute::Members<xpute::abi::SysMinor> {
    static constexpr std::array<xpute::abi::SysMinor, 1> ALL{xpute::abi::SysMinor::OPENED};
};

namespace xpute::abi {

// xpute's own commands.
enum class Command : std::uint32_t {
    // Replies with its packet's value, or 0 where it has none — the rings' own test.
    NOP = 0x0000,
    // An OPENAT answered: the file's size, or -errno — ENOENT where there is no such file.
    // Never sent for a descriptor already closed.
    SYS_OPENED = 0x0101,
};

} // namespace xpute::abi

template <> struct xpute::Members<xpute::abi::Command> {
    static constexpr std::array<xpute::abi::Command, 2> ALL{xpute::abi::Command::NOP, xpute::abi::Command::SYS_OPENED};
};

namespace xpute::abi {

// Command::NOP's packet.
struct Nop {
    std::uint32_t val{};

    static Result<Nop> read(std::span<const std::uint8_t> pkt) noexcept {
        Result<xtp::TreeReader> t = xtp::TreeReader::make(pkt);
        if (!t) return t.error();
        Result<xtp::BranchCursor> f = t->read_branch();
        if (!f) return f.error();
        Nop out;
        {
            Result<xtp::Cursor> c = f->at(0);
            if (!c) return c.error();
            auto v = c->get_number();
            if (!v) return v.error();
            auto n = integer<std::uint32_t>(*v);
            if (!n) return n.error();
            out.val = *n;
        }
        return out;
    }
};

// Command::SYS_OPENED's packet.
struct SysOpened {
    std::uint32_t fd{};
    std::int32_t res{};
    // Whatever number the host refused with — for HTTP its status — kept for a reader to print and for nothing to branch on.
    std::uint32_t detail{};

    static Result<SysOpened> read(std::span<const std::uint8_t> pkt) noexcept {
        Result<xtp::TreeReader> t = xtp::TreeReader::make(pkt);
        if (!t) return t.error();
        Result<xtp::BranchCursor> f = t->read_branch();
        if (!f) return f.error();
        SysOpened out;
        {
            Result<xtp::Cursor> c = f->at(0);
            if (!c) return c.error();
            auto v = c->get_number();
            if (!v) return v.error();
            auto n = integer<std::uint32_t>(*v);
            if (!n) return n.error();
            out.fd = *n;
        }
        {
            Result<xtp::Cursor> c = f->at(1);
            if (!c) return c.error();
            auto v = c->get_number();
            if (!v) return v.error();
            auto n = integer<std::int32_t>(*v);
            if (!n) return n.error();
            out.res = *n;
        }
        {
            Result<xtp::Cursor> c = f->at(2);
            if (!c) return c.error();
            auto v = c->get_number();
            if (!v) return v.error();
            auto n = integer<std::uint32_t>(*v);
            if (!n) return n.error();
            out.detail = *n;
        }
        return out;
    }
};

} // namespace xpute::abi

// cpp/xpute/guest/abi/handle.hpp

// A handle is `slot | generation << 16`, the far side reading only the slot,
// so a handle kept past its release names nothing. Slot 0 is never handed out.

namespace xpute::abi {

// The rest is the generation, hence its 16 bits. The host uses the same 16.
inline constexpr std::uint32_t SLOT_BITS = 16;
static_assert(SLOT_BITS + std::numeric_limits<std::uint16_t>::digits == 32, "a slot and a generation fill a handle");

constexpr std::uint32_t handle_slot(std::uint32_t handle) noexcept {
    return handle & ((1u << SLOT_BITS) - 1);
}

// N slots, each holding a `T` while its handle is live; a slot given back is
// the next one taken. All-zero bytes are an empty table (for a `T` whose zero
// is its default), so one may lie in memory that starts zeroed, or be emptied
// by zeroing it, never built and moved.
template <class T, std::size_t N> class Table {
    static_assert(N >= 2 && N <= (std::size_t{1} << SLOT_BITS), "a slot is 16 bits, and slot 0 is never handed out");

  public:
    // None when every slot is taken.
    std::optional<std::uint32_t> insert(T val) noexcept {
        std::uint32_t slot;
        if (free_ != 0) {
            slot = free_;
            free_ = slots_[slot].next;
        } else if (std::size_t{top_} + 1 < N) {
            slot = ++top_;
        } else {
            return std::nullopt;
        }
        Slot &s = slots_[slot];
        // Never 0, so no handle is 0.
        auto generation = static_cast<std::uint16_t>(s.generation + 1);
        s.generation = generation == 0 ? 1 : generation;
        s.made = epoch_ + 1;
        s.val = std::move(val);
        cnt_++;
        return handle_of(slot, s.generation);
    }

    bool live(std::uint32_t handle) const noexcept {
        std::uint32_t slot = handle_slot(handle);
        return slot != 0 && slot < N && slots_[slot].made != 0 && slots_[slot].generation == handle >> SLOT_BITS;
    }

    T *get(std::uint32_t handle) noexcept { return live(handle) ? &slots_[handle_slot(handle)].val : nullptr; }
    const T *get(std::uint32_t handle) const noexcept { return const_cast<Table *>(this)->get(handle); }

    std::optional<T> remove(std::uint32_t handle) noexcept {
        if (!live(handle)) return std::nullopt;
        std::uint32_t slot = handle_slot(handle);
        Slot &s = slots_[slot];
        s.made = 0;
        s.next = free_;
        free_ = slot;
        cnt_--;
        return std::exchange(s.val, T{});
    }

    // From here, what is taken is what `rollback` gives back.
    void begin() noexcept { epoch_++; }

    // Every handle taken since `begin` and not yet removed, removed and handed
    // to `gone`: what a transaction that did not commit made, which the other
    // side never saw.
    template <class F> void rollback(F &&gone) noexcept {
        for (std::uint32_t slot = 1; slot <= top_; slot++) {
            std::uint32_t handle = handle_of(slot, slots_[slot].generation);
            if (slots_[slot].made != epoch_ + 1) continue;
            if (std::optional<T> val = remove(handle)) gone(handle, std::move(*val));
        }
    }

    std::uint32_t count() const noexcept { return cnt_; }

    template <class F> void each(F &&f) {
        for (std::uint32_t slot = 1; slot <= top_; slot++) {
            std::uint32_t handle = handle_of(slot, slots_[slot].generation);
            if (live(handle)) f(handle, slots_[slot].val);
        }
    }

  private:
    struct Slot {
        std::uint16_t generation = 0;
        // One past the epoch the slot was taken in; 0 while free. 64 bits, so
        // no count of epochs a program lives through brings a stale one round.
        std::uint64_t made = 0;
        // A free slot's next free slot; 0, never handed out, ends the list.
        std::uint32_t next = 0;
        T val{};
    };

    static constexpr std::uint32_t handle_of(std::uint32_t slot, std::uint16_t generation) noexcept {
        return slot | std::uint32_t{generation} << SLOT_BITS;
    }

    std::array<Slot, N> slots_{};
    // Advanced by `begin`: the slots taken since are those made in it.
    std::uint64_t epoch_ = 0;
    std::uint32_t free_ = 0;
    // The highest slot handed out; those past it never have been.
    std::uint32_t top_ = 0;
    std::uint32_t cnt_ = 0;
};

} // namespace xpute::abi

// cpp/xpute/guest/clock.hpp

// Milliseconds, as `performance.now()`. Until the host installs one, time
// stands at zero, which tests run on.

namespace xpute {

using Clock = double (*)();

void set_clock(Clock clock) noexcept;

double now() noexcept;

} // namespace xpute

// cpp/xpute/guest/hostcall.hpp

// The host's `hostcall`: a record at an offset, run before it returns, and
// its answer, a value or -errno. Until the host installs one, ENOSYS, which
// tests run on.

namespace xpute {

using Hostcall = std::int32_t (*)(std::uint32_t at);

void set_hostcall(Hostcall hostcall) noexcept;

// `at` is the record's offset in the core memory.
std::int32_t hostcall(std::uint32_t at) noexcept;

} // namespace xpute

// cpp/xpute/guest/ipc/frame.spec.hpp
//
// GENERATED from spec/ipc/frame.json
//
// A frame in a ring (ring.json): four words. The tag names the port at the guest's
// far side in its high bits — who sent a frame the guest takes, who is to have one it
// posts — and below them an id, so a reply, which keeps its submission's tag, goes back
// to its sender; the command shares its word with the flags; a reply carries a value or
// a negative errno; and a packet, where there is one, lies in the ring's payload slot.
//
// The host is port 0, and a command it sends is xpute's own (abi/cmd.json); every
// other number is the program's to give, and so is what its commands mean. The host
// hands a frame to the port it names where one is attached, and drops it where none
// is.
//
// Reserved for later, so taking them moves no word: flag bits 2 and up, and the result
// word on a submission.

namespace xpute::frame {

// The far port from PORT_SHIFT up; below it the sender's id, echoed on the
// reply, or an event's subject.
inline constexpr std::uint32_t TAG = 0u;

// The command in the low FLAG_SHIFT bits, the flags above them.
inline constexpr std::uint32_t CMD = 1u;

// On a reply, a value or a negative errno; else 0.
inline constexpr std::uint32_t RESULT = 2u;

// Where its XTP packet is, as the transport names it; 0 for none.
inline constexpr std::uint32_t PKT = 3u;

// Words a frame takes.
inline constexpr std::uint32_t WORDS = 4u;

// Where the far port starts in the TAG word: one of 256.
inline constexpr std::uint32_t PORT_SHIFT = 24u;

// The host's port, whose commands are xpute's own.
inline constexpr std::uint32_t HOST = 0u;

// Where the flags start in the CMD word.
inline constexpr std::uint32_t FLAG_SHIFT = 16u;

// A flag: a reply, as against an event. Set by the sender, never by a handler.
inline constexpr std::uint32_t RES = 1u;

// A flag: always reply; without it, only a failure is replied to.
inline constexpr std::uint32_t ACKREQ = 2u;

} // namespace xpute::frame

// cpp/xpute/guest/ipc/frame.hpp

// A frame's four words, laid out by spec/ipc/frame.json.

namespace xpute {

// A frame's four words as read, its command word split.
struct Frame {
    std::uint32_t tag;
    std::uint32_t cmd;
    std::uint32_t flags;
    std::int32_t result;
    // 0 for none.
    std::uint32_t pkt_at;

    bool operator==(const Frame &) const = default;
};

} // namespace xpute

namespace xpute::frame {

inline constexpr std::uint32_t CMD_MASK = (1u << FLAG_SHIFT) - 1;

constexpr std::uint32_t cmd_word(std::uint32_t cmd, std::uint32_t flags) noexcept {
    return flags << FLAG_SHIFT | (cmd & CMD_MASK);
}

constexpr std::uint32_t cmd_of(std::uint32_t word) noexcept {
    return word & CMD_MASK;
}

constexpr std::uint32_t flags_of(std::uint32_t word) noexcept {
    return word >> FLAG_SHIFT;
}

// The far port a frame's tag names.
constexpr std::uint32_t port_of(std::uint32_t tag) noexcept {
    return tag >> PORT_SHIFT;
}

// The tag of a frame for `port`, its id or subject `id`.
constexpr std::uint32_t tagged(std::uint32_t port, std::uint32_t id) noexcept {
    return port << PORT_SHIFT | id;
}

} // namespace xpute::frame

// cpp/xpute/guest/mem.hpp

// The memory both sides see, and the one place an offset becomes a pointer.
// A word is copied rather than dereferenced, since an offset from the other
// side carries no alignment a compiler may assume. A part laid over a region
// takes a `Memory` and an offset; the guest's own memory is `mem::core`.

namespace xpute {

class Memory {
  public:
    explicit Memory(std::uintptr_t base, std::uintptr_t bytes = UINTPTR_MAX) noexcept : base_(base), bytes_(bytes) {}

    std::uintptr_t base() const noexcept { return base_; }

    std::uint8_t *at(std::uint32_t off) const noexcept { return reinterpret_cast<std::uint8_t *>(base_ + off); }

    // A word past the memory's end is a bug.
    std::uint32_t word(std::uint32_t off, std::source_location loc = std::source_location::current()) const noexcept {
        within(off, sizeof(std::uint32_t), loc);
        std::uint32_t w;
        std::memcpy(&w, at(off), sizeof w);
        return w;
    }

    void set_word(std::uint32_t off, std::uint32_t w, std::source_location loc = std::source_location::current()) const noexcept {
        within(off, sizeof w, loc);
        std::memcpy(at(off), &w, sizeof w);
    }

    // A pointer outside the memory is a bug; on a native build a static or the
    // binary's read-only data is not in it.
    std::uint32_t off_of(const void *p, std::source_location loc = std::source_location::current()) const noexcept {
        std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
        ensure(a >= base_ && a - base_ < bytes_ && a - base_ <= UINT32_MAX, Errno::efault, a, base_, loc);
        return static_cast<std::uint32_t>(a - base_);
    }

    // An empty span's pointer is only its alignment, so it names 0.
    template <class T> std::uint32_t off_of(std::span<T> s, std::source_location loc = std::source_location::current()) const noexcept {
        return s.empty() ? 0 : off_of(static_cast<const void *>(s.data()), loc);
    }

  private:
    void within(std::uint32_t off, std::uint32_t len, std::source_location loc) const noexcept {
        ensure(static_cast<std::uintptr_t>(off) + len <= bytes_, Errno::efault, off, bytes_, loc);
    }

    std::uintptr_t base_;
    std::uintptr_t bytes_;
};

namespace mem {

// The guest's own memory, set once before anything is allocated: its start
// and span. Until then it starts at 0 and spans everything, a module's memory
// and a native test's.
void set(std::uintptr_t base, std::uintptr_t bytes = UINTPTR_MAX) noexcept;

Memory core() noexcept;

inline std::uint8_t *at(std::uint32_t off) noexcept {
    return core().at(off);
}

inline std::uint32_t off_of(const void *p, std::source_location loc = std::source_location::current()) noexcept {
    return core().off_of(p, loc);
}

template <class T> std::uint32_t off_of(std::span<T> s, std::source_location loc = std::source_location::current()) noexcept {
    return core().off_of(s, loc);
}

} // namespace mem

} // namespace xpute

// cpp/xpute/guest/ipc/ring.spec.hpp
//
// GENERATED from spec/ipc/ring.json
//
// A single-producer, single-consumer ring of frames (frame.json) in memory both sides
// see, named by its offset so it means the same to either. A header, then the frames;
// their payloads lie apart in slots, as in virtio and AF_XDP. Position n owns frame
// and slot n modulo CAP, so a slot is free exactly when its frame is.
// Positions only grow, and each side writes only its own word, so no lock is needed.

namespace xpute::ring {

// How many frames, a power of two.
inline constexpr std::uint32_t CAP = 0u;

// The consumer's position: the next frame it reads.
inline constexpr std::uint32_t HEAD = 1u;

// The producer's position: where the next frame goes.
inline constexpr std::uint32_t TAIL = 2u;

// A payload slot's size, a power of two and a multiple of 8.
inline constexpr std::uint32_t SLOT_BYTES = 3u;

// Where the slots start, as an offset in the memory.
inline constexpr std::uint32_t SLOT_BASE = 4u;

// Kept at 0.
inline constexpr std::uint32_t RESERVED_0 = 5u;

// Kept at 0.
inline constexpr std::uint32_t RESERVED_1 = 6u;

// Kept at 0.
inline constexpr std::uint32_t RESERVED_2 = 7u;

// Words before the first frame.
inline constexpr std::uint32_t HEADER_WORDS = 8u;

} // namespace xpute::ring

// cpp/xpute/guest/ipc/ring.hpp

// A ring of frames, laid out by spec/ipc/ring.json.

namespace xpute {

constexpr std::uint32_t ring_bytes(std::uint32_t cap) noexcept {
    return (ring::HEADER_WORDS + cap * frame::WORDS) * 4;
}

constexpr std::uint32_t slots_bytes(std::uint32_t cap, std::uint32_t slot_bytes) noexcept {
    return cap * slot_bytes;
}

class Ring {
  public:
    // A cap that is not a power of two is a bug.
    Ring(Memory mem, std::uint32_t at) noexcept;

    static Ring init(Memory mem, std::uint32_t at, std::uint32_t cap, std::uint32_t slot_base, std::uint32_t slot_bytes) noexcept;

    std::uint32_t cap() const noexcept { return cap_; }
    std::uint32_t slot_bytes() const noexcept { return slot_bytes_; }
    std::uint32_t head() const noexcept { return header(ring::HEAD); }
    std::uint32_t tail() const noexcept { return header(ring::TAIL); }
    std::uint32_t len() const noexcept { return tail() - head(); }
    bool empty() const noexcept { return len() == 0; }

    // The word, from the ring's start, where the frame at `position` begins.
    std::uint32_t entry_at(std::uint32_t pos) const noexcept { return ring::HEADER_WORDS + (pos & (cap_ - 1)) * frame::WORDS; }

    // EAGAIN when full, EMSGSIZE when larger than a slot.
    Errno push(std::uint32_t tag, std::uint32_t cmd, std::uint32_t flags, std::int32_t result,
               std::optional<std::span<const std::uint8_t>> pkt = std::nullopt) const noexcept;

    // `write` gets the whole slot payload and answers the length, or none if it
    // did not fit (EMSGSIZE).
    template <class Write> Errno push_in_place(std::uint32_t tag, std::uint32_t cmd, std::uint32_t flags, std::int32_t result, Write &&write) const noexcept {
        if (len() >= cap()) return Errno::eagain;
        std::uint32_t slot = slot_at(tail());
        std::optional<std::uint32_t> n = write(std::span<std::uint8_t>(mem_.at(slot), slot_bytes()));
        if (!n || *n > slot_bytes()) return Errno::emsgsize;
        pad(slot, *n);
        publish(tag, cmd, flags, result, slot);
        return Errno::ok;
    }

    // The head's frame, none when empty. Read it, then `advance`.
    std::optional<Frame> peek() const noexcept;

    // Valid until `advance`. EBADMSG when the frame names bytes outside a slot.
    Result<std::span<const std::uint8_t>> pkt(const Frame &f) const noexcept;

    void advance() const noexcept;

  private:
    std::uint32_t header(std::uint32_t word) const noexcept { return mem_.word(at_ + 4 * word); }
    void set_header(std::uint32_t word, std::uint32_t w) const noexcept { mem_.set_word(at_ + 4 * word, w); }
    std::uint32_t slot_at(std::uint32_t pos) const noexcept;
    void pad(std::uint32_t slot, std::uint32_t n) const noexcept;
    void publish(std::uint32_t tag, std::uint32_t cmd, std::uint32_t flags, std::int32_t result, std::uint32_t pkt_at) const noexcept;

    Memory mem_;
    std::uint32_t at_;
    // Read once, when the ring is made: the host's later writes to them are not the ring's.
    std::uint32_t cap_;
    std::uint32_t slot_bytes_;
    std::uint32_t slot_base_;
};

} // namespace xpute

// cpp/xpute/guest/ipc/door.hpp

// The guest's end of its two rings. A command is started only while the
// completion ring has room for all it may leave (`reserve`), as a credit-based
// link bounds a sender, so one started never finds the ring full; what is not
// started waits in order. Overrunning the ring is a sizing fault.

namespace xpute {

// Errno::enosys for a command the guest does not number.
struct Applied {
    Errno err = Errno::ok;
    std::int32_t val = 0;
};

// How full the rings ran since the stat was last taken (`Door::take_stat`):
// each figure the most it reached.
struct DoorStat {
    std::uint32_t cq_peak = 0;
    // The submission ring's length as a drain started: how close it came to full.
    std::uint32_t sq_peak = 0;
    // The submissions a drain left for lack of room.
    std::uint32_t left_peak = 0;

    bool operator==(const DoorStat &) const = default;
};

class Door {
  public:
    // `most_events` is the most one command raises, or the turn once its drain
    // is done: the program's bound.
    Door(Ring sq, Ring cq, std::uint32_t most_events) noexcept : sq_(sq), cq_(cq), reserve_(most_events + 1) {}

    const Ring &sq() const noexcept { return sq_; }
    const Ring &cq() const noexcept { return cq_; }

    DoorStat take_stat() const noexcept { return std::exchange(stat_, {}); }

    void post(std::uint32_t tag, std::uint32_t cmd, std::uint32_t flags, std::int32_t result) const noexcept {
        posted(cmd, cq_.push(tag, cmd, flags, result));
    }

    template <class Write> void post_in_place(std::uint32_t tag, std::uint32_t cmd, std::uint32_t flags, std::int32_t result, Write &&write) const noexcept {
        posted(cmd, cq_.push_in_place(tag, cmd, flags, result, write));
    }

    // An event for `port`.
    template <class Fill> void raise(std::uint32_t port, std::uint32_t cmd, std::uint32_t cnt, Fill &&fill) const noexcept {
        post_in_place(frame::tagged(port, 0), cmd, 0, 0, [&](std::span<std::uint8_t> out) -> std::optional<std::uint32_t> {
            xtp::PacketWriter w(out);
            w.branch(cnt, fill);
            Result<std::uint32_t> n = w.finish();
            return n ? std::optional(*n) : std::nullopt;
        });
    }

    // Applies waiting commands in order while there is room for what one more may
    // leave; replies to ACKREQ and to failures. Answers how many completions it made.
    // What the host sent, port 0, is xpute's own and applied here: NOP answers
    // its value, and an open the host completed goes to `opened`; every other
    // sender's goes to `apply`. A drain runs before
    // any phase's budget, so the program records what it is handed and returns.
    template <class Opened, class Apply> std::uint32_t drain(Opened &&opened, Apply &&apply) const noexcept {
        std::uint32_t tail = cq_.tail();
        stat_.sq_peak = std::max(stat_.sq_peak, sq_.len());
        while (has_room()) {
            std::optional<Frame> f = sq_.peek();
            if (!f) break;
            Result<std::span<const std::uint8_t>> pkt = sq_.pkt(*f);
            Applied a = pkt ? applied(*f, *pkt, opened, apply) : Applied{.err = pkt.error().errnum};
            sq_.advance();
            if (a.err != Errno::ok) {
                post(f->tag, f->cmd, frame::RES, static_cast<std::int32_t>(a.err));
            } else if (f->flags & frame::ACKREQ) {
                post(f->tag, f->cmd, frame::RES, a.val);
            }
        }
        stat_.left_peak = std::max(stat_.left_peak, sq_.len());
        return cq_.tail() - tail;
    }

    bool pending() const noexcept { return !sq_.empty(); }

  private:
    bool has_room() const noexcept { return cq_.len() + reserve_ <= cq_.cap(); }

    template <class Opened, class Apply> static Applied applied(const Frame &f, std::span<const std::uint8_t> pkt, Opened &opened, Apply &apply) noexcept {
        if (frame::port_of(f.tag) != frame::HOST) return apply(f.cmd, pkt);
        if (f.cmd == static_cast<std::uint32_t>(abi::Command::NOP)) {
            if (pkt.empty()) return {};
            Result<abi::Nop> nop = abi::Nop::read(pkt);
            if (!nop) return {.err = nop.error().errnum};
            return {.val = static_cast<std::int32_t>(nop->val)};
        }
        if (f.cmd == static_cast<std::uint32_t>(abi::Command::SYS_OPENED)) {
            Result<abi::SysOpened> o = abi::SysOpened::read(pkt);
            if (!o) return {.err = o.error().errnum};
            return opened(*o);
        }
        return {.err = Errno::enosys};
    }

    void posted(std::uint32_t cmd, Errno e) const noexcept {
        stat_.cq_peak = std::max(stat_.cq_peak, cq_.len());
        ensure(e == Errno::ok, Errno::enospc, cmd, static_cast<std::uint64_t>(static_cast<std::int64_t>(e)));
    }

    Ring sq_;
    Ring cq_;
    // What one command may leave: its events, or the turn's after the drain, and its reply.
    std::uint32_t reserve_;
    // Kept by the const calls that move the rings, as the rings' own words are.
    mutable DoorStat stat_;
};

} // namespace xpute

// cpp/xpute/guest/ipc/stream.hpp

// A turn's calls from guest to host as records in one range. The host runs
// them after the turn returns and before the next, so a record may name
// memory the host writes and the guest reads next turn.

namespace xpute {

inline constexpr std::uint32_t WORD = sizeof(std::uint32_t);

// A record's words: its op and count, `args`, and `bytes` as their count then words.
constexpr std::uint64_t record_words(std::uint64_t args, std::optional<std::uint64_t> bytes) noexcept {
    return 2 + args + (bytes ? 1 + (*bytes + WORD - 1) / WORD : 0);
}

// A record's size as a stream's room is counted.
constexpr std::uint64_t record_bytes(std::uint64_t args, std::optional<std::uint64_t> bytes) noexcept {
    return record_words(args, bytes) * WORD;
}

class Stream {
  public:
    static constexpr std::uint32_t END = 0;

    Stream(Memory mem, std::uint32_t at, std::uint32_t words, std::uint32_t head = 1) noexcept;

    std::uint32_t head_at() const noexcept { return at_; }

    Memory mem() const noexcept { return mem_; }

    std::uint32_t left() const noexcept { return (words_ - head_ - len_) * WORD; }

    // Whether records of `bytes` (`record_bytes`, gpu/encode's `call::*_bytes`)
    // fit with `leave` bytes left after them.
    bool fits(std::uint64_t bytes, std::uint32_t leave) const noexcept { return std::uint64_t{left()} >= bytes + leave; }

    // Answers where the `n` words begin. Past the range is a bug, reported at
    // the caller.
    std::uint32_t open(std::uint32_t op, std::uint32_t n, std::source_location at = std::source_location::current()) noexcept;

    // A record of `n` words for its writer to fill; none where an open
    // transaction no longer fits it, which drops the transaction.
    std::optional<std::uint32_t> reserve(std::uint32_t op, std::uint32_t n, std::source_location at = std::source_location::current()) noexcept;

    void record(std::uint32_t op, std::span<const std::uint32_t> args, std::span<const std::uint32_t> tail = {},
                std::optional<std::span<const std::uint8_t>> bytes = std::nullopt, std::source_location at = std::source_location::current()) noexcept;

    // A record laid past the last the turn has recorded and never published,
    // for a hostcall: its offset in the memory, or none where it does not
    // fit. The next record written takes its words.
    std::optional<std::uint32_t> past(std::uint32_t op, std::span<const std::uint32_t> args,
                                      std::optional<std::span<const std::uint8_t>> bytes = std::nullopt) noexcept;

    void publish() noexcept;

    // The records from here to `commit` are one: one that does not fit drops
    // them all, and those after it, where outside a transaction it is a bug.
    void begin() noexcept { mark_ = {Mark::open, len_}; }

    // False where the transaction did not fit: the stream is as at `begin`.
    bool commit() noexcept {
        bool fit = mark_.state != Mark::spilled;
        if (!fit) len_ = mark_.at;
        mark_ = {};
        return fit;
    }

  private:
    // A record's words past its op and count, from `w`: `args`, `tail`, and
    // `bytes` as their count then words.
    void fill(std::uint32_t w, std::span<const std::uint32_t> args, std::span<const std::uint32_t> tail,
              std::optional<std::span<const std::uint8_t>> bytes) noexcept;

    Memory mem_;
    std::uint32_t at_;
    std::uint32_t words_;
    std::uint32_t head_;
    std::uint32_t len_ = 0;
    // Whether a transaction is open, where it began, and whether a record
    // that did not fit has dropped it.
    struct Mark {
        enum State { off, open, spilled } state = off;
        std::uint32_t at = 0;
    };
    Mark mark_;
};

} // namespace xpute

// cpp/xpute/guest/ipc/sys.spec.hpp
//
// GENERATED from spec/ipc/sys.json
//
// A guest's hostcalls, its calls into its host: its files, the report that stopped it and
// the program's own calls, as records in a stream (ipc/stream.rs) the host runs once each
// turn has returned and before the next begins. A record may name memory the guest holds:
// the host reads or writes it in that gap, and the guest keeps it that long. The toc
// says where the stream is (key SYS).
//
// Files borrow Plan 9's file namespace, in POSIX's words. The guest issues the descriptor,
// a generational handle, and names a file by a root the program numbers and a path below
// it; the program binds each root to where it keeps things, and a path that leaves it is
// refused. OPENAT is the one call that waits: the host answers it with the command
// SYS_OPENED (abi/cmd.json) once the file can be read whole. READ and WRITE are done in
// the gap; CLOSE ends the descriptor, opened or not, and nothing is said of it after.
//
// A program's own calls go in the same stream, numbered with the PROGRAM bit, and a host
// that is not the program's refuses them.
//
// A program's hostcall the host answers from what it already holds may be run now instead:
// the same record, laid past the last the turn has recorded and never published, its offset
// handed to the host's `hostcall` entry, which runs it before it returns and answers
// with a value or -errno. It writes only what the record names, and ENOSYS where the call
// is not the program's or no program answers. xpute's own calls go in the stream alone.

namespace xpute::sys {

// Words before the first record: END, then a word kept at 0.
inline constexpr std::uint32_t HEAD = 2u;

// A record's call, then its arguments.
namespace op {

// fd, root, flags, then the path's bytes: the file at `path` below `root` opened as `fd`,
// answered by SYS_OPENED with its size or -errno.
inline constexpr std::uint32_t OPENAT = 1u;

// fd, at, len: the opened file's first `len` bytes copied to `at`.
inline constexpr std::uint32_t READ = 2u;

// fd, at, len: the `len` bytes at `at` added to what the file opened for writing holds.
inline constexpr std::uint32_t WRITE = 3u;

// fd: the descriptor ended. A file opened for writing is replaced by what was written; an
// open not yet answered is canceled, and never answered.
inline constexpr std::uint32_t CLOSE = 4u;

// errno, line, column, a (low, high word), b (low, high word), then the file's bytes:
// the broken invariant the guest reports before it stops (xpute-kit status/bug.rs).
inline constexpr std::uint32_t ABORT = 5u;

// The bit every call a program numbers for itself carries.
inline constexpr std::uint32_t PROGRAM = 2147483648u;
} // namespace op

// How many words of arguments each call's record takes, as its `op` lists them; OPENAT's
// and ABORT's end in bytes after them.
namespace args {

inline constexpr std::uint32_t OPENAT = 3u;

inline constexpr std::uint32_t READ = 3u;

inline constexpr std::uint32_t WRITE = 3u;

inline constexpr std::uint32_t CLOSE = 1u;

inline constexpr std::uint32_t ABORT = 7u;
} // namespace args

// Where ABORT's two values lie among its arguments, each low word then high.
namespace abort {

// a.
inline constexpr std::uint32_t A = 3u;

// b.
inline constexpr std::uint32_t B = 5u;
} // namespace abort

// How OPENAT opens.
namespace flag {

// To read, whole.
inline constexpr std::uint32_t O_RDONLY = 0u;

// To write: created where it is not, and replaced whole at CLOSE (O_CREAT | O_TRUNC).
inline constexpr std::uint32_t O_WRONLY = 1u;
} // namespace flag

} // namespace xpute::sys

// cpp/xpute/guest/ipc/sys.hpp

// A guest's hostcalls, recorded in a stream the host runs after the turn
// returns; what a call names in the guest's memory is kept until the next
// rise. A descriptor is a handle of the guest's own table, so a late answer to
// a closed one names nothing. A program's call made as a `hostcall` is
// answered before it returns.

namespace xpute {

using Fd = std::uint32_t;

// Each call's arguments, which its record takes as an array of this length.
inline constexpr std::size_t OPENAT_ARGS = sys::args::OPENAT;
inline constexpr std::size_t READ_ARGS = sys::args::READ;
inline constexpr std::size_t WRITE_ARGS = sys::args::WRITE;
inline constexpr std::size_t CLOSE_ARGS = sys::args::CLOSE;
inline constexpr std::size_t ABORT_ARGS = sys::args::ABORT;

// The most one descriptor records in a turn, its name `path` bytes long: its
// open, a read or a write, and its close, twice where it is stopped.
constexpr std::uint64_t fd_turn_bytes(std::uint64_t path) noexcept {
    std::uint64_t moved = READ_ARGS > WRITE_ARGS ? READ_ARGS : WRITE_ARGS;
    return record_bytes(OPENAT_ARGS, path) + record_bytes(moved, std::nullopt) + 2 * record_bytes(CLOSE_ARGS, std::nullopt);
}

class Sys {
  public:
    Sys(Memory mem, std::uint32_t at, std::uint32_t words) noexcept : mem_(mem), stream_(mem, at, words, sys::HEAD) {}

    void rise() noexcept { held_.clear(); }

    void fall() noexcept { stream_.publish(); }

    // Answered by SYS_OPENED on a later turn.
    void openat(Fd fd, std::uint32_t root, std::uint32_t flags, std::string_view path) noexcept;

    // `dst` is filled from the next turn on and must be kept until then.
    void read(Fd fd, std::span<std::uint8_t> dst) noexcept;

    void write(Fd fd, std::vector<std::uint8_t> bytes) noexcept;

    // A write is committed, an open not yet answered cancelled.
    void close(Fd fd) noexcept;

    // `op` carries the PROGRAM bit.
    void call(std::uint32_t op, std::span<const std::uint32_t> args, std::span<const std::uint8_t> bytes) noexcept;

    // `call` run by the host before this returns, from what it already holds:
    // its value or its errno, ENOSPC where the record does not fit past the
    // turn's. What `args` name it writes during the call.
    Result<std::uint32_t> call_now(std::uint32_t op, std::span<const std::uint32_t> args, std::span<const std::uint8_t> bytes) noexcept;

    // `call` where it fits with `reserve` bytes left after it; false, and nothing
    // recorded, where it does not, for the caller to ask again a later turn.
    bool try_call(std::uint32_t op, std::span<const std::uint32_t> args, std::span<const std::uint8_t> bytes, std::uint32_t reserve) noexcept;

    // The one call of the turn, published at once; what was recorded before it is
    // dropped.
    void abort(Errno errnum, const std::source_location &at, std::uint64_t a, std::uint64_t b) noexcept;

  private:
    Memory mem_;
    Stream stream_;
    std::vector<std::vector<std::uint8_t>> held_;
};

} // namespace xpute

// cpp/xpute/guest/ipc/file.hpp

// A guest's open files. A descriptor is a table handle, so an answer to one
// already closed reaches no one, which is the whole of a stale answer's handling.

namespace xpute {

// At most `N - 1` open at once; one more is a bug.
template <class O, std::size_t N> class Files {
  public:
    Fd open(Sys &sys, std::uint32_t root, std::string_view path, O owner) {
        Fd fd = take(std::move(owner));
        sys.openat(fd, root, sys::flag::O_RDONLY, path);
        return fd;
    }

    // Answered or not.
    std::optional<O> close(Sys &sys, Fd fd) {
        sys.close(fd);
        return held_.remove(fd);
    }

    // Opened, written and closed in one turn, so no answer comes.
    void write_whole(Sys &sys, std::uint32_t root, std::string_view path, std::vector<std::uint8_t> bytes, O owner) {
        Fd fd = take(std::move(owner));
        sys.openat(fd, root, sys::flag::O_WRONLY, path);
        sys.write(fd, std::move(bytes));
        close(sys, fd);
    }

    // None for a descriptor already closed.
    // The owner of the open `opened` completes, or none where its descriptor was
    // closed since; a descriptor whose open failed is closed here.
    std::optional<O> opened(Sys &sys, const abi::SysOpened &opened) {
        const O *held = held_.get(opened.fd);
        if (held == nullptr) return std::nullopt;
        O owner = *held;
        if (opened.res < 0) close(sys, opened.fd);
        return owner;
    }

    const O *owner(Fd fd) const noexcept { return held_.get(fd); }

    template <class Which> std::optional<Fd> held_for(Which &&which) {
        std::optional<Fd> found;
        held_.each([&](std::uint32_t fd, O &o) {
            if (!found && which(o)) found = fd;
        });
        return found;
    }

  private:
    Fd take(O owner) noexcept {
        std::optional<Fd> fd = held_.insert(std::move(owner));
        ensure(fd.has_value(), Errno::enospc, N);
        return *fd;
    }

    abi::Table<O, N> held_;
};

} // namespace xpute

// cpp/xpute/guest/ipc/toc.spec.hpp
//
// GENERATED from spec/ipc/toc.json
//
// The toc: where a host finds what its guest laid out in the memory. The guest's
// first turn writes it, and the host reads it once, at the one offset it knows outside
// the memory (xpute-guest ipc/toc.rs, and the contract's rule 0 in
// ARCHITECTURE.md).
//
// Its words: MAGIC, VERSION, how many entries follow, and a word kept at 0; then that
// many entries, a key and a value each, no key twice. A reader looks up the keys it
// knows and passes over the rest, the way a process reads its ELF auxiliary vector, so
// a toc that grows an entry is still read by a host that predates it.

namespace xpute::toc {

// "XPUT" as four bytes read little-endian: what tells a toc from memory nothing wrote.
inline constexpr std::uint32_t MAGIC = 1414877272u;

// Raised when a key comes to mean something else. A new key does not raise it.
inline constexpr std::uint32_t VERSION = 1u;

// Words before the first entry.
inline constexpr std::uint32_t HEAD = 4u;

// What an entry's value is. 3 to 5 are retired and given to nothing else: they were the
// numbers of a host's scheduling policy, which the program now injects when the host
// boots (the host's sched/policy.ts).
namespace key {

// Where the submission ring's words begin.
inline constexpr std::uint32_t SQ = 1u;

// Where the completion ring's words begin.
inline constexpr std::uint32_t CQ = 2u;

// Where the hostcall stream's words begin (ipc/sys.json).
inline constexpr std::uint32_t SYS = 6u;

// The bit every key a program numbers for itself carries: xpute gives none of
// them a meaning, and a host that is not the program's passes over them.
inline constexpr std::uint32_t PROGRAM = 2147483648u;
} // namespace key

} // namespace xpute::toc

// cpp/xpute/guest/ipc/toc.hpp

// The toc a guest's first turn writes, where its host finds what it laid
// out: a head, then key-value entries, no key twice.

namespace xpute {

struct Entry {
    std::uint32_t key;
    std::uint32_t val;
};

constexpr std::uint32_t toc_words(std::uint32_t entries) noexcept {
    return toc::HEAD + 2 * entries;
}

// The toc at `at`, `words` long. A key may appear once, so no two
// readers can pick different values.
void write_toc(Memory mem, std::uint32_t at, std::uint32_t words, std::span<const Entry> entries) noexcept;

} // namespace xpute

// cpp/xpute/guest/mem/heap.hpp

// Blocks out of the guest's heap, named by offsets in its memory
// (`mem::core`). The allocator is installed by the guest (`set_heap`), and
// `operator new` points at it too, so nothing allocates from memory the guest
// was not given.

namespace xpute {

// An offset and a size rather than a pointer, because a host names a block by
// its offset, and giving a block back needs the size.
struct Block {
    std::uint32_t at;
    std::uint32_t bytes;
    bool operator==(const Block &) const = default;
};

inline constexpr std::size_t BLOCK_ALIGN = 8;

// The same size and alignment are told back on free.
struct Allocator {
    void *(*alloc)(std::size_t bytes, std::size_t align) noexcept;
    void (*free)(void *at, std::size_t bytes, std::size_t align) noexcept;
};

void set_heap(Allocator heap) noexcept;

// One reading of a heap, so compared fields are from one moment.
struct HeapStat {
    std::size_t range = 0;
    // A high-water mark, not a holding.
    std::size_t used = 0;
    std::size_t live = 0;
    std::size_t run = 0;
    std::size_t slot = 0;
    // Past the tree's current root the range is untouched rather than listed,
    // so this is not the free space.
    std::size_t largest_free = 0;
    // Moves when a page is released, which is when a request of a page or
    // more refused before might now be served; a slot returned to a run still
    // in use does not move it (`SlabMalloc::releases`).
    std::uint32_t releases = 0;
    std::uint32_t refusals = 0;
    std::uint32_t requests = 0;

    bool operator==(const HeapStat &) const = default;
};

// Reads the slab whose calls the guest installed as its heap (`set_heap`).
template <Range R> HeapStat heap_stat(const SlabMalloc<R> &a) noexcept {
    return HeapStat{
        .range = a.range_bytes(),
        .used = a.used(),
        .live = a.live(),
        .run = a.run_live(),
        .slot = a.slot_live(),
        .largest_free = a.largest_free(),
        .releases = a.releases(),
        .refusals = a.refusals(),
        .requests = static_cast<std::uint32_t>(a.pages().allocs()),
    };
}

Allocator heap() noexcept;

// None for no room, no allocator, or zero bytes.
inline std::optional<Block> alloc(std::uint32_t bytes) noexcept {
    Allocator h = heap();
    if (bytes == 0 || h.alloc == nullptr) return std::nullopt;
    void *p = h.alloc(bytes, BLOCK_ALIGN);
    if (p == nullptr) return std::nullopt;
    return Block{mem::off_of(p), bytes};
}

inline void free(Block block) noexcept {
    heap().free(mem::at(block.at), block.bytes, BLOCK_ALIGN);
}

} // namespace xpute

// cpp/xpute/guest/mem/section.hpp

// Ranges laid end to end in a linear memory, as a linker script places
// sections at `. = ALIGN(n)` boundaries.

namespace xpute {

// The unit sections align to and the least block a guest's heap hands out: a
// 32-bit machine's page, borrowed, not the host's.
inline constexpr std::uint32_t PAGE_BYTES = 4096;

constexpr std::uint32_t align_up(std::uint32_t n, std::uint32_t unit) noexcept {
    return (n + unit - 1) / unit * unit;
}

// Includes the last one's own rounding, so this is where whatever follows starts.
constexpr std::uint32_t span_of(std::span<const std::uint32_t> bytes, std::uint32_t unit) noexcept {
    std::uint32_t span = 0;
    for (std::uint32_t s : bytes) span += align_up(s, unit);
    return span;
}

constexpr std::uint32_t nth_at(std::uint32_t base, std::span<const std::uint32_t> bytes, std::uint32_t unit, std::size_t n) noexcept {
    return base + span_of(bytes.first(n), unit);
}

struct Section {
    std::uint32_t off = 0;
    std::uint32_t bytes = 0;

    bool operator==(const Section &) const = default;

    constexpr std::uint32_t end() const noexcept { return off + bytes; }

    // None where they would run past the end. The one way into a section, so
    // nothing written through it can land in its neighbor.
    constexpr std::optional<std::uint32_t> at(std::uint32_t off, std::uint32_t n) const noexcept {
        std::uint64_t past = std::uint64_t{off} + n;
        if (past > bytes) return std::nullopt;
        return this->off + off;
    }

    // The `n`th of ranges of `bytes` each; what is laid must fit the section.
    constexpr Section nth(std::span<const std::uint32_t> bytes, std::uint32_t unit, std::size_t n) const noexcept {
        if (span_of(bytes, unit) > this->bytes) bug(Errno::einval, span_of(bytes, unit), this->bytes);
        return Section{nth_at(off, bytes, unit, n), bytes[n]};
    }
};

} // namespace xpute

// cpp/xpute/guest/sched/wake.hpp

// What a falling edge answers besides a delay. When the next turn runs, and
// how much it gets, is the host's scheduling policy's.

namespace xpute {

// The guest waits on something only the host can bring, an input or an arrival.
inline constexpr double NO_WAKE = -1.0;

} // namespace xpute

// cpp/xpute/guest/sched/task.hpp

// Coroutine tasks and the executor they run on, installed once like a global
// allocator. A task's frame comes from the guest's heap; one that finds no
// room makes no task. An executor may ignore priority, so nothing may rest on
// the order tasks are polled in.

#include <coroutine>
#include <cstdlib>
#include <new>

namespace xpute {

// Lower is sooner.
using Priority = std::uint8_t;

class Executor {
  public:
    virtual bool spawn(std::coroutine_handle<> task, Priority priority) noexcept = 0;
    virtual void wake(std::coroutine_handle<> task) noexcept = 0;
    // Never from inside a task.
    virtual void poll() noexcept = 0;
    virtual bool pending() const noexcept = 0;

  protected:
    ~Executor() = default;
};

struct Waker {
    std::coroutine_handle<> task;
    Executor *executor = nullptr;

    void wake() const noexcept {
        if (executor != nullptr) executor->wake(task);
    }
};

template <class T = void> class Task;

namespace task_detail {

// A spawned task has no continuation; its frame is destroyed when it ends.
struct PromiseBase {
    Executor *executor = nullptr;
    std::coroutine_handle<> continuation;

    static void *operator new(std::size_t n) noexcept { return ::operator new(n, std::nothrow); }
    static void operator delete(void *p, std::size_t n) noexcept { ::operator delete(p, n); }

    std::suspend_always initial_suspend() const noexcept { return {}; }

    struct Final {
        bool await_ready() const noexcept { return false; }
        template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept {
            std::coroutine_handle<> next = h.promise().continuation;
            if (next) return next;
            h.destroy();
            return std::noop_coroutine();
        }
        void await_resume() const noexcept {}
    };
    Final final_suspend() const noexcept { return {}; }

    // Exceptions are off; nothing can arrive here.
    void unhandled_exception() const noexcept { std::abort(); }
};

template <class T> struct Promise : PromiseBase {
    std::optional<T> val;
    Task<T> get_return_object() noexcept;
    static Task<T> get_return_object_on_allocation_failure() noexcept;
    template <class U> void return_value(U &&v) noexcept { val.emplace(std::forward<U>(v)); }
};

template <> struct Promise<void> : PromiseBase {
    Task<void> get_return_object() noexcept;
    static Task<void> get_return_object_on_allocation_failure() noexcept;
    void return_void() const noexcept {}
};

} // namespace task_detail

template <class T> class [[nodiscard]] Task {
  public:
    using promise_type = task_detail::Promise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle h) noexcept : h_(h) {}
    Task(Task &&o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task &operator=(Task &&o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;
    ~Task() {
        if (h_) h_.destroy();
    }

    explicit operator bool() const noexcept { return bool(h_); }

    Handle release() noexcept { return std::exchange(h_, {}); }

    struct Awaiter {
        Handle h;
        bool await_ready() const noexcept { return false; }
        template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) const noexcept {
            h.promise().continuation = parent;
            h.promise().executor = parent.promise().executor;
            return h;
        }
        T await_resume() const noexcept {
            if constexpr (!std::is_void_v<T>) return std::move(*h.promise().val);
        }
    };

    Awaiter operator co_await() noexcept { return Awaiter{h_}; }

  private:
    Handle h_;
};

namespace task_detail {

template <class T> Task<T> Promise<T>::get_return_object() noexcept {
    return Task<T>(std::coroutine_handle<Promise<T>>::from_promise(*this));
}
template <class T> Task<T> Promise<T>::get_return_object_on_allocation_failure() noexcept {
    return Task<T>();
}
inline Task<void> Promise<void>::get_return_object() noexcept {
    return Task<void>(std::coroutine_handle<Promise<void>>::from_promise(*this));
}
inline Task<void> Promise<void>::get_return_object_on_allocation_failure() noexcept {
    return Task<void>();
}

} // namespace task_detail

template <class P> Waker waker_of(std::coroutine_handle<P> h) noexcept {
    return Waker{h, h.promise().executor};
}

void set_executor(Executor &executor) noexcept;

Executor *executor() noexcept;

bool spawn(Task<> task, Priority priority) noexcept;

void poll() noexcept;

bool pending() noexcept;

} // namespace xpute

// cpp/xpute/guest/sched/edge.hpp

// A guest's side of the turn: the quota as a deadline, the tasks parked until
// the next rising edge, and what the falling edge asks for.

namespace xpute {

template <std::size_t N> class Edge {
  public:
    void rise(double quota_ms) noexcept {
        edge_at_ = now();
        quota_ms_ = quota_ms;
        until_ = edge_at_ + quota_ms;
        std::size_t n = std::exchange(parked_len_, 0);
        for (std::size_t i = 0; i < n; i++) parked_[i].wake();
    }

    // Reading clears it.
    double fall() noexcept {
        bool asked = std::exchange(asked_, false) || parked_len_ > 0;
        double at = std::exchange(wake_at_, std::numeric_limits<double>::infinity());
        if (asked) return 0.0;
        if (std::isfinite(at)) return std::max(at - now(), 0.0);
        return NO_WAKE;
    }

    double edge_at() const noexcept { return edge_at_; }
    double quota_ms() const noexcept { return quota_ms_; }

    bool spent() const noexcept { return now() >= until_; }

    // The tasks yield no sooner than `at`, whatever the quota, so what overran
    // it does not take the tasks' turn too.
    void extend_to(double at) noexcept { until_ = std::max(until_, at); }

    void ask_next() noexcept { asked_ = true; }

    // The earliest asked wins.
    void wake_at(double at) noexcept { wake_at_ = std::min(wake_at_, at); }

    void park(Waker waker) noexcept {
        ensure(parked_len_ < N, Errno::enospc, N);
        parked_[parked_len_++] = waker;
    }

  private:
    double edge_at_ = 0;
    double quota_ms_ = 0;
    // When the tasks yield: the quota's end, or later (`extend_to`).
    double until_ = 0;
    std::array<Waker, N> parked_{};
    std::size_t parked_len_ = 0;
    bool asked_ = false;
    double wake_at_ = std::numeric_limits<double>::infinity();
};

template <std::size_t N> struct ParkAwaiter {
    Edge<N> &edge;
    bool ready;
    bool await_ready() const noexcept { return ready; }
    template <class P> void await_suspend(std::coroutine_handle<P> h) const noexcept { edge.park(waker_of(h)); }
    void await_resume() const noexcept {}
};

template <std::size_t N> ParkAwaiter<N> yield_if_spent(Edge<N> &edge) noexcept {
    return ParkAwaiter<N>{edge, !edge.spent()};
}

template <std::size_t N> ParkAwaiter<N> next_turn(Edge<N> &edge) noexcept {
    return ParkAwaiter<N>{edge, false};
}

// A wait for something to arrive that asks for no turn meanwhile, so an idle
// guest is left alone. A notice that comes before the wait is kept for it.
class Notify {
  public:
    void notify() noexcept {
        notified_ = true;
        if (std::optional<Waker> w = std::exchange(waker_, std::nullopt)) w->wake();
    }

    struct Awaiter {
        Notify &n;
        bool await_ready() const noexcept { return std::exchange(n.notified_, false); }
        template <class P> void await_suspend(std::coroutine_handle<P> h) const noexcept { n.waker_ = waker_of(h); }
        void await_resume() const noexcept { n.notified_ = false; }
    };

  private:
    std::optional<Waker> waker_;
    bool notified_ = false;
};

inline Notify::Awaiter notified(Notify &notify) noexcept {
    return Notify::Awaiter{notify};
}

} // namespace xpute

// cpp/xpute/guest/sched/pass.hpp

// What is left of a turn's budget, spent on ordered lists of steps (`run`).
// Neither the budget nor a step cap applies until one step has done work, so
// one expensive item can never block everything behind it.

namespace xpute {

struct PassStat {
    std::uint32_t ran = 0;
    // With the steps that did no work, and the one that tripped the stop.
    std::uint32_t visited = 0;
    double elapsed_ms = 0;
    bool stopped_early = false;

    bool operator==(const PassStat &) const = default;
};

struct Pass {
    // The rising edge, so what the turn spent before comes off.
    double t0 = 0;
    double total_ms = 0;
    // Held back for the phases after the current one.
    double reserve_ms = 0;
    // Past what is left, never waived: a step can cost time and still report
    // no work.
    double hard_slack_ms = 0;

    Pass(double total, double rise, double slack) noexcept : t0(rise), total_ms(total), hard_slack_ms(slack) {}

    double remaining() const noexcept { return std::max(t0 + total_ms - reserve_ms - now(), 0.0); }

    // `step` answers whether it did work; only work counts to `max_steps`.
    template <class Items, class Step> PassStat run(Items &&items, Step &&step, std::optional<std::uint32_t> max_steps = std::nullopt) const {
        double start = now();
        double deadline = start + remaining();
        double hard_deadline = deadline + hard_slack_ms;
        std::uint32_t cap = max_steps.value_or(UINT32_MAX);
        PassStat stat;
        for (auto &&item : items) {
            double at = now();
            // Before the waiver below, but the first item is always tried.
            if ((stat.visited > 0 && at >= hard_deadline) || (stat.ran > 0 && (stat.ran >= cap || at >= deadline))) {
                stat.stopped_early = true;
                break;
            }
            stat.visited++;
            if (step(item)) stat.ran++;
        }
        stat.elapsed_ms = now() - start;
        return stat;
    }
};

} // namespace xpute

// cpp/xpute/guest/sched/tick.hpp

// A guest's cooperative scheduler: one tick a turn, its phases in the order
// the program numbers them, against one budget counted from the rising edge.
// While a phase runs, the share of the budget its policy holds back for the
// phases after it is not its to take, so they are not starved while it keeps
// busy. What the phases are, and what each holds back, are the program's. A
// step keeps its own state.

namespace xpute {

// A phase, by its place in the program's order.
using Phase = std::uint8_t;

// The most phases a tick runs.
inline constexpr std::size_t PHASES_MAX = 8;

struct TickPolicy {
    // Hard ceiling over what is left, so a step that costs time and reports
    // nothing done cannot walk the whole list.
    double hard_slack_ms;
    // The program's phases, in the order they run: each the share of the
    // budget held back from it for the phases after it, 0 for none.
    std::span<const double> held_back;
};

inline constexpr std::size_t OFFS_MAX = 8;

class TickContext {
  public:
    double delta;
    Pass pass;
    std::array<double, PHASES_MAX> phase_ms{};

    // The guest is strobed only when asked, so a step that animates asks here.
    void invalidate() noexcept { invalidated_ = true; }

    void off_tick(std::uint32_t id) noexcept {
        ensure(off_len_ < OFFS_MAX, Errno::enospc, OFFS_MAX);
        offs_[off_len_++] = id;
    }

  private:
    template <std::size_t> friend class Tick;
    TickContext(double d, Pass p) noexcept : delta(d), pass(p) {}

    bool invalidated_ = false;
    std::array<std::uint32_t, OFFS_MAX> offs_{};
    std::size_t off_len_ = 0;
};

using TickStep = void (*)(TickContext &);

// Two registrations under one name in one phase add into one row.
struct StepStat {
    Phase phase = 0;
    std::uint32_t name = 0;
    double ms = 0;
    std::uint32_t cnt = 0;
    double max_ms = 0;

    bool operator==(const StepStat &) const = default;
};

struct TickReport {
    double budget_ms = 0;
    std::array<double, PHASES_MAX> phase_ms{};
    double total_ms = 0;
    bool invalidated = false;

    bool operator==(const TickReport &) const = default;
};

template <std::size_t N> class Tick {
  public:
    explicit Tick(TickPolicy policy) noexcept : policy_(policy) {
        ensure(policy.held_back.size() <= PHASES_MAX, Errno::einval, policy.held_back.size());
    }

    std::uint32_t on_tick(Phase phase, TickStep step, std::uint32_t name) noexcept {
        ensure(len_ < N, Errno::enospc, N);
        ensure(phase < policy_.held_back.size(), Errno::einval, phase);
        std::uint32_t id = next_id_++;
        registry_[len_++] = Registered{id, phase, step, name};
        return id;
    }

    // Inside a tick, a step asks its context instead.
    void off_tick(std::uint32_t id) noexcept { (void)remove(id); }

    void set_step_timing(bool on) noexcept { step_timing_ = on; }

    std::span<const StepStat> step_stats() const noexcept { return {stats_.data(), stats_len_}; }
    void step_stats_reset() noexcept { stats_len_ = 0; }

    TickReport last() const noexcept { return last_report_; }

    TickReport run(double delta, double quota_ms, double t0) noexcept {
        double start = now();
        TickContext ctx(delta, Pass(quota_ms, t0, policy_.hard_slack_ms));
        for (std::size_t phase = 0; phase < policy_.held_back.size(); phase++) {
            ctx.pass.reserve_ms = ctx.pass.total_ms * policy_.held_back[phase];
            run_phase(static_cast<Phase>(phase), ctx);
        }
        ctx.pass.reserve_ms = 0;
        last_report_ = TickReport{ctx.pass.total_ms, ctx.phase_ms, now() - start, ctx.invalidated_};
        return last_report_;
    }

  private:
    struct Registered {
        std::uint32_t id;
        Phase phase;
        TickStep step;
        std::uint32_t name;
    };

    std::optional<std::size_t> remove(std::uint32_t id) noexcept {
        for (std::size_t at = 0; at < len_; at++) {
            if (registry_[at].id != id) continue;
            std::copy(registry_.begin() + at + 1, registry_.begin() + len_, registry_.begin() + at);
            len_--;
            return at;
        }
        return std::nullopt;
    }

    // A table with no row left drops the sample.
    void record(Phase phase, std::uint32_t name, double ms) noexcept {
        std::size_t at = 0;
        while (at < stats_len_ && !(stats_[at].phase == phase && stats_[at].name == name)) at++;
        if (at == stats_len_) {
            if (stats_len_ == N) return;
            stats_[stats_len_++] = StepStat{.phase = phase, .name = name};
        }
        StepStat &s = stats_[at];
        s.ms += ms;
        s.cnt++;
        s.max_ms = std::max(s.max_ms, ms);
    }

    void run_phase(Phase phase, TickContext &ctx) noexcept {
        double start = now();
        // An index, not a snapshot: a step may unregister itself or a sibling.
        std::size_t i = 0;
        while (i < len_) {
            Registered r = registry_[i++];
            if (r.phase != phase) continue;
            if (step_timing_) {
                double t = now();
                r.step(ctx);
                record(r.phase, r.name, now() - t);
            } else {
                r.step(ctx);
            }
            for (std::size_t k = 0; k < ctx.off_len_; k++) {
                std::optional<std::size_t> at = remove(ctx.offs_[k]);
                if (at && *at < i) i--;
            }
            ctx.off_len_ = 0;
        }
        ctx.phase_ms[static_cast<std::size_t>(phase)] = now() - start;
    }

    TickPolicy policy_;
    std::array<Registered, N> registry_{};
    std::size_t len_ = 0;
    std::uint32_t next_id_ = 1;
    std::array<StepStat, N> stats_{};
    std::size_t stats_len_ = 0;
    bool step_timing_ = false;
    TickReport last_report_{};
};

} // namespace xpute

// cpp/xpute/guest/wasm.hpp

// The guest built as one WebAssembly module: what it reads of the module
// itself, what it asks of its host (`hostcall`), and its entry, `run`, which the floor names and the program
// defines. Linked as every module guest is (cpp/Makefile `WASM_LINK`).

#ifdef __wasm__

extern "C" {

// Defined by wasm-ld. The end of .rodata, .data and .bss, where a guest may
// begin placing its own ranges: not __heap_base, which a C runtime's sbrk
// starts at, since a guest's heap is where the guest says.
extern const std::uint8_t __data_end;
// The top of the stack: -z stack-size past its start, which --stack-first
// puts at the bottom of the memory.
extern const std::uint8_t __stack_high;

// Imports of `env`, the module the host instantiates with.
__attribute__((import_module("env"), import_name("now"))) double xpute_wasm_now(void);
__attribute__((import_module("env"), import_name("hostcall"))) std::int32_t xpute_wasm_hostcall(std::uint32_t at);

// The entry: a turn of quota_ms, and when the guest wants the next, in ms
// (0 for the next turn, NO_WAKE in xpute/guest/sched/wake.hpp for none).
double run(double quota_ms);
}

namespace xpute::wasm {

// A WebAssembly page, the unit a memory is declared and grown in.
inline constexpr std::uint64_t WASM_PAGE_BYTES = 1 << 16;

inline std::uint32_t data_end() noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&__data_end));
}

// Its size, with the stack first.
inline std::uint32_t stack_high() noexcept {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&__stack_high));
}

// The bytes of memory the host has grown the module to.
inline std::uint64_t given() noexcept {
    return std::uint64_t{__builtin_wasm_memory_size(0)} * WASM_PAGE_BYTES;
}

inline double now() noexcept {
    return xpute_wasm_now();
}

// What the record names, the host reads and writes before it returns.
inline std::int32_t hostcall(std::uint32_t at) noexcept {
    return xpute_wasm_hostcall(at);
}

} // namespace xpute::wasm

#endif

// cpp/xpute/gpu/api.spec.hpp
//
// GENERATED from spec/gpu/api.json
//
// The calls a guest makes to a GPU it cannot reach, as records of a stream (ipc/stream):
// WebGPU's vocabulary, its descriptors written out as words, for any program. A record is
// an op, a count and the op's words in the order each op's doc gives; bytes go as a count
// and padded words, as the stream lays them. Objects are the guest's handles (abi/handle).
// An optional field holds NONE where unset. Render and compute sit side by side.
// A transaction (ipc/stream) that does not commit drops its records and gives back the handles they took, so the host never hears of the objects: the encoder's begin and commit open and close both.

namespace xpute::gpu {

// An optional field left unset, an index or a handle absent.
inline constexpr std::uint32_t NONE = 4294967295u;

// Or-ed into a ticket where a completion word is written (COPY_EXTERNAL_IMAGE, MAP_ASYNC, QUEUE_DONE).
inline constexpr std::uint32_t DONE = 2147483648u;

// In CONFIGURE's format: the format the host's surface prefers.
inline constexpr std::uint32_t SURFACE_PREFERRED = 255u;

// A bind group layout's entry (BIND_GROUP_LAYOUT).
inline constexpr std::uint32_t BINDING_ENTRY_WORDS = 6u;

// A bind group's entry (BIND_GROUP).
inline constexpr std::uint32_t RESOURCE_ENTRY_WORDS = 5u;

// The ops, numbered from 1: 0 ends a stream.
namespace op {

// id, size, usage (BUFFER_USAGE), mapped at creation (0 or 1).
inline constexpr std::uint32_t BUFFER = 1u;

// id, width, height, depth or layers, mip levels, samples, dimension (TEXTURE_DIMENSION), format (TEXTURE_FORMAT), usage (TEXTURE_USAGE), view formats' count, that many formats.
inline constexpr std::uint32_t TEXTURE = 2u;

// id, texture, format or NONE, dimension (TEXTURE_VIEW_DIMENSION) or NONE, aspect (TEXTURE_ASPECT), base mip, mips or NONE, base layer, layers or NONE.
inline constexpr std::uint32_t VIEW = 3u;

// id, address u, v, w (ADDRESS_MODE), mag and min (FILTER_MODE), mipmap (MIPMAP_FILTER_MODE), lod min and max (f32 bits), compare (COMPARE_FUNCTION) or NONE, max anisotropy.
inline constexpr std::uint32_t SAMPLER = 4u;

// id, the WGSL source as bytes.
inline constexpr std::uint32_t SHADER_MODULE = 5u;

// id, entries' count, then each entry's BINDING_ENTRY_WORDS: binding, visibility (SHADER_STAGE), kind (BINDING_KIND) and three words the kind reads — a buffer its type (BUFFER_BINDING_TYPE), dynamic offset (0 or 1), min binding size; a sampler its type (SAMPLER_BINDING_TYPE); a texture its sample type (TEXTURE_SAMPLE_TYPE), view dimension, multisampled (0 or 1); a storage texture its access (STORAGE_TEXTURE_ACCESS), format, view dimension.
inline constexpr std::uint32_t BIND_GROUP_LAYOUT = 6u;

// id, bind group layouts' count, that many ids.
inline constexpr std::uint32_t PIPELINE_LAYOUT = 7u;

// id, layout, entries' count, then each entry's RESOURCE_ENTRY_WORDS: binding, kind (RESOURCE_KIND), the object's id, offset, size or NONE (a buffer's; 0 for the others).
inline constexpr std::uint32_t BIND_GROUP = 8u;

// id, layout or NONE for an automatic one; vertex: module, entry (bytes), buffers' count, each: stride, step (VERTEX_STEP_MODE), attributes' count, each: format (VERTEX_FORMAT), offset, location; primitive: topology (PRIMITIVE_TOPOLOGY), strip index format (INDEX_FORMAT) or NONE, front face (FRONT_FACE), cull (CULL_MODE), unclipped depth (0 or 1); depth-stencil: format or NONE for none, the rest present either way: depth write (0 or 1), compare, stencil front and back each compare, fail, depth fail and pass (STENCIL_OPERATION), read mask, write mask, bias (i32), slope scale and clamp (f32 bits); multisample: count, mask, alpha to coverage (0 or 1); fragment: module or NONE for none, the rest present either way (no bytes, no targets): entry (bytes), targets' count, each: format, blend color op, src, dst and alpha op, src, dst (BLEND_OPERATION, BLEND_FACTOR) or NONE in the color op for no blending and the five after it present either way, write mask (COLOR_WRITE).
inline constexpr std::uint32_t RENDER_PIPELINE = 9u;

// id, layout or NONE, module, entry (bytes).
inline constexpr std::uint32_t COMPUTE_PIPELINE = 10u;

// id, type (QUERY_TYPE), count.
inline constexpr std::uint32_t QUERY_SET = 11u;

// id: any object, made by any op above.
inline constexpr std::uint32_t DESTROY = 12u;

// buffer, offset, the bytes.
inline constexpr std::uint32_t WRITE_BUFFER = 13u;

// buffer, offset, address, bytes: read from the guest's memory when the host runs it.
inline constexpr std::uint32_t WRITE_BUFFER_AT = 14u;

// texture, mip, origin x, y, z, aspect, bytes per row, rows per image, width, height, depth, the bytes.
inline constexpr std::uint32_t WRITE_TEXTURE = 15u;

// as WRITE_TEXTURE, the bytes an address and a count in the guest's memory.
inline constexpr std::uint32_t WRITE_TEXTURE_AT = 16u;

// texture, mip, origin x, y, z, aspect (TEXTURE_ASPECT), flip y (0 or 1), premultiplied (0 or 1), address, bytes, word, ticket: an encoded image (PNG, JPEG, WebP) the host decodes into the texture, and once it is there writes ticket | DONE at the word, where the ticket still stands; an image the host cannot decode is left out and its word not written.
inline constexpr std::uint32_t COPY_EXTERNAL_IMAGE = 17u;

// id.
inline constexpr std::uint32_t ENCODER = 18u;

// encoder, source, its offset, destination, its offset, size.
inline constexpr std::uint32_t COPY_BUFFER_TO_BUFFER = 19u;

// encoder, buffer, offset, bytes per row, rows per image, texture, mip, origin x, y, z, aspect, width, height, depth.
inline constexpr std::uint32_t COPY_BUFFER_TO_TEXTURE = 20u;

// encoder, texture, mip, origin x, y, z, aspect, buffer, offset, bytes per row, rows per image, width, height, depth.
inline constexpr std::uint32_t COPY_TEXTURE_TO_BUFFER = 21u;

// encoder, source, mip, origin x, y, z, aspect, destination, mip, origin x, y, z, aspect, width, height, depth.
inline constexpr std::uint32_t COPY_TEXTURE_TO_TEXTURE = 22u;

// encoder, buffer, offset, size or NONE.
inline constexpr std::uint32_t CLEAR_BUFFER = 23u;

// encoder, query set, first, count, buffer, offset.
inline constexpr std::uint32_t RESOLVE_QUERY_SET = 24u;

// id, encoder, color attachments' count, each: view, resolve view or NONE, load (LOAD_OP), store (STORE_OP), clear r, g, b, a (f32 bits); depth-stencil: view or NONE for none, the rest present either way: depth load, store, clear (f32 bits), read only (0 or 1), stencil load, store, clear, read only; timestamps: query set or NONE, begin index or NONE, end index or NONE; occlusion query set or NONE.
inline constexpr std::uint32_t RENDER_PASS = 25u;

// id, encoder; timestamps: query set or NONE, begin index or NONE, end index or NONE.
inline constexpr std::uint32_t COMPUTE_PASS = 26u;

// pass, pipeline: render or compute, as the pass is.
inline constexpr std::uint32_t SET_PIPELINE = 27u;

// pass, index, group, dynamic offsets' count, that many offsets.
inline constexpr std::uint32_t SET_BIND_GROUP = 28u;

// pass, slot, buffer, offset, size or NONE.
inline constexpr std::uint32_t SET_VERTEX_BUFFER = 29u;

// pass, buffer, format (INDEX_FORMAT), offset, size or NONE.
inline constexpr std::uint32_t SET_INDEX_BUFFER = 30u;

// pass, x, y, width, height, min depth, max depth (f32 bits).
inline constexpr std::uint32_t SET_VIEWPORT = 31u;

// pass, x, y, width, height.
inline constexpr std::uint32_t SET_SCISSOR = 32u;

// pass, r, g, b, a (f32 bits).
inline constexpr std::uint32_t SET_BLEND_CONSTANT = 33u;

// pass, reference.
inline constexpr std::uint32_t SET_STENCIL_REFERENCE = 34u;

// pass, vertices, instances, first vertex, first instance.
inline constexpr std::uint32_t DRAW = 35u;

// pass, indices, instances, first index, base vertex (i32), first instance.
inline constexpr std::uint32_t DRAW_INDEXED = 36u;

// pass, buffer, offset.
inline constexpr std::uint32_t DRAW_INDIRECT = 37u;

// pass, buffer, offset.
inline constexpr std::uint32_t DRAW_INDEXED_INDIRECT = 38u;

// pass, workgroups x, y, z.
inline constexpr std::uint32_t DISPATCH = 39u;

// pass, buffer, offset.
inline constexpr std::uint32_t DISPATCH_INDIRECT = 40u;

// pass: ended, and let go with the record.
inline constexpr std::uint32_t END = 41u;

// encoders' count, that many encoders, each finished and let go with the record.
inline constexpr std::uint32_t SUBMIT = 42u;

// buffer, offset, size, address, word, ticket: WebGPU's mapAsync for reading, of a buffer the guest made MAP_READ | COPY_DST and copied into on an encoder it submitted before. Once mapped, the range is copied to the address, ticket | DONE written at the word and the buffer unmapped. The word is the mapping's own: the guest maps the buffer again only once it holds DONE, and judges by the ticket whose bytes landed.
inline constexpr std::uint32_t MAP_ASYNC = 43u;

// word, ticket: ticket | DONE at the word once what was submitted before it is done, where the ticket still stands.
inline constexpr std::uint32_t QUEUE_DONE = 44u;

// surface, format (TEXTURE_FORMAT, or SURFACE_PREFERRED), usage (TEXTURE_USAGE), alpha mode (CANVAS_ALPHA_MODE).
inline constexpr std::uint32_t CONFIGURE = 45u;

// surface, width, height.
inline constexpr std::uint32_t RESIZE = 46u;

// id, surface: the surface's texture for this turn, as a view.
inline constexpr std::uint32_t CURRENT = 47u;

// address, words: the host writes there the device's limits (LIMIT_NAME, a word each), then its features (FEATURE_NAME, a bit each, in words), then its timestamp period (f32 bits: the nanoseconds a resolved timestamp's unit is, 1 where they are nanoseconds as WebGPU's are), then the surface format it prefers (TEXTURE_FORMAT: what SURFACE_PREFERRED names, as the web's getPreferredCanvasFormat), then STATUS.
inline constexpr std::uint32_t DEVICE = 48u;
} // namespace op

enum class BindingKind : std::uint8_t {
    buffer = 0,
    sampler = 1,
    texture = 2,
    storage_texture = 3,
};

inline constexpr std::array<std::string_view, 4> BINDING_KIND{"buffer", "sampler", "texture", "storage-texture"};

enum class ResourceKind : std::uint8_t {
    buffer = 0,
    sampler = 1,
    texture_view = 2,
};

inline constexpr std::array<std::string_view, 3> RESOURCE_KIND{"buffer", "sampler", "texture-view"};

// GPUBufferUsage, its bits as WebGPU numbers them.
namespace BUFFER_USAGE {

inline constexpr std::uint32_t MAP_READ = 1u;

inline constexpr std::uint32_t MAP_WRITE = 2u;

inline constexpr std::uint32_t COPY_SRC = 4u;

inline constexpr std::uint32_t COPY_DST = 8u;

inline constexpr std::uint32_t INDEX = 16u;

inline constexpr std::uint32_t VERTEX = 32u;

inline constexpr std::uint32_t UNIFORM = 64u;

inline constexpr std::uint32_t STORAGE = 128u;

inline constexpr std::uint32_t INDIRECT = 256u;

inline constexpr std::uint32_t QUERY_RESOLVE = 512u;
} // namespace BUFFER_USAGE

// GPUTextureUsage, its bits as WebGPU numbers them.
namespace TEXTURE_USAGE {

inline constexpr std::uint32_t COPY_SRC = 1u;

inline constexpr std::uint32_t COPY_DST = 2u;

inline constexpr std::uint32_t TEXTURE_BINDING = 4u;

inline constexpr std::uint32_t STORAGE_BINDING = 8u;

inline constexpr std::uint32_t RENDER_ATTACHMENT = 16u;
} // namespace TEXTURE_USAGE

// GPUShaderStage, its bits as WebGPU numbers them.
namespace SHADER_STAGE {

inline constexpr std::uint32_t VERTEX = 1u;

inline constexpr std::uint32_t FRAGMENT = 2u;

inline constexpr std::uint32_t COMPUTE = 4u;
} // namespace SHADER_STAGE

// GPUColorWrite, its bits as WebGPU numbers them.
namespace COLOR_WRITE {

inline constexpr std::uint32_t RED = 1u;

inline constexpr std::uint32_t GREEN = 2u;

inline constexpr std::uint32_t BLUE = 4u;

inline constexpr std::uint32_t ALPHA = 8u;

inline constexpr std::uint32_t ALL = 15u;
} // namespace COLOR_WRITE

// DEVICE's last word: what the host knows of the device.
namespace STATUS {

inline constexpr std::uint32_t READY = 1u;

inline constexpr std::uint32_t LOST = 2u;
} // namespace STATUS

} // namespace xpute::gpu

// cpp/xpute/gpu/enum.spec.hpp
//
// GENERATED from spec/gpu/enum.json
//
// WebGPU's own enumerations, each its spec's strings in order: a number is its
// place in the list, so a host hands the string to the device as it is and a guest
// names a member by its generated constant. Nothing here is any one program's; a
// value WebGPU leaves optional is NONE (api.json) where unset.

namespace xpute::gpu {

// GPUTextureFormat, the core spec's and its texture-compression features'.
enum class TextureFormat : std::uint8_t {
    r8unorm = 0,
    r8snorm = 1,
    r8uint = 2,
    r8sint = 3,
    r16uint = 4,
    r16sint = 5,
    r16float = 6,
    rg8unorm = 7,
    rg8snorm = 8,
    rg8uint = 9,
    rg8sint = 10,
    r32uint = 11,
    r32sint = 12,
    r32float = 13,
    rg16uint = 14,
    rg16sint = 15,
    rg16float = 16,
    rgba8unorm = 17,
    rgba8unorm_srgb = 18,
    rgba8snorm = 19,
    rgba8uint = 20,
    rgba8sint = 21,
    bgra8unorm = 22,
    bgra8unorm_srgb = 23,
    rgb9e5ufloat = 24,
    rgb10a2uint = 25,
    rgb10a2unorm = 26,
    rg11b10ufloat = 27,
    rg32uint = 28,
    rg32sint = 29,
    rg32float = 30,
    rgba16uint = 31,
    rgba16sint = 32,
    rgba16float = 33,
    rgba32uint = 34,
    rgba32sint = 35,
    rgba32float = 36,
    stencil8 = 37,
    depth16unorm = 38,
    depth24plus = 39,
    depth24plus_stencil8 = 40,
    depth32float = 41,
    depth32float_stencil8 = 42,
    bc1_rgba_unorm = 43,
    bc1_rgba_unorm_srgb = 44,
    bc2_rgba_unorm = 45,
    bc2_rgba_unorm_srgb = 46,
    bc3_rgba_unorm = 47,
    bc3_rgba_unorm_srgb = 48,
    bc4_r_unorm = 49,
    bc4_r_snorm = 50,
    bc5_rg_unorm = 51,
    bc5_rg_snorm = 52,
    bc6h_rgb_ufloat = 53,
    bc6h_rgb_float = 54,
    bc7_rgba_unorm = 55,
    bc7_rgba_unorm_srgb = 56,
    etc2_rgb8unorm = 57,
    etc2_rgb8unorm_srgb = 58,
    etc2_rgb8a1unorm = 59,
    etc2_rgb8a1unorm_srgb = 60,
    etc2_rgba8unorm = 61,
    etc2_rgba8unorm_srgb = 62,
    eac_r11unorm = 63,
    eac_r11snorm = 64,
    eac_rg11unorm = 65,
    eac_rg11snorm = 66,
    astc_4x4_unorm = 67,
    astc_4x4_unorm_srgb = 68,
    astc_5x4_unorm = 69,
    astc_5x4_unorm_srgb = 70,
    astc_5x5_unorm = 71,
    astc_5x5_unorm_srgb = 72,
    astc_6x5_unorm = 73,
    astc_6x5_unorm_srgb = 74,
    astc_6x6_unorm = 75,
    astc_6x6_unorm_srgb = 76,
    astc_8x5_unorm = 77,
    astc_8x5_unorm_srgb = 78,
    astc_8x6_unorm = 79,
    astc_8x6_unorm_srgb = 80,
    astc_8x8_unorm = 81,
    astc_8x8_unorm_srgb = 82,
    astc_10x5_unorm = 83,
    astc_10x5_unorm_srgb = 84,
    astc_10x6_unorm = 85,
    astc_10x6_unorm_srgb = 86,
    astc_10x8_unorm = 87,
    astc_10x8_unorm_srgb = 88,
    astc_10x10_unorm = 89,
    astc_10x10_unorm_srgb = 90,
    astc_12x10_unorm = 91,
    astc_12x10_unorm_srgb = 92,
    astc_12x12_unorm = 93,
    astc_12x12_unorm_srgb = 94,
};

inline constexpr std::array<std::string_view, 95> TEXTURE_FORMAT{"r8unorm", "r8snorm", "r8uint", "r8sint", "r16uint", "r16sint", "r16float", "rg8unorm", "rg8snorm", "rg8uint", "rg8sint", "r32uint", "r32sint", "r32float", "rg16uint", "rg16sint", "rg16float", "rgba8unorm", "rgba8unorm-srgb", "rgba8snorm", "rgba8uint", "rgba8sint", "bgra8unorm", "bgra8unorm-srgb", "rgb9e5ufloat", "rgb10a2uint", "rgb10a2unorm", "rg11b10ufloat", "rg32uint", "rg32sint", "rg32float", "rgba16uint", "rgba16sint", "rgba16float", "rgba32uint", "rgba32sint", "rgba32float", "stencil8", "depth16unorm", "depth24plus", "depth24plus-stencil8", "depth32float", "depth32float-stencil8", "bc1-rgba-unorm", "bc1-rgba-unorm-srgb", "bc2-rgba-unorm", "bc2-rgba-unorm-srgb", "bc3-rgba-unorm", "bc3-rgba-unorm-srgb", "bc4-r-unorm", "bc4-r-snorm", "bc5-rg-unorm", "bc5-rg-snorm", "bc6h-rgb-ufloat", "bc6h-rgb-float", "bc7-rgba-unorm", "bc7-rgba-unorm-srgb", "etc2-rgb8unorm", "etc2-rgb8unorm-srgb", "etc2-rgb8a1unorm", "etc2-rgb8a1unorm-srgb", "etc2-rgba8unorm", "etc2-rgba8unorm-srgb", "eac-r11unorm", "eac-r11snorm", "eac-rg11unorm", "eac-rg11snorm", "astc-4x4-unorm", "astc-4x4-unorm-srgb", "astc-5x4-unorm", "astc-5x4-unorm-srgb", "astc-5x5-unorm", "astc-5x5-unorm-srgb", "astc-6x5-unorm", "astc-6x5-unorm-srgb", "astc-6x6-unorm", "astc-6x6-unorm-srgb", "astc-8x5-unorm", "astc-8x5-unorm-srgb", "astc-8x6-unorm", "astc-8x6-unorm-srgb", "astc-8x8-unorm", "astc-8x8-unorm-srgb", "astc-10x5-unorm", "astc-10x5-unorm-srgb", "astc-10x6-unorm", "astc-10x6-unorm-srgb", "astc-10x8-unorm", "astc-10x8-unorm-srgb", "astc-10x10-unorm", "astc-10x10-unorm-srgb", "astc-12x10-unorm", "astc-12x10-unorm-srgb", "astc-12x12-unorm", "astc-12x12-unorm-srgb"};

// GPUVertexFormat.
enum class VertexFormat : std::uint8_t {
    uint8 = 0,
    uint8x2 = 1,
    uint8x4 = 2,
    sint8 = 3,
    sint8x2 = 4,
    sint8x4 = 5,
    unorm8 = 6,
    unorm8x2 = 7,
    unorm8x4 = 8,
    snorm8 = 9,
    snorm8x2 = 10,
    snorm8x4 = 11,
    uint16 = 12,
    uint16x2 = 13,
    uint16x4 = 14,
    sint16 = 15,
    sint16x2 = 16,
    sint16x4 = 17,
    unorm16 = 18,
    unorm16x2 = 19,
    unorm16x4 = 20,
    snorm16 = 21,
    snorm16x2 = 22,
    snorm16x4 = 23,
    float16 = 24,
    float16x2 = 25,
    float16x4 = 26,
    float32 = 27,
    float32x2 = 28,
    float32x3 = 29,
    float32x4 = 30,
    uint32 = 31,
    uint32x2 = 32,
    uint32x3 = 33,
    uint32x4 = 34,
    sint32 = 35,
    sint32x2 = 36,
    sint32x3 = 37,
    sint32x4 = 38,
    unorm10_10_10_2 = 39,
    unorm8x4_bgra = 40,
};

inline constexpr std::array<std::string_view, 41> VERTEX_FORMAT{"uint8", "uint8x2", "uint8x4", "sint8", "sint8x2", "sint8x4", "unorm8", "unorm8x2", "unorm8x4", "snorm8", "snorm8x2", "snorm8x4", "uint16", "uint16x2", "uint16x4", "sint16", "sint16x2", "sint16x4", "unorm16", "unorm16x2", "unorm16x4", "snorm16", "snorm16x2", "snorm16x4", "float16", "float16x2", "float16x4", "float32", "float32x2", "float32x3", "float32x4", "uint32", "uint32x2", "uint32x3", "uint32x4", "sint32", "sint32x2", "sint32x3", "sint32x4", "unorm10-10-10-2", "unorm8x4-bgra"};

enum class PrimitiveTopology : std::uint8_t {
    point_list = 0,
    line_list = 1,
    line_strip = 2,
    triangle_list = 3,
    triangle_strip = 4,
};

inline constexpr std::array<std::string_view, 5> PRIMITIVE_TOPOLOGY{"point-list", "line-list", "line-strip", "triangle-list", "triangle-strip"};

enum class IndexFormat : std::uint8_t {
    uint16 = 0,
    uint32 = 1,
};

inline constexpr std::array<std::string_view, 2> INDEX_FORMAT{"uint16", "uint32"};

enum class FrontFace : std::uint8_t {
    ccw = 0,
    cw = 1,
};

inline constexpr std::array<std::string_view, 2> FRONT_FACE{"ccw", "cw"};

enum class CullMode : std::uint8_t {
    none = 0,
    front = 1,
    back = 2,
};

inline constexpr std::array<std::string_view, 3> CULL_MODE{"none", "front", "back"};

enum class CompareFunction : std::uint8_t {
    never = 0,
    less = 1,
    equal = 2,
    less_equal = 3,
    greater = 4,
    not_equal = 5,
    greater_equal = 6,
    always = 7,
};

inline constexpr std::array<std::string_view, 8> COMPARE_FUNCTION{"never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always"};

enum class StencilOperation : std::uint8_t {
    keep = 0,
    zero = 1,
    replace = 2,
    invert = 3,
    increment_clamp = 4,
    decrement_clamp = 5,
    increment_wrap = 6,
    decrement_wrap = 7,
};

inline constexpr std::array<std::string_view, 8> STENCIL_OPERATION{"keep", "zero", "replace", "invert", "increment-clamp", "decrement-clamp", "increment-wrap", "decrement-wrap"};

enum class BlendOperation : std::uint8_t {
    add = 0,
    subtract = 1,
    reverse_subtract = 2,
    min = 3,
    max = 4,
};

inline constexpr std::array<std::string_view, 5> BLEND_OPERATION{"add", "subtract", "reverse-subtract", "min", "max"};

enum class BlendFactor : std::uint8_t {
    zero = 0,
    one = 1,
    src = 2,
    one_minus_src = 3,
    src_alpha = 4,
    one_minus_src_alpha = 5,
    dst = 6,
    one_minus_dst = 7,
    dst_alpha = 8,
    one_minus_dst_alpha = 9,
    src_alpha_saturated = 10,
    constant = 11,
    one_minus_constant = 12,
    src1 = 13,
    one_minus_src1 = 14,
    src1_alpha = 15,
    one_minus_src1_alpha = 16,
};

inline constexpr std::array<std::string_view, 17> BLEND_FACTOR{"zero", "one", "src", "one-minus-src", "src-alpha", "one-minus-src-alpha", "dst", "one-minus-dst", "dst-alpha", "one-minus-dst-alpha", "src-alpha-saturated", "constant", "one-minus-constant", "src1", "one-minus-src1", "src1-alpha", "one-minus-src1-alpha"};

enum class VertexStepMode : std::uint8_t {
    vertex = 0,
    instance = 1,
};

inline constexpr std::array<std::string_view, 2> VERTEX_STEP_MODE{"vertex", "instance"};

enum class AddressMode : std::uint8_t {
    clamp_to_edge = 0,
    repeat = 1,
    mirror_repeat = 2,
};

inline constexpr std::array<std::string_view, 3> ADDRESS_MODE{"clamp-to-edge", "repeat", "mirror-repeat"};

enum class FilterMode : std::uint8_t {
    nearest = 0,
    linear = 1,
};

inline constexpr std::array<std::string_view, 2> FILTER_MODE{"nearest", "linear"};

enum class MipmapFilterMode : std::uint8_t {
    nearest = 0,
    linear = 1,
};

inline constexpr std::array<std::string_view, 2> MIPMAP_FILTER_MODE{"nearest", "linear"};

enum class TextureDimension : std::uint8_t {
    _1d = 0,
    _2d = 1,
    _3d = 2,
};

inline constexpr std::array<std::string_view, 3> TEXTURE_DIMENSION{"1d", "2d", "3d"};

enum class TextureViewDimension : std::uint8_t {
    _1d = 0,
    _2d = 1,
    _2d_array = 2,
    cube = 3,
    cube_array = 4,
    _3d = 5,
};

inline constexpr std::array<std::string_view, 6> TEXTURE_VIEW_DIMENSION{"1d", "2d", "2d-array", "cube", "cube-array", "3d"};

enum class TextureAspect : std::uint8_t {
    all = 0,
    stencil_only = 1,
    depth_only = 2,
};

inline constexpr std::array<std::string_view, 3> TEXTURE_ASPECT{"all", "stencil-only", "depth-only"};

enum class BufferBindingType : std::uint8_t {
    uniform = 0,
    storage = 1,
    read_only_storage = 2,
};

inline constexpr std::array<std::string_view, 3> BUFFER_BINDING_TYPE{"uniform", "storage", "read-only-storage"};

enum class SamplerBindingType : std::uint8_t {
    filtering = 0,
    non_filtering = 1,
    comparison = 2,
};

inline constexpr std::array<std::string_view, 3> SAMPLER_BINDING_TYPE{"filtering", "non-filtering", "comparison"};

enum class TextureSampleType : std::uint8_t {
    float_ = 0,
    unfilterable_float = 1,
    depth = 2,
    sint = 3,
    uint = 4,
};

inline constexpr std::array<std::string_view, 5> TEXTURE_SAMPLE_TYPE{"float", "unfilterable-float", "depth", "sint", "uint"};

enum class StorageTextureAccess : std::uint8_t {
    write_only = 0,
    read_only = 1,
    read_write = 2,
};

inline constexpr std::array<std::string_view, 3> STORAGE_TEXTURE_ACCESS{"write-only", "read-only", "read-write"};

enum class LoadOp : std::uint8_t {
    load = 0,
    clear = 1,
};

inline constexpr std::array<std::string_view, 2> LOAD_OP{"load", "clear"};

enum class StoreOp : std::uint8_t {
    store = 0,
    discard = 1,
};

inline constexpr std::array<std::string_view, 2> STORE_OP{"store", "discard"};

enum class QueryType : std::uint8_t {
    occlusion = 0,
    timestamp = 1,
};

inline constexpr std::array<std::string_view, 2> QUERY_TYPE{"occlusion", "timestamp"};

enum class CanvasAlphaMode : std::uint8_t {
    opaque = 0,
    premultiplied = 1,
};

inline constexpr std::array<std::string_view, 2> CANVAS_ALPHA_MODE{"opaque", "premultiplied"};

// GPUFeatureName: a device's features are a bit each, by place (DEVICE in api.json).
enum class FeatureName : std::uint8_t {
    depth_clip_control = 0,
    depth32float_stencil8 = 1,
    texture_compression_bc = 2,
    texture_compression_bc_sliced_3d = 3,
    texture_compression_etc2 = 4,
    texture_compression_astc = 5,
    texture_compression_astc_sliced_3d = 6,
    timestamp_query = 7,
    indirect_first_instance = 8,
    shader_f16 = 9,
    rg11b10ufloat_renderable = 10,
    bgra8unorm_storage = 11,
    float32_filterable = 12,
    float32_blendable = 13,
    clip_distances = 14,
    dual_source_blending = 15,
    subgroups = 16,
};

inline constexpr std::array<std::string_view, 17> FEATURE_NAME{"depth-clip-control", "depth32float-stencil8", "texture-compression-bc", "texture-compression-bc-sliced-3d", "texture-compression-etc2", "texture-compression-astc", "texture-compression-astc-sliced-3d", "timestamp-query", "indirect-first-instance", "shader-f16", "rg11b10ufloat-renderable", "bgra8unorm-storage", "float32-filterable", "float32-blendable", "clip-distances", "dual-source-blending", "subgroups"};

// GPUSupportedLimits' members: a device's limits are a word each, by place (DEVICE in api.json).
enum class LimitName : std::uint8_t {
    maxTextureDimension1D = 0,
    maxTextureDimension2D = 1,
    maxTextureDimension3D = 2,
    maxTextureArrayLayers = 3,
    maxBindGroups = 4,
    maxBindGroupsPlusVertexBuffers = 5,
    maxBindingsPerBindGroup = 6,
    maxDynamicUniformBuffersPerPipelineLayout = 7,
    maxDynamicStorageBuffersPerPipelineLayout = 8,
    maxSampledTexturesPerShaderStage = 9,
    maxSamplersPerShaderStage = 10,
    maxStorageBuffersPerShaderStage = 11,
    maxStorageTexturesPerShaderStage = 12,
    maxUniformBuffersPerShaderStage = 13,
    maxUniformBufferBindingSize = 14,
    maxStorageBufferBindingSize = 15,
    minUniformBufferOffsetAlignment = 16,
    minStorageBufferOffsetAlignment = 17,
    maxVertexBuffers = 18,
    maxBufferSize = 19,
    maxVertexAttributes = 20,
    maxVertexBufferArrayStride = 21,
    maxInterStageShaderVariables = 22,
    maxColorAttachments = 23,
    maxColorAttachmentBytesPerSample = 24,
    maxComputeWorkgroupStorageSize = 25,
    maxComputeInvocationsPerWorkgroup = 26,
    maxComputeWorkgroupSizeX = 27,
    maxComputeWorkgroupSizeY = 28,
    maxComputeWorkgroupSizeZ = 29,
    maxComputeWorkgroupsPerDimension = 30,
};

inline constexpr std::array<std::string_view, 31> LIMIT_NAME{"maxTextureDimension1D", "maxTextureDimension2D", "maxTextureDimension3D", "maxTextureArrayLayers", "maxBindGroups", "maxBindGroupsPlusVertexBuffers", "maxBindingsPerBindGroup", "maxDynamicUniformBuffersPerPipelineLayout", "maxDynamicStorageBuffersPerPipelineLayout", "maxSampledTexturesPerShaderStage", "maxSamplersPerShaderStage", "maxStorageBuffersPerShaderStage", "maxStorageTexturesPerShaderStage", "maxUniformBuffersPerShaderStage", "maxUniformBufferBindingSize", "maxStorageBufferBindingSize", "minUniformBufferOffsetAlignment", "minStorageBufferOffsetAlignment", "maxVertexBuffers", "maxBufferSize", "maxVertexAttributes", "maxVertexBufferArrayStride", "maxInterStageShaderVariables", "maxColorAttachments", "maxColorAttachmentBytesPerSample", "maxComputeWorkgroupStorageSize", "maxComputeInvocationsPerWorkgroup", "maxComputeWorkgroupSizeX", "maxComputeWorkgroupSizeY", "maxComputeWorkgroupSizeZ", "maxComputeWorkgroupsPerDimension"};

} // namespace xpute::gpu

// cpp/xpute/gpu/encode.hpp

// A guest's GPU calls written as a stream's records (ipc/stream), in the
// words spec/gpu/api.json gives each op, as rust/gpu/src/encode.rs writes
// them. Objects are handles from one table; a record that does not fit an
// open transaction is not written, and an object it would have made is
// handed back. Every record is written twice by the same function: once to
// count its words, then into the room the stream reserved for them.

namespace xpute::gpu {

// An object the guest made: a handle of the table its encoder holds.
using Id = std::uint32_t;

struct BufferDescriptor {
    std::uint32_t size;
    // BUFFER_USAGE bits.
    std::uint32_t usage;
    bool mapped_at_creation;
};

struct TextureDescriptor {
    // Width, height, and depth or layers.
    std::array<std::uint32_t, 3> size;
    std::uint32_t mip_levels;
    std::uint32_t samples;
    TextureDimension dimension;
    TextureFormat format;
    // TEXTURE_USAGE bits.
    std::uint32_t usage;
    std::span<const TextureFormat> view_formats;
};

struct ViewDescriptor {
    std::optional<TextureFormat> format;
    std::optional<TextureViewDimension> dimension;
    std::optional<TextureAspect> aspect;
    std::uint32_t base_mip;
    std::optional<std::uint32_t> mips;
    std::uint32_t base_layer;
    std::optional<std::uint32_t> layers;
};

struct SamplerDescriptor {
    std::array<AddressMode, 3> address;
    FilterMode mag;
    FilterMode min;
    MipmapFilterMode mipmap;
    std::array<float, 2> lod;
    std::optional<CompareFunction> compare;
    std::uint32_t max_anisotropy;
};

struct BufferBinding {
    BufferBindingType ty;
    bool dynamic_offset;
    std::uint32_t min_binding_size;
};

struct SamplerBinding {
    SamplerBindingType ty;
};

struct TextureBinding {
    TextureSampleType sample;
    TextureViewDimension dimension;
    bool multisampled;
};

struct StorageTextureBinding {
    StorageTextureAccess access;
    TextureFormat format;
    TextureViewDimension dimension;
};

using BindingLayout = std::variant<BufferBinding, SamplerBinding, TextureBinding, StorageTextureBinding>;

struct BindGroupLayoutEntry {
    std::uint32_t binding;
    // SHADER_STAGE bits.
    std::uint32_t visibility;
    BindingLayout layout;
};

struct BufferResource {
    Id buffer;
    std::uint32_t offset;
    std::optional<std::uint32_t> size;
};

struct SamplerResource {
    Id sampler;
};

struct ViewResource {
    Id view;
};

using Resource = std::variant<BufferResource, SamplerResource, ViewResource>;

struct BindGroupEntry {
    std::uint32_t binding;
    Resource resource;
};

struct VertexAttribute {
    VertexFormat format;
    std::uint32_t offset;
    std::uint32_t location;
};

struct VertexBufferLayout {
    std::uint32_t stride;
    VertexStepMode step;
    std::span<const VertexAttribute> attributes;
};

struct VertexState {
    Id module;
    std::string_view entry;
    std::span<const VertexBufferLayout> buffers;
};

struct PrimitiveState {
    PrimitiveTopology topology;
    std::optional<IndexFormat> strip_index_format;
    FrontFace front_face;
    CullMode cull;
    bool unclipped_depth;
};

// WebGPU's default face: always, keep, keep, keep.
struct StencilFaceState {
    CompareFunction compare = CompareFunction::always;
    StencilOperation fail = StencilOperation::keep;
    StencilOperation depth_fail = StencilOperation::keep;
    StencilOperation pass = StencilOperation::keep;
};

struct DepthStencilState {
    TextureFormat format;
    bool depth_write;
    CompareFunction depth_compare;
    StencilFaceState stencil_front;
    StencilFaceState stencil_back;
    std::uint32_t stencil_read_mask;
    std::uint32_t stencil_write_mask;
    std::int32_t bias;
    float slope_scale;
    float clamp;
};

// One sample, every sample's bit set.
struct MultisampleState {
    std::uint32_t count = 1;
    std::uint32_t mask = UINT32_MAX;
    bool alpha_to_coverage = false;
};

struct BlendComponent {
    BlendOperation operation;
    BlendFactor src;
    BlendFactor dst;
};

struct BlendState {
    BlendComponent color;
    BlendComponent alpha;
};

struct ColorTargetState {
    TextureFormat format;
    std::optional<BlendState> blend;
    // COLOR_WRITE bits.
    std::uint32_t write_mask;
};

struct FragmentState {
    Id module;
    std::string_view entry;
    std::span<const ColorTargetState> targets;
};

struct RenderPipelineDescriptor {
    // None for a layout the device makes from the shaders.
    std::optional<Id> layout;
    VertexState vertex;
    PrimitiveState primitive;
    std::optional<DepthStencilState> depth_stencil;
    MultisampleState multisample;
    std::optional<FragmentState> fragment;
};

struct ComputePipelineDescriptor {
    std::optional<Id> layout;
    Id module;
    std::string_view entry;
};

// A texture's place a copy reads or writes.
struct TexelCopyTexture {
    Id texture;
    std::uint32_t mip;
    std::array<std::uint32_t, 3> origin;
    TextureAspect aspect;
};

// A buffer's bytes laid out as texels.
struct TexelCopyBuffer {
    Id buffer;
    std::uint32_t offset;
    std::uint32_t bytes_per_row;
    std::uint32_t rows_per_image;
};

struct ColorAttachment {
    Id view;
    std::optional<Id> resolve;
    LoadOp load;
    StoreOp store;
    std::array<float, 4> clear;
};

struct DepthStencilAttachment {
    Id view;
    LoadOp depth_load;
    StoreOp depth_store;
    float depth_clear;
    bool depth_read_only;
    LoadOp stencil_load;
    StoreOp stencil_store;
    std::uint32_t stencil_clear;
    bool stencil_read_only;
};

struct TimestampWrites {
    Id query_set;
    std::optional<std::uint32_t> begin;
    std::optional<std::uint32_t> end;
};

struct RenderPassDescriptor {
    std::span<const ColorAttachment> colors;
    std::optional<DepthStencilAttachment> depth_stencil;
    std::optional<TimestampWrites> timestamps;
    std::optional<Id> occlusion;
};

namespace detail {

// A record's words counted.
struct Count {
    std::uint32_t n = 0;
    void word(std::uint32_t) noexcept { n++; }
    // A count, then the bytes padded to words.
    void bytes(std::span<const std::uint8_t> b) noexcept { n += 1 + static_cast<std::uint32_t>((b.size() + WORD - 1) / WORD); }
};

// A record's words written where the stream reserved them.
struct Fill {
    Memory mem;
    std::uint32_t at;
    std::uint32_t n = 0;
    void word(std::uint32_t v) noexcept { mem.set_word(at + WORD * n++, v); }
    void bytes(std::span<const std::uint8_t> b) noexcept {
        word(static_cast<std::uint32_t>(b.size()));
        for (std::size_t k = 0; k < b.size(); k += WORD) {
            std::uint32_t w = 0;
            std::memcpy(&w, b.data() + k, b.size() - k < WORD ? b.size() - k : WORD);
            word(w);
        }
    }
};

inline std::uint32_t f(float v) noexcept {
    std::uint32_t w;
    std::memcpy(&w, &v, sizeof w);
    return w;
}

template <class T> std::uint32_t opt(std::optional<T> v) noexcept {
    return v ? static_cast<std::uint32_t>(*v) : NONE;
}

inline std::uint32_t flag(bool v) noexcept { return v ? 1 : 0; }

template <class E> std::uint32_t u(E e) noexcept { return static_cast<std::uint32_t>(e); }

inline std::span<const std::uint8_t> text(std::string_view s) noexcept { return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()}; }

template <class S> void words(S &s, std::initializer_list<std::uint32_t> w) noexcept {
    for (std::uint32_t v : w) s.word(v);
}

// Writes a record of `op` whose words `put` gives; false where an open
// transaction no longer fits it.
template <class F> bool emit(Stream &stream, std::uint32_t op, F &&put) noexcept {
    Count count;
    put(count);
    std::optional<std::uint32_t> at = stream.reserve(op, count.n);
    if (!at) return false;
    Fill fill{stream.mem(), *at};
    put(fill);
    ensure(fill.n == count.n, Errno::enotrecoverable, op);
    return true;
}

// What a record takes of a stream's room, measured as `emit` writes it.
template <class F> std::uint64_t measured(F &&put) noexcept {
    Count count;
    put(count);
    return (2 + std::uint64_t{count.n}) * WORD;
}

template <class S> void timestamps(S &s, const std::optional<TimestampWrites> &t) noexcept {
    if (t) words(s, {t->query_set, opt(t->begin), opt(t->end)});
    else words(s, {NONE, NONE, NONE});
}

template <class S> void texture_place(S &s, const TexelCopyTexture &t) noexcept {
    words(s, {t.texture, t.mip, t.origin[0], t.origin[1], t.origin[2], u(t.aspect)});
}

template <class S> void buffer_layout(S &s, const TexelCopyBuffer &b) noexcept { words(s, {b.buffer, b.offset, b.bytes_per_row, b.rows_per_image}); }

template <class S> void extent(S &s, std::array<std::uint32_t, 3> e) noexcept { words(s, {e[0], e[1], e[2]}); }

template <class S> void stencil_face(S &s, const StencilFaceState &f) noexcept { words(s, {u(f.compare), u(f.fail), u(f.depth_fail), u(f.pass)}); }

} // namespace detail

// The ops below DESTROY make what lasts until it is destroyed, the objects a
// program holds; those past it make what a turn lets go (an encoder, a pass,
// a surface's current texture).
inline constexpr std::size_t OBJECT_OPS = op::DESTROY;

// The calls, over a table of `N` handles, each holding the op that made it
// where what it made lasts, 0 otherwise.
template <std::size_t N> class Gpu {
  public:
    std::optional<Id> buffer(Stream &stream, const BufferDescriptor &d) noexcept {
        return make(stream, op::BUFFER, [&](auto &s) { detail::words(s, {d.size, d.usage, detail::flag(d.mapped_at_creation)}); });
    }

    std::optional<Id> texture(Stream &stream, const TextureDescriptor &d) noexcept {
        using detail::u;
        return make(stream, op::TEXTURE, [&](auto &s) {
            detail::words(s, {d.size[0], d.size[1], d.size[2], d.mip_levels, d.samples, u(d.dimension), u(d.format), d.usage, static_cast<std::uint32_t>(d.view_formats.size())});
            for (TextureFormat f : d.view_formats) s.word(u(f));
        });
    }

    std::optional<Id> view(Stream &stream, Id texture, const ViewDescriptor &d) noexcept {
        using detail::opt;
        return make(stream, op::VIEW, [&](auto &s) {
            detail::words(s, {texture, opt(d.format), opt(d.dimension), detail::u(d.aspect.value_or(TextureAspect::all)), d.base_mip, opt(d.mips), d.base_layer, opt(d.layers)});
        });
    }

    std::optional<Id> sampler(Stream &stream, const SamplerDescriptor &d) noexcept {
        using detail::f;
        using detail::u;
        return make(stream, op::SAMPLER, [&](auto &s) {
            detail::words(s, {u(d.address[0]), u(d.address[1]), u(d.address[2]), u(d.mag), u(d.min), u(d.mipmap), f(d.lod[0]), f(d.lod[1]), detail::opt(d.compare), d.max_anisotropy});
        });
    }

    std::optional<Id> shader_module(Stream &stream, std::string_view wgsl) noexcept {
        return make(stream, op::SHADER_MODULE, [&](auto &s) { s.bytes(detail::text(wgsl)); });
    }

    std::optional<Id> bind_group_layout(Stream &stream, std::span<const BindGroupLayoutEntry> entries) noexcept {
        using detail::flag;
        using detail::u;
        return make(stream, op::BIND_GROUP_LAYOUT, [&](auto &s) {
            s.word(static_cast<std::uint32_t>(entries.size()));
            for (const BindGroupLayoutEntry &e : entries) {
                std::array<std::uint32_t, 4> w;
                if (auto *b = std::get_if<BufferBinding>(&e.layout)) w = {u(BindingKind::buffer), u(b->ty), flag(b->dynamic_offset), b->min_binding_size};
                else if (auto *b = std::get_if<SamplerBinding>(&e.layout)) w = {u(BindingKind::sampler), u(b->ty), 0, 0};
                else if (auto *b = std::get_if<TextureBinding>(&e.layout)) w = {u(BindingKind::texture), u(b->sample), u(b->dimension), flag(b->multisampled)};
                else {
                    auto &t = std::get<StorageTextureBinding>(e.layout);
                    w = {u(BindingKind::storage_texture), u(t.access), u(t.format), u(t.dimension)};
                }
                detail::words(s, {e.binding, e.visibility, w[0], w[1], w[2], w[3]});
            }
        });
    }

    std::optional<Id> pipeline_layout(Stream &stream, std::span<const Id> layouts) noexcept {
        return make(stream, op::PIPELINE_LAYOUT, [&](auto &s) {
            s.word(static_cast<std::uint32_t>(layouts.size()));
            for (Id l : layouts) s.word(l);
        });
    }

    std::optional<Id> bind_group(Stream &stream, Id layout, std::span<const BindGroupEntry> entries) noexcept {
        using detail::u;
        return make(stream, op::BIND_GROUP, [&](auto &s) {
            detail::words(s, {layout, static_cast<std::uint32_t>(entries.size())});
            for (const BindGroupEntry &e : entries) {
                std::array<std::uint32_t, 4> w;
                if (auto *b = std::get_if<BufferResource>(&e.resource)) w = {u(ResourceKind::buffer), b->buffer, b->offset, detail::opt(b->size)};
                else if (auto *r = std::get_if<SamplerResource>(&e.resource)) w = {u(ResourceKind::sampler), r->sampler, 0, 0};
                else w = {u(ResourceKind::texture_view), std::get<ViewResource>(e.resource).view, 0, 0};
                detail::words(s, {e.binding, w[0], w[1], w[2], w[3]});
            }
        });
    }

    std::optional<Id> render_pipeline(Stream &stream, const RenderPipelineDescriptor &d) noexcept {
        using detail::f;
        using detail::flag;
        using detail::opt;
        using detail::u;
        return make(stream, op::RENDER_PIPELINE, [&](auto &s) {
            s.word(opt(d.layout));
            const VertexState &v = d.vertex;
            s.word(v.module);
            s.bytes(detail::text(v.entry));
            s.word(static_cast<std::uint32_t>(v.buffers.size()));
            for (const VertexBufferLayout &b : v.buffers) {
                detail::words(s, {b.stride, u(b.step), static_cast<std::uint32_t>(b.attributes.size())});
                for (const VertexAttribute &a : b.attributes) detail::words(s, {u(a.format), a.offset, a.location});
            }
            const PrimitiveState &p = d.primitive;
            detail::words(s, {u(p.topology), opt(p.strip_index_format), u(p.front_face), u(p.cull), flag(p.unclipped_depth)});
            if (const auto &ds = d.depth_stencil) {
                detail::words(s, {u(ds->format), flag(ds->depth_write), u(ds->depth_compare)});
                detail::stencil_face(s, ds->stencil_front);
                detail::stencil_face(s, ds->stencil_back);
                detail::words(s, {ds->stencil_read_mask, ds->stencil_write_mask, static_cast<std::uint32_t>(ds->bias), f(ds->slope_scale), f(ds->clamp)});
            } else {
                // Present either way, so the record's shape is fixed.
                s.word(NONE);
                for (int k = 0; k < 2 + 8 + 5; k++) s.word(0);
            }
            const MultisampleState &m = d.multisample;
            detail::words(s, {m.count, m.mask, flag(m.alpha_to_coverage)});
            if (const auto &fr = d.fragment) {
                s.word(fr->module);
                s.bytes(detail::text(fr->entry));
                s.word(static_cast<std::uint32_t>(fr->targets.size()));
                for (const ColorTargetState &t : fr->targets) {
                    s.word(u(t.format));
                    if (const auto &b = t.blend) {
                        detail::words(s, {u(b->color.operation), u(b->color.src), u(b->color.dst), u(b->alpha.operation), u(b->alpha.src), u(b->alpha.dst)});
                    } else {
                        s.word(NONE);
                        for (int k = 0; k < 5; k++) s.word(0);
                    }
                    s.word(t.write_mask);
                }
            } else {
                s.word(NONE);
                s.bytes({});
                s.word(0);
            }
        });
    }

    std::optional<Id> compute_pipeline(Stream &stream, const ComputePipelineDescriptor &d) noexcept {
        return make(stream, op::COMPUTE_PIPELINE, [&](auto &s) {
            detail::words(s, {detail::opt(d.layout), d.module});
            s.bytes(detail::text(d.entry));
        });
    }

    std::optional<Id> query_set(Stream &stream, QueryType ty, std::uint32_t count) noexcept {
        return make(stream, op::QUERY_SET, [&](auto &s) { detail::words(s, {detail::u(ty), count}); });
    }

    std::optional<Id> encoder(Stream &stream) noexcept {
        return make(stream, op::ENCODER, [](auto &) {});
    }

    std::optional<Id> render_pass(Stream &stream, Id encoder, const RenderPassDescriptor &d) noexcept {
        using detail::f;
        using detail::flag;
        using detail::u;
        return make(stream, op::RENDER_PASS, [&](auto &s) {
            detail::words(s, {encoder, static_cast<std::uint32_t>(d.colors.size())});
            for (const ColorAttachment &c : d.colors) {
                detail::words(s, {c.view, detail::opt(c.resolve), u(c.load), u(c.store), f(c.clear[0]), f(c.clear[1]), f(c.clear[2]), f(c.clear[3])});
            }
            if (const auto &ds = d.depth_stencil) {
                detail::words(s, {ds->view, u(ds->depth_load), u(ds->depth_store), f(ds->depth_clear), flag(ds->depth_read_only), u(ds->stencil_load), u(ds->stencil_store), ds->stencil_clear,
                                  flag(ds->stencil_read_only)});
            } else {
                s.word(NONE);
                for (int k = 0; k < 8; k++) s.word(0);
            }
            detail::timestamps(s, d.timestamps);
            s.word(detail::opt(d.occlusion));
        });
    }

    std::optional<Id> compute_pass(Stream &stream, Id encoder, const std::optional<TimestampWrites> &t) noexcept {
        return make(stream, op::COMPUTE_PASS, [&](auto &s) {
            s.word(encoder);
            detail::timestamps(s, t);
        });
    }

    // This turn's texture of `surface`, as a view.
    std::optional<Id> current(Stream &stream, std::uint32_t surface) noexcept {
        return make(stream, op::CURRENT, [&](auto &s) { s.word(surface); });
    }

    // The records from here to `commit` are one, and so are the objects they
    // make: a transaction that does not fit drops both.
    void begin(Stream &stream) noexcept {
        stream.begin();
        handles_.begin();
    }

    // False where the transaction did not fit: the stream is as at `begin`,
    // and every handle taken since is free again, as the host never saw it.
    bool commit(Stream &stream) noexcept {
        bool fit = stream.commit();
        if (!fit) handles_.rollback([this](Id, std::uint8_t op) { forget(op); });
        return fit;
    }

    // Ends a pass. Its handle is free whether or not the record was written:
    // one dropped with its transaction left nothing on the host.
    bool end(Stream &stream, Id pass) noexcept {
        bool sent = detail::emit(stream, op::END, [&](auto &s) { s.word(pass); });
        release(pass);
        return sent;
    }

    // Each encoder finished, submitted in order and let go, as `end` lets a pass go.
    bool submit(Stream &stream, std::span<const Id> encoders) noexcept {
        bool sent = detail::emit(stream, op::SUBMIT, [&](auto &s) {
            s.word(static_cast<std::uint32_t>(encoders.size()));
            for (Id e : encoders) s.word(e);
        });
        for (Id e : encoders) release(e);
        return sent;
    }

    void destroy(Stream &stream, Id id) noexcept {
        if (release(id)) detail::emit(stream, op::DESTROY, [&](auto &s) { s.word(id); });
    }

    bool live(Id id) const noexcept { return handles_.live(id); }

    // The live objects by the op that made them (slot `op`, below DESTROY).
    const std::array<std::uint32_t, OBJECT_OPS> &objects() const noexcept { return objects_; }

  private:
    // An object's handle, written by `op` as the record's first word; none
    // where the table is full or the record did not fit.
    template <class F> std::optional<Id> make(Stream &stream, std::uint32_t op, F &&put) noexcept {
        bool lasts = op < OBJECT_OPS;
        std::optional<std::uint32_t> id = handles_.insert(lasts ? static_cast<std::uint8_t>(op) : 0);
        if (!id) return std::nullopt;
        if (!detail::emit(stream, op, [&](auto &s) {
                s.word(*id);
                put(s);
            })) {
            handles_.remove(*id);
            return std::nullopt;
        }
        if (lasts) objects_[op]++;
        return id;
    }

    // `id` let go, where it was live.
    bool release(Id id) noexcept {
        std::optional<std::uint8_t> op = handles_.remove(id);
        if (op) forget(*op);
        return op.has_value();
    }

    void forget(std::uint8_t op) noexcept {
        if (op != 0) objects_[op]--;
    }

    abi::Table<std::uint8_t, N> handles_;
    // The live objects by the op that made them, slot `op`.
    std::array<std::uint32_t, OBJECT_OPS> objects_{};
};

// The calls that make no object: an op's words straight to the stream, false
// where an open transaction no longer fits them.
namespace call {

bool write_buffer(Stream &stream, Id buffer, std::uint32_t offset, std::span<const std::uint8_t> bytes) noexcept;
// What `write_buffer` takes of a stream's room (Stream::fits), so a caller
// writing several asks once.
std::uint64_t write_buffer_bytes(std::span<const std::uint8_t> bytes) noexcept;
// The bytes read where they lie in the guest's memory when the host runs the
// stream, so they must stay until it has.
bool write_buffer_at(Stream &stream, Id buffer, std::uint32_t offset, std::span<const std::uint8_t> bytes) noexcept;
// `layout`: bytes per row, rows per image.
bool write_texture(Stream &stream, const TexelCopyTexture &place, std::array<std::uint32_t, 2> layout, std::array<std::uint32_t, 3> size, std::span<const std::uint8_t> bytes) noexcept;
// As write_buffer_bytes: the words are the same whatever the place, layout and size.
std::uint64_t write_texture_bytes(std::span<const std::uint8_t> bytes) noexcept;
bool write_texture_at(Stream &stream, const TexelCopyTexture &place, std::array<std::uint32_t, 2> layout, std::array<std::uint32_t, 3> size, std::span<const std::uint8_t> bytes) noexcept;
// An encoded image (PNG, JPEG, WebP) where it lies in the guest's memory,
// decoded into `to`; `word` where `ticket | DONE` lands once it is there.
bool copy_external_image(Stream &stream, const TexelCopyTexture &to, bool flip_y, bool premultiplied, std::span<const std::uint8_t> image, std::uint32_t &word, std::uint32_t ticket) noexcept;
// `from` and `to`: a buffer and its offset.
bool copy_buffer_to_buffer(Stream &stream, Id encoder, std::array<std::uint32_t, 2> from, std::array<std::uint32_t, 2> to, std::uint32_t size) noexcept;
bool copy_buffer_to_texture(Stream &stream, Id encoder, const TexelCopyBuffer &from, const TexelCopyTexture &to, std::array<std::uint32_t, 3> size) noexcept;
bool copy_texture_to_buffer(Stream &stream, Id encoder, const TexelCopyTexture &from, const TexelCopyBuffer &to, std::array<std::uint32_t, 3> size) noexcept;
bool copy_texture_to_texture(Stream &stream, Id encoder, const TexelCopyTexture &from, const TexelCopyTexture &to, std::array<std::uint32_t, 3> size) noexcept;
bool clear_buffer(Stream &stream, Id encoder, Id buffer, std::uint32_t offset, std::optional<std::uint32_t> size) noexcept;
bool resolve_query_set(Stream &stream, Id encoder, Id query_set, std::uint32_t first, std::uint32_t count, Id buffer, std::uint32_t offset) noexcept;
// A render or a compute pipeline, as the pass is.
bool set_pipeline(Stream &stream, Id pass, Id pipeline) noexcept;
bool set_bind_group(Stream &stream, Id pass, std::uint32_t index, Id group, std::span<const std::uint32_t> dynamic_offsets = {}) noexcept;
bool set_vertex_buffer(Stream &stream, Id pass, std::uint32_t slot, Id buffer, std::uint32_t offset, std::optional<std::uint32_t> size) noexcept;
bool set_index_buffer(Stream &stream, Id pass, Id buffer, IndexFormat format, std::uint32_t offset, std::optional<std::uint32_t> size) noexcept;
// `rect`: x, y, width, height; `depth`: min and max.
bool set_viewport(Stream &stream, Id pass, std::array<float, 4> rect, std::array<float, 2> depth) noexcept;
bool set_scissor(Stream &stream, Id pass, std::array<std::uint32_t, 4> rect) noexcept;
bool set_blend_constant(Stream &stream, Id pass, std::array<float, 4> rgba) noexcept;
bool set_stencil_reference(Stream &stream, Id pass, std::uint32_t reference) noexcept;
bool draw(Stream &stream, Id pass, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first_vertex, std::uint32_t first_instance) noexcept;
bool draw_indexed(Stream &stream, Id pass, std::uint32_t indices, std::uint32_t instances, std::uint32_t first_index, std::int32_t base_vertex, std::uint32_t first_instance) noexcept;
bool draw_indirect(Stream &stream, Id pass, Id buffer, std::uint32_t offset) noexcept;
bool draw_indexed_indirect(Stream &stream, Id pass, Id buffer, std::uint32_t offset) noexcept;
bool dispatch(Stream &stream, Id pass, std::array<std::uint32_t, 3> workgroups) noexcept;
bool dispatch_indirect(Stream &stream, Id pass, Id buffer, std::uint32_t offset) noexcept;
// `buffer` (MAP_READ | COPY_DST, copied into by an encoder submitted before)
// read into the guest's memory at `into`; `ticket | DONE` lands at `word` once
// the bytes are there and the buffer is unmapped.
bool map_async(Stream &stream, Id buffer, std::uint32_t offset, std::span<std::uint8_t> into, std::uint32_t &word, std::uint32_t ticket) noexcept;
// `ticket | DONE` at `word` once what was submitted before is done.
bool queue_done(Stream &stream, std::uint32_t &word, std::uint32_t ticket) noexcept;
// `format` none for the format the host's surface prefers.
bool configure(Stream &stream, std::uint32_t surface, std::optional<TextureFormat> format, std::uint32_t usage, CanvasAlphaMode alpha) noexcept;
bool resize(Stream &stream, std::uint32_t surface, std::array<std::uint32_t, 2> size) noexcept;
// Where the host writes the device's limits, features and status.
bool device(Stream &stream, std::span<std::uint32_t> into) noexcept;

} // namespace call

} // namespace xpute::gpu
