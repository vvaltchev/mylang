#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <array>
#include <tuple>
#include <string_view>
#include <variant>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <cctype>
#include <cassert>

using namespace std;

enum class TokType {
    invalid = 0,
    num = 1,
    id = 2,
    op = 3,
    unknown = 4,
};

enum class Op {

    invalid,

    plus,
    minus,
    times,
    div,
    parenL,
    parenR,
};

static const string OpToString[] =
{
    "invalid",

    "+",
    "-",
    "*",
    "/",
    "(",
    ")",
};

struct InvalidTokenEx {
    string_view val;
};

struct SyntaxErrorEx { };
struct DivisionByZeroEx { };

ostream &operator<<(ostream &s, TokType t)
{
    static const char *tt_str[] =
    {
        "inv",
        "num",
        "id_",
        "op_",
        "unk",
    };

    return s << tt_str[(int)t];
}

Op get_op_type(string_view val)
{
    if (val.empty())
        return Op::invalid;

    switch (val[0]) {

        case '+':
            return Op::plus;

        case '-':
            return Op::minus;

        case '*':
            return Op::times;

        case '/':
            return Op::div;

        case '(':
            return Op::parenL;

        case ')':
            return Op::parenR;

        default:
            return Op::invalid;
    }
}

class Tok {

public:

    const string_view value;
    const TokType type;
    const Op op;

    Tok(TokType type = TokType::invalid, string_view value = string_view())
        : type(type)
        , value(value)
        , op(value.empty() ? Op::invalid : get_op_type(value))
    { }

    Tok(Op op)
        : type(TokType::op)
        , op(op)
    { }

    Tok(const Tok &rhs) = default;
    Tok &operator=(const Tok &rhs) = delete;

    bool operator==(Op rhs) const {
        return op == rhs;
    }

    bool operator==(TokType t) const {
        return type == t;
    }

    bool operator==(const Tok &rhs) const {

        if (type == TokType::op)
            return op == rhs.op;

        return type == rhs.type;
    }
};

static const set<string, less<>> operators = {
    "+", "-", "*", "/", "(", ")",
};

bool is_operator(string_view s)
{
    return operators.find(s) != operators.end();
}

void
tokenize(string_view in_str, vector<Tok> &result)
{
    int i, tok_start = 0;
    TokType tok_type = TokType::invalid;

    for (i = 0; i < in_str.length(); i++) {

        if (tok_type == TokType::invalid)
            tok_start = i;

        const char c = in_str[i];
        string_view val = in_str.substr(tok_start, i - tok_start + 1);
        string_view val_until_prev = in_str.substr(tok_start, i - tok_start);

        if (isspace(c) || is_operator(string_view(&c, 1))) {

            if (tok_type != TokType::invalid) {
                result.emplace_back(tok_type, val_until_prev);
                tok_type = TokType::invalid;
            }

            if (!isspace(c)) {

                string_view op = in_str.substr(i, 1);

                if (i + 1 < in_str.length() &&
                    is_operator(in_str.substr(i, 2)))
                {
                    /*
                     * Handle two-chars wide operators. Note: it is required,
                     * with the current implementation, an 1-char prefix operator to
                     * exist for each one of them. For example:
                     *
                     *      <= requires '<' to exist independently
                     *      += requires '+' to exist independently
                     *
                     * Reason: the check `is_operator(string_view(&c, 1))` above.
                     */
                    op = in_str.substr(i, 2);
                    i++;
                }

                result.emplace_back(TokType::op, op);
            }

        } else if (isalnum(c) || c == '_') {

            if (tok_type == TokType::invalid) {

                tok_start = i;

                if (isdigit(c))
                    tok_type = TokType::num;
                else
                    tok_type = TokType::id;

            } else if (tok_type == TokType::num) {

                if (!isdigit(c))
                    throw InvalidTokenEx{val};
            }

        } else {

            if (tok_type != TokType::invalid)
                throw InvalidTokenEx{val};

            tok_start = i;
            tok_type = TokType::unknown;
        }
    }

    if (tok_type != TokType::invalid)
        result.emplace_back(tok_type, in_str.substr(tok_start, i - tok_start));
}

class TokenStream {

private:

    typename vector<Tok>::const_iterator pos;
    typename vector<Tok>::const_iterator end;

public:

