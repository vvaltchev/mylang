/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "defs.h"
#include "evalvalue.h"
#include "parser.h"
#include "uniqueid.h"

enum pFlags : unsigned {

    pNone           = 1 << 0,
    pInDecl         = 1 << 1,
    pInConstDecl    = 1 << 2,
    pInLoop         = 1 << 3,
    pInStmt         = 1 << 4,
    pInFuncBody     = 1 << 5,
    pInCatchBody    = 1 << 6,
    pInOptDecl      = 1 << 7,   /* `var opt`/`const opt`: declared nullable */
    pInDynDecl      = 1 << 8,   /* `var dyn`/`const dyn`: declared dynamic */
};

enum class ConstructType {

    other,
    nop,
    ret,
    idlist,
    block,
    id,
    lit_int,
    subscript,
};

/*
 * Static-type hint stamped on an expression node by the type inferencer
 * (inferencer.cpp) when its value is statically a non-null int or float. The
 * specializer (specialize_types) uses it to rewrite hot scalar nodes into
 * TypedScalarExpr and to drive typed-condition fast paths. `none` means "not a
 * known non-null scalar" (the default; treated dynamically).
 */
enum class TypeHint : unsigned char { none, i, f };

/*
 * Representation hint for an array-producing node (a literal, a folded
 * LiteralObj, or the args of a range()/array()/make_array() call), stamped by
 * the inferencer from the DESTINATION's inferred type so the array is built in
 * its final representation at creation - no runtime promotion. `dflt` =
 * value-driven (build flat iff the elements are homogeneous int/float). See
 * plans/type-driven-specialization.md.
 */
enum class ArrHint : unsigned char { dflt, general, flat_i, flat_f, flat_b };

/*
 * Explicit type annotation on a declaration / parameter (e.g. `int x = 5;`,
 * `func f(str s)`). `none` = no annotation (plain `var`/inferred). The scalar
 * kinds pin the symbol's static type and check assignability (with bool<=int<=
 * float coercion); `arr`/`dict` are generic kind constraints whose element/key/
 * value types are still inferred. Set by the parser onto the decl/param
 * Identifier; read by the inferencer (typing/checking) and the runtime
 * (coercion + zero-value default-init). See the README "explicit types".
 */
enum class DeclType : unsigned char {
    none, b, i, f, s, arr, dict
};

/*
 * Result of the name-resolution pass (resolver.cpp) for an Identifier.
 *
 * `local` means the identifier was resolved to a fixed slot in the current
 * function call's Frame (see eval.h), so it's an O(1) array index instead of a
 * scope-chain map lookup. `unresolved` (the default) means "fall back to the
 * runtime EvalContext map walk" - used for everything the resolver doesn't
 * (yet) handle: top-level symbols, captures, builtins, globals.
 */
enum class SymKind : unsigned char {
    unresolved,
    local,
};

struct ResolvedSym {
    SymKind kind = SymKind::unresolved;
    int slot = -1;          /* index into Frame::slots when kind == local */
};

struct InlineCtx;

class Construct {

public:
    const char *const name;
    const ConstructType ct;     /* Purpose: avoid dynamic_cast in some cases */

    bool is_const;
    Loc start;
    Loc end;

    /*
     * Set by the type inferencer when this expression is statically a non-null
     * int/float; consumed by the specializer and typed-condition fast paths.
     * Default `none`. Copied by copy_base_fields() so clones (the inliner)
     * preserve it.
     */
    TypeHint th = TypeHint::none;

    /*
     * Representation hint for an array-producing node, set by the inferencer
     * from the destination type so the array is built in its final
     * representation (type-driven creation, no promotion). On a CallExpr's args
     * ExprList for range()/array()/make_array(); on the node itself for an
     * array literal / folded LiteralObj. Default `dflt`. Copied by
     * copy_base_fields().
     */
    ArrHint arr_hint = ArrHint::dflt;

