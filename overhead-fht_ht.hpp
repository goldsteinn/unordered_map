#ifndef _FHT_HT_H_
#define _FHT_HT_H_

#include <assert.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string>
#include <type_traits>


// if using big pages might want to use a seperate allocator and redefine
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096

#define LOCAL_PAGE_SIZE_DEFINE
#endif

// make sure these are correct. Something like $> cat /proc/cpuinfo should give
// you everything you need
#ifndef L1_CACHE_LINE_SIZE

#define L1_CACHE_LINE_SIZE     (64UL)
#define L1_LOG_CACHE_LINE_SIZE 6

#define LOCAL_CACHE_SIZE_DEFINE

#endif

#define FHT_TAGS_PER_CLINE L1_CACHE_LINE_SIZE

//////////////////////////////////////////////////////////////////////
// Table params
// return values

const uint64_t FHT_NOT_ERASED = 0;
const uint64_t FHT_ERASED     = 1;


// tunable

// whether keys/vals passed in can be constructed with std::move
//#define DESTROYABLE_INSERT
#ifdef DESTROYABLE_INSERT
#define SRC_WRAPPER(X) std::move(X)
#else
#define SRC_WRAPPER(X) (X)
#endif

#define NEW(type, dst, src) (new ((void * const)(&(dst))) type(src))

// basically its a speedup to prefetch keys for larger node types and slowdown
// for smaller key types. Generally 8 has worked well go me but set to w.e
#define PREFETCH_BOUND 8
template<typename K>
static constexpr
    typename std::enable_if<(sizeof(K) > PREFETCH_BOUND), void>::type
    __attribute__((const)) __attribute__((always_inline))
    prefetch(const void * const ptr) {
    __builtin_prefetch(ptr);
}

template<typename K>
static constexpr
    typename std::enable_if<!(sizeof(K) > PREFETCH_BOUND), void>::type
    __attribute__((const)) __attribute__((always_inline))
    prefetch(const void * const ptr) {}


// when to change pass by from actual value to reference
#define FHT_PASS_BY_VAL_THRESH 8

// special types have keys / values stored in seperate memory. This will
// increase delete/query performance (at a slight cost to insert). Importantly
// it will change the return of the iterators operator* and operator-> to return
// fht_special_pair. This can be used to access key/value but only with
// get<uint32_t>(). first/second field DO NOT EXIST in fht_special_pair
struct _fht_empty_t {};
#define FHT_SPECIAL_TYPES _fht_empty_t
//////////////////////////////////////////////////////////////////////

// tunable but less important

// max memory willing to use (this doesn't really have effect with default
// allocator)
const uint64_t FHT_DEFAULT_INIT_MEMORY = ((1UL) << 35);

// default init size (since mmap is backend for allocation less than page size
// has no effect)
const uint32_t FHT_DEFAULT_INIT_SIZE = PAGE_SIZE;

// literally does not matter unless you care about universal hashing or have
// some fancy shit in mind
const uint32_t FHT_HASH_SEED = 0;


//////////////////////////////////////////////////////////////////////
// These macros have no real place in this file. I feel the clutter they will
// add warrants a seperate .h
#include "FHT_SPECIAL_TYPE_MACROS.h"

//////////////////////////////////////////////////////////////////////
// SSE / tags stuff
// necessary includes
#include <emmintrin.h>
#include <immintrin.h>
#include <pmmintrin.h>
#include <smmintrin.h>


static const int8_t INVALID_MASK = ((int8_t)0x80);
static const int8_t ERASED_MASK  = ((int8_t)0xC0);
static const int8_t CONTENT_MASK = ((int8_t)0x7F);
#define CONTENT_BITS 7

#define FHT_IS_ERASED(tag)  (((tag)) == ERASED_MASK)
#define FHT_SET_ERASED(tag) ((tag) = ERASED_MASK)

#define FHT_MM_SET(X)          _mm_set1_epi8(X)
#define FHT_MM_MASK(X, Y)      ((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(X, Y)))
#define FHT_MM_EMPTY(X)        ((uint32_t)_mm_movemask_epi8(_mm_sign_epi8(X, X)))
#define FHT_MM_EMPTY_OR_DEL(X) ((uint32_t)_mm_movemask_epi8(X))


static const __m256i FHT_RESET_VEC = _mm256_set1_epi8(INVALID_MASK);


static const uint32_t FHT_MM_LINE      = FHT_TAGS_PER_CLINE / sizeof(__m128i);
static const uint32_t FHT_MM_LINE_MASK = FHT_MM_LINE - 1;

static const uint32_t FHT_MM_IDX_MULT = FHT_TAGS_PER_CLINE / FHT_MM_LINE;
static const uint32_t FHT_MM_IDX_MASK = FHT_MM_IDX_MULT - 1;

//////////////////////////////////////////////////////////////////////
// manipulation of resize of hash_val
#define FHT_TO_MASK(n) ((((hash_type_t)1) << ((n))) - 1)

// get nths bith

#define FHT_GET_NTH_BIT(X, n)                                                  \
    ((((X) >> (CONTENT_BITS - L1_LOG_CACHE_LINE_SIZE)) >> ((n))) & 0x1)

#define FHT_HASH_TO_IDX(hash_val, tbl_log)                                     \
    ((((hash_val) >> (CONTENT_BITS - L1_LOG_CACHE_LINE_SIZE)) &                \
      FHT_TO_MASK(tbl_log)) /                                                  \
     FHT_TAGS_PER_CLINE)

#define FHT_GEN_TAG(hash_val) ((hash_val)&CONTENT_MASK)
#define FHT_GEN_START_IDX(hash_val)                                            \
    (const uint32_t)((hash_val) >> (8 * sizeof(hash_type_t) - 2))
//////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////
// forward declaration of default helper struct
// near their implementations below are some alternatives ive written

// Must define:
// const uint32_t operator()(K) or const uint32_t operator()(K const &)
template<typename K>
struct DEFAULT_HASH_32;

// const uint64_t operator()(K) or const uint64_t operator()(K const &)
template<typename K>
struct DEFAULT_HASH_64;

// remaps each time table resizes
template<typename K, typename V>
struct DEFAULT_MMAP_ALLOC;

// this will perform significantly better if the copy time on either keys or
// values is large (it tries to avoid at least a portion of the copying step in
// resize)
template<typename K, typename V>
struct INPLACE_MMAP_ALLOC;

//////////////////////////////////////////////////////////////////////
// helpers

// intentionally not specify for inline as this is far from critical path and
// compiler knows better if its going to bloat executable
static uint64_t __attribute__((const)) log_b2(uint64_t n) {
    uint64_t s, t;
    t = (n > 0xffffffffUL) * (32);
    n >>= t;
    t = (n > 0xffffUL) * (16);
    n >>= t;
    s = (n > 0xffUL) * (8);
    n >>= s, t |= s;
    s = (n > 0xfUL) * (4);
    n >>= s, t |= s;
    s = (n > 0x3UL) * (2);
    n >>= s, t |= s;
    return (t | (n / 2));
}

