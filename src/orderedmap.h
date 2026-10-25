/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include "defs.h"
#include "poolalloc.h"

/*
 * THE DICT'S MAP (#50): a chained hash table whose entries also form a
 * doubly-linked list in INSERTION ORDER, which is the order every walk
 * over it visits them - begin()/end() follow the list, never the buckets.
 *
 * MyLang dicts keep insertion order (README, *Dictionary*): storing over an
 * existing key keeps its place, an erased key that is inserted again goes
 * last. It used to be a std::unordered_map, whose iteration order is the
 * standard library's own business: libstdc++'s copy constructor keeps it
 * and libc++'s rebuilds it, and a map rebuilt key by key (a deep copy, a
 * .myv load) came out in another order - so a folded dict printed one way
 * and the same dict built at run time another (RULE 2). Here a copy, a
 * rebuild and a load all insert in the order they walk, so the order is a
 * property of the PROGRAM.
 *
 * What it keeps from the unordered_map it replaced:
 *  - NODE STABILITY: an entry never moves until it is erased (a rehash
 *    moves only the bucket array), so a held LValue* or iterator stays
 *    valid across inserts - the runtime writes dict values through held
 *    LValue*s;
 *  - the pooled nodes (poolalloc.h), one allocation per entry;
 *  - the CACHED HASH: each entry keeps its key's hash, so a lookup compares
 *    hashes before calling ==, a rehash never hashes again, and a copy
 *    hashes nothing (hashing a container key walks it deeply);
 *  - the ARGUMENT ORDER of the equality test: the key looked up on the
 *    left, the stored key on the right, as libstdc++ passes them (a
 *    builtin or an exception value has no ==, so the order is observable).
 *
 * The subset of the std::unordered_map interface the runtime uses, with
 * the same meaning; a value_type is std::pair<const K, V>, so `kv.first`,
 * `it->second` and structured bindings read as they did.
 *
 * Bucket index: Fibonacci hashing (the hash times 2^64 / phi, its top bits)
 * over a power-of-two bucket count - an int key hashes to itself, and the
 * multiply is what spreads 0, 1024, 2048, ... over the buckets. The table
 * grows to keep at most one entry per bucket on average.
 */
template <class K, class V, class Hash, class Eq>
class OrderedHashMap {

public:

    typedef std::pair<const K, V> value_type;
    typedef size_t size_type;

private:

    struct Node {

        ML_POOL_NEW_DELETE

        value_type kv;
        size_t h;              /* the key's hash, computed once */
        Node *chain;           /* the next node in this bucket */
        Node *prev;            /* insertion order */
        Node *next;

        template <class KA, class VA>
        Node(KA &&k, VA &&v, size_t h)
            : kv(std::forward<KA>(k), std::forward<VA>(v)), h(h),
              chain(nullptr), prev(nullptr), next(nullptr)
        { }
    };

    std::vector<Node *> buckets;   /* empty, or a power of two */
    unsigned shift = 0;            /* 64 - log2(buckets.size()) */
    Node *head = nullptr;
    Node *tail = nullptr;
    size_t nent = 0;

    static constexpr size_t MIN_BUCKETS = 8;

    size_t bucket_of(size_t h) const
    {
        const uint64_t x = static_cast<uint64_t>(h) * 0x9E3779B97F4A7C15ull;
        return static_cast<size_t>(x >> shift);
    }

    Node *find_node(const K &k, size_t h) const
    {
        if (!nent)
            return nullptr;
        for (Node *n = buckets[bucket_of(h)]; n; n = n->chain)
            if (n->h == h && Eq()(k, n->kv.first))
                return n;
        return nullptr;
    }

    /* Rebuild the buckets with `nb` (a power of two) chains; the nodes and
     * the order list stay exactly where they are. */
    void rehash_to(size_t nb)
    {
        unsigned log2 = 0;
        while ((size_t(1) << log2) < nb)
            log2++;
        std::vector<Node *> nbk(nb, nullptr);
        buckets.swap(nbk);
        shift = 64 - log2;
        for (Node *n = head; n; n = n->next) {
            Node *&b = buckets[bucket_of(n->h)];
            n->chain = b;
            b = n;
        }
    }

    /* Room for `n` entries at one per bucket, BEFORE a node is made, so a
     * failed allocation leaves the map as it was. */
    void grow_for(size_t n)
    {
        if (n <= buckets.size())
            return;
        size_t nb = buckets.empty() ? MIN_BUCKETS : buckets.size();
        while (nb < n)
            nb *= 2;
        rehash_to(nb);
    }

    /* Link a fresh node (its hash set) into its bucket and at the end of
     * the order; the caller has made room. */
    Node *link_last(Node *n)
    {
        Node *&b = buckets[bucket_of(n->h)];
        n->chain = b;
        b = n;
        n->prev = tail;
        if (tail)
            tail->next = n;
        else
            head = n;
        tail = n;
        nent++;
        return n;
    }