    /*
     * Set on nodes spliced in by function inlining: the "inlined-at" chain used
     * to rebuild virtual backtrace frames for the (physically absent) inlined
     * call. Null for the vast majority of nodes; consulted only on the error
     * path (see Construct::eval / flush_inline_frames). No inliner sets it yet.
     */
    const InlineCtx *inline_ctx = nullptr;

    Construct(const char *name,
              bool is_const = false,
              ConstructType ct = ConstructType::other)
        : name(name)
        , ct(ct)
        , is_const(is_const)
        , start(Loc())
        , end(Loc())
    { }

    bool is_nop() const { return ct == ConstructType::nop; }
    bool is_ret() const { return ct == ConstructType::ret; }
    bool is_idlist() const { return ct == ConstructType::idlist; }
    bool is_block() const { return ct == ConstructType::block; }
    bool is_id() const { return ct == ConstructType::id; }
    bool is_lit_int() const { return ct == ConstructType::lit_int; }
    bool is_subscript() const { return ct == ConstructType::subscript; }

    virtual ~Construct() = default;

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return none;
    }

    virtual void serialize(ostream &s, int level = 0) const = 0;
    virtual EvalValue eval(EvalContext *ctx, bool rec = true) const;

    /*
     * Typed (unboxed) evaluation, used by the M8 specializer. The default
     * implementations box through eval()/RValue (and so work for any node), so
     * a typed node may call eval_int()/eval_float() on an arbitrary child; the
     * specialized nodes override them to compute directly, avoiding num_bin_op
     * dispatch and intermediate EvalValue boxing. Only ever called on a node the
     * inferencer proved is int/float (a get<>() mismatch otherwise throws).
     */
    virtual int_type eval_int(EvalContext *ctx) const;
    virtual float_type eval_float(EvalContext *ctx) const;

    /*
     * Deep-clone this subtree: a faithful copy (children, locs, is_const,
     * inline_ctx, and any resolved/slot state). Pure virtual, so every concrete
     * node MUST implement it - a missing one is a compile error. Leaf clones
     * use clone_as() for children and copy_base_fields()/clone_ops_into()/
     * clone_elems_into() for the shared parts. Used by function inlining /
     * specialization to copy a callee body before splicing it.
     */
    virtual unique_ptr<Construct> clone() const = 0;

protected:

    void copy_base_fields(Construct &d) const {
        d.is_const = is_const;
        d.start = start;
        d.end = end;
        d.inline_ctx = inline_ctx;
        d.th = th;
        d.arr_hint = arr_hint;
    }
};

/*
 * Clone a child pointer, preserving its static type: c->clone() returns a
 * unique_ptr<Construct> whose dynamic type is exactly T, so the downcast is
 * safe. Null in -> null out.
 */
template <class T>
inline unique_ptr<T> clone_as(const unique_ptr<T> &c)
{
    return c ? unique_ptr<T>(static_cast<T *>(c->clone().release()))
             : nullptr;
}

class ChildlessConstruct : public Construct {

public:

    ChildlessConstruct(const char *name, Loc start = Loc(), Loc end = Loc())
        : Construct(name)
    {
        this->start = start;
        this->end = end;
    }

    void serialize(ostream &s, int level = 0) const override;
};

class SingleChildConstruct : public Construct {

public:
    unique_ptr<Construct> elem;

    SingleChildConstruct(const char *name) : Construct(name) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return elem->do_eval(ctx);
    }

    void serialize(ostream &s, int level = 0) const override;
};

class MultiOpConstruct : public Construct {

public:
    std::vector<std::pair<Op, unique_ptr<Construct>>> elems;

    MultiOpConstruct(const char *name) : Construct(name) { }
    void serialize(ostream &s, int level = 0) const override;

    /* Special methods */
    EvalValue eval_first_rvalue(EvalContext *ctx) const;

protected:
    void clone_ops_into(MultiOpConstruct &d) const {
        for (const auto &pr : elems)
            d.elems.emplace_back(pr.first, clone_as(pr.second));
    }
};

