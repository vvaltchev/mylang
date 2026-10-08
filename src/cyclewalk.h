/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

/*
 * THE CYCLE GUARD (plans/reference-cycles.md, Part A; README *Values that
 * contain themselves*).
 *
 * Arrays, dicts and boxed structs are references, so a value can contain
 * itself (`append(a, a)`, `d["self"] = d`). Every recursive walk over a
 * value graph - printing, `==`, `hash`, the deep copies, the compiler's
 * walks over a constant's value, the .myv writer - must terminate on one.
 * They all do it the same way, through this header: a stack of the
 * containers the walk is currently INSIDE (its path from the root), and a
 * container met again while it is on that stack is a BACK EDGE, which the
 * walk answers without entering it again. A new walk over a value graph
 * uses these types too; CLAUDE.md makes that a rule.
 *
 * Only a container that CAN hold a reference is pushed: a general-storage
 * array, a dict, a boxed struct. A flat array (int/float/bool/str/POD
 * struct storage), a POD struct, a string and a scalar hold none, so they
 * can never be on a cycle and pay nothing. A function value is never
 * walked into by any of these walks (it prints as `<function>`, compares
 * by identity, has no hash, and every copy shares it), so a closure's
 * captures need no key either.
 *
 * A stack is not a memo: a container shared at two places that is NOT on
 * the current path is walked (printed, copied) at each place, exactly as
 * before - only the path decides. MyLang is single-threaded, so the
 * stacks of the walks that run through Type's virtual methods (print,
 * hash, ==) are process-wide statics (cyc_render / cyc_hash / cyc_eq
 * below); a walk that is an ordinary recursive function keeps its own
 * stack as a local.
 */

#include "defs.h"
#include "evalvalue.h"
#include "structtype.h"
#include "hashing.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/*
 * A container's identity on a walk. Dicts and boxed structs are their
 * object. An array is the window it shows of its shared storage: a
 * non-slice is the whole storage, a slice its [off, off + len) - so a slice
 * (which behaves as an independent copy) is a different container from the
 * array it was taken from, unless it shows exactly the same elements,
 * which is also when TypeArr::eq's identity fast path calls the two equal.
 */
struct CycKey {
    const void *obj = nullptr;
    size_t off = 0;
    size_t len = 0;
};

inline bool operator==(const CycKey &a, const CycKey &b)
{
    return a.obj == b.obj && a.off == b.off && a.len == b.len;
}

inline bool operator!=(const CycKey &a, const CycKey &b)
{
    return !(a == b);
}

struct CycKeyHash {
    size_t operator()(const CycKey &k) const
    {
        size_t h = reinterpret_cast<size_t>(k.obj);
        hash_combine(h, k.off);
        hash_combine(h, k.len);
        return h;
    }
};

/* The key of `v` when it is a container that can hold a reference (and so
 * can be on a cycle); false for everything else. */
inline bool cyc_key(const EvalValue &v, CycKey &k)
{
    switch (v.get_type()->t) {

    case Type::t_arr: {
        const SharedArrayObj &a = v.get_ref<SharedArrayObj>();
        if (a.skind() != SharedArrayObj::Storage::general)
            return false;
        k.obj = a.storage_id();
        k.off = a.offset();
        k.len = a.size();
        return true;
    }

    case Type::t_dict:
        k.obj = v.get_ref<intrusive_ptr<DictObject>>().get();
        k.off = k.len = 0;
        return true;

    case Type::t_struct: {
        const StructObject *s = v.get_ref<intrusive_ptr<StructObject>>().get();
        if (!s->def || s->is_pod())
            return false;
        k.obj = s;
        k.off = k.len = 0;
        return true;
    }

    default:
        return false;
    }
}

/* The key of a dict or a boxed struct, from the object itself. */
inline CycKey cyc_key_obj(const void *obj)
{
    CycKey k;
    k.obj = obj;
    return k;
}

/* The key of a struct TYPE's const members, for a compiler walk that
 * follows a struct instance into its def's folded `const` values. */
inline CycKey cyc_key_consts(const StructTypeDef *def)
{
    CycKey k;
    k.obj = def;
    k.off = k.len = static_cast<size_t>(-1);   /* never a container's */
    return k;
}

/*
 * The containers a single-value walk is inside, outermost first. `depth`
 * is the position of a container on it - the back edge's DEPTH, which the
 * hash of a cyclic value is made of.
 */
class CycStack {

public:

