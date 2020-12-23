/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include "defs.h"

#include "errors.h"
#include "lexer.h"
#include <vector>
#include <memory>
#include <string_view>

using namespace std;

class EvalContext;
class Block;

class TokenStream {

private:

    typename vector<Tok>::const_iterator pos;
    typename vector<Tok>::const_iterator end;

public:

    TokenStream(const vector<Tok> &tokens)
        : pos(tokens.cbegin())
        , end(tokens.cend()) { }

    const Tok &get() const {

        if (pos != end)
            return *pos;

        return invalid_tok;
    }

    void next() {

        if (pos != end)
            pos++;
    }
};

class ParseContext {

public:

    TokenStream ts;
    const bool const_eval;
    EvalContext *const_ctx;

    ParseContext(const TokenStream &ts, bool const_eval);

    /* token operations */
    const Tok &operator*() const { return ts.get(); }
    const Tok &get_tok() const { return ts.get(); }
    Op get_op() const { return ts.get().op; }
    Loc get_loc() const { return ts.get().loc; }
    string_view get_str() const { return ts.get().value; }
    bool eoi() const { return ts.get() == TokType::invalid; }

    /* token operations with side-effect */
    Tok operator++(int) { Tok val = ts.get(); ts.next(); return val; }
    void next() { ts.next(); }
};

unique_ptr<Construct>
pBlock(ParseContext &c, unsigned fl = 0);