template <class ElemT = Construct>
class MultiElemConstruct : public Construct {

public:
    typedef ElemT ElemType;
    std::vector<unique_ptr<ElemType>> elems;

    MultiElemConstruct(const char *name, ConstructType ct = ConstructType::other)
        : Construct(name, false, ct)
    { }

    void serialize(ostream &s, int level = 0) const override;

protected:
    void clone_elems_into(MultiElemConstruct &d) const {
        for (const auto &e : elems)
            d.elems.push_back(clone_as(e));
    }
};

template <class T>
void MultiElemConstruct<T>::serialize(ostream &s, int level) const
{
    std::string indent(level * 2, ' ');

    s << indent;
    s << name << "(\n";

    for (const auto &e: elems) {
        e->serialize(s, level + 1);
        s << endl;
    }

    s << indent;
    s << ")";
}

inline ostream &operator<<(ostream &s, const Construct &c)
{
    c.serialize(s);
    return s;
}

class Literal : public Construct {

public:

    Literal() : Construct("Literal", true) { }

protected:
    /* Lets a concrete literal carry a ConstructType tag (LiteralInt uses it to
     * be recognized without a dynamic_cast on the hot compound-assign path). */
    Literal(ConstructType ct) : Construct("Literal", true, ct) { }
};

class LiteralInt final: public Literal {

    const int_type value;

public:

    LiteralInt(int_type v) : Literal(ConstructType::lit_int), value(v) { }

    int_type ival() const { return value; }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    int_type eval_int(EvalContext *ctx) const override { return value; }
    float_type eval_float(EvalContext *ctx) const override {
        return static_cast<float_type>(value);
    }

    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralInt>(value);
        copy_base_fields(*c);
        return c;
    }
};

/* The `true` / `false` literals - the two values of the bool type. */
class LiteralBool final: public Literal {

    const bool value;

public:

    LiteralBool(bool v) : value(v) { }

    bool bval() const { return value; }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return EvalValue(value);
    }

    /* a bool is 0/1 in the unboxed numeric path (bool <= int <= float) */
    int_type eval_int(EvalContext *ctx) const override { return value; }
    float_type eval_float(EvalContext *ctx) const override { return value; }

    void serialize(ostream &s, int level = 0) const override {
        s << std::string(level * 2, ' ')
          << (value ? "Bool(true)" : "Bool(false)");
    }

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralBool>(value);
        copy_base_fields(*c);
        return c;
    }
};

class LiteralFloat final: public Literal {

    const float_type value;

public:

    LiteralFloat(float_type v) : value(v) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    float_type eval_float(EvalContext *ctx) const override { return value; }

    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralFloat>(value);
        copy_base_fields(*c);
        return c;
    }
};

class LiteralNone final: public Literal {

public:

    LiteralNone() : Literal() { }
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralNone>();
        copy_base_fields(*c);
        return c;
    }
};

class NopConstruct final: public Construct {

public:

    NopConstruct() : Construct("nop", true, ConstructType::nop) { }

    virtual void serialize(ostream &s, int level = 0) const {
        /* NopConstructs should never remain in the final syntax tree */
        throw InternalErrorEx();
    }

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<NopConstruct>();
        copy_base_fields(*c);
        return c;
    }
};

class LiteralStr final: public Literal {

    EvalValue value;

public:

    LiteralStr(const std::string_view &v);
    LiteralStr(const EvalValue &v) : value(v) { }
    LiteralStr(EvalValue &&v) : value(move(v)) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralStr>(value);
        copy_base_fields(*c);
        return c;
    }
};

class LiteralArray final: public MultiElemConstruct<> {

public:

    LiteralArray() : MultiElemConstruct<>("LiteralArray") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralArray>();
        copy_base_fields(*c);
        clone_elems_into(*c);
        return c;
    }
};