// intentionally not specify for inline as this is far from critical path and
// compiler knows better if its going to bloat executable
static uint64_t __attribute__((const)) roundup_next_p2(uint64_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

// these get optimized to popcnt. For resize
static inline uint32_t
bitcount_32(uint32_t v) {
    uint32_t c;
    c = v - ((v >> 1) & 0x55555555);
    c = ((c >> 2) & 0x33333333) + (c & 0x33333333);
    c = ((c >> 4) + c) & 0x0F0F0F0F;
    c = ((c >> 8) + c) & 0x00FF00FF;
    c = ((c >> 16) + c) & 0x0000FFFF;
    return c;
}


//////////////////////////////////////////////////////////////////////
// Really just typedef structs
template<typename K, typename V>
struct fht_node {
    K key;
    V val;
};


// node layout for smaller keys
template<typename K, typename V>
struct fht_seperate_kv {
    K keys[L1_CACHE_LINE_SIZE];
    V vals[L1_CACHE_LINE_SIZE];
};


// node layout for larger keys
template<typename K, typename V>
struct fht_combined_kv {
    fht_node<K, V> nodes[L1_CACHE_LINE_SIZE];
};

// chunk containing cache line of tags and a single node (either fht_combined_kv
// or fht_seperate_kv). Either way each chunk contains bytes per cache line
// number of key value pairs (on most 64 bit machines this will mean 64 Key
// value pairs)
template<typename K, typename V>
struct fht_chunk {


    // determine best way to pass K/V depending on size. Generally passing
    // values machine word size is ill advised
    template<typename T>
    using pass_type_t =
        typename std::conditional<(std::is_arithmetic<T>::value ||
                                   std::is_pointer<T>::value),
                                  const T,
                                  const T &>::type;

    // determine node type based on K/V
    template<typename _K = K, typename _V = V>
    using _node_t = typename std::conditional<FHT_SPECIAL_CONDITIONAL,
                                              fht_seperate_kv<_K, _V>,
                                              fht_combined_kv<_K, _V>>::type;


    // typedefs to fht_table can access these variables
    typedef pass_type_t<K> key_pass_t;
    typedef pass_type_t<V> val_pass_t;
    typedef _node_t<K, V>  node_t;

    // actual content of chunk
    const __m128i tags_vec[FHT_MM_LINE];
    node_t        nodes;

    inline constexpr uint32_t __attribute__((always_inline))
    get_del(const uint32_t idx) const {
        return FHT_MM_EMPTY_OR_DEL(this->tags_vec[idx]);
    }

    inline constexpr uint32_t __attribute__((always_inline))
    get_empty_or_del(const uint32_t idx) const {
        return FHT_MM_EMPTY_OR_DEL(this->tags_vec[idx]);
    }

    inline constexpr uint32_t __attribute__((always_inline))
    get_empty(const uint32_t idx) const {
        return FHT_MM_EMPTY(this->tags_vec[idx]);
    }

    inline constexpr uint32_t __attribute__((always_inline))
    is_deleted_n(const uint32_t n) const {
        return FHT_IS_ERASED(((const int8_t * const)this->tags_vec)[n]);
    }

    inline void constexpr __attribute__((always_inline))
    delete_tag_n(const uint32_t n) {
        FHT_SET_ERASED(((int8_t * const)this->tags_vec)[n]);
    }

    // this undeletes
    inline constexpr void __attribute__((always_inline))
    set_tag_n(const uint32_t n, const int8_t new_tag) {
        ((int8_t * const)this->tags_vec)[n] = new_tag;
    }


    // the following exist for key/val in a far more complicated format
    inline constexpr int8_t __attribute__((always_inline))
    get_tag_n(const uint32_t n) const {
        return ((const int8_t * const)this->tags_vec)[n];
    }


    // overloaded key/value helpers
    //////////////////////////////////////////////////////////////////////
    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<FHT_SPECIAL_CONDITIONAL, key_pass_t>::type
        __attribute__((always_inline)) get_key_n(const uint32_t n) const {
        return this->nodes.keys[n];
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<FHT_SPECIAL_CONDITIONAL, const uint32_t>::type
        __attribute__((always_inline))
        compare_key_n(const uint32_t n, key_pass_t other_key) const {
        return this->nodes.keys[n] == other_key;
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<FHT_SPECIAL_CONDITIONAL, val_pass_t>::type
        __attribute__((always_inline)) get_val_n(const uint32_t n) const {
        return this->nodes.vals[n];
    }


    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<FHT_SPECIAL_CONDITIONAL, const K * const>::type
        __attribute__((always_inline)) get_key_n_ptr(const uint32_t n) const {
        return (const K * const)(&(this->nodes.keys[n]));
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<FHT_SPECIAL_CONDITIONAL, V * const>::type
        __attribute__((always_inline)) get_val_n_ptr(const uint32_t n) const {
        return (V * const)(&(this->nodes.vals[n]));
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<(FHT_SPECIAL_CONDITIONAL &&
                                 sizeof(_V) <= FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        val_pass_t     new_val) {

        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.keys[n], SRC_WRAPPER(new_key));
        NEW(V, this->nodes.vals[n], SRC_WRAPPER(new_val));
    }


    template<typename _K = K, typename _V = V, typename... Args>
    inline constexpr
        typename std::enable_if<(FHT_SPECIAL_CONDITIONAL &&
                                 sizeof(_V) > FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        Args &&... args) {
        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.keys[n], SRC_WRAPPER(new_key));
        NEW(V, this->nodes.vals[n], std::forward<Args>(args)...);
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<(FHT_SPECIAL_CONDITIONAL &&
                                 sizeof(_V) > FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        const V &      new_val) {
        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.keys[n], SRC_WRAPPER(new_key));
        NEW(V, this->nodes.vals[n], SRC_WRAPPER(new_val));
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<(FHT_SPECIAL_CONDITIONAL &&
                                 sizeof(_V) > FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        V &            new_val) {
        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.keys[n], SRC_WRAPPER(new_key));
        NEW(V, this->nodes.vals[n], SRC_WRAPPER(new_val));
    }


    //////////////////////////////////////////////////////////////////////
    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<((!FHT_SPECIAL_CONDITIONAL)), key_pass_t>::type
        __attribute__((always_inline)) get_key_n(const uint32_t n) const {
        return this->nodes.nodes[n].key;
    }

    template<typename _K = K, typename _V = V>
    inline constexpr typename std::enable_if<((!FHT_SPECIAL_CONDITIONAL)),
                                             const uint32_t>::type
        __attribute__((always_inline))
        compare_key_n(const uint32_t n, key_pass_t other_key) const {
        return this->nodes.nodes[n].key == other_key;
    }


    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<((!FHT_SPECIAL_CONDITIONAL)), val_pass_t>::type
        __attribute__((always_inline)) get_val_n(const uint32_t n) const {
        return this->nodes.nodes[n].val;
    }


    template<typename _K = K, typename _V = V>
    inline constexpr typename std::enable_if<((!FHT_SPECIAL_CONDITIONAL)),
                                             const K * const>::type
        __attribute__((always_inline)) get_key_n_ptr(const uint32_t n) const {
        return (const K * const)(&(this->nodes.nodes[n].key));
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<((!FHT_SPECIAL_CONDITIONAL)), V * const>::type
        __attribute__((always_inline)) get_val_n_ptr(const uint32_t n) const {
        return (V * const)(&(this->nodes.nodes[n].val));
    }


    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<(((!FHT_SPECIAL_CONDITIONAL)) &&
                                 sizeof(_V) <= FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        val_pass_t     new_val) {
        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.nodes[n].key, SRC_WRAPPER(new_key));
        NEW(V, this->nodes.nodes[n].val, SRC_WRAPPER(new_val));
    }

    template<typename _K = K, typename _V = V, typename... Args>
    inline constexpr
        typename std::enable_if<(((!FHT_SPECIAL_CONDITIONAL)) &&
                                 sizeof(_V) > FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        Args &&... args) {
        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.nodes[n].key, SRC_WRAPPER(new_key));
        NEW(V, this->nodes.nodes[n].val, std::forward<Args>(args)...);
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<(((!FHT_SPECIAL_CONDITIONAL)) &&
                                 sizeof(_V) > FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        const V &      new_val) {

        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.nodes[n].key, SRC_WRAPPER(new_key));
        NEW(V, this->nodes.nodes[n].val, SRC_WRAPPER(new_val));
    }

    template<typename _K = K, typename _V = V>
    inline constexpr
        typename std::enable_if<(((!FHT_SPECIAL_CONDITIONAL)) &&
                                 sizeof(_V) > FHT_PASS_BY_VAL_THRESH),
                                void>::type __attribute__((always_inline))
        set_key_val_tag(const uint32_t n,
                        const int8_t   tag,
                        key_pass_t     new_key,
                        V &            new_val) {
        ((int8_t * const)this->tags_vec)[n] = tag;
        NEW(K, this->nodes.nodes[n].key, SRC_WRAPPER(new_key));
        NEW(V, this->nodes.nodes[n].val, SRC_WRAPPER(new_val));
    }
};


//////////////////////////////////////////////////////////////////////
// "pair" for iterator return if special type key is being used. get<1/2>()
// functionality is same
template<typename K, typename V>
struct fht_special_pair {
    const int8_t * const cur_tag;

    fht_special_pair() {
        fprintf(stderr, "This call should NEVER be constructed manually!\n");
        assert(0);
    }
    ~fht_special_pair() {}

    inline const K &
    first() const {
        const fht_chunk<K, V> * const cur_chunk =
            (const fht_chunk<K, V> * const)(((uint64_t)(this->cur_tag)) &
                                            (~(FHT_TAGS_PER_CLINE - 1)));

        return *(cur_chunk->get_key_n_ptr(((uint64_t)(this->cur_tag)) &
                                          ((FHT_TAGS_PER_CLINE - 1))));
    }

    inline V &
    second() const {
        const fht_chunk<K, V> * const cur_chunk =
            (const fht_chunk<K, V> * const)(((uint64_t)(this->cur_tag)) &
                                            (~(FHT_TAGS_PER_CLINE - 1)));

        return *(cur_chunk->get_val_n_ptr(((uint64_t)(this->cur_tag)) &
                                          ((FHT_TAGS_PER_CLINE - 1))));
    }


    template<uint32_t I>
    inline constexpr typename std::enable_if<(I == 0), const K &>::type
    get() const {
        return this->first();
    }

    template<uint32_t I>
    inline constexpr typename std::enable_if<(I == 1), V &>::type
    get() const {
        return this->second();
    }

    template<typename... Args>
    inline constexpr void
    set_val(Args &&... args) const {
        const fht_chunk<K, V> * const cur_chunk =
            (const fht_chunk<K, V> * const)(((uint64_t)(this->cur_tag)) &
                                            (~(FHT_TAGS_PER_CLINE - 1)));

        NEW(V,
            *(cur_chunk->get_val_n_ptr(((uint64_t)(this->cur_tag)) &
                                       ((FHT_TAGS_PER_CLINE - 1)))),
            std::forward<Args>(args)...);
    }


    std::pair<K, V>
    as_pair() {
        return std::pair<K, V>(this->first(), this->second());
    }

    //////////////////////////////////////////////////////////////////////
    // == operators with std::pair<K, V>
    inline friend bool
    operator==(const fht_special_pair<K, V> & fht_a, std::pair<K, V> && std_b) {
        return std_b.first == fht_a.first() && std_b.second == fht_a.second();
    }

    inline friend bool
    operator==(const fht_special_pair<K, V> & fht_a,
               const std::pair<K, V> &        std_b) {
        return std_b.first == fht_a.first() && std_b.second == fht_a.second();
    }

    inline friend bool
    operator==(std::pair<K, V> && std_b, const fht_special_pair<K, V> & fht_a) {
        return std_b.first == fht_a.first() && std_b.second == fht_a.second();
    }

    inline friend bool
    operator==(const std::pair<K, V> &        std_b,
               const fht_special_pair<K, V> & fht_a) {
        return std_b.first == fht_a.first() && std_b.second == fht_a.second();
    }
    //////////////////////////////////////////////////////////////////////
    // != operators with std::pair<K, V>

    inline friend bool
    operator!=(const fht_special_pair<K, V> & fht_a, std::pair<K, V> && std_b) {
        return std_b.first != fht_a.first() || std_b.second != fht_a.second();
    }

    inline friend bool
    operator!=(const fht_special_pair<K, V> & fht_a,
               const std::pair<K, V> &        std_b) {
        return std_b.first != fht_a.first() || std_b.second != fht_a.second();
    }

    inline friend bool
    operator!=(std::pair<K, V> && std_b, const fht_special_pair<K, V> & fht_a) {
        return std_b.first != fht_a.first() || std_b.second != fht_a.second();
    }

    inline friend bool
    operator!=(const std::pair<K, V> &        std_b,
               const fht_special_pair<K, V> & fht_a) {
        return std_b.first != fht_a.first() || std_b.second != fht_a.second();
    }

    inline friend bool
    operator==(const fht_special_pair<K, V> & fht_a,
               const fht_special_pair<K, V> & fht_b) {
        return fht_b.first() == fht_a.first() &&
               fht_b.second() == fht_a.second();
    }

    inline friend bool
    operator!=(const fht_special_pair<K, V> & fht_a,
               const fht_special_pair<K, V> & fht_b) {
        return fht_b.first() != fht_a.first() ||
               fht_b.second() != fht_a.second();
    }
};
//////////////////////////////////////////////////////////////////////
// would be nice to implement in 4 bytes so iterator + bool fits in
// register
template<typename K, typename V, typename fht_it_pair>
struct fht_iterator_t {

    const int8_t * cur_tag;

    fht_iterator_t(const int8_t * const init_tag_pos) {
        this->cur_tag = init_tag_pos;
    }

    fht_iterator_t(const int8_t * init_tag_pos, const uint64_t end) {
        // initialization of new iterator (not from find but from begin) so that
        // it starts at a valid slot
        while (((uint64_t)init_tag_pos) < end &&
               (*init_tag_pos) & INVALID_MASK) {
            if (__builtin_expect(
                    ((uint64_t)(init_tag_pos) % FHT_TAGS_PER_CLINE) ==
                        (FHT_TAGS_PER_CLINE - 1),
                    0)) {
                init_tag_pos += (sizeof(fht_chunk<K, V>) - FHT_TAGS_PER_CLINE);
            }
            ++init_tag_pos;
        }
        this->cur_tag = init_tag_pos;
    }

    inline fht_iterator_t &
    operator=(const fht_iterator_t & other) {
        this->cur_tag = other.cur_tag;
        return *this;
    }

    fht_iterator_t &
    operator++() {
        do {
            if (__builtin_expect(
                    ((uint64_t)(this->cur_tag) % FHT_TAGS_PER_CLINE) ==
                        (FHT_TAGS_PER_CLINE - 1),
                    0)) {
                this->cur_tag += (sizeof(fht_chunk<K, V>) - FHT_TAGS_PER_CLINE);
            }
            this->cur_tag++;
        } while ((*(this->cur_tag)) & INVALID_MASK);
        return *this;
    }

    inline fht_iterator_t &
    operator++(int) {
        ++(this);
        return *this;
    }

    inline fht_iterator_t &
    operator+=(uint32_t n) {
        while (n) {
            this ++;
        }
    }

    fht_iterator_t &
    operator--() {
        do {
            if (__builtin_expect(
                    ((uint64_t)(this->cur_tag) % FHT_TAGS_PER_CLINE) == 0,
                    0)) {
                this->cur_tag -= sizeof(typename fht_chunk<K, V>::node_t);
            }
            this->cur_tag--;
        } while ((*(this->cur_tag)) & INVALID_MASK);
        return *this;
    }

    inline fht_iterator_t &
    operator--(int) {
        --(*this);
        return *this;
    }

    inline fht_iterator_t &
    operator-=(uint32_t n) {
        while (n) {
            this --;
        }
    }

    template<typename pair_type = fht_it_pair>
    inline constexpr
        typename std::enable_if<std::is_same<pair_type, std::pair<K, V>>::value,
                                const fht_it_pair *>::type
        to_address() const {
        // basically if we are using std::pair go to the actual pair,
        // fht_it_pair is basically just and extension of it with K / V
        // getting functionality
        return ((const fht_it_pair *)(((uint64_t)(this->cur_tag +
                                                  FHT_TAGS_PER_CLINE)) &
                                      (~(FHT_TAGS_PER_CLINE - 1)))) +
               (((uint64_t)(this->cur_tag)) & (FHT_TAGS_PER_CLINE - 1));
    }

    template<typename pair_type = fht_it_pair>
    inline constexpr typename std::enable_if<
        !(std::is_same<pair_type, std::pair<K, V>>::value),
        const fht_it_pair *>::type
    to_address() const {
        // basically if we are using std::pair go to the actual pair,
        // fht_it_pair is basically just and extension of it with K / V
        // getting functionality
        return ((const fht_it_pair *)this);
    }


    inline const fht_it_pair & operator*() const {
        return *(this->to_address());
    }


    inline const fht_it_pair * operator->() const {
        return this->to_address();
    }


    inline friend bool
    operator==(const fht_iterator_t & it_a, const fht_iterator_t & it_b) {
        return ((uint64_t)(it_a.cur_tag)) == ((uint64_t)(it_b.cur_tag));
    }

    inline friend bool
    operator!=(const fht_iterator_t & it_a, const fht_iterator_t & it_b) {
        return ((uint64_t)(it_a.cur_tag)) != ((uint64_t)(it_b.cur_tag));
    }

    inline friend bool
    operator<(const fht_iterator_t & it_a, const fht_iterator_t & it_b) {
        return ((uint64_t)(it_a.cur_tag)) < ((uint64_t)(it_b.cur_tag));
    }

    inline friend bool
    operator>(const fht_iterator_t & it_a, const fht_iterator_t & it_b) {
        return ((uint64_t)(it_a.cur_tag)) > ((uint64_t)(it_b.cur_tag));
    }

    inline friend bool
    operator<=(const fht_iterator_t & it_a, const fht_iterator_t & it_b) {
        return ((uint64_t)(it_a.cur_tag)) <= ((uint64_t)(it_b.cur_tag));
    }

    inline friend bool
    operator>=(const fht_iterator_t & it_a, const fht_iterator_t & it_b) {
        return ((uint64_t)(it_a.cur_tag)) >= ((uint64_t)(it_b.cur_tag));
    }
};

//////////////////////////////////////////////////////////////////////
// Table class
template<typename K,
         typename V,
         typename Hasher    = DEFAULT_HASH_64<K>,
         typename Allocator = INPLACE_MMAP_ALLOC<K, V>>
struct fht_table {


    // log of table size
    uint32_t log_incr;
    uint32_t npairs;

    // chunk array
    fht_chunk<K, V> * chunks;

    // helper classes
    Hasher    hash;
    Allocator alloc_mmap;

    //////////////////////////////////////////////////////////////////////
    template<typename _K = K, typename _Hasher = Hasher>
    using _hash_type_t = typename std::result_of<_Hasher(K)>::type;
    typedef _hash_type_t<K, Hasher> hash_type_t;

    using key_pass_t = typename fht_chunk<K, V>::key_pass_t;
    using val_pass_t = typename fht_chunk<K, V>::val_pass_t;

    template<typename _K = K, typename _V = V>
    using _fht_iterator_t = typename std::conditional<
        (!FHT_SPECIAL_CONDITIONAL),
        fht_iterator_t<_K, _V, std::pair<_K, _V>>,
        fht_iterator_t<_K, _V, fht_special_pair<_K, _V>>>::type;

    typedef _fht_iterator_t<K, V> fht_iterator;
    //////////////////////////////////////////////////////////////////////
    fht_table(const uint64_t init_size) {

        // ensure init_size is above min
        const uint64_t _init_size = init_size > FHT_DEFAULT_INIT_SIZE
                                        ? roundup_next_p2(init_size)
                                        : FHT_DEFAULT_INIT_SIZE;

        const uint32_t _log_init_size = (const uint32_t)log_b2(_init_size);
        //    int *  test = Allocator::new (NULL) int;

        // alloc chunks
        this->chunks =
            this->alloc_mmap.allocate((_init_size / FHT_TAGS_PER_CLINE));

        // might be faster with _m512 but this is a mile from any critical path
        // and makes less portable
        for (uint32_t i = 0; i < (_init_size / FHT_TAGS_PER_CLINE); i++) {
            for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
                ((__m256i * const)(this->chunks + i))[0] = FHT_RESET_VEC;
                ((__m256i * const)(this->chunks + i))[1] = FHT_RESET_VEC;
            }
            this->log_incr = _log_init_size;
            this->npairs   = 0;
        }
    }
    fht_table() : fht_table(FHT_DEFAULT_INIT_SIZE) {}


    ~fht_table() {
        this->alloc_mmap.deallocate(
            this->chunks,
            (((1UL) << (this->log_incr)) / FHT_TAGS_PER_CLINE));
    }


    //////////////////////////////////////////////////////////////////////
    // very basic info
    inline constexpr bool
    empty() const {
        return !(this->npairs);
    }
    inline constexpr uint64_t
    size() const {
        return this->npairs;
    }
    inline constexpr uint64_t
    max_size() const {
        return (1UL) << this->log_size;
    }

    inline constexpr double
    load_factor() const {
        return ((double)this->size()) / ((double)this->max_size());
    }

    // aint gunna do anything about this
    inline constexpr double
    max_load_factor() const {
        return 1.0;
    }
    //////////////////////////////////////////////////////////////////////

    //////////////////////////////////////////////////////////////////////
    // rehashing
    template<typename _K         = K,
             typename _V         = V,
             typename _Hasher    = Hasher,
             typename _Allocator = Allocator>
    typename std::enable_if<
        std::is_same<_Allocator, INPLACE_MMAP_ALLOC<_K, _V>>::value,
        void>::type
    rehash() {

        // incr table log
        const uint32_t          _new_log_incr = ++(this->log_incr);
        fht_chunk<K, V> * const old_chunks    = this->chunks;


        const uint32_t _num_chunks =
            ((1u) << (_new_log_incr - 1)) / FHT_TAGS_PER_CLINE;


        // allocate new chunk array
        fht_chunk<K, V> * const new_chunks =
            this->alloc_mmap.allocate(_num_chunks);


        uint32_t to_move = 0;
        uint32_t new_starts;
        uint32_t old_start_good_slots;
        // iterate through all chunks and re-place nodes
        for (uint32_t i = 0; i < _num_chunks; i++) {
            new_starts           = 0;
            old_start_good_slots = 0;

            uint32_t old_start_pos[FHT_MM_LINE]     = { 0 };
            uint64_t old_start_to_move[FHT_MM_LINE] = { 0 };


            fht_chunk<K, V> * const old_chunk = old_chunks + i;
            fht_chunk<K, V> * const new_chunk = new_chunks + i;

            // all intents and purposes not an important optimization but faster
            // way to reset deleted
            __m256i * const set_tags_vec =
                (__m256i * const)(old_chunk->tags_vec);

            //            __m256i * const set_tags_vec_new =
            //                (__m256i * const)(new_chunk->tags_vec);


            // turn all deleted tags -> INVALID (reset basically)
            set_tags_vec[0] = _mm256_min_epu8(set_tags_vec[0], FHT_RESET_VEC);
            set_tags_vec[1] = _mm256_min_epu8(set_tags_vec[1], FHT_RESET_VEC);

            //            set_tags_vec_new[0] = FHT_RESET_VEC;
            //            set_tags_vec_new[1] = FHT_RESET_VEC;

            uint64_t j_idx,
                iter_mask = ~(
                    (((uint64_t)_mm256_movemask_epi8(set_tags_vec[1])) << 32) |
                    (((uint32_t)_mm256_movemask_epi8(set_tags_vec[0])) &
                     (0xffffffffU)));

            while (iter_mask) {
                __asm__("tzcnt %1, %0" : "=r"((j_idx)) : "rm"((iter_mask)));
                iter_mask ^= ((1UL) << j_idx);


                // if node is invalid or deleted skip it. Can't just bvec iter
                // here because need to reset to invalid

                const hash_type_t raw_slot =
                    this->hash(old_chunk->get_key_n((const uint32_t)j_idx));
                const uint32_t start_idx = FHT_GEN_START_IDX(raw_slot);

                if (FHT_GET_NTH_BIT(raw_slot, _new_log_incr - 1)) {
                    const int8_t tag =
                        old_chunk->get_tag_n((const uint32_t)j_idx);
                    old_chunk->set_tag_n((const uint32_t)j_idx, INVALID_MASK);

                    // place new node w.o duplicate check
                    for (uint32_t new_j = 0; new_j < FHT_MM_LINE; new_j++) {
                        const uint32_t outer_idx =
                            (new_j + start_idx) & FHT_MM_LINE_MASK;
                        const uint32_t inner_idx =
                            (new_starts >> (8 * outer_idx)) & 0xff;

                        if (__builtin_expect(inner_idx != FHT_MM_IDX_MULT, 1)) {
                            const uint32_t true_idx =
                                FHT_MM_IDX_MULT * outer_idx + inner_idx;

                            ((int8_t * const)new_chunk->tags_vec)[true_idx] =
                                tag;

                            NEW(K,
                                *(new_chunk->get_key_n_ptr(true_idx)),
                                std::move(*(old_chunk->get_key_n_ptr(
                                    (const uint32_t)j_idx))));
                            NEW(V,
                                *(new_chunk->get_val_n_ptr(true_idx)),
                                std::move(*(old_chunk->get_val_n_ptr(
                                    (const uint32_t)j_idx))));


                            new_starts += ((1u) << (8 * outer_idx));
                            break;
                        }
                    }
                }
                else {
                    // unplaceable slots
                    old_start_pos[j_idx / FHT_MM_IDX_MULT] |=
                        ((1u) << (j_idx & FHT_MM_IDX_MASK));
                    if ((j_idx / FHT_MM_IDX_MULT) != start_idx) {
                        old_start_to_move[start_idx] |= ((1UL) << j_idx);
                        to_move |= ((1u) << start_idx);
                    }
                    else {
                        old_start_good_slots += ((1u) << (8 * start_idx));
                    }
                }
            }

            for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
                const uint32_t inner_idx = (new_starts >> (8 * j)) & 0xff;
                for (uint32_t _j = inner_idx; _j < FHT_MM_IDX_MULT; _j++) {
                    new_chunk->set_tag_n(j * FHT_MM_IDX_MULT + _j,
                                         INVALID_MASK);
                }
            }

            uint64_t to_move_idx;
            uint32_t to_place_idx;
            while (to_move) {
                uint32_t j;
                __asm__("tzcnt %1, %0" : "=r"((j)) : "rm"((to_move)));

                // has space and has items to move to it
                while (old_start_pos[j] != 0xffff && old_start_to_move[j]) {

                    __asm__("tzcnt %1, %0"
                            : "=r"((to_move_idx))
                            : "rm"((old_start_to_move[j])));

                    __asm__("tzcnt %1, %0"
                            : "=r"((to_place_idx))
                            : "rm"((~old_start_pos[j])));

                    old_start_to_move[j] ^= ((1UL) << to_move_idx);

                    const uint32_t true_idx =
                        FHT_MM_IDX_MULT * j + to_place_idx;

                    old_chunk->set_tag_n(
                        true_idx,
                        old_chunk->get_tag_n((const uint32_t)to_move_idx));


                    NEW(K,
                        *(old_chunk->get_key_n_ptr(true_idx)),
                        std::move(*(old_chunk->get_key_n_ptr(
                            (const uint32_t)to_move_idx))));

                    NEW(V,
                        *(old_chunk->get_val_n_ptr(true_idx)),
                        std::move(*(old_chunk->get_val_n_ptr(
                            (const uint32_t)to_move_idx))));


                    old_chunk->set_tag_n((const uint32_t)to_move_idx,
                                         INVALID_MASK);

                    old_start_good_slots += ((1u) << (8 * j));
                    old_start_pos[j] |= ((1u) << to_place_idx);
                    old_start_pos[to_move_idx / FHT_MM_IDX_MULT] ^=
                        ((1u) << (to_move_idx & FHT_MM_IDX_MASK));
                }
                // j is full of items that belong but more entries wants to be
                // in j
                if (__builtin_expect(old_start_to_move[j] &&
                                         ((old_start_good_slots >> (8 * j)) &
                                          0xff) == FHT_MM_IDX_MULT,
                                     0)) {

                    // move the indexes that this need to be placed (i.e if
                    // any of the nodes that needed to be moved to j (now
                    // j + 1) are already in j + 1 we can remove them from
                    // to_move list
                    old_start_to_move[(j + 1) & FHT_MM_LINE_MASK] |=
                        (~((0xffffUL) << (FHT_MM_IDX_MULT *
                                          ((j + 1) & FHT_MM_LINE_MASK)))) &
                        old_start_to_move[j];


                    const uint32_t new_mask =
                        (old_start_to_move[j] >>
                         (FHT_MM_IDX_MULT * ((j + 1) & FHT_MM_LINE_MASK))) &
                        0xffff;

                    old_start_pos[(j + 1) & FHT_MM_LINE_MASK] |= new_mask;
                    old_start_good_slots +=
                        bitcount_32(new_mask)
                        << (8 * ((j + 1) & FHT_MM_LINE_MASK));

                    // if j + 1 was done set it back
                    if (old_start_to_move[(j + 1) & FHT_MM_LINE_MASK]) {
                        to_move |= ((1u) << ((j + 1) & FHT_MM_LINE_MASK));
                    }
                    old_start_to_move[j] = 0;
                    to_move ^= ((1u) << j);
                }
                else if (__builtin_expect(!old_start_to_move[j], 1)) {
                    to_move ^= ((1u) << j);
                }
            }
        }
    }


    // standard rehash which copies all elements
    template<typename _K         = K,
             typename _V         = V,
             typename _Hasher    = Hasher,
             typename _Allocator = Allocator>
    typename std::enable_if<
        !(std::is_same<_Allocator, INPLACE_MMAP_ALLOC<_K, _V>>::value),
        void>::type
    rehash() {

        // incr table log
        const uint32_t                _new_log_incr = ++(this->log_incr);
        const fht_chunk<K, V> * const old_chunks    = this->chunks;


        const uint32_t _num_chunks =
            ((1u) << (_new_log_incr - 1)) / FHT_TAGS_PER_CLINE;

        // allocate new chunk array
        fht_chunk<K, V> * const new_chunks =
            this->alloc_mmap.allocate(2 * _num_chunks);

        // set this while its definetly still in cache
        this->chunks = new_chunks;


        // iterate through all chunks and re-place nodes
        for (uint32_t i = 0; i < _num_chunks; i++) {
            uint64_t slot_idx = 0;

            const fht_chunk<K, V> * const old_chunk = old_chunks + i;

            // which one is optimal here really depends on the quality of the
            // hash function.

            uint64_t taken_slots, j_idx;

            const uint32_t temp_taken_slots =
                (old_chunk->get_empty_or_del(1) << 16) |
                (old_chunk->get_empty_or_del(0) & 0xffff);

            taken_slots = (old_chunk->get_empty_or_del(3) << 16) |
                          (old_chunk->get_empty_or_del(2) & 0xffff);


            taken_slots = (taken_slots << 32) | temp_taken_slots;
            taken_slots = ~taken_slots;

            while (taken_slots) {
                __asm__("tzcnt %1, %0" : "=r"((j_idx)) : "rm"((taken_slots)));
                taken_slots ^= ((1UL) << j_idx);


                const hash_type_t raw_slot =
                    this->hash(old_chunk->get_key_n((const uint32_t)j_idx));
                const uint32_t start_idx = FHT_GEN_START_IDX(raw_slot);
                const uint32_t nth_bit =
                    FHT_GET_NTH_BIT(raw_slot, _new_log_incr - 1);

                // 50 50 of hashing to same slot or slot + .5 * new table size
                fht_chunk<K, V> * const new_chunk =
                    new_chunks + (i | (nth_bit ? _num_chunks : 0));

                // place new node w.o duplicate check
                for (uint32_t new_j = 0; new_j < FHT_MM_LINE; new_j++) {
                    const uint32_t outer_idx =
                        (new_j + start_idx) & FHT_MM_LINE_MASK;
                    const uint32_t inner_idx =
                        (slot_idx >> (8 * outer_idx + 32 * nth_bit)) & 0xff;

                    if (__builtin_expect(inner_idx != FHT_MM_IDX_MULT, 1)) {
                        const uint32_t true_idx =
                            FHT_MM_IDX_MULT * outer_idx + inner_idx;

                        new_chunk->set_tag_n(
                            true_idx,
                            old_chunk->get_tag_n((const uint32_t)j_idx));
                        NEW(K,
                            *(new_chunk->get_key_n_ptr(true_idx)),
                            std::move(*(old_chunk->get_key_n_ptr(
                                (const uint32_t)j_idx))));
                        NEW(V,
                            *(new_chunk->get_val_n_ptr(true_idx)),
                            std::move(*(old_chunk->get_val_n_ptr(
                                (const uint32_t)j_idx))));


                        slot_idx += ((1UL) << (8 * outer_idx + 32 * nth_bit));
                        break;
                    }
                }
            }
            // set remaining to INVALID_MASK
            for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
                const uint32_t inner_idx = (slot_idx >> (8 * j)) & 0xff;
                for (uint32_t _j = inner_idx; _j < FHT_MM_IDX_MULT; _j++) {
                    new_chunks[i].set_tag_n(FHT_MM_IDX_MULT * j + _j,
                                            INVALID_MASK);
                }
            }
            for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
                const uint32_t inner_idx = (slot_idx >> (8 * j + 32)) & 0xff;
                for (uint32_t _j = inner_idx; _j < FHT_MM_IDX_MULT; _j++) {
                    new_chunks[i | _num_chunks].set_tag_n(
                        FHT_MM_IDX_MULT * j + _j,
                        INVALID_MASK);
                }
            }
        }
        // deallocate old table
        this->alloc_mmap.deallocate(
            (fht_chunk<K, V> * const)old_chunks,
            (((1u) << (_new_log_incr - 1)) / FHT_TAGS_PER_CLINE));
    }


    //////////////////////////////////////////////////////////////////////
    // add new key value pair stuff

    // slightly different logic than emplace...
    template<typename... Args>
    inline std::pair<fht_iterator, bool>
    insert_or_assign(key_pass_t new_key, Args &&... args) {
        const uint64_t res =
            (const uint64_t)add(new_key, std::forward<Args>(args)...);
        if (res & 0x1) {
            NEW(V,
                *((V * const)((res >> 1) << 1)),
                std::forward<Args>(args)...);
        }
        return std::pair<fht_iterator, bool>(
            fht_iterator((const int8_t * const)((res >> 1) << 1)),
            !(res & 0x1));
    }

    template<typename... Args>
    inline constexpr std::pair<fht_iterator, bool>
    insert(key_pass_t new_key, Args &&... args) {
        return emplace(new_key, std::forward<Args>(args)...);
    }

    //////////////////////////////////////////////////////////////////////
    // Its probably a bad idea to use either of these inserts
    inline constexpr std::pair<fht_iterator, bool>
    insert(const std::pair<const K, V> & bad_pair) {
        return insert(bad_pair.first, bad_pair.second);
    }


    inline void
    insert(std::initializer_list<const std::pair<const K, V>> ilist) {

        // supposedly this is a few assembly ops faster than:
        // for(it = begin(); it != end(); ++it)
        // but you really should never be using this.
        for (auto p : ilist) {
            emplace(p.first, p.second);
        }
    }
    //////////////////////////////////////////////////////////////////////


    // at some point I will try and implement google's shit where they try
    // and find a key argument in pair arguments
    template<typename... Args>
    inline constexpr std::pair<fht_iterator, bool>
    emplace(Args &&... args) {
        return insert(std::forward<Args>(args)...);
    }

    // add new key value pair
    template<typename... Args>
    inline std::pair<fht_iterator, bool>
    emplace(key_pass_t new_key, Args &&... args) {
        // for now going to force explicit key & value
        const int8_t * const res = add(new_key, std::forward<Args>(args)...);
        return (((uint64_t)res) & ((1UL) << 48))
                   ? (std::pair<fht_iterator, bool>(this->end(), false))
                   : (std::pair<fht_iterator, bool>(fht_iterator(res),
                                                    ++this->npairs));
    }

    template<typename... Args>
    const int8_t *
    add(key_pass_t new_key, Args &&... args) {
        // get all derferncing of this out of the way
        const uint32_t    _log_incr = this->log_incr;
        const hash_type_t raw_slot  = this->hash(new_key);

        fht_chunk<K, V> * const chunk = (fht_chunk<K, V> * const)(
            (this->chunks) + (FHT_HASH_TO_IDX(raw_slot, _log_incr)));

        __builtin_prefetch(chunk);

        // get tag and start_idx from raw_slot
        const int8_t   tag       = FHT_GEN_TAG(raw_slot);
        const __m128i  tag_match = FHT_MM_SET(tag);
        const uint32_t start_idx = FHT_GEN_START_IDX(raw_slot);


        prefetch<K>((const void * const)(
            chunk->get_key_n_ptr((FHT_MM_IDX_MULT * start_idx))));

        // check for valid slot or duplicate
        uint32_t idx, slot_mask, del_idx = FHT_TAGS_PER_CLINE;
        for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
            const uint32_t outer_idx = (j + start_idx) & FHT_MM_LINE_MASK;

            slot_mask = FHT_MM_MASK(tag_match, chunk->tags_vec[outer_idx]);
            while (slot_mask) {
                __asm__("tzcnt %1, %0" : "=r"((idx)) : "rm"((slot_mask)));
                const uint32_t true_idx = FHT_MM_IDX_MULT * outer_idx + idx;

                if (__builtin_expect((chunk->compare_key_n(true_idx, new_key)),
                                     1)) {
                    return (const int8_t * const)(
                        (((uint64_t)chunk->get_val_n_ptr(true_idx)) |
                         ((1UL) << 48)));
                }

                slot_mask ^= ((1u) << idx);
            }

            // we always go here 1st loop (where most add calls find a slot)
            // and if no deleted elements alwys go here as well
            if (__builtin_expect(del_idx & FHT_TAGS_PER_CLINE, 1)) {
                const uint32_t _slot_mask = chunk->get_empty_or_del(outer_idx);
                if (__builtin_expect(_slot_mask, 1)) {
                    __asm__("tzcnt %1, %0" : "=r"((idx)) : "rm"((_slot_mask)));
                    const uint32_t true_idx = FHT_MM_IDX_MULT * outer_idx + idx;

                    // some tunable param here would be useful
                    if (chunk->is_deleted_n(true_idx)) {
                        // even though this adds an operation it avoids
                        // worst case where alot of deleted items =
                        // unnecissary iterations. 2 iterations approx =
                        // this cost.
                        if (chunk->get_empty(outer_idx)) {

                            chunk->set_key_val_tag(true_idx,
                                                   tag,
                                                   new_key,
                                                   std::forward<Args>(args)...);
                            return ((const int8_t * const)chunk) + true_idx;
                        }

                        del_idx = true_idx;
                    }
                    else {
                        chunk->set_key_val_tag(true_idx,
                                               tag,
                                               new_key,
                                               std::forward<Args>(args)...);
                        return ((const int8_t * const)chunk) + true_idx;
                    }
                }
            }
            else if (chunk->get_empty(outer_idx)) {
                chunk->set_key_val_tag(del_idx,
                                       tag,
                                       new_key,
                                       std::forward<Args>(args)...);
                return ((const int8_t * const)chunk) + del_idx;
            }
        }

        if (del_idx != FHT_TAGS_PER_CLINE) {
            chunk->set_key_val_tag(del_idx,
                                   tag,
                                   new_key,
                                   std::forward<Args>(args)...);
            return ((const int8_t * const)chunk) + del_idx;
        }

        // no valid slot found so rehash
        this->rehash();

        fht_chunk<K, V> * const new_chunk = (fht_chunk<K, V> * const)(
            this->chunks + FHT_HASH_TO_IDX(raw_slot, _log_incr + 1));


        // after rehash add without duplication check
        for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
            const uint32_t outer_idx  = (j + start_idx) & FHT_MM_LINE_MASK;
            const uint32_t _slot_mask = new_chunk->get_empty(outer_idx);

            if (__builtin_expect(_slot_mask, 1)) {
                __asm__("tzcnt %1, %0" : "=r"((idx)) : "rm"((_slot_mask)));
                const uint32_t true_idx = FHT_MM_IDX_MULT * outer_idx + idx;
                new_chunk->set_key_val_tag(true_idx,
                                           tag,
                                           new_key,
                                           std::forward<Args>(args)...);


                return ((const int8_t * const)new_chunk) + true_idx;
            }
        }
        // probability of this is 1 / (2 ^ 64)
        assert(0);
    }
    //////////////////////////////////////////////////////////////////////
    // stuff related to finding elements

    template<typename _K         = K,
             typename _V         = V,
             typename _Hasher    = Hasher,
             typename _Allocator = Allocator>
    typename std::enable_if<FHT_SPECIAL_CONDITIONAL, const int8_t * const>::type
        __attribute__((pure)) _find(key_pass_t key) const {

        // same deal with add
        const uint32_t                _log_incr = this->log_incr;
        const hash_type_t             raw_slot  = this->hash(key);
        const fht_chunk<K, V> * const chunk = (const fht_chunk<K, V> * const)(
            (this->chunks) + (FHT_HASH_TO_IDX(raw_slot, _log_incr)));
        __builtin_prefetch(chunk);

        // by setting valid here we can remove delete check
        const __m128i  tag_match = FHT_MM_SET(FHT_GEN_TAG(raw_slot));
        const uint32_t start_idx = FHT_GEN_START_IDX(raw_slot);

        // prefetch is good for perf
        __builtin_prefetch(chunk->get_key_n_ptr((FHT_MM_IDX_MULT * start_idx)));

        // check for valid slot of duplicate
        uint32_t idx, slot_mask;
        for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
            // seeded with start_idx we go through idx function
            const uint32_t outer_idx = (j + start_idx) & FHT_MM_LINE_MASK;


            slot_mask = FHT_MM_MASK(tag_match, chunk->tags_vec[outer_idx]);

            while (slot_mask) {
                __asm__("tzcnt %1, %0" : "=r"((idx)) : "rm"((slot_mask)));
                const uint32_t true_idx = FHT_MM_IDX_MULT * outer_idx + idx;
                if (__builtin_expect((chunk->compare_key_n(true_idx, key)),
                                     1)) {
                    return ((const int8_t * const)chunk) + true_idx;
                }
                slot_mask ^= ((1u) << idx);
            }

            if (__builtin_expect(chunk->get_empty(outer_idx), 1)) {
                return NULL;
            }
        }
        return NULL;
    }


    template<typename _K         = K,
             typename _V         = V,
             typename _Hasher    = Hasher,
             typename _Allocator = Allocator>
    typename std::enable_if<(!FHT_SPECIAL_CONDITIONAL),
                            const int8_t * const>::type __attribute__((pure))
    _find(key_pass_t key) const {

        // seperate version of find
        const uint32_t    _log_incr = this->log_incr;
        const hash_type_t raw_slot  = this->hash(key);


        // instead of doing everything through calls to just do directly. My
        // compiler at least does a bad job of optimizing out many of the
        // reference passes
        const __m128i * const tags = (const __m128i * const)(
            (this->chunks + FHT_HASH_TO_IDX(raw_slot, _log_incr)));

        // by setting valid here we can remove delete check
        const __m128i  tag_match = FHT_MM_SET(FHT_GEN_TAG(raw_slot));
        const uint32_t start_idx = FHT_GEN_START_IDX(raw_slot);

        // this is the key to seperate find. Basically instead of passing
        // reference to string to all of the chunk helper functions we can
        // gurantee inline and use direct values
        const fht_node<K, V> * const nodes =
            (const fht_node<K, V> * const)(tags + FHT_MM_LINE);

        __builtin_prefetch(tags + (start_idx));
        __builtin_prefetch(nodes + (FHT_MM_IDX_MULT * start_idx));

        // check for valid slot of duplicate
        uint32_t idx, slot_mask;
        for (uint32_t j = 0; j < FHT_MM_LINE; j++) {
            // seeded with start_idx we go through idx function
            const uint32_t outer_idx = (j + start_idx) & FHT_MM_LINE_MASK;
            slot_mask                = FHT_MM_MASK(tag_match, tags[outer_idx]);

            // consider adding incrmenter to compiler knows its not infinite
            while (slot_mask) {
                __asm__("tzcnt %1, %0" : "=r"((idx)) : "rm"((slot_mask)));
                const uint32_t true_idx = FHT_MM_IDX_MULT * outer_idx + idx;

                if ((nodes[true_idx].key == key)) {
                    return ((const int8_t * const)tags) + true_idx;
                }
                slot_mask ^= ((1u) << idx);
            }

            if (__builtin_expect(FHT_MM_EMPTY(tags[outer_idx]), 1)) {
                return NULL;
            }
        }
        return NULL;
    }


    inline constexpr fht_iterator
    find(K && key) const {
        const int8_t * const res = _find(key);
        return (res == NULL) ? this->end() : fht_iterator(res);
    }

    inline constexpr fht_iterator
    find(const K & key) const {
        const int8_t * const res = _find(key);
        return (res == NULL) ? this->end() : fht_iterator(res);
    }

    inline constexpr uint64_t
    count(const K & key) const {
        return (_find(key) != NULL);
    }

    inline constexpr uint64_t
    count(K && key) const {
        return (_find(key) != NULL);
    }

    inline constexpr bool
    contains(const K & key) const {
        return count(key);
    }

    inline constexpr bool
    contains(K && key) const {
        return count(key);
    }

    inline constexpr V &
    at(const K & key) const {
        const uint64_t                res = (const uint64_t)_find(key);
        const fht_chunk<K, V> * const chunk =
            (const fht_chunk<K, V> * const)(res & (~(FHT_TAGS_PER_CLINE - 1)));
        return *(chunk->get_val_n_ptr(res & (FHT_TAGS_PER_CLINE - 1)));
    }

    inline constexpr V &
    at(K && key) const {
        const uint64_t                res = (const uint64_t)_find(key);
        const fht_chunk<K, V> * const chunk =
            (const fht_chunk<K, V> * const)(res & (~(FHT_TAGS_PER_CLINE - 1)));
        return *(chunk->get_val_n_ptr(res & (FHT_TAGS_PER_CLINE - 1)));
    }

    inline constexpr V & operator[](const K & key) const {
        return at(key);
    }

    inline constexpr V & operator[](K && key) const {
        return at(key);
    }


    //////////////////////////////////////////////////////////////////////
    // deleting stuff
    template<typename _K         = K,
             typename _V         = V,
             typename _Hasher    = Hasher,
             typename _Allocator = Allocator>
    typename std::enable_if<FHT_SPECIAL_CONDITIONAL, uint64_t>::type
    erase(key_pass_t key) {

        // basically exact same as find but instead of storing the val just set
        // tag to deleted

        const uint32_t          _log_incr = this->log_incr;
        const hash_type_t       raw_slot  = this->hash(key);
        fht_chunk<K, V> * const chunk     = (fht_chunk<K, V> * const)(
            (this->chunks) + (FHT_HASH_TO_IDX(raw_slot, _log_incr)));
        __builtin_prefetch(chunk);

        const __m128i  tag_match = FHT_MM_SET(FHT_GEN_TAG(raw_slot));
        const uint32_t start_idx = FHT_GEN_START_IDX(raw_slot);


        __builtin_prefetch(chunk->get_key_n_ptr((FHT_MM_IDX_MULT * start_idx)));

        // check for valid slot of duplicate
        uint32_t idx, slot_mask;
        for (uint32_t j = 0; j < FHT_MM_LINE; j++) {

            const uint32_t outer_idx = (j + start_idx) & FHT_MM_LINE_MASK;

            slot_mask = FHT_MM_MASK(tag_match, chunk->tags_vec[outer_idx]);
            while (slot_mask) {
                __asm__("tzcnt %1, %0" : "=r"((idx)) : "rm"((slot_mask)));
                const uint32_t true_idx = FHT_MM_IDX_MULT * outer_idx + idx;
                if ((chunk->compare_key_n(true_idx, key))) {
                    chunk->delete_tag_n(true_idx);
                    this->npairs--;
                    return FHT_ERASED;
                }
                slot_mask ^= ((1u) << idx);
            }

            if (__builtin_expect(chunk->get_empty(outer_idx), 1)) {
                return FHT_NOT_ERASED;
            }
        }

        return FHT_NOT_ERASED;
    }


    template<typename _K         = K,
             typename _V         = V,
             typename _Hasher    = Hasher,
             typename _Allocator = Allocator>
    typename std::enable_if<(!FHT_SPECIAL_CONDITIONAL), uint64_t>::type
    erase(key_pass_t key) {

        // same logic as the find function above
        const uint32_t    _log_incr = this->log_incr;
        const hash_type_t raw_slot  = this->hash(key);

        const __m128i * const tags = (const __m128i * const)(
            (this->chunks + FHT_HASH_TO_IDX(raw_slot, _log_incr)));

        int8_t * const tags8 = (int8_t * const)(
            this->chunks + FHT_HASH_TO_IDX(raw_slot, _log_incr));
        const __m128i  tag_match = FHT_MM_SET(FHT_GEN_TAG(raw_slot));
        const uint32_t start_idx = FHT_GEN_START_IDX(raw_slot);

        const fht_node<K, V> * const nodes =
            (const fht_node<K, V> * const)(tags + FHT_MM_LINE);

        __builtin_prefetch(tags + (start_idx));
        __builtin_prefetch(nodes + (FHT_MM_IDX_MULT * start_idx));

        // check for valid slot of duplicate
        uint32_t idx, slot_mask;
        for (uint32_t j = 0; j < FHT_MM_LINE; j++) {

            const uint32_t outer_idx = (j + start_idx) & FHT_MM_LINE_MASK;

            slot_mask = FHT_MM_MASK(tag_match, tags[outer_idx]);
            while (slot_mask) {
                __asm__("tzcnt %1, %0" : "=r"((idx)) : "rm"((slot_mask)));
                const uint32_t true_idx = FHT_MM_IDX_MULT * outer_idx + idx;
                if ((nodes[true_idx].key == key)) {
                    FHT_SET_ERASED(tags8[true_idx]);
                    this->npairs--;
                    return FHT_ERASED;
                }
                slot_mask ^= ((1u) << idx);
            }

            if (__builtin_expect(FHT_MM_EMPTY(tags[outer_idx]), 1)) {
                return FHT_NOT_ERASED;
            }
        }

        return FHT_NOT_ERASED;
    }

    inline constexpr uint64_t
    erase(fht_iterator fht_it) {
        return erase(fht_it->first);
    }
    
    void
    clear() {
        const uint32_t _num_chunks =
            ((1u) << (this->log_incr - L1_LOG_CACHE_LINE_SIZE));
        
        for (uint32_t i = 0; i < _num_chunks; i++) {
            ((__m256i * const)(this->chunks + i))[0] = FHT_RESET_VEC;
            ((__m256i * const)(this->chunks + i))[1] = FHT_RESET_VEC;
        }
        this->npairs = 0;
    }


    inline constexpr fht_iterator
    begin() const {
        if (this->empty()) {
            return this->end();
        }
        else {
            return fht_iterator((const int8_t *)this->chunks,
                                (const uint64_t)(this->end().cur_tag));
        }
    }

    inline constexpr fht_iterator
    end() const {
        return fht_iterator(
            ((const int8_t *)this->chunks) +
            sizeof(fht_chunk<K, V>) *
                ((1UL) << (this->log_incr - L1_LOG_CACHE_LINE_SIZE)));
    }
};