    int depth(const CycKey &k) const
    {
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == k)
                return static_cast<int>(i);
        return -1;
    }

    bool contains(const CycKey &k) const { return depth(k) >= 0; }
    size_t size() const { return keys.size(); }
    bool empty() const { return keys.empty(); }
    void push(const CycKey &k) { keys.push_back(k); }
    void pop() { keys.pop_back(); }

private:
    std::vector<CycKey> keys;
};

/* Push a container for the rest of the scope - exception-safe: a throw in
 * the middle of a walk still pops it. With `on` false it does nothing (a
 * value that holds no reference is not pushed). */
class CycGuard {

public:

    CycGuard(CycStack &st, const CycKey &k, bool on = true)
        : st(st), on(on)
    {
        if (on)
            st.push(k);
    }

    ~CycGuard()
    {
        if (on)
            st.pop();
    }

    CycGuard(const CycGuard &) = delete;
    CycGuard &operator=(const CycGuard &) = delete;

private:
    CycStack &st;
    const bool on;
};

/* Is `v` an array, a dict or a struct - a value that may be keyed? */
inline bool cyc_maybe_container(const EvalValue &v)
{
    const Type::TypeE t = v.get_type()->t;
    return t == Type::t_arr || t == Type::t_dict || t == Type::t_struct;
}

/*
 * Calls f(child) for each CONTAINER directly inside `v` - among a general
 * array's elements, a dict's keys, values and default, a boxed struct's
 * fields - until one call returns true; returns whether one did. A scalar,
 * a string or a function child is skipped (no walk keys one), and a flat
 * array or a POD struct has no child that can hold a reference.
 */
template <class F>
bool cyc_any_child(const EvalValue &v, F f)
{
    auto g = [&](const EvalValue &c) { return cyc_maybe_container(c) && f(c); };

    switch (v.get_type()->t) {

    case Type::t_arr: {
        const SharedArrayObj &a = v.get_ref<SharedArrayObj>();
        if (a.skind() == SharedArrayObj::Storage::objs) {
            /* the flat class storage: its class instances (not keyed
             * itself - a cycle through it passes through one of them) */
            const auto &ov = a.flat_objs();
            for (size_type i = 0; i < a.size(); i++)
                if (StructObject *p = ov[a.offset() + i])
                    if (g(obj_elem_value(p)))
                        return true;
            return false;
        }
        if (a.skind() != SharedArrayObj::Storage::general)
            return false;
        const ArrayConstView view = a.get_view();
        for (size_type i = 0; i < view.size(); i++)
            if (g(view[i].get()))
                return true;
        return false;
    }

    case Type::t_dict: {
        const DictObject &d = *v.get_ref<intrusive_ptr<DictObject>>();
        for (const auto &kv : d.get_ref())
            if (g(kv.first) || g(kv.second.get()))
                return true;
        return d.get_has_default() && g(d.get_default());
    }

    case Type::t_struct: {
        const StructObject &s = *v.get_ref<intrusive_ptr<StructObject>>();
        if (!s.def || s.is_pod())
            return false;
        for (const LValue &fl : s.fields)
            if (g(fl.get()))
                return true;
        return false;
    }

    default:
        return false;
    }
}

/*
 * Can more than one reference reach the container `v`? Every reference a
 * value graph holds - an element, a dict key or value, a field - is
 * counted (only a frame slot can BORROW, #94), so a container whose count
 * is 1 is reached through its one holder only.
 */
inline bool cyc_shared(const EvalValue &v)
{
    switch (v.get_type()->t) {
    case Type::t_arr:
        return v.get_ref<SharedArrayObj>().use_count() > 1;
    case Type::t_dict:
        return v.get_ref<intrusive_ptr<DictObject>>().use_count() > 1;
    case Type::t_struct:
        return v.get_ref<intrusive_ptr<StructObject>>().use_count() > 1;
    default:
        return true;
    }
}

/*
 * Does a cycle lie on some path from `v`? A depth-first search: `path` is
 * the search's own path, `done` the containers already proven to reach no
 * cycle (so a container shared at many places is searched once). Only a
 * SHARED container is recorded there: one with a single holder is met only
 * when that holder is expanded, and every holder is expanded at most once
 * (a shared one is in `done` after its first search; an unshared one by
 * the same argument, one level up). The insert was most of the search's
 * cost on a tree.
 */