/*
 * A const array/dict *value* baked into the tree as a single node, the way
 * LiteralStr bakes a string. The const-folder produces this (instead of
 * expanding the value into one Construct per element) when an operation over
 * const objects yields an array/dict - so `const y = x[1:3]` stores one node
 * holding the result, not N element literals. The stored `value` is standalone
 * (cloned at bake time, so a small slice of a huge const array does not pin the
 * huge buffer).
 *
 * do_eval hands out a *fresh, fully-mutable deep copy* of `value` on every
 * evaluation - exactly what the old per-element LiteralArray/LiteralDict
 * produced at runtime (see make_mutable_clone in eval.cpp). That freshness is
 * required: a `var` bound to it must be writable, and re-evaluating the node (a
 * loop body, a function called twice) must never observe a prior mutation.
 * Const immutability is enforced earlier, by folding const reads to literals,
 * not by runtime element flags.
 *
 * It is const (is_const = true) but deliberately NOT a `Literal`: in this
 * codebase `Literal` means a *scalar* literal (auto-const's is_scalar_literal
 * keys off it), and an array/dict value must not be mistaken for a promotable
 * scalar - just as LiteralArray/LiteralDict are not Literals either.
 */
class LiteralObj final: public Construct {

    EvalValue value;

    /*
     * True when this value materializes a `const`-decl target: do_eval then
     * hands out a deep read-only copy (see make_const_clone in eval.cpp).
     * False for a `var` target / read-only consumer: a fresh mutable copy.
     */
    bool immutable;

public:

    LiteralObj(const EvalValue &v, bool immutable = false)
        : Construct("LiteralObj", true)
        , value(v)
        , immutable(immutable) { }
    LiteralObj(EvalValue &&v, bool immutable = false)
        : Construct("LiteralObj", true)
        , value(move(v))
        , immutable(immutable) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    /* The baked const value (read-only). Used by the type inferencer to derive
     * the static type of a folded const array/dict literal. */
    const EvalValue &literal_value() const { return value; }

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralObj>(value, immutable);
        copy_base_fields(*c);
        return c;
    }
};

class LiteralDictKVPair final: public Construct {

public:
    unique_ptr<Construct> key;
    unique_ptr<Construct> value;

    LiteralDictKVPair() : Construct("LiteralDictKVPair") { }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        throw InternalErrorEx(); /* Construct not meant to be evaluated directly */
    }

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralDictKVPair>();
        copy_base_fields(*c);
        c->key = clone_as(key);
        c->value = clone_as(value);
        return c;
    }
};

class LiteralDict final: public MultiElemConstruct<LiteralDictKVPair> {

public:

    LiteralDict() : MultiElemConstruct<LiteralDictKVPair>("LiteralDict") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<LiteralDict>();
        copy_base_fields(*c);
        clone_elems_into(*c);
        return c;
    }
};


class Identifier final: public Construct {

public:

    const UniqueId *uid;
    ResolvedSym sym;        /* filled in by the name-resolution pass */

    /*
     * Only meaningful when this Identifier is a function parameter (an element
     * of FuncDeclStmt::params). `const_param` is set by the parser for a param
     * declared `const` (e.g. `func f(const x, y)`); `auto_const_param` is set
     * by the resolver when a (non-const-declared) param is never reassigned in
     * the body, so it is effectively const. Both make isconst() true for it.
     */
    bool const_param = false;
    bool auto_const_param = false;

    /*
     * Type-inference modifiers (see plans/type-inference.md). Set by the parser
     * for a param or a var/const declared `opt` (nullable: may hold `none`) or
     * `dyn` (dynamically typed: behaves as today, inference does not constrain
     * it). Only meaningful on a declaration / parameter identifier.
     */
    bool opt_mod = false;
    bool dyn_mod = false;