    void unlink(Node *n)
    {
        Node **pp = &buckets[bucket_of(n->h)];
        while (*pp != n)
            pp = &(*pp)->chain;
        *pp = n->chain;
        if (n->prev)
            n->prev->next = n->next;
        else
            head = n->next;
        if (n->next)
            n->next->prev = n->prev;
        else
            tail = n->prev;
        nent--;
    }

    void free_nodes() noexcept
    {
        Node *n = head;
        while (n) {
            Node *nx = n->next;
            delete n;
            n = nx;
        }
        head = tail = nullptr;
        nent = 0;
    }

    template <bool C>
    class iter {

        friend class OrderedHashMap;
        Node *n = nullptr;

    public:

        typedef std::forward_iterator_tag iterator_category;
        typedef typename OrderedHashMap::value_type value_type;
        typedef std::ptrdiff_t difference_type;
        typedef typename std::conditional<C, const value_type *,
                                          value_type *>::type pointer;
        typedef typename std::conditional<C, const value_type &,
                                          value_type &>::type reference;

        iter() = default;
        explicit iter(Node *n) : n(n) { }

        /* an iterator converts to a const_iterator */
        template <bool C2, class = typename std::enable_if<C && !C2>::type>
        iter(const iter<C2> &o) : n(o.n) { }

        reference operator*() const { return n->kv; }
        pointer operator->() const { return &n->kv; }

        iter &operator++() { n = n->next; return *this; }
        iter operator++(int) { iter t = *this; n = n->next; return t; }

        template <bool C2>
        bool operator==(const iter<C2> &o) const { return n == o.n; }
        template <bool C2>
        bool operator!=(const iter<C2> &o) const { return n != o.n; }

        template <bool> friend class iter;
    };

public:

    typedef iter<false> iterator;
    typedef iter<true> const_iterator;

    OrderedHashMap() = default;

    OrderedHashMap(const OrderedHashMap &o)
    {
        reserve(o.nent);
        for (Node *n = o.head; n; n = n->next)
            link_last(new Node(n->kv.first, n->kv.second, n->h));
    }

    OrderedHashMap(OrderedHashMap &&o) noexcept
        : buckets(std::move(o.buckets)), shift(o.shift), head(o.head),
          tail(o.tail), nent(o.nent)
    {
        o.buckets.clear();
        o.head = o.tail = nullptr;
        o.nent = 0;
    }

    OrderedHashMap &operator=(OrderedHashMap o) noexcept
    {
        swap(o);
        return *this;
    }

    ~OrderedHashMap() { free_nodes(); }

    void swap(OrderedHashMap &o) noexcept
    {
        buckets.swap(o.buckets);
        std::swap(shift, o.shift);
        std::swap(head, o.head);
        std::swap(tail, o.tail);
        std::swap(nent, o.nent);
    }

    size_t size() const { return nent; }
    bool empty() const { return nent == 0; }

    iterator begin() { return iterator(head); }
    iterator end() { return iterator(); }
    const_iterator begin() const { return const_iterator(head); }
    const_iterator end() const { return const_iterator(); }

    void reserve(size_t n) { grow_for(n); }

    iterator find(const K &k)
    {
        return iterator(find_node(k, Hash()(k)));
    }

    const_iterator find(const K &k) const
    {
        return const_iterator(find_node(k, Hash()(k)));
    }

    size_t count(const K &k) const { return find(k) != end() ? 1 : 0; }

    /* Add k -> v at the END unless k is present; the entry either way. */
    template <class KA, class VA>
    std::pair<iterator, bool> emplace(KA &&k, VA &&v)
    {
        const size_t h = Hash()(k);
        if (Node *n = find_node(k, h))
            return { iterator(n), false };
        grow_for(nent + 1);
        Node *n = new Node(std::forward<KA>(k), std::forward<VA>(v), h);
        return { iterator(link_last(n)), true };
    }

    template <class P>
    std::pair<iterator, bool> insert(P &&p)
    {
        return emplace(std::forward<P>(p).first, std::forward<P>(p).second);
    }

    /* Store v at k: a present key keeps its place, a new one goes last. */
    template <class KA, class VA>
    std::pair<iterator, bool> insert_or_assign(KA &&k, VA &&v)
    {
        const size_t h = Hash()(k);
        if (Node *n = find_node(k, h)) {
            n->kv.second = std::forward<VA>(v);
            return { iterator(n), false };
        }
        grow_for(nent + 1);
        Node *n = new Node(std::forward<KA>(k), std::forward<VA>(v), h);
        return { iterator(link_last(n)), true };
    }

    /* Remove the entry at `it`; the one after it in the order. */
    iterator erase(const_iterator it)
    {
        Node *n = it.n;
        Node *nx = n->next;
        unlink(n);
        delete n;
        return iterator(nx);
    }

    size_t erase(const K &k)
    {
        const const_iterator it = find(k);
        if (it == end())
            return 0;
        erase(it);
        return 1;
    }

    /* Drop every entry; the bucket array is kept. */
    void clear() noexcept
    {
        free_nodes();
        for (Node *&b : buckets)
            b = nullptr;
    }
};
