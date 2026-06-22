/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"

#include "errors.h"
#include "lexer.h"
#include <vector>
#include <memory>
#include <string_view>

class EvalContext;
class Block;
struct AnalysisInfo;
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
     * A declaration's pending explicit-type annotation (e.g. the `int` in
     * `int x = 5`), set by pStmt/pFuncParam after recognizing the type keyword
     * and consumed where the decl's Identifier is built (pExpr14 / pFuncParam).
     * Transient: it applies to exactly the next declared identifier.
     * Initialized to DeclType::none in the (out-of-line) constructor, since the
     * enumerators aren't visible here (only a forward declaration).
     */
    DeclType pending_decl_type;

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