    /*
     * Explicit type annotation (`int x = ...`, `func f(str s)`). `none` when the
     * declaration is a plain `var`/`const`/inferred param. Propagated by the
     * resolver from a declaration to every use, so an assignment can coerce.
     */
    DeclType decl_type = DeclType::none;

    Identifier(const std::string_view &str)
        : Construct("Id", false, ConstructType::id)
        , uid(UniqueId::get(str))
    { }

    std::string_view get_str() const { return uid->val; }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    int_type eval_int(EvalContext *ctx) const override;
    float_type eval_float(EvalContext *ctx) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Identifier>(get_str());
        copy_base_fields(*c);
        c->sym = sym;
        c->const_param = const_param;
        c->auto_const_param = auto_const_param;
        c->opt_mod = opt_mod;
        c->dyn_mod = dyn_mod;
        c->decl_type = decl_type;
        return c;
    }
};

class ExprList final: public MultiElemConstruct<> {

public:

    /*
     * Named-argument labels, parallel to `elems`. A *transient* carrier from
     * the parser to the inferencer's lower_named_args() desugaring step, which
     * rewrites a named call into a plain positional one and clears these. EMPTY
     * means every argument is positional (the common case, zero overhead);
     * otherwise it is sized == elems.size(), with arg_names[i] == nullptr for a
     * positional argument and the param label (an interned UniqueId, so it is
     * pointer-comparable to a param Identifier's `uid`) for a named one. By the
     * time any later pass (resolve_names, the optimizers, eval) sees this list
     * it is positional again, so named args have zero effect on them.
     */
    std::vector<const UniqueId *> arg_names;

    ExprList() : MultiElemConstruct<>("ExprList") { }
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<ExprList>();
        copy_base_fields(*c);
        clone_elems_into(*c);
        c->arg_names = arg_names;
        return c;
    }
};

class IdList final: public MultiElemConstruct<Identifier> {

public:

    IdList()
        : MultiElemConstruct<Identifier>("IdList", ConstructType::idlist)
    { }

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<IdList>();
        copy_base_fields(*c);
        clone_elems_into(*c);
        return c;
    }
};

class CallExpr final: public Construct {

public:
    unique_ptr<Construct> what;
    unique_ptr<ExprList> args;

    CallExpr() : Construct("CallExpr") { }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<CallExpr>();
        copy_base_fields(*c);
        c->what = clone_as(what);
        c->args = clone_as(args);
        return c;
    }
};

/*
 * A normalized view of one callee parameter, enough to desugar named arguments
 * against it: the interned name (pointer-comparable to ExprList::arg_names) and
 * whether it is optional. Both desugaring sites build a vector of these from
 * their own param representation (the parser from a FuncObject's `Identifier`s,
 * the inferencer from its `TypeSym`s) and hand it to desugar_named_call.
 */
struct ParamSpec {
    const UniqueId *name;
    bool opt;
};

/*
 * Rewrite a named-argument call (`call->args` carries ExprList::arg_names) into
 * the equivalent positional one: map each label to its parameter by name,
 * enforce the strict ordering (a leading run of positional args, then names in
 * declaration order), fill a skipped interior optional with `none`, and replace
 * `call->args` with the positional list. Reordering / duplicate / unknown name
 * / too-many / missing-required throw a compile-time TypeMismatchEx /
 * WrongArgCountEx. The single source of truth shared by both desugaring sites
 * (parser const-fold path and the inferencer's lower_named_args).
 */
void desugar_named_call(CallExpr *call, const std::vector<ParamSpec> &params);

class Expr01 final: public SingleChildConstruct {

public:
    Expr01() : SingleChildConstruct("Expr01") { }

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr01>();
        copy_base_fields(*c);
        c->elem = clone_as(elem);
        return c;
    }
};

class Expr02 final: public MultiOpConstruct {

public:

    Expr02() : MultiOpConstruct("Expr02") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr02>();
        copy_base_fields(*c);
        clone_ops_into(*c);
        return c;
    }
};


