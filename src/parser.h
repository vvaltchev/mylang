/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"

#include "errors.h"
#include "lexer.h"
#include <vector>
#include <memory>
#include <string_view>

class EvalContext;
class UniqueId;
class Block;
struct AnalysisInfo;
struct StructTypeDef;
struct TypeAnnot;
enum class DeclType : unsigned char;   /* defined in syntax.h */

/*
 * Parse-time common-subexpression cache (de-duplication of const array/dict
 * results). PIMPL'd so this header need not pull in the value model; defined
 * in parser.cpp. See cse_materialize() there.
 */
struct CseCache;

class TokenStream {

private:

    typename std::vector<Tok>::const_iterator pos;
    typename std::vector<Tok>::const_iterator end;

public:

    TokenStream(const std::vector<Tok> &tokens)
        : pos(tokens.cbegin())
        , end(tokens.cend()) { }

    const Tok &get() const {

        if (pos != end)
            return *pos;

        return invalid_tok;
    }

    /* Look ahead `n` tokens without consuming (n == 0 is get()). */
    const Tok &peek(int n) const {

        auto p = pos;
        for (int i = 0; i < n && p != end; i++)
            ++p;

        return p != end ? *p : invalid_tok;
    }

    void next() {

        if (pos != end)
            pos++;
    }
};

class ParseContext {

    unique_ptr<EvalContext> const_ctx_owner;

public:

    TokenStream ts;

    /*
     * #54 - FOLDING, NOT CONST-EVALUATION. `-nc` clears this, and it turns
     * off exactly one thing: REPLACING a constant expression by its value
     * (a call, a subscript, an operator chain, an expression statement or
     * a `var` initializer baked into a literal, and the CSE that shares
     * those). Everything else the parse-time evaluator does is part of the
     * program's MEANING and runs either way, because RULE 2 forbids a
     * switch from changing whether a program compiles or what it does:
     *  - a `const` declaration is evaluated and bound (a struct const
     *    member or another const may read it; a scalar const NAME still
     *    denotes its value, which is what a const scalar IS);
     *  - a `struct` registers its descriptor (so its name is a TYPE in a
     *    declaration, and a nested POD field embeds inline - one layout);
     *  - a `pure func` registers itself (a const initializer may call it);
     *  - a statically-dead branch (`if`/`while`/`foreach`/`?:`/`??` on a
     *    constant) is discarded, since what it holds is not checked;
     *  - a constant expression is still EVALUATED, so an error it raises
     *    is the same compile-time error.
     * It was one flag, `const_eval`, until #54: `-nc` then refused
     * `struct S { const Z = K * 2; }`, called `P p;` "not a type" and
     * boxed a nested POD field.
     */
    const bool fold;

    /*
     * Inside a `pure func` body the parse-time evaluator folds with or
     * without `-nc`: the body is the const evaluator's OWN code, and a
     * const container it reads is reachable at compile time only through a
     * folded use (`len(NAMES)`, `NAMES[1]`) - un-folded, `const T = f(5);`
     * failed under -nc with "Undefined variable 'NAMES' while evaluating a
     * PURE function" where the default compiled. So `-nc` shows a pure
     * body folded; everything else stays as written.
     */
    int pure_depth = 0;
    bool folding() const { return fold || pure_depth > 0; }
    EvalContext *const_ctx; // points to const_ctx_owner's object
    unique_ptr<CseCache> cse; // const-expr de-dup cache (per-block scopes)

    /*
     * -a/--analyze only: when set, the parser records parse-time optimizations
     * it would otherwise erase silently - a const CallExpr folded to a literal
     * (magenta) and a dead branch dropped by const-condition DCE (dim). Null in
     * a normal run, so it costs nothing. See analyzer.h / mylang.cpp.
     */
    AnalysisInfo *analysis = nullptr;

    ParseContext(const TokenStream &ts, bool fold);
    ~ParseContext(); // out-of-line: CseCache is incomplete here (PIMPL)

    /*
     * #133 - THE SHADOWED-CONST-BUILTIN SET. A declaration may name a const
     * builtin (`func abs(x) { return 42; }`, a param, a foreach var), and the
     * parse-time const evaluator used to ignore it completely: `abs(-1)`
     * folded through to the BUILTIN while `abs(runtime(-1))` called the user's
     * function - the same call spelled two ways giving two answers, and a
     * RULE 2 violation (`-nc` disagreed with the default).
     *
     * So each such name is recorded here for the extent of its scope, and
     * pAcceptId refuses to resolve it to the const builtin. A flat vector with
     * per-scope MARKS, not a map: an entry is only ever added for a name that
     * IS a const builtin, so in every real program the set is EMPTY and
     * `shadowed.empty()` short-circuits the lookup to one compare.
     *
     * NOT for a `pure func`: that IS the const evaluator's own binding (it
     * registers itself in const_ctx and must keep folding).
     */
    std::vector<const UniqueId *> shadowed;
    std::vector<size_t> shadow_marks;