    TokenStream(const vector<Tok> &tokens)
        : pos(tokens.cbegin())
        , end(tokens.cend()) { }

    Tok get() const {

        if (pos != end)
            return *pos;

        return Tok();
    }

    void next() {

        if (pos != end)
            pos++;
    }
};


typedef TokenStream TS;


class Context {

public:
    TS ts;

    /* token operations */
    Tok operator*() const { return ts.get(); }
    Op get_op() const { return ts.get().op; }
    string_view get_str() const { return ts.get().value; }

    /* token operations with side-effect */
    Tok operator++(int dummy) { Tok val = ts.get(); ts.next(); return val; }
    void next() { ts.next(); }
};

// ----------- Recursive Descent Parser -------------------

class LiteralInt;
class Factor;
class Term;
class Expr;

class EvalValue {

public:
    long value;

    EvalValue(long v) : value(v) { }
};

class EvalContext {

public:
};

class Construct {

public:
    Construct() = default;
    virtual ~Construct() = default;
    virtual EvalValue eval(EvalContext *) const = 0;
    virtual void serialize(ostream &s, int level = 0) const = 0;
};

ostream &operator<<(ostream &s, const Construct &c)
{
    c.serialize(s);
    return s;
}


class LiteralInt : public Construct {

public:
    long value;

    LiteralInt(long v) : value(v) { }

    virtual ~LiteralInt() = default;

    virtual EvalValue eval(EvalContext *ctx) const {
        return value;
    }

    virtual void serialize(ostream &s, int level = 0) const {

        string indent(level * 2, ' ');

        s << indent;
        s << string("Int(");
        s << to_string(value);
        s << ")";
    }
};


class Factor : public Construct {

public:
    Construct *value;

    virtual ~Factor() = default;

    virtual EvalValue eval(EvalContext *ctx) const {
        return value->eval(ctx);
    }

    virtual void serialize(ostream &s, int level = 0) const {

        string indent(level * 2, ' ');

        s << indent;
        s << "Factor(" << endl;
        value->serialize(s, level + 1);
        s << endl;
        s << string(level * 2, ' ');
        s << ")";
    }
};

class Term : public Construct {

public:
    vector<pair<Op, Factor *>> factors;

    virtual ~Term() = default;

    virtual EvalValue eval(EvalContext *ctx) const {

        long val = 1;

        for (const auto &[op, f] : factors) {

            if (op == Op::times) {

                val *= f->eval(ctx).value;

            } else if (op == Op::div) {

                long e = f->eval(ctx).value;

                if (e == 0)
                    throw DivisionByZeroEx();

                val /= e;

            } else {

                val = f->eval(ctx).value;
            }
        }

        return val;
    }

    virtual void serialize(ostream &s, int level = 0) const {

        string indent(level * 2, ' ');

        s << indent;
        s << "Term(\n";

        for (const auto &[op, f] : factors) {

            if (op != Op::invalid) {
                s << string((level + 1) * 2, ' ');
                s << "Op '" << OpToString[(int)op] << "'";
                s << endl;
            }

            f->serialize(s, level + 1);
            s << endl;
        }

        s << indent;
        s << ")";
    }
};

class Expr : public Construct {

public:
    vector<pair<Op, Term *>> terms;

    virtual ~Expr() = default;

    virtual EvalValue eval(EvalContext *ctx) const {

        long val = 0;

        for (const auto &[op, t] : terms) {

            if (op == Op::plus)
                val += t->eval(ctx).value;
            else if (op == Op::minus)
                val -= t->eval(ctx).value;
            else if (op == Op::invalid)
                val = t->eval(ctx).value;

        }

        return val;
    }

    virtual void serialize(ostream &s, int level = 0) const {

        string indent(level * 2, ' ');

        s << indent;
        s << "Expr(";
        s << endl;

        for (const auto &[op, t] : terms) {

            if (op != Op::invalid) {
                s << string((level + 1) * 2, ' ');
                s << "Op '" << OpToString[(int)op] << "'";
                s << endl;
            }

            t->serialize(s, level + 1);
            s << endl;
        }

        s << indent;
        s << ")";
    }
};