class Expr03 final: public MultiOpConstruct {

public:

    Expr03() : MultiOpConstruct("Expr03") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr03>();
        copy_base_fields(*c);
        clone_ops_into(*c);
        return c;
    }
};

class Expr04 final: public MultiOpConstruct {

public:

    Expr04() : MultiOpConstruct("Expr04") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr04>();
        copy_base_fields(*c);
        clone_ops_into(*c);
        return c;
    }
};

class Expr06 final: public MultiOpConstruct {

public:

    Expr06() : MultiOpConstruct("Expr06") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr06>();
        copy_base_fields(*c);
        clone_ops_into(*c);
        return c;
    }
};

class Expr07 final: public MultiOpConstruct {

public:

    Expr07() : MultiOpConstruct("Expr07") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr07>();
        copy_base_fields(*c);
        clone_ops_into(*c);
        return c;
    }
};

class Expr11 final: public MultiOpConstruct {

public:

    Expr11() : MultiOpConstruct("Expr11") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr11>();
        copy_base_fields(*c);
        clone_ops_into(*c);
        return c;
    }
};

class Expr12 final: public MultiOpConstruct {

public:

    Expr12() : MultiOpConstruct("Expr12") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr12>();
        copy_base_fields(*c);
        clone_ops_into(*c);
        return c;
    }
};

/*
 * A specialized scalar expression, produced by the M8 type specializer
 * (specialize_types, inferencer.cpp) from a generic Expr03/04/06/07/11/12/02
 * whose operands the inferencer proved are int/float. It evaluates through
 * eval_int()/eval_float() with no num_bin_op promotion dispatch, no PMF virtual
 * call, and no intermediate EvalValue boxing - the tree-walker's speed payoff.
 * `kind` is the operand/result kind (i or f). `cat` selects the operation
 * family; `elems` mirrors MultiOpConstruct (the leading op is Op::invalid).
 */
class TypedScalarExpr final: public Construct {

public:
    enum class Cat : unsigned char { arith, cmp, logical, neg, lnot };

    Cat cat;
    TypeHint kind;     /* operand kind (i/f); result is `kind` except cmp/lnot */
    std::vector<std::pair<Op, unique_ptr<Construct>>> elems;

    TypedScalarExpr(Cat cat, TypeHint kind)
        : Construct("TypedScalarExpr"), cat(cat), kind(kind) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    int_type eval_int(EvalContext *ctx) const override;
    float_type eval_float(EvalContext *ctx) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<TypedScalarExpr>(cat, kind);
        copy_base_fields(*c);
        for (const auto &pr : elems)
            c->elems.emplace_back(pr.first, clone_as(pr.second));
        return c;
    }
};

class Expr14 final: public Construct {

public:
    unique_ptr<Construct> lvalue;
    unique_ptr<Construct> rvalue;
    unsigned fl;
    Op op;

    Expr14() : Construct("Expr14"), fl(pNone), op(Op::invalid) { }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Expr14>();
        copy_base_fields(*c);
        c->lvalue = clone_as(lvalue);
        c->rvalue = clone_as(rvalue);
        c->fl = fl;
        c->op = op;
        return c;
    }
};

class IfStmt final: public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> thenBlock;
    unique_ptr<Construct> elseBlock;

    IfStmt() : Construct("IfStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<IfStmt>();
        copy_base_fields(*c);
        c->condExpr = clone_as(condExpr);
        c->thenBlock = clone_as(thenBlock);
        c->elseBlock = clone_as(elseBlock);
        return c;
    }
};

class Block final: public MultiElemConstruct<> {

public:
    /*
     * Slot range [slot_start, slot_start + slot_count) of the resolved locals
     * declared inside this block (assigned by the resolver; 0/0 when the block
     * has no slotted locals or its function isn't resolved). Block::do_eval
     * clears these slots' live bits on entry so a re-entered block (e.g. a loop
     * body) starts with its locals undefined again - matching the old
     * fresh-EvalContext-per-iteration semantics.
     */
    int slot_start = 0;
    int slot_count = 0;