//////////////////////////////////////////////////////////////////////
// 32 bit hashers
static const uint32_t u32_sizeof_u32 = sizeof(uint32_t);
static uint32_t
crc_32(const uint32_t * const data, const uint32_t len) {
    uint32_t       res = 0;
    const uint32_t l1  = len / u32_sizeof_u32;
    for (uint32_t i = 0; i < l1; i++) {
        res ^= __builtin_ia32_crc32si(FHT_HASH_SEED, data[i]);
    }

    if (len & 0x3) {
        const uint32_t final_k = data[l1] & (((1u) << (8 * (len & 0x3))) - 1);
        res ^= __builtin_ia32_crc32si(FHT_HASH_SEED, final_k);
    }

    return res;
}


template<typename K>
struct HASH_32 {

    constexpr uint32_t
    operator()(K const & key) const {
        return crc_32((const uint32_t * const)(&key), sizeof(K));
    }
};


template<typename K>
struct HASH_32_4 {

    constexpr uint32_t
    operator()(const K key) const {
        return __builtin_ia32_crc32si(FHT_HASH_SEED, key);
    }
};

template<typename K>
struct HASH_32_8 {

    constexpr uint32_t
    operator()(const K key) const {
        return __builtin_ia32_crc32si(FHT_HASH_SEED, key) ^
               __builtin_ia32_crc32si(FHT_HASH_SEED, key >> 32);
    }
};

