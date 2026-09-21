/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "flatval.h"
#include "intrusiveptr.h"
#include <unordered_map>
#include "poolalloc.h"

template <class EvalValueT, class LValueT>
class DictObjectTempl : public RefCounted {

public:

    ML_POOL_NEW_DELETE
    /* H2 v2: the NODE-POOLED map (poolalloc.h) - a chained unordered_map
     * heap-allocated a ~96-byte node per insert; the pool serves those
     * from program-lifetime free lists. Node-pointer stability (what the
     * held-LValue* runtime paths rely on) is untouched - rehash moves
     * only the bucket array. */
    typedef std::unordered_map<
        EvalValueT, LValueT,
        std::hash<EvalValueT>, std::equal_to<EvalValueT>,
        PoolAlloc<std::pair<const EvalValueT, LValueT>>> inner_type;

private:
    inner_type data;

    /*
     * When set, this dict is read-only: it backs a `const` value, so
     * subscript/member writes (and auto-vivification) and erase are rejected.
     * A clone() is always mutable, so TypeDict::clone clears it on the copy.
     * See make_const_clone() in eval.cpp.
     */
    bool readonly = false;

    /*
     * Default value for a missing key (a "default dict", from
     * dict(default_value)). When `has_default` is set, reading a missing key
     * returns (and inserts) `default_val` instead of throwing - so `d[k] += 1`
     * works without a none-check. Copied by the default copy/move ctors.
     */
    bool has_default = false;
    EvalValueT default_val;

public:

    /*
     * #53: a FOREACH over a dict walks it through a Cursor, and the body may
     * insert or erase keys of that same dict. A live unordered_map iterator
     * does not survive either (an insert may rehash; an erase frees the
     * node the iterator points at - both were use-after-frees, and the
     * engines visited different sequences). The DEFINED semantics:
     *
     *   the loop visits each key the dict held when the loop STARTED, once,
     *   in the dict's order, if the key is still present when its turn
     *   comes - with its value at that moment; a key the body inserts is
     *   not visited.
     *
     * Free until the body restructures the dict: the cursor walks the live
     * iterator, and every live cursor is linked into its dict's list. A
     * STRUCTURAL change (inserting a key, erasing one) first calls
     * will_restructure(), which snapshots each linked cursor's REMAINING
     * keys and unlinks it; from then on the cursor looks each key up when
     * its turn comes. Snapshotting early is unobservable (the same keys, the
     * same order, the same current values), so a site that might restructure
     * may call the hook unconditionally.
     *
     * The cursor is MOVABLE (the VM keeps its iterator states in a vector
     * that grows with the activation) and relinks itself on a move.
     */
    class Cursor {
        friend class DictObjectTempl;
        DictObjectTempl *owner = nullptr;   /* non-null while LINKED */
        Cursor *prev = nullptr, *next = nullptr;
        typename inner_type::iterator it;
        std::vector<EvalValueT> keys;       /* the remaining keys, snapped */
        size_t kpos = 0;
        size_t start_size = 0;              /* the tripwire, see next_entry */
        bool snap = false;

        void link(DictObjectTempl *d)
        {
            owner = d;
            prev = nullptr;
            next = d->cursors;
            if (next)
                next->prev = this;
            d->cursors = this;
        }
        void unlink()
        {
            if (!owner)
                return;
            if (prev)
                prev->next = next;
            else
                owner->cursors = next;
            if (next)
                next->prev = prev;
            owner = nullptr;
            prev = next = nullptr;
        }
        void take(Cursor &o) noexcept
        {
            owner = o.owner; prev = o.prev; next = o.next;
            it = o.it; keys = std::move(o.keys); kpos = o.kpos;
            start_size = o.start_size; snap = o.snap;
            if (owner) {
                if (prev)
                    prev->next = this;
                else
                    owner->cursors = this;
                if (next)
                    next->prev = this;
            }
            o.owner = nullptr;
            o.prev = o.next = nullptr;
        }

    public:
        Cursor() = default;
        Cursor(const Cursor &) = delete;
        Cursor &operator=(const Cursor &) = delete;
        Cursor(Cursor &&o) noexcept { take(o); }
        Cursor &operator=(Cursor &&o) noexcept
        {
            if (this != &o) {
                unlink();
                take(o);
            }
            return *this;
        }
        ~Cursor() { unlink(); }

        /* Begin a walk of `d` (which the caller keeps alive for it). */
        void start(DictObjectTempl &d)
        {
            unlink();
            keys.clear();
            kpos = 0;
            snap = false;
            it = d.data.begin();
            start_size = d.data.size();
            link(&d);
        }

        /* The next entry of the walk over `d`, or null when it is over (the
         * cursor then unlinks, so a finished loop costs later stores
         * nothing). The caller copies the key/value BEFORE running the body:
         * the body may erase the entry. */
        const std::pair<const EvalValueT, LValueT> *
        next_entry(DictObjectTempl &d)
        {
            if (!snap) {
                /* a structural change that bypassed will_restructure would
                 * leave a live cursor over a map of another size */
                ML_CHECK(d.data.size() == start_size);
                if (it != d.data.end()) {
                    const auto *p = &*it;
                    ++it;
                    return p;
                }
            } else {
                while (kpos < keys.size()) {
                    const auto f = d.data.find(keys[kpos++]);
                    if (f != d.data.end())
                        return &*f;
                }
            }
            unlink();
            keys.clear();
            return nullptr;
        }
    };

private:
    Cursor *cursors = nullptr;

    void snapshot_cursors()
    {
        while (Cursor *c = cursors) {
            c->keys.clear();
            for (auto x = c->it; x != data.end(); ++x)
                c->keys.push_back(x->first);
            c->kpos = 0;
            c->snap = true;
            c->unlink();
        }
    }

public:

    DictObjectTempl() = default;
    /* A copy/move is a DIFFERENT dict: it never inherits the source's
     * walkers (the RefCounted base resets the count the same way). */
    DictObjectTempl(const DictObjectTempl &o)
        : RefCounted(), data(o.data), readonly(o.readonly),
          has_default(o.has_default), default_val(o.default_val)
    { }
    DictObjectTempl(DictObjectTempl &&o)
        : RefCounted(),
          readonly(o.readonly), has_default(o.has_default),
          default_val(std::move(o.default_val))
    {
        o.will_restructure();          /* its walkers keep their keys */
        data = std::move(o.data);
    }
    ~DictObjectTempl()
    {
        /* a walk pins its dict, so this is belt and braces */
        while (Cursor *c = cursors)
            c->unlink();
    }

    DictObjectTempl(const inner_type &) = delete;
    DictObjectTempl(inner_type &&d)
        : data(std::move(d))
    { }

    /* #53: call BEFORE inserting or erasing a key (see Cursor). */
    void will_restructure()
    {
        if (cursors)
            snapshot_cursors();
    }

    inner_type &get_ref() { return data; }
    const inner_type &get_ref() const { return data; }

    bool is_readonly() const { return readonly; }
    void set_readonly() { readonly = true; }
    void clear_readonly() { readonly = false; }

    bool get_has_default() const { return has_default; }
    const EvalValueT &get_default() const { return default_val; }
    void set_default(const EvalValueT &v)
        { has_default = true; default_val = v; }
};