    /*
     * Set by the resolver when every declaration in this block is a frame slot
     * (no map-bound decl: no capture, no nested-func name, no slot-budget
     * overflow). Such a block never touches the EvalContext map, so do_eval can
     * run its statements in the parent context and skip building (and tearing
     * down) a per-entry EvalContext - a real win for loop/if bodies, which
     * re-enter every iteration. Default false (keep the scope) is always safe.
     */
    bool scope_free = false;

    Block() : MultiElemConstruct("Block", ConstructType::block) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Block>();
        copy_base_fields(*c);
        clone_elems_into(*c);
        c->slot_start = slot_start;
        c->slot_count = slot_count;
        c->scope_free = scope_free;
        return c;
    }
};

class BreakStmt final: public ChildlessConstruct {

public:
    BreakStmt(): ChildlessConstruct("BreakStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<BreakStmt>();
        copy_base_fields(*c);
        return c;
    }
};

class ContinueStmt final: public ChildlessConstruct {

public:
    ContinueStmt(): ChildlessConstruct("ContinueStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<ContinueStmt>();
        copy_base_fields(*c);
        return c;
    }
};

class ReturnStmt final: public Construct {

public:
    unique_ptr<Construct> elem;

    ReturnStmt(): Construct("ReturnStmt", false, ConstructType::ret) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<ReturnStmt>();
        copy_base_fields(*c);
        c->elem = clone_as(elem);
        return c;
    }
};

class WhileStmt final: public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> body;

    WhileStmt() : Construct("WhileStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<WhileStmt>();
        copy_base_fields(*c);
        c->condExpr = clone_as(condExpr);
        c->body = clone_as(body);
        return c;
    }
};

class FuncDeclStmt final: public Construct {

public:
    unique_ptr<Identifier> id;  /* NULL when the func is defined inside an expr */
    unique_ptr<IdList> captures;
    unique_ptr<IdList> params;
    unique_ptr<Construct> body;

    /*
     * Filled in by the name-resolution pass (resolver.cpp). When `resolved` is
     * true, do_func_call builds a Frame of `frame_size` slots (params first,
     * then locals) instead of an EvalContext map, and body references to those
     * symbols are O(1) slot reads.
     *
     * `slot_writes[i]` counts how many times slot i is written in the body: for
     * a param that's body reassignments only (so 0 == never reassigned); for a
     * local it includes the declaration (so 1 == declared once, never
     * reassigned == write-once). Slots 0..params-1 are the params. Used by the
     * auto-const folder and to detect auto-const parameters.
     */
    bool resolved = false;
    int frame_size = 0;
    std::vector<int> slot_writes;

    /*
     * Purity. `explicit_pure` is set by the parser for a `pure func`.
     * `effective_pure` is `explicit_pure` OR a function the resolver proves
     * effectively pure (reads only consts + its params, calls only const
     * builtins / pure funcs, no captures). isconst()/ispure() read these; an
     * effectively-pure function's calls fold when given constant arguments.
     */
    bool explicit_pure = false;
    bool effective_pure = false;

    /*
     * Name to show in a backtrace, when it should differ from `id`. Empty for
     * normal functions (the backtrace uses `id`). A specialization clone has a
     * synthetic `id` (so its call resolves to the clone) but sets this to the
     * original function's name, so errors still report the user's name.
     */
    std::string display_name;

    FuncDeclStmt() : Construct("FuncDeclStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<FuncDeclStmt>();
        copy_base_fields(*c);
        c->id = clone_as(id);
        c->captures = clone_as(captures);
        c->params = clone_as(params);
        c->body = clone_as(body);
        c->resolved = resolved;
        c->frame_size = frame_size;
        c->slot_writes = slot_writes;
        c->explicit_pure = explicit_pure;
        c->effective_pure = effective_pure;
        c->display_name = display_name;
        return c;
    }
};