template<typename K>
struct HASH_32_CPP_STR {

    constexpr uint32_t
    operator()(K const & key) const {
        return crc_32((const uint32_t * const)(key.c_str()),
                      (const uint32_t)key.length());
    }
};


template<typename K>
struct DEFAULT_HASH_32 {


    template<typename _K = K>
    constexpr typename std::enable_if<(std::is_arithmetic<_K>::value &&
                                       sizeof(_K) <= 4),
                                      uint32_t>::type
    operator()(const K key) const {
        return __builtin_ia32_crc32si(FHT_HASH_SEED, key);
    }

    template<typename _K = K>
    constexpr typename std::enable_if<(std::is_arithmetic<_K>::value &&
                                       sizeof(_K) == 8),
                                      uint32_t>::type
    operator()(const K key) const {
        return __builtin_ia32_crc32si(FHT_HASH_SEED, key) ^
               __builtin_ia32_crc32si(FHT_HASH_SEED, key >> 32);
    }

    template<typename _K = K>
    constexpr typename std::enable_if<(std::is_same<_K, std::string>::value),
                                      uint32_t>::type
    operator()(K const & key) const {
        return crc_32((const uint32_t * const)(key.c_str()),
                      (const uint32_t)key.length());
    }

    template<typename _K = K>
    constexpr typename std::enable_if<(!std::is_same<_K, std::string>::value) &&
                                          (!std::is_arithmetic<_K>::value),
                                      uint32_t>::type
    operator()(K const & key) const {
        return crc_32((const uint32_t * const)(&key), sizeof(K));
    }
};