inline bool cyc_reaches_cycle(const EvalValue &v, CycStack &path,
                              std::unordered_set<CycKey, CycKeyHash> &done)
{
    CycKey k;
    if (!cyc_key(v, k) || (!done.empty() && done.count(k)))
        return false;
    if (path.contains(k))
        return true;
    CycGuard g(path, k);
    if (cyc_any_child(v, [&](const EvalValue &c) {
            return cyc_reaches_cycle(c, path, done);
        }))
        return true;
    if (cyc_shared(v))
        done.insert(k);
    return false;
}

/*
 * THE PAIR STACK of `==` (and of every walk that compares two values
 * side by side): the (lhs, rhs) container pairs currently being compared.
 * A pair about to be compared is a BACK EDGE when its lhs is on the stack
 * as an lhs, or its rhs as an rhs; the two are then equal iff they are at
 * the SAME depth - so two rings of the same shape are equal, and a ring
 * and its one-step unrolling are not. Neither on the stack: compare as
 * always.
 */
class CycPairStack {

    friend class CycFlip;

public:

    /* -1: not a back edge (compare the two); else 1 equal, 0 not. */
    int back_edge(const CycKey &l, const CycKey &r) const
    {
        const CycKey &a = flipped ? r : l;
        const CycKey &b = flipped ? l : r;
        int da = -1, db = -1;
        for (size_t i = 0; i < as.size(); i++) {
            if (da < 0 && as[i] == a)
                da = static_cast<int>(i);
            if (db < 0 && bs[i] == b)
                db = static_cast<int>(i);
        }
        if (da < 0 && db < 0)
            return -1;
        return da == db ? 1 : 0;
    }

    /*
     * May the walk answer "equal" for a container compared with ITSELF
     * without entering it (TypeArr::eq's identity fast path)? Always while
     * every pair on the stack compares a container with itself (the top
     * level, `a == a`): then the two sides are the same walk. Otherwise
     * only when no cycle can be reached from it - a container reaching
     * none cannot reach the pairs on the stack either, so entering it
     * would compare it equal to itself, as the shortcut does; one on a
     * cycle is compared, and the depths decide (`a == clone(a)` for a ring
     * `a` is false: the clone closes the ring one level deeper). The
     * search runs once per container per top-level comparison.
     */
    bool identity_ok(const EvalValue &x, const CycKey &k)
    {
        if (!nonid)                     /* every pair: one container twice */
            return true;
        auto it = memo.find(k);
        if (it != memo.end())
            return it->second;
        CycStack path;
        const bool ok = !cyc_reaches_cycle(x, path, done);
        memo.emplace(k, ok);
        return ok;
    }

    void push(const CycKey &l, const CycKey &r)
    {
        as.push_back(flipped ? r : l);
        bs.push_back(flipped ? l : r);
        if (l != r)
            nonid++;
    }

    void pop()
    {
        if (as.back() != bs.back())
            nonid--;
        as.pop_back();
        bs.pop_back();
        if (as.empty() && !memo.empty()) {   /* the comparison is over */
            memo.clear();
            done.clear();
        }
    }

    bool empty() const { return as.empty(); }

private:
    /*
     * The two SIDES of the comparison: side A is the outermost `==`'s left
     * operand, side B its right one. A nested comparison normally keeps
     * its left operand on side A; `flipped` says it is the other way round
     * (CycFlip) - a dict compares a value of side B against one of side A,
     * as unordered_map's operator== always did.
     */
    std::vector<CycKey> as, bs;
    bool flipped = false;
    size_t nonid = 0;      /* pairs on the stack with lhs != rhs */
    std::unordered_map<CycKey, bool, CycKeyHash> memo;
    std::unordered_set<CycKey, CycKeyHash> done;
};

/* For the scope: the next comparison's LEFT operand comes from side B. */
class CycFlip {

public:

    explicit CycFlip(CycPairStack &st) : st(st) { st.flipped = !st.flipped; }
    ~CycFlip() { st.flipped = !st.flipped; }

    CycFlip(const CycFlip &) = delete;
    CycFlip &operator=(const CycFlip &) = delete;

private:
    CycPairStack &st;
};

class CycPairGuard {

public:

    CycPairGuard(CycPairStack &st, const CycKey &l, const CycKey &r,
                 bool on = true)
        : st(st), on(on)
    {
        if (on)
            st.push(l, r);
    }

    ~CycPairGuard()
    {
        if (on)
            st.pop();
    }

    CycPairGuard(const CycPairGuard &) = delete;
    CycPairGuard &operator=(const CycPairGuard &) = delete;

private:
    CycPairStack &st;
    const bool on;
};