class Subscript final: public Construct {

public:

    unique_ptr<Construct> what;
    unique_ptr<Construct> index;

    Subscript() : Construct("Subscript", false, ConstructType::subscript) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    int_type eval_int(EvalContext *ctx) const override;
    float_type eval_float(EvalContext *ctx) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Subscript>();
        copy_base_fields(*c);
        c->what = clone_as(what);
        c->index = clone_as(index);
        return c;
    }
};

class Slice final: public Construct {

public:

    unique_ptr<Construct> what;
    unique_ptr<Construct> start_idx;
    unique_ptr<Construct> end_idx;

    Slice() : Construct("Slice") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<Slice>();
        copy_base_fields(*c);
        c->what = clone_as(what);
        c->start_idx = clone_as(start_idx);
        c->end_idx = clone_as(end_idx);
        return c;
    }
};

struct AllowedExList {

    unique_ptr<IdList> exList;
    unique_ptr<Identifier> asId;
};

class TryCatchStmt final: public Construct {

public:

    unique_ptr<Construct> tryBody;
    unique_ptr<Construct> finallyBody;
    std::vector<std::pair<AllowedExList, unique_ptr<Construct>>> catchStmts;

    TryCatchStmt() : Construct("TryCatchStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<TryCatchStmt>();
        copy_base_fields(*c);
        c->tryBody = clone_as(tryBody);
        c->finallyBody = clone_as(finallyBody);
        for (const auto &cs : catchStmts) {
            AllowedExList ael;
            ael.exList = clone_as(cs.first.exList);
            ael.asId = clone_as(cs.first.asId);
            c->catchStmts.emplace_back(move(ael), clone_as(cs.second));
        }
        return c;
    }
};

class RethrowStmt final: public ChildlessConstruct {

public:

    RethrowStmt(Loc start = Loc(), Loc end = Loc())
        : ChildlessConstruct("RethrowStmt", start, end) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<RethrowStmt>();
        copy_base_fields(*c);
        return c;
    }
};

class ThrowStmt final: public SingleChildConstruct {

public:
    ThrowStmt(): SingleChildConstruct("ThrowStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<ThrowStmt>();
        copy_base_fields(*c);
        c->elem = clone_as(elem);
        return c;
    }
};

class ForeachStmt final: public Construct {

    bool do_iter(EvalContext *ctx,
                 size_type index,
                 const EvalValue *elems,
                 size_type count) const;

public:
    unique_ptr<IdList> ids;
    unique_ptr<Construct> container;
    unique_ptr<Construct> body;
    bool idsVarDecl;
    bool indexed;

    ForeachStmt() : Construct("ForeachStmt"), idsVarDecl(false), indexed(false) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<ForeachStmt>();
        copy_base_fields(*c);
        c->ids = clone_as(ids);
        c->container = clone_as(container);
        c->body = clone_as(body);
        c->idsVarDecl = idsVarDecl;
        c->indexed = indexed;
        return c;
    }
};

class MemberExpr final: public Construct {

public:

    unique_ptr<Construct> what;
    EvalValue memId;

    MemberExpr() : Construct("MemberExpr") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<MemberExpr>();
        copy_base_fields(*c);
        c->what = clone_as(what);
        c->memId = memId;
        return c;
    }
};

class ForStmt final: public Construct {

public:

    unique_ptr<Construct> init;
    unique_ptr<Construct> cond;
    unique_ptr<Construct> inc;
    unique_ptr<Construct> body;

    ForStmt() : Construct("ForStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;

    unique_ptr<Construct> clone() const override {
        auto c = make_unique<ForStmt>();
        copy_base_fields(*c);
        c->init = clone_as(init);
        c->cond = clone_as(cond);
        c->inc = clone_as(inc);
        c->body = clone_as(body);
        return c;
    }
};