bool pAcceptLiteralInt(Context &c, Construct *&v);
bool pAcceptOp(Context &c, Op exp);
void pExpectLiteralInt(Context &c, Construct *&v);
void pExpectOp(Context &c, Op exp);

Factor *pFactor(Context &c);
Term *pTerm(Context &c);
Expr *pExpr(Context &c);

bool pAcceptLiteralInt(Context &c, Construct *&v)
{
    if (*c == TokType::num) {
        v = new LiteralInt{ stol(string(c.get_str())) };
        c++;
        return true;
    }

    return false;
}

bool pAcceptOp(Context &c, Op exp)
{
    if (*c == exp) {
        c++;
        return true;
    }

    return false;
}

void pExpectLiteralInt(Context &c, Construct *&v)
{
    if (!pAcceptLiteralInt(c, v))
        throw SyntaxErrorEx();
}

void pExpectOp(Context &c, Op exp)
{
    if (!pAcceptOp(c, exp))
        throw SyntaxErrorEx();
}

Factor *pFactor(Context &c)
{
    Factor *ret = new Factor;
    Construct *v;

    if (pAcceptLiteralInt(c, v)) {

        ret->value = v;

    } else if (pAcceptOp(c, Op::parenL)) {

        ret->value = pExpr(c);
        pExpectOp(c, Op::parenR);

    } else {

        throw SyntaxErrorEx();
    }

    return ret;
}

Term *pTerm(Context &c)
{
    Term *ret = new Term;
    Factor *f;

    f = pFactor(c);
    ret->factors.emplace_back(Op::invalid, f);

    while (*c == Op::times || *c == Op::div) {
        Op op = c.get_op();
        c++;
        f = pFactor(c);
        ret->factors.emplace_back(op, f);
    }

    return ret;
}

Expr *pExpr(Context &c)
{
    Expr *e = new Expr();
    Op op = Op::invalid;
    Term *t;

    if (*c == Op::plus || *c == Op::minus) {
        op = c.get_op();
        c++;
    }

    t = pTerm(c);
    e->terms.emplace_back(op, t);

    while (*c == Op::plus || *c == Op::minus) {
        op = c.get_op();
        c++;
        t = pTerm(c);
        e->terms.emplace_back(op, t);
    }

    return e;
}

// ----------- Recursive Descent Parser [end] -------------

void help()
{
    cout << "syntax:" << endl;
    cout << "   mylang < FILE" << endl;
    cout << "   mylang -e EXPRESSION" << endl;
    cout << endl;
}

void parse_args(int argc, char **argv, vector<string> &lines, vector<Tok> &tokens)
{
    if (argc >= 3) {

        string a1 = argv[1];

        if (a1 == "-e") {

            lines.emplace_back(argv[2]);
            tokenize(lines[0], tokens);

        } else {

            help();
            exit(1);
        }

    } else if (argc > 1) {

        string a1 = argv[1];

        if (a1 == "-h" || a1 == "--help") {
            help();
            exit(0);
        }
    }
}

void read_input(vector<string>& lines, vector<Tok> &tokens)
{
    if (tokens.empty()) {

        string line;

        while (getline(cin, line))
            lines.push_back(move(line));

        for (const auto &s: lines)
            tokenize(s, tokens);

        if (tokens.empty()) {
            help();
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    vector<string> lines;
    vector<Tok> tokens;

    try {

        parse_args(argc, argv, lines, tokens);
        read_input(lines, tokens);

        Context ctx{TS(tokens)};

        if (1) {
            cout << "Tokens" << endl;
            cout << "--------------------------" << endl;

            for (auto tok : tokens) {
                cout << "Tok(" << tok.type << "): '" << tok.value << "'" << endl;
            }
        }

        cout << "Syntax tree" << endl;
        cout << "--------------------------" << endl;

        Construct *c = pExpr(ctx);
        cout << *c << endl;

        cout << endl;
        cout << "Value" << endl;
        cout << "--------------------------" << endl;

        cout << c->eval(nullptr).value << endl;


    } catch (InvalidTokenEx e) {

        cout << "Invalid token: " << e.val << endl;

    } catch (SyntaxErrorEx e) {

        cout << "SyntaxError" << endl;

    } catch (DivisionByZeroEx e) {

        cout << "DivisionByZeroEx" << endl;
    }

    return 0;
}