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
};

enum class ConstructType {

    other,
    nop,
    ret,
    idlist,
    block,
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

class Construct {

public:
    const char *const name;
    const ConstructType ct;     /* Purpose: avoid dynamic_cast in some cases */

    bool is_const;
    Loc start;
    Loc end;

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

    virtual ~Construct() = default;

    virtual EvalValue do_eval(EvalContext *ctx, bool rec = true) const {
        return none;
    }

    virtual void serialize(ostream &s, int level = 0) const = 0;
    virtual EvalValue eval(EvalContext *ctx, bool rec = true) const;
};

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
};

class LiteralInt final: public Literal {

    const int_type value;

public:

    LiteralInt(int_type v) : value(v) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    void serialize(ostream &s, int level = 0) const override;
};

class LiteralFloat final: public Literal {

    const float_type value;

public:

    LiteralFloat(float_type v) : value(v) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override {
        return value;
    }

    void serialize(ostream &s, int level = 0) const override;
};

class LiteralNone final: public Literal {

public:

    LiteralNone() : Literal() { }
    void serialize(ostream &s, int level = 0) const override;
};

class NopConstruct final: public Construct {

public:

    NopConstruct() : Construct("nop", true, ConstructType::nop) { }

    virtual void serialize(ostream &s, int level = 0) const {
        /* NopConstructs should never remain in the final syntax tree */
        throw InternalErrorEx();
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
};

class LiteralArray final: public MultiElemConstruct<> {

public:

    LiteralArray() : MultiElemConstruct<>("LiteralArray") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
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

public:

    LiteralObj(const EvalValue &v)
        : Construct("LiteralObj", true), value(v) { }
    LiteralObj(EvalValue &&v)
        : Construct("LiteralObj", true), value(move(v)) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
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
};

class LiteralDict final: public MultiElemConstruct<LiteralDictKVPair> {

public:

    LiteralDict() : MultiElemConstruct<LiteralDictKVPair>("LiteralDict") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
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

    Identifier(const std::string_view &str)
        : Construct("Id")
        , uid(UniqueId::get(str))
    { }

    std::string_view get_str() const { return uid->val; }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ExprList final: public MultiElemConstruct<> {

public:

    ExprList() : MultiElemConstruct<>("ExprList") { }
};

class IdList final: public MultiElemConstruct<Identifier> {

public:

    IdList()
        : MultiElemConstruct<Identifier>("IdList", ConstructType::idlist)
    { }
};

class CallExpr final: public Construct {

public:
    unique_ptr<Construct> what;
    unique_ptr<ExprList> args;

    CallExpr() : Construct("CallExpr") { }
    void serialize(ostream &s, int level = 0) const override;
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr01 final: public SingleChildConstruct {

public:
    Expr01() : SingleChildConstruct("Expr01") { }
};

class Expr02 final: public MultiOpConstruct {

public:

    Expr02() : MultiOpConstruct("Expr02") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};


class Expr03 final: public MultiOpConstruct {

public:

    Expr03() : MultiOpConstruct("Expr03") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr04 final: public MultiOpConstruct {

public:

    Expr04() : MultiOpConstruct("Expr04") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr06 final: public MultiOpConstruct {

public:

    Expr06() : MultiOpConstruct("Expr06") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr07 final: public MultiOpConstruct {

public:

    Expr07() : MultiOpConstruct("Expr07") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr11 final: public MultiOpConstruct {

public:

    Expr11() : MultiOpConstruct("Expr11") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class Expr12 final: public MultiOpConstruct {

public:

    Expr12() : MultiOpConstruct("Expr12") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
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
};

class IfStmt final: public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> thenBlock;
    unique_ptr<Construct> elseBlock;

    IfStmt() : Construct("IfStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
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

    Block() : MultiElemConstruct("Block", ConstructType::block) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class BreakStmt final: public ChildlessConstruct {

public:
    BreakStmt(): ChildlessConstruct("BreakStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ContinueStmt final: public ChildlessConstruct {

public:
    ContinueStmt(): ChildlessConstruct("ContinueStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ReturnStmt final: public Construct {

public:
    unique_ptr<Construct> elem;

    ReturnStmt(): Construct("ReturnStmt", false, ConstructType::ret) { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class WhileStmt final: public Construct {

public:
    unique_ptr<Construct> condExpr;
    unique_ptr<Construct> body;

    WhileStmt() : Construct("WhileStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
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

    FuncDeclStmt() : Construct("FuncDeclStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class Subscript final: public Construct {

public:

    unique_ptr<Construct> what;
    unique_ptr<Construct> index;

    Subscript() : Construct("Subscript") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
};

class Slice final: public Construct {

public:

    unique_ptr<Construct> what;
    unique_ptr<Construct> start_idx;
    unique_ptr<Construct> end_idx;

    Slice() : Construct("Slice") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
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
};

class RethrowStmt final: public ChildlessConstruct {

public:

    RethrowStmt(Loc start = Loc(), Loc end = Loc())
        : ChildlessConstruct("RethrowStmt", start, end) { }

    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
};

class ThrowStmt final: public SingleChildConstruct {

public:
    ThrowStmt(): SingleChildConstruct("ThrowStmt") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
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
};

class MemberExpr final: public Construct {

public:

    unique_ptr<Construct> what;
    EvalValue memId;

    MemberExpr() : Construct("MemberExpr") { }
    EvalValue do_eval(EvalContext *ctx, bool rec = true) const override;
    void serialize(ostream &s, int level = 0) const override;
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
};