/*
 * The stacks of the walks that run through Type's virtual methods, which
 * cannot pass a stack along. Function-local statics, so no static
 * initializer anywhere can meet one unconstructed:
 *   cyc_render() - to_string / to_string_repr / pretty;
 *   cyc_hash()   - hash;
 *   cyc_eq()     - == and != .
 */
inline CycStack &cyc_render()
{
    static CycStack st;
    return st;
}

inline CycStack &cyc_hash()
{
    static CycStack st;
    return st;
}

inline CycPairStack &cyc_eq()
{
    static CycPairStack st;
    return st;
}

/*
 * A walk that starts a NEW, independent question in the middle of another
 * one: a dict's key equality and key hash. Keys are matched by their own
 * equality wherever the dict sits (the map's key_equal), so `==` looks a
 * key up and `hash` hashes a key with empty stacks, restored after. Only a
 * key that can hold a reference needs it - a scalar or string key never
 * reaches a stack.
 */
class CycFreshScope {

public:

    CycFreshScope()
    {
        std::swap(eq, cyc_eq());
        std::swap(hash, cyc_hash());
    }

    ~CycFreshScope()
    {
        std::swap(eq, cyc_eq());
        std::swap(hash, cyc_hash());
    }

    CycFreshScope(const CycFreshScope &) = delete;
    CycFreshScope &operator=(const CycFreshScope &) = delete;

private:
    CycPairStack eq;
    CycStack hash;
};

/*
 * A cycle NO PROGRAM CAN BREAK - a frozen one (a constant, or a value
 * frozen as a dict key: every container in it is read-only) or one the
 * compiler built while evaluating a constant and then dropped - would never
 * be freed: reference counting cannot free a cycle, and reclaiming one in
 * general is plans/reference-cycles.md part B, undesigned. Such a value is
 * kept here until the program ENDS, and then freed: cyc_release_kept,
 * which the first keep registers with atexit (so it runs after main and
 * before the INT object census and LeakSanitizer read the heap), empties
 * every container reachable from each kept value, which breaks the cycles.
 * Nothing runs after it, so emptying a read-only container is unobservable.
 * A cycle the PROGRAM built (a mutable one) is never kept: breaking it is
 * the program's job, and the leak detectors keep reporting it.
 */
void cyc_keep_until_exit(const EvalValue &v);
void cyc_release_kept();

/* Can a cycle be reached from `v`? */
inline bool cyc_value_reaches_cycle(const EvalValue &v)
{
    CycStack path;
    std::unordered_set<CycKey, CycKeyHash> done;
    return cyc_reaches_cycle(v, path, done);
}

/* Keep `v` until exit when it is a container from which a cycle can be
 * reached (a compile-time value the compiler is about to drop). */
inline void cyc_keep_if_cyclic(const EvalValue &v)
{
    if (cyc_value_reaches_cycle(v))
        cyc_keep_until_exit(v);
}

/* The hash of a back edge at `depth` on the hash stack. */
inline size_t cyc_backedge_hash(int depth)
{
    size_t h = hash_salt_backedge;
    hash_combine(h, static_cast<size_t>(depth));
    return h;
}

/*
 * The DEEP COPIES' stack: each container being copied, with its copy in
 * progress. A container met again while on it is linked to that copy, so
 * the copy reproduces the cycle instead of following it forever.
 */
class CycCopyStack {

public:

    /* The copy in progress of the container `k`, when the walk is inside
     * it (a back edge: the copy closes the cycle here); else null. */
    const EvalValue *link(const CycKey &k)
    {
        for (const auto &e : st)
            if (e.first == k) {
                closed = true;
                return &e.second;
            }
        return nullptr;
    }

    void push(const CycKey &k, const EvalValue &copy)
    {
        st.emplace_back(k, copy);
    }

    void pop() { st.pop_back(); }

    /* did the copy close a cycle? (the result then contains one) */
    bool closed = false;

private:
    std::vector<std::pair<CycKey, EvalValue>> st;
};

class CycCopyGuard {

public:

    CycCopyGuard(CycCopyStack &st, const CycKey &k, const EvalValue &copy)
        : st(st)
    {
        st.push(k, copy);
    }

    ~CycCopyGuard() { st.pop(); }

    CycCopyGuard(const CycCopyGuard &) = delete;
    CycCopyGuard &operator=(const CycCopyGuard &) = delete;

private:
    CycCopyStack &st;
};