    void shadow_push() { shadow_marks.push_back(shadowed.size()); }
    void shadow_pop()
    {
        ML_CHECK(!shadow_marks.empty());   /* an unbalanced push/pop */
        shadowed.resize(shadow_marks.back());
        shadow_marks.pop_back();
    }
    /* Record `uid` iff it names a const builtin (else a no-op, so the set
     * stays empty for normal code). Defined out-of-line: const_builtins. */
    void shadow_add(const UniqueId *uid);
    bool is_shadowed(const UniqueId *uid) const
    {
        if (shadowed.empty())
            return false;
        for (const UniqueId *s : shadowed)
            if (s == uid)
                return true;
        return false;
    }

    /*
     * A declaration's pending explicit-type annotation (e.g. the `int` in
     * `int x = 5`), set by pStmt/pFuncParam after recognizing the type keyword
     * and consumed where the decl's Identifier is built (pExpr14 / pFuncParam).
     * Transient: it applies to exactly the next declared identifier.
     * Initialized to DeclType::none in the (out-of-line) constructor, since the
     * enumerators aren't visible here (only a forward declaration).
     */
    DeclType pending_decl_type;

    /* When pending_decl_type == strct, the struct type of the pending decl
     * (`A obj`); nullptr otherwise. Transient, like pending_decl_type. */
    const StructTypeDef *pending_decl_struct = nullptr;

    /* When the pending decl is a PARAMETERIZED container (`array<int>`,
     * `dict<str, Point>`), its recursive element/key/value type; nullptr for a
     * generic `array`/`dict` or any non-container. Transient. */
    std::shared_ptr<TypeAnnot> pending_decl_annot;

    /*
     * The `>>` / `>>>` token-split state for parsing nested generics
     * (`array<array<int>>`): the lexer makes `>>` one token, so when a type's
     * closing `>` is part of a `>>`/`>>>`, we consume the token and record the
     * leftover `>`s here for the enclosing level(s) to consume. See
     * pAcceptCloseAngle (parser.cpp). 0 outside a type parse.
     */
    int pending_gt = 0;

    /*
     * #137: NESTING DEPTH. The parser is recursive descent, so a deeply
     * nested source recurses the C stack once per level - and `(((...1...)))`
     * at ~800 levels BLEW IT: a SIGSEGV in the release build, an ASan
     * DEADLYSIGNAL in the debug one. A crash is never an acceptable answer to
     * an input file (RULE 1), so the depth is capped and a program past it is
     * REFUSED with an ordinary syntax error.
     *
     * 256 is far above anything written or generated in practice (the corpus
     * peaks in single digits) and far below the smallest stack this runs on -
     * MSVC's debug frames are the fattest, which is why the margin is large
     * rather than tuned to the measured Linux threshold.
     */
    int nest_depth = 0;
    static const int MAX_NEST = 256;

    /*
     * #47: FUNCTION LITERALS A CONST BAKE WOULD HAVE FREED. When the parser
     * replaces a const subtree with ONE baked LiteralObj (cse_materialize),
     * the replaced subtree dies - and with it any `pure func` LITERAL inside
     * it, whose FuncDeclStmt owns the FuncDescriptor (desc_owner). A value
     * like `[pure func(a, b) => a < b]` holds a FuncObject naming exactly
     * that descriptor, so the bake left it dangling: a heap-use-after-free
     * in the inferencer's baked-value walk, and a function with no body for
     * codegen to compile. Such a decl is DETACHED instead and parked here;
     * pBlock re-inserts it as a plain expression statement just before the
     * statement that baked it, so every later pass (resolver, inferencer,
     * codegen, descriptor ownership) sees a live, ordinary lambda. `pure`
     * forbids captures, so evaluating it earlier is unobservable.
     */
    std::vector<unique_ptr<Construct>> baked_funcs;

    /* token operations */
    const Tok &operator*() const { return ts.get(); }
    const Tok &get_tok() const { return ts.get(); }
    const Tok &peek_tok(int n) const { return ts.peek(n); }
    Op get_op() const { return ts.get().op; }
    Loc get_loc() const { return ts.get().loc; }
    std::string_view get_str() const { return ts.get().value; }
    bool eoi() const { return ts.get() == TokType::invalid; }

    /* token operations with side-effect */
    Tok operator++(int) { Tok val = ts.get(); ts.next(); return val; }
    void next() { ts.next(); }
};

/*
 * Parse a block of statements. `push_const_scope` (default true) pushes a fresh
 * nested const-eval scope for the block, popped on exit - the normal lexical
 * behavior. The REPL passes false for its top-level input so the statements
 * parse directly into the persistent const context it set on `c.const_ctx`,
 * letting a `const`/`pure func`/`struct` from one input fold in the next.
 */
unique_ptr<Construct>
pBlock(ParseContext &c, unsigned fl = 0, bool push_const_scope = true);
