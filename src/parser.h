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
    const bool const_eval;
    EvalContext *const_ctx; // points to const_ctx_owner's object
    unique_ptr<CseCache> cse; // const-expr de-dup cache (per-block scopes)

    /*
     * -a/--analyze only: when set, the parser records parse-time optimizations
     * it would otherwise erase silently - a const CallExpr folded to a literal
     * (magenta) and a dead branch dropped by const-condition DCE (dim). Null in
     * a normal run, so it costs nothing. See analyzer.h / mylang.cpp.
     */
    AnalysisInfo *analysis = nullptr;

    ParseContext(const TokenStream &ts, bool const_eval);
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