//////////////////////////////////////////////////////////////////////
// 64 bit hashes
static const uint32_t u32_sizeof_u64 = sizeof(uint64_t);

static uint64_t
crc_64(const uint64_t * const data, const uint32_t len) {
    uint64_t       res = 0;
    const uint32_t l1  = len / u32_sizeof_u64;
    for (uint32_t i = 0; i < l1; i++) {
        res ^= _mm_crc32_u64(FHT_HASH_SEED, data[i]);
    }

    if (len & 0x7) {
        const uint64_t final_k = data[l1] & (((1UL) << (8 * (len & 0x7))) - 1);
        res ^= _mm_crc32_u64(FHT_HASH_SEED, final_k);
    }
    return res;
}


template<typename K>
struct HASH_64 {

    constexpr uint64_t
    operator()(K const & key) const {
        return crc_64((const uint64_t * const)(&key), sizeof(K));
    }
};


// really no reason to 64 bit hash a 32 bit value...
template<typename K>
struct HASH_64_4 {


    constexpr uint32_t
    operator()(const K key) const {
        return __builtin_ia32_crc32si(FHT_HASH_SEED, key);
    }
};

template<typename K>
struct HASH_64_8 {

    constexpr uint64_t
    operator()(const K key) const {
        return _mm_crc32_u64(FHT_HASH_SEED, key);
    }
};

template<typename K>
struct HASH_64_CPP_STR {

    constexpr uint64_t
    operator()(K const & key) const {
        return crc_64((const uint64_t * const)(key.c_str()),
                      (const uint32_t)key.length());
    }
};


template<typename K>
struct DEFAULT_HASH_64 {

    // we dont want 64 bit hash of 32 bit val....
    template<typename _K = K>
    constexpr typename std::enable_if<(std::is_arithmetic<_K>::value &&
                                       sizeof(_K) <= 4),
                                      uint32_t>::type
    operator()(const K key) const {
        return __builtin_ia32_crc32si(FHT_HASH_SEED, key);
    }

    template<typename _K = K>
    constexpr typename std::enable_if<(std::is_arithmetic<_K>::value &&
                                       sizeof(_K) == 8),
                                      uint64_t>::type
    operator()(const K key) const {
        return _mm_crc32_u64(FHT_HASH_SEED, key);
    }

    template<typename _K = K>
    constexpr typename std::enable_if<(std::is_same<_K, std::string>::value),
                                      uint64_t>::type
    operator()(K const & key) const {
        return crc_64((const uint64_t * const)(key.c_str()),
                      (const uint32_t)key.length());
    }

    template<typename _K = K>
    constexpr typename std::enable_if<(!std::is_same<_K, std::string>::value) &&
                                          (!std::is_arithmetic<_K>::value),
                                      uint64_t>::type
    operator()(K const & key) const {
        return crc_64((const uint64_t * const)(&key), sizeof(K));
    }
};

//////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////
// Memory Allocators
static void *
myMmap(void *   addr,
       uint64_t length,
       int32_t  prot_flags,
       int32_t  mmap_flags,
       int32_t  fd,
       int32_t  offset) {
    void * p = mmap(addr, length, prot_flags, mmap_flags, fd, offset);
    assert(p != MAP_FAILED);
    return p;
}


static void
myMunmap(void * addr, uint64_t length) {
    assert(munmap(addr, length) != -(1));
}

// just wrapper that fills in flags
#define mymmap_alloc(Y, X)                                                     \
    myMmap((Y),                                                                \
           (X),                                                                \
           (PROT_READ | PROT_WRITE),                                           \
           (MAP_ANONYMOUS | MAP_PRIVATE),                                      \
           -1,                                                                 \
           0)


// less syscalls this way
template<typename K, typename V>
struct SMALL_INPLACE_MMAP_ALLOC {
    SMALL_INPLACE_MMAP_ALLOC() {}

    ~SMALL_INPLACE_MMAP_ALLOC() {}

    constexpr fht_chunk<K, V> *
    allocate(const uint64_t size) const {
        assert(size <= sizeof(fht_chunk<K, V>) *
                           (FHT_DEFAULT_INIT_MEMORY / sizeof(fht_chunk<K, V>)));
        return (fht_chunk<K, V> *)myMmap(
            NULL,
            sizeof(fht_chunk<K, V>) *
                (FHT_DEFAULT_INIT_MEMORY / sizeof(fht_chunk<K, V>)),
            (PROT_READ | PROT_WRITE),
            (MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE),
            (-1),
            0);
    }

    constexpr void
    deallocate(fht_chunk<K, V> const * ptr, const size_t size) const {
        myMunmap((void *)ptr,
                 sizeof(fht_chunk<K, V>) *
                     (FHT_DEFAULT_INIT_MEMORY / sizeof(fht_chunk<K, V>)));
    }
};


// less syscalls this way
template<typename K, typename V>
struct INPLACE_MMAP_ALLOC {

    uint32_t                cur_size;
    uint32_t                start_offset;
    const fht_chunk<K, V> * base_address;
    INPLACE_MMAP_ALLOC() {
        this->base_address = (const fht_chunk<K, V> * const)myMmap(
            NULL,
            sizeof(fht_chunk<K, V>) *
                (FHT_DEFAULT_INIT_MEMORY / sizeof(fht_chunk<K, V>)),
            (PROT_READ | PROT_WRITE),
            (MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE),
            (-1),
            0);

        this->cur_size     = FHT_DEFAULT_INIT_MEMORY / sizeof(fht_chunk<K, V>);
        this->start_offset = 0;
    }
    ~INPLACE_MMAP_ALLOC() {
        myMunmap((void *)this->base_address, this->cur_size);
    }

    fht_chunk<K, V> *
    allocate(const size_t size) {
        const size_t old_start_offset = this->start_offset;
        this->start_offset += size;
        if (this->start_offset >= this->cur_size) {
            // maymove breaks inplace so no flags. This will very probably
            // fail. Can't really generically specify a unique addr.
            // Assumption is that FHT_DEFAULT_INIT_MEMORY will be sufficient
            assert(MAP_FAILED !=
                   mremap((void *)this->base_address,
                          sizeof(fht_chunk<K, V>) * this->cur_size,
                          2 * sizeof(fht_chunk<K, V>) * this->cur_size,
                          0));
            this->cur_size = 2 * this->cur_size;
        }
        return (fht_chunk<K, V> *)(this->base_address + old_start_offset);
    }

    void
    deallocate(fht_chunk<K, V> * const ptr, const size_t size) const {
        return;
    }
};


template<typename K, typename V>
struct DEFAULT_MMAP_ALLOC {

    fht_chunk<K, V> *
    allocate(const size_t size) const {
        return (fht_chunk<K, V> * const)mymmap_alloc(
            NULL,
            size * sizeof(fht_chunk<K, V>) +
                1);  // + 1 is in a sense null term for iterator
    }
    void
    deallocate(fht_chunk<K, V> * const ptr, const size_t size) const {
        myMunmap((void * const)ptr, size * sizeof(fht_chunk<K, V>));
    }
};


//////////////////////////////////////////////////////////////////////
// Undefs
#ifdef LOCAL_PAGE_SIZE_DEFINE
#undef LOCAL_PAGE_SIZE_DEFINE
#undef PAGE_SIZE
#endif

#ifdef LOCAL_CACHE_SIZE_DEFINE
#undef LOCAL_CACHE_SIZE_DEFINE
#undef L1_CACHE_LINE_SIZE
#undef L1_LOG_CACHE_LINE_SIZE
#endif


#undef FHT_TAGS_PER_CLINE
#undef DESTROYABLE_INSERT
#undef SRC_WRAPPER
#undef SRC_WRAPPER
#undef NEW
#undef PREFETCH_BOUND
#undef FHT_PASS_BY_VAL_THRESH
#undef FHT_SPECIAL_TYPES
#undef CONTENT_BITS
#undef FHT_IS_ERASED
#undef FHT_SET_ERASED
#undef FHT_MM_SET
#undef FHT_MM_MASK
#undef FHT_MM_EMPTY
#undef FHT_MM_EMPTY_OR_DEL
#undef FHT_TO_MASK
#undef FHT_GET_NTH_BIT
#undef FHT_HASH_TO_IDX
#undef FHT_GEN_TAG
#undef FHT_GEN_START_IDX
#undef mymmap_alloc


#include "UNDEF_FHT_SPECIAL_TYPE_MACROS.h"
//////////////////////////////////////////////////////////////////////

#endif