/* SPDX-License-Identifier: BSD-2-Clause */

#include "eval.h"
#include "errors.h"
#include "syntax.h"
#include "lexer.h"
#include "backtrace.h"
#include "bitops.h"

#include <cmath>

using std::pair;
using std::vector;
using std::string;
using std::string_view;

/* ------------------ string un-escaping -------------- */

string
unescape_str(const string_view &v)
{
    string s;
    s.reserve(v.size());

    for (size_t i = 0; i < v.size(); i++) {

        if (v[i] == '\\') {

            /*
             * We know FOR SURE that '\' is NOT the last char in the
             * literal simply because otherwise we'll have something like
             * "xxx\" and the tokenize will accept < xxx" > instead of < xxx\ >.
             */

            switch (v[i + 1]) {

                case '\\':
                    s += '\\';
                    break;
                case '\"':
                    s += '\"';
                    break;
                case 'r':
                    s += '\r';
                    break;
                case 'n':
                    s += '\n';
                    break;
                case 't':
                    s += '\t';
                    break;
                case 'v':
                    s += '\v';
                    break;
                case 'a':
                    s += '\a';
                    break;
                case 'b':
                    s += '\b';
                    break;

                default:
                    s += v[i];
                    s += v[i + 1];
                    break;
            }

            i++;

        } else {

            s += v[i];
        }
    }

    return s;
}

/* ------------------ EvalContext ------------------ */

EvalContext::EvalContext(EvalContext *parent, bool const_ctx, bool func_ctx)
    : parent(parent)
    , const_ctx(const_ctx)
    , func_ctx(func_ctx)
    , frame(parent ? parent->frame : nullptr)
    , flow((parent && !func_ctx) ? parent->flow : &flow_state)
{
    if (!parent) {
        symbols.insert(const_builtins.begin(), const_builtins.end());

        if (!const_ctx) {
            symbols.insert(builtins.begin(), builtins.end());
        }
    }
}

LValue *EvalContext::lookup(const Identifier *id)
{
    auto &&it = symbols.find(id->uid);

    if (it != symbols.end())
        return &it->second;

    return nullptr;
}

bool EvalContext::erase(const Identifier *id)
{
    if (id->sym.kind == SymKind::local && frame) {

        /* undef() on a resolved local: clear its live bit (cannot store the
         * UndefinedId sentinel in a slot - LValue forbids it). */
        const uint64_t bit = static_cast<uint64_t>(1) << id->sym.slot;
        const bool was_defined = frame->live & bit;
        frame->live &= ~bit;
        return was_defined;
    }

    const auto &it = symbols.find(id->uid);

    if (it == symbols.end())
        return false;

    symbols.erase(it);
    return true;
}

void EvalContext::emplace(const Identifier *id, const EvalValue &val, bool is_const)
{
    symbols.emplace(id->uid, LValue(val, is_const));
}

void EvalContext::emplace(const Identifier *id, EvalValue &&val, bool is_const)
{
    symbols.emplace(id->uid, LValue(move(val), is_const));
}

void EvalContext::emplace(const std::string_view &id, EvalValue &&val, bool is_const)
{
    symbols.emplace(UniqueId::get(id), LValue(move(val), is_const));
}

void EvalContext::collect_symbols(
    std::vector<std::pair<const UniqueId *, const LValue *>> &out) const
{
    for (const auto &kv : symbols)
        out.emplace_back(kv.first, &kv.second);
}

/* ------------------ Constructs ------------------- */

EvalValue Construct::eval(EvalContext *ctx, bool rec) const
{
    try {

        return do_eval(ctx, rec);

    } catch (Exception &e) {

        if (!e.loc_start) {
            e.loc_start = start;
            e.loc_end = end;
        }

        /*
         * If this node was spliced in by inlining, emit its virtual
         * ("inlined-at") frames now, so the physically-absent inlined calls
         * still show in the backtrace. This is keyed off
         * `inline_origin_emitted` rather than the loc-stamp above, because many
         * errors arrive with a loc already set (a builtin call, a not-an-lvalue
         * assignment, ...) and would otherwise slip past the loc once-guard and
         * lose their frames. The innermost inlined node wins; `do_func_call`
         * sets the same flag for a real call so the CallExpr doesn't re-emit.
         */
        if (inline_ctx && !e.inline_origin_emitted) {
            flush_inline_frames(inline_ctx, e);
            e.inline_origin_emitted = true;
        }

        throw;
    }
}

LiteralStr::LiteralStr(const std::string_view &v)
    : value(v.empty() ? empty_str : EvalValue(SharedStr(unescape_str(v))))
{ }

/*
 * Default typed evaluation: box through eval()/RValue. Works for any node (a
 * function call, a `dyn` value, ...), so a specialized node may call them on an
 * arbitrary child. eval()'s wrapper still stamps the source loc on errors.
 */
int_type Construct::eval_int(EvalContext *ctx) const
{
    const EvalValue v = RValue(eval(ctx));
    if (v.is<bool>())
        return v.get<bool>() ? 1 : 0;     /* bool promotes to int 0/1 */
    return v.get<int_type>();
}

float_type Construct::eval_float(EvalContext *ctx) const
{
    const EvalValue v = RValue(eval(ctx));
    if (v.is<int_type>())
        return static_cast<float_type>(v.get<int_type>());
    if (v.is<bool>())
        return v.get<bool>() ? 1.0 : 0.0;
    return v.get<float_type>();
}

/* Resolved-local fast paths: read the slot's scalar directly, skipping the
 * LValue wrapper and the EvalValue copy. Falls back to the default (map walk /
 * undefined-variable error) for non-slotted or undefined symbols. */
int_type Identifier::eval_int(EvalContext *ctx) const
{
    if (sym.kind == SymKind::local && ctx->frame &&
        (ctx->frame->live & (static_cast<uint64_t>(1) << sym.slot))) {
        const LValue &lv = ctx->frame->slots[sym.slot];
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1 : 0;   /* bool slot -> int 0/1 */
        return lv.getval<int_type>();
    }
    return Construct::eval_int(ctx);
}

float_type Identifier::eval_float(EvalContext *ctx) const
{
    if (sym.kind == SymKind::local && ctx->frame &&
        (ctx->frame->live & (static_cast<uint64_t>(1) << sym.slot))) {
        const LValue &lv = ctx->frame->slots[sym.slot];
        if (lv.is<int_type>())
            return static_cast<float_type>(lv.getval<int_type>());
        if (lv.is<bool>())
            return lv.getval<bool>() ? 1.0 : 0.0;
        return lv.getval<float_type>();
    }
    return Construct::eval_float(ctx);
}

EvalValue Identifier::do_eval(EvalContext *ctx, bool rec) const
{
    if (sym.kind == SymKind::local && ctx->frame) {

        /* Resolved local: an O(1) slot read instead of a scope-chain walk. */
        const uint64_t bit = static_cast<uint64_t>(1) << sym.slot;

        if (ctx->frame->live & bit)
            return EvalValue(&ctx->frame->slots[sym.slot]);

        return UndefinedId{get_str()};   /* slot exists but was undef()'d */
    }

    while (ctx) {

        LValue *lval = ctx->lookup(this);

        if (lval)
            return EvalValue(lval);

        ctx = ctx->parent;

        if (!rec)
            break;
    }

    return UndefinedId{get_str()};
}

/*
 * return / break / continue no longer use C++ exceptions: they set the
 * EvalContext's FlowState (see eval.h) and unwind via ordinary returns.
 * rethrow is genuinely exceptional (it re-throws a real exception object from
 * inside a catch block) and stays an exception.
 */
struct RethrowEx { Loc start; Loc end; };

static inline EvalValue
do_func_return(EvalValue &&tmp, Construct *retExpr)
{
    if (tmp.is<UndefinedId>()) {
        throw UndefinedVariableEx(
            tmp.get<UndefinedId>().id,
            retExpr->start,
            retExpr->end
        );
    }

    return RValue(tmp);
}

static EvalValue coerce_to_decl_type(const EvalValue &v, DeclType dt);

/*
 * Bind one parameter. When `frame` is set (the function was resolved), the
 * value goes into its fixed slot and the slot is marked live; otherwise it is
 * emplaced into the args context map (the unresolved / const-eval path).
 *
 * A param with an explicit numeric type coerces a widening argument to it
 * (`func f(float x); f(3)` binds 3.0), via the same rule as a typed variable.
 */
static inline void
bind_param(EvalContext *args_ctx,
           Frame *frame,
           int idx,
           const Identifier *param,
           EvalValue val,
           bool is_const)
{
    if (param->decl_type == DeclType::f || param->decl_type == DeclType::i)
        val = coerce_to_decl_type(val, param->decl_type);

    if (frame) {
        frame->slots[idx] = LValue(move(val), is_const);
        frame->live |= static_cast<uint64_t>(1) << idx;
    } else {
        args_ctx->emplace(param, move(val), is_const);
    }
}

/*
 * The minimum number of arguments a call must supply: 1 + the index of the last
 * non-`opt` parameter (0 if every parameter is `opt`). Trailing `opt` params
 * may be omitted by the caller; an omitted one binds to `none`. A non-opt param
 * after an opt one simply raises the minimum to include it (it can't be
 * skipped), so `f(x, opt y, z)` still requires all three.
 */
static size_t
min_required_args(const vector<unique_ptr<Identifier>> &params)
{
    size_t n = 0;
    for (size_t i = 0; i < params.size(); i++)
        if (!params[i]->opt_mod)
            n = i + 1;
    return n;
}

/*
 * Bind call arguments to the function's parameters. There is one overload per
 * argument representation - unevaluated argument expressions (the normal call
 * path), an already-evaluated value vector, a single value, and a pair (used by
 * builtins that invoke a callback). Each evaluates/forwards the args and hands
 * the actual storage to bind_param (slot Frame when resolved, else the map).
 *
 * The caller may pass between min_required_args() and funcParams.size()
 * arguments; any trailing `opt` parameter it omits is bound to `none`.
 */
static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const vector<unique_ptr<Construct>> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    const size_t nparams = funcParams.size();
    if (args.size() > nparams || args.size() < min_required_args(funcParams))
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++) {
        /*
         * A param's binding is const iff it was declared `const` - NOT merely
         * because we are const-evaluating. This lets a (pure) function reassign
         * its own by-value parameters during const-eval, while a `const` param
         * stays immutable everywhere.
         */
        bind_param(
            args_ctx, frame, i, funcParams[i].get(),
            i < args.size() ? RValue(args[i]->eval(ctx)) : EvalValue(),
            funcParams[i]->const_param
        );
    }
}

static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const vector<EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    const size_t nparams = funcParams.size();
    if (args.size() > nparams || args.size() < min_required_args(funcParams))
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++) {
        bind_param(
            args_ctx, frame, i, funcParams[i].get(),
            i < args.size() ? args[i] : EvalValue(), ctx->const_ctx
        );
    }
}

static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const EvalValue &arg,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    const size_t nparams = funcParams.size();
    if (nparams < 1 || min_required_args(funcParams) > 1)
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++)
        bind_param(args_ctx, frame, i, funcParams[i].get(),
                   i == 0 ? arg : EvalValue(), ctx->const_ctx);
}


static void
do_func_bind_params(const vector<unique_ptr<Identifier>> &funcParams,
                    const pair<EvalValue, EvalValue> &args,
                    EvalContext *ctx,
                    EvalContext *args_ctx,
                    Frame *frame)
{
    const size_t nparams = funcParams.size();
    if (nparams < 2 || min_required_args(funcParams) > 2)
        throw InvalidNumberOfArgsEx();

    for (size_t i = 0; i < nparams; i++)
        bind_param(args_ctx, frame, i, funcParams[i].get(),
                   i == 0 ? args.first : i == 1 ? args.second : EvalValue(),
                   ctx->const_ctx);
}

/*
 * Invoke `obj` with `args`. Builds the callee's argument context (its own
 * FlowState and, when the function was resolved, a flat slot Frame), binds the
 * params, evaluates the body, and returns what the body returned via the
 * FlowState (or none). An UndefinedVariableEx escaping a pure func is tagged so
 * the error message can point at the pure-func restriction. `call_site` (the
 * CallExpr's loc) is recorded into an unwinding exception's backtrace.
 */
template <class ArgsVecT>
static EvalValue
do_func_call(EvalContext *ctx,
             FuncObject &obj,
             const ArgsVecT &args,
             Loc call_site = Loc(),
             const InlineCtx *call_site_inl = nullptr)
{
    /* func_ctx == true gives this call its own FlowState (see eval.h) */
    EvalContext args_ctx(&obj.capture_ctx, false, true);

    /*
     * When the function was resolved, params live in a flat slot Frame (O(1)
     * access) instead of the args context map. The Frame lives on this stack
     * frame for the whole call; nested blocks inherit the pointer.
     */
    Frame frame;

    if (obj.func->resolved) {
        frame.init(obj.func->frame_size);
        args_ctx.frame = &frame;
    }

    if (obj.func->params) {
        const auto &funcParams = obj.func->params->elems;
        do_func_bind_params(
            funcParams, args, ctx, &args_ctx,
            obj.func->resolved ? &frame : nullptr
        );
    }

    try {

        if (!obj.func->body->is_block()) {

            /* Single-expression body: `func f(...) => expr;` */
            return do_func_return(
                obj.func->body->eval(&args_ctx),
                obj.func->body.get()
            );
        }

        /*
         * Block body: statements run until one sets the FlowState to `ret`
         * (Block::do_eval stops there). No exception is thrown for `return`.
         */
        obj.func->body->eval(&args_ctx);

    } catch (Exception &e) {

        if (obj.func->is_const)
            if (auto *undefEx = dynamic_cast<UndefinedVariableEx *>(&e))
                undefEx->in_pure_func = true;

        /*
         * Record this frame as the exception unwinds (innermost first). Capture
         * the name/params as strings now: the AST is destroyed during unwinding
         * before the top-level handler builds the backtrace.
         */
        BacktraceFrame bf;
        bf.name = !obj.func->display_name.empty()
                      ? obj.func->display_name        /* e.g. a spec. clone */
                      : obj.func->id ? string(obj.func->id->get_str())
                                     : "<lambda>";
        if (obj.func->params)
            for (const auto &p : obj.func->params->elems)
                bf.params.push_back(string(p->get_str()));
        bf.call_site = call_site;
        e.backtrace.push_back(move(bf));

        /*
         * If this call was physically made from inside inlined code, emit the
         * virtual frames for the inlined call(s) right above it. Setting the
         * flag stops the enclosing CallExpr::eval from emitting this same chain
         * again; a deeper physical call's own flush still runs (this is
         * unconditional), so multi-level inlined call sites all show up.
         */
        if (call_site_inl) {
            flush_inline_frames(call_site_inl, e);
            e.inline_origin_emitted = true;
        }

        throw;
    }

    FlowState &fs = *args_ctx.flow;

    if (fs.type == FlowState::ret)
        return move(fs.value);

    return none;
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const vector<EvalValue> &args)
{
    return do_func_call(ctx, obj, args);
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const EvalValue &arg)
{
    return do_func_call(ctx, obj, arg);
}

EvalValue eval_func(EvalContext *ctx,
                    FuncObject &obj,
                    const pair<EvalValue, EvalValue> &args)
{
    return do_func_call(ctx, obj, args);
}

static void stamp_operand_loc(const Construct *c, Exception &e);

/*
 * Build a struct instance from a (positional, already-desugared) argument list.
 * v1 storage is boxed: one LValue slot per field, in declaration order. A
 * numeric field coerces a widening argument (int field <- bool, float field <-
 * int/bool), like a typed parameter. The inferencer has already type-checked
 * the arguments against the field types (for a statically-known callee), so
 * this is the construction, not the validation.
 */
/* Coerce + runtime-validate one field value. The inferencer already checks a
 * statically-known construction, but this guards a `dyn`-laundered value and
 * makes a parse-time (const) construction type-safe (so it can fold). */
static EvalValue
coerce_struct_field(const FieldDef &fd, EvalValue v, Loc s, Loc e)
{
    if (v.is<NoneVal>()) {
        if (fd.is_opt)
            return v;
        throw TypeErrorEx(intern_msg("struct field '" +
                          string(fd.name->val) + "' cannot be none"), s, e);
    }

    switch (fd.kind) {
        case FieldKind::f_dyn:
            return v;
        case FieldKind::f_int:
            if (v.is<bool>() || v.is<int_type>())
                return coerce_to_decl_type(v, DeclType::i);
            break;
        case FieldKind::f_float:
            if (v.is<bool>() || v.is<int_type>() || v.is<float_type>())
                return coerce_to_decl_type(v, DeclType::f);
            break;
        case FieldKind::f_bool:
            if (v.is<bool>())
                return v;
            break;
        case FieldKind::f_str:
            if (v.is<SharedStr>())
                return v;
            break;
        case FieldKind::f_array:
            if (v.is<SharedArrayObj>())
                return v;
            break;
        case FieldKind::f_dict:
            if (v.is<intrusive_ptr<DictObject>>())
                return v;
            break;
        case FieldKind::f_struct:
            if (v.is<intrusive_ptr<StructObject>>() &&
                v.get<intrusive_ptr<StructObject>>()->def->name == fd.struct_ty)
                return v;
            break;
    }

    throw TypeErrorEx(intern_msg("struct field '" + string(fd.name->val) +
                      "' got a value of the wrong type"), s, e);
}

static EvalValue
construct_struct(EvalContext *ctx, StructTypeDef *def, ExprList *args)
{
    const size_t nfields = def->fields.size();
    const size_t nargs = args->elems.size();

    /* A trailing opt field may be omitted (binds to none); an interior skipped
     * one was already filled with an explicit `none` by the desugar. */
    size_t min_args = 0;
    for (size_t i = 0; i < nfields; i++)
        if (!def->fields[i].is_opt)
            min_args = i + 1;

    if (nargs < min_args || nargs > nfields)
        throw InvalidNumberOfArgsEx(args->start, args->end);

    auto obj = make_intrusive<StructObject>(def);   /* resizes bytes if POD */

    if (def->is_pod()) {
        /* POD has no opt fields, so nargs == nfields; store each scalar into
         * its byte slot. */
        for (size_t i = 0; i < nfields; i++) {
            EvalValue v = coerce_struct_field(
                def->fields[i], RValue(args->elems[i]->eval(ctx)),
                args->elems[i]->start, args->elems[i]->end);
            obj->pod_set(static_cast<int>(i), v);
        }
        return intrusive_ptr<StructObject>(obj);
    }

    obj->fields.reserve(nfields);

    for (size_t i = 0; i < nfields; i++) {
        const FieldDef &fd = def->fields[i];
        const Loc s = i < nargs ? args->elems[i]->start : args->start;
        const Loc e = i < nargs ? args->elems[i]->end : args->end;
        /* an omitted trailing opt field binds to none */
        EvalValue v = i < nargs ? RValue(args->elems[i]->eval(ctx))
                                : EvalValue();
        obj->fields.emplace_back(coerce_struct_field(fd, move(v), s, e), false);
    }

    return intrusive_ptr<StructObject>(obj);
}

void pod_store_field(const FieldDef &f, char *base, const EvalValue &v)
{
    char *p = base + f.offset;
    switch (f.kind) {
        case FieldKind::f_bool:
            *p = v.get<bool>() ? 1 : 0;
            break;
        case FieldKind::f_int: {
            int_type x = v.get<int_type>();
            std::memcpy(p, &x, sizeof x);
            break;
        }
        case FieldKind::f_float: {
            float_type x = v.get<float_type>();
            std::memcpy(p, &x, sizeof x);
            break;
        }
        case FieldKind::f_struct: {
            const StructObject &o = *v.get<intrusive_ptr<StructObject>>().get();
            std::memcpy(p, o.bytes.data(), f.struct_def->size);
            break;
        }
        default:
            throw InternalErrorEx();
    }
}

/*
 * The build-hot fast path behind `append(arr, Point(...))`: when appending a
 * struct constructor call to a flat POD-struct array of that exact type,
 * construct the element STRAIGHT into the array's byte buffer - no temporary
 * StructObject (which would be two heap allocations per element, the dominant
 * cost of building an array<Struct>). Returns false (caller falls back to the
 * normal `eval arg -> append value` path) unless every condition holds.
 *
 * Field args are evaluated/coerced into a stack buffer FIRST, so a throw mid-
 * construction (a type error, a side-effecting arg) leaves the array unchanged;
 * only on full success is the slot committed (resize + copy). Declared in
 * structtype.h so builtin_append (a different TU) can call it.
 */
bool try_construct_into_struct_array(EvalContext *ctx, SharedArrayObj &arr,
                                     Construct *arg);
bool try_construct_into_struct_array(EvalContext *ctx, SharedArrayObj &arr,
                                     Construct *arg)
{
    auto *cc = dynamic_cast<CallExpr *>(arg);
    if (!cc || !cc->args)
        return false;

    const EvalValue cv = RValue(cc->what->eval(ctx));
    if (!cv.is<StructTypeDef *>())
        return false;

    StructTypeDef *def = cv.get<StructTypeDef *>();
    auto &sv = arr.flat_structs();
    if (def != sv.def || !def->is_pod())
        return false;

    const size_t nfields = def->fields.size();
    /* POD has no opt fields: an exact arity is required. A mismatch falls back
     * so the normal path raises the proper InvalidNumberOfArgsEx. */
    if (cc->args->elems.size() != nfields)
        return false;

    /* Build into a stack buffer (POD structs are small); cap defensively. */
    constexpr int STACK_CAP = 512;
    if (def->size > STACK_CAP)
        return false;
    char tmp[STACK_CAP];

    for (size_t i = 0; i < nfields; i++) {
        EvalValue v = coerce_struct_field(
            def->fields[i], RValue(cc->args->elems[i]->eval(ctx)),
            cc->args->elems[i]->start, cc->args->elems[i]->end);
        pod_store_field(def->fields[i], tmp, v);
    }

    const size_t at = sv.buf.size();
    sv.buf.resize(at + sv.stride);
    std::memcpy(sv.buf.data() + at, tmp, sv.stride);
    return true;
}

EvalValue CallExpr::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue callable_storage;

    /* Point an undefined-callee error at the callee, not the whole call. */
    try {
        callable_storage = RValue(what->eval(ctx));
    } catch (Exception &e) {
        stamp_operand_loc(what.get(), e);
        throw;
    }

    const EvalValue &callable = callable_storage;

    try {

        if (callable.is<Builtin>())
            return callable.get<Builtin>().func(ctx, args.get());

        if (callable.is<shared_ptr<FuncObject>>()) {
            return do_func_call(
                ctx,
                *callable.get<shared_ptr<FuncObject>>().get(),
                args->elems,
                start,           /* call site = this CallExpr's location */
                inline_ctx       /* virtual frames if this call is inlined */
            );
        }

        /* Calling a struct type descriptor constructs an instance. By this
         * point a named call has been desugared to positional. */
        if (callable.is<StructTypeDef *>())
            return construct_struct(ctx, callable.get<StructTypeDef *>(),
                                    args.get());

    } catch (Exception &e) {

        if (!e.loc_start) {
            e.loc_start = args->start;
            e.loc_end = args->end;
        }
        throw;
    }

    throw NotCallableEx(what->start, what->end);
}

EvalValue LiteralArray::do_eval(EvalContext *ctx, bool rec) const
{
    if (!elems.size()) {
        /* An empty array<POD struct> destination: start flat (with the element
         * type from the hint) so a built-up `var a = []; append(a, S(..))`
         * stays unboxed. */
        if (arr_hint == ArrHint::flat_s && arr_hint_struct) {
            StructTypeDef *def = const_cast<StructTypeDef *>(arr_hint_struct);
            return SharedArrayObj(
                SharedArrayObj::svec_type({}, def, def->size));
        }
        return empty_arr;
    }

    /*
     * Build flat (unboxed) int/float storage when every element is that one
     * scalar kind - a literal's element types ARE its type, so this
     * value-driven check yields exactly the type-driven representation (plans/
     * type-driven-specialization.md). Optimistic: accumulate into the unboxed
     * vector while the kind holds, spilling to a general vector<LValue> at the
     * first off-kind element. A mixed literal is general.
     */
    const size_t n = elems.size();
    SharedArrayObj::ivec_type ivec;
    SharedArrayObj::fvec_type fvec;
    SharedArrayObj::bvec_type bvec;
    SharedArrayObj::vec_type  gvec;
    /* mode 5: a flat array of same-type POD structs (their bytes packed) */
    std::vector<char> svecbuf;
    StructTypeDef *sdef = nullptr;
    int sstride = 0;

    auto is_pod_struct_of = [](const EvalValue &v, StructTypeDef *d) -> bool {
        return v.is<intrusive_ptr<StructObject>>() &&
               v.get<intrusive_ptr<StructObject>>()->is_pod() &&
               (d == nullptr || v.get<intrusive_ptr<StructObject>>()->def == d);
    };
    auto append_struct_bytes = [&](const EvalValue &v) {
        const StructObject &o = *v.get<intrusive_ptr<StructObject>>().get();
        const size_t at = svecbuf.size();
        svecbuf.resize(at + sstride);
        std::memcpy(svecbuf.data() + at, o.bytes.data(), sstride);
    };
    /*
     * 0 = empty, 1 = ints, 2 = floats, 4 = bools, 3 = general. Type-driven: a
     * literal bound to a dynamically-typed destination (arr_hint general, set by
     * the inferencer) is built general from the start, so a later mixed write to
     * it never has to promote. (The flat_i/flat_f/flat_b hints need no special
     * case - the value-driven scan already produces flat for an all-one-kind
     * literal, which is exactly when those hints are set.)
     */
    int mode = arr_hint == ArrHint::general ? 3 : 0;
    if (mode == 3)
        gvec.reserve(n);

    auto spill_to_general = [&]() {
        gvec.reserve(n);
        if (mode == 1)
            for (int_type x : ivec)
                gvec.emplace_back(EvalValue(x), ctx->const_ctx);
        else if (mode == 2)
            for (float_type x : fvec)
                gvec.emplace_back(EvalValue(x), ctx->const_ctx);
        else if (mode == 4)
            for (unsigned char x : bvec)
                gvec.emplace_back(EvalValue(static_cast<bool>(x)),
                                  ctx->const_ctx);
        else if (mode == 5) {
            const size_t cnt = sstride ? svecbuf.size() / sstride : 0;
            for (size_t i = 0; i < cnt; i++) {
                auto o = make_intrusive<StructObject>(sdef);
                std::memcpy(o->bytes.data(),
                            svecbuf.data() + i * sstride, sstride);
                gvec.emplace_back(EvalValue(intrusive_ptr<StructObject>(o)),
                                  ctx->const_ctx);
            }
        }
        ivec.clear();
        fvec.clear();
        bvec.clear();
        svecbuf.clear();
        mode = 3;
    };

    for (const auto &e : elems) {

        EvalValue v = RValue(e->eval(ctx));

        if (mode == 0) {
            if (v.is<int_type>()) {
                mode = 1; ivec.push_back(v.get<int_type>());
            } else if (v.is<float_type>()) {
                mode = 2; fvec.push_back(v.get<float_type>());
            } else if (v.is<bool>()) {
                mode = 4; bvec.push_back(v.get<bool>() ? 1 : 0);
            } else if (is_pod_struct_of(v, nullptr)) {
                mode = 5;
                sdef = v.get<intrusive_ptr<StructObject>>()->def;
                sstride = sdef->size;
                svecbuf.reserve(n * sstride);
                append_struct_bytes(v);
            } else {
                mode = 3; gvec.reserve(n);
                gvec.emplace_back(v, ctx->const_ctx);
            }
        } else if (mode == 1 && v.is<int_type>()) {
            ivec.push_back(v.get<int_type>());
        } else if (mode == 2 && v.is<float_type>()) {
            fvec.push_back(v.get<float_type>());
        } else if (mode == 4 && v.is<bool>()) {
            bvec.push_back(v.get<bool>() ? 1 : 0);
        } else if (mode == 5 && is_pod_struct_of(v, sdef)) {
            append_struct_bytes(v);
        } else {
            if (mode != 3)
                spill_to_general();
            gvec.emplace_back(v, ctx->const_ctx);
        }
    }

    if (mode == 1) return SharedArrayObj(move(ivec));
    if (mode == 2) return SharedArrayObj(move(fvec));
    if (mode == 4) return SharedArrayObj(move(bvec));
    if (mode == 5)
        return SharedArrayObj(
            SharedArrayObj::svec_type(move(svecbuf), sdef, sstride));
    return SharedArrayObj(move(gvec));
}

/*
 * Produce a fresh, mutable copy of a const-evaluated container value.
 *
 * `through_readonly` controls how read-only (const-backed) sub-objects are
 * handled:
 *   - false (make_mutable_clone): a read-only sub-object is *shared* as-is, not
 *     copied. So a fresh mutable top is built, but const sub-objects stay const
 *     and shared. This keeps clone() shallow (a const inside the result remains
 *     read-only) and makes const-ness propagate into fresh literals
 *     (`var a = [y]` with `y` const keeps `a[0]` read-only). Mutable
 *     sub-objects are still copied fresh, so re-evaluating the node never
 *     observes a prior mutation.
 *   - true (make_deep_mutable_clone): every level is copied and made mutable
 *     (read-only dropped), yielding a fully independent, writable value. This
 *     backs deepclone().
 * Scalars and strings are returned as-is. An empty array collapses to the
 * shared empty_arr singleton, matching LiteralArray.
 */
static EvalValue
clone_to_mutable(const EvalValue &v, bool through_readonly)
{
    if (v.is<SharedArrayObj>()) {

        const SharedArrayObj &arr = v.get<SharedArrayObj>();

        if (arr.is_readonly() && !through_readonly)
            return v;   /* share the const sub-object, don't copy it */

        if (!arr.size())
            return empty_arr;

        /*
         * Flat homogeneous array: copy into fresh mutable unboxed storage. The
         * elements are scalars (nothing to recurse into), so this is sound for
         * both make_mutable_clone and make_deep_mutable_clone.
         */
        if (arr.skind() == SharedArrayObj::Storage::ints) {
            const auto &iv = arr.flat_ints();
            return SharedArrayObj(SharedArrayObj::ivec_type(
                iv.cbegin() + arr.offset(),
                iv.cbegin() + arr.offset() + arr.size()
            ));
        }

        if (arr.skind() == SharedArrayObj::Storage::floats) {
            const auto &fv = arr.flat_floats();
            return SharedArrayObj(SharedArrayObj::fvec_type(
                fv.cbegin() + arr.offset(),
                fv.cbegin() + arr.offset() + arr.size()
            ));
        }

        if (arr.skind() == SharedArrayObj::Storage::bools) {
            const auto &bv = arr.flat_bools();
            return SharedArrayObj(SharedArrayObj::bvec_type(
                bv.cbegin() + arr.offset(),
                bv.cbegin() + arr.offset() + arr.size()
            ));
        }

        /* Flat POD-struct array: a byte copy keeps it flat (POD bytes hold no
         * references, so it is a full mutable copy). */
        if (arr.skind() == SharedArrayObj::Storage::structs) {
            const auto &sv = arr.flat_structs();
            std::vector<char> nb(
                sv.buf.cbegin() + arr.offset() * sv.stride,
                sv.buf.cbegin() + (arr.offset() + arr.size()) * sv.stride
            );
            return SharedArrayObj(
                SharedArrayObj::svec_type(move(nb), sv.def, sv.stride));
        }

        const ArrayConstView &view = arr.get_view();

        SharedArrayObj::vec_type vec;
        vec.reserve(view.size());

        for (unsigned i = 0; i < view.size(); i++) {
            vec.emplace_back(
                clone_to_mutable(view[i].get(), through_readonly), false
            );
        }

        return SharedArrayObj(move(vec));
    }

    if (v.is<intrusive_ptr<DictObject>>()) {

        const auto &obj = v.get<intrusive_ptr<DictObject>>();

        if (obj->is_readonly() && !through_readonly)
            return v;   /* share the const sub-object, don't copy it */

        DictObject::inner_type data;

        for (const auto &p : obj->get_ref()) {
            data.emplace(
                p.first,
                LValue(
                    clone_to_mutable(p.second.get(), through_readonly),
                    false
                )
            );
        }

        auto out = make_intrusive<DictObject>(move(data));
        if (obj->get_has_default())   /* preserve the default-dict default */
            out->set_default(
                clone_to_mutable(obj->get_default(), through_readonly));
        return intrusive_ptr<DictObject>(out);
    }

    if (v.is<intrusive_ptr<StructObject>>()) {

        const auto &obj = v.get<intrusive_ptr<StructObject>>();

        if (obj->is_readonly() && !through_readonly)
            return v;   /* share the const sub-object, don't copy it */

        /* POD: a byte copy is a full mutable copy (no nested references). */
        if (obj->is_pod()) {
            auto out = make_intrusive<StructObject>(*obj);   /* copies bytes */
            out->clear_readonly();
            return intrusive_ptr<StructObject>(out);
        }

        auto out = make_intrusive<StructObject>(obj->def);
        out->fields.reserve(obj->fields.size());
        for (const auto &f : obj->fields)
            out->fields.emplace_back(
                clone_to_mutable(f.get(), through_readonly), false);
        return intrusive_ptr<StructObject>(out);
    }

    return v;
}

EvalValue make_mutable_clone(const EvalValue &v)
{
    return clone_to_mutable(v, false);
}

EvalValue make_deep_mutable_clone(const EvalValue &v)
{
    return clone_to_mutable(v, true);
}

/*
 * Produce a deep, read-only copy of a const-evaluated container value: every
 * array/dict in the result (recursively) is flagged read-only, so writing
 * through ANY alias of it - including a non-const function parameter bound to a
 * const argument - is rejected. This is what makes `const` deep read-only at
 * runtime; previously const-ness was enforced only by parse-time folding of
 * direct reads, leaving the runtime value mutable through aliasing. Scalars and
 * strings are returned as-is (already immutable). An empty array gets its own
 * read-only object (not the shared empty_arr singleton, which must stay
 * mutable).
 */
EvalValue
make_const_clone(const EvalValue &v)
{
    if (v.is<SharedArrayObj>()) {

        const SharedArrayObj &src = v.get<SharedArrayObj>();

        /*
         * Flat homogeneous array: bake a flat read-only copy, keeping the
         * unboxed storage. The elements are scalars, so there is nothing to
         * recurse into - a const flat int/float array stays flat (and so does
         * everything cloned from it, since clone_internal_vec is kind-aware).
         */
        if (src.skind() == SharedArrayObj::Storage::ints) {
            const auto &iv = src.flat_ints();
            SharedArrayObj::ivec_type nv(
                iv.cbegin() + src.offset(),
                iv.cbegin() + src.offset() + src.size()
            );
            SharedArrayObj arr(move(nv));
            arr.set_readonly();
            return arr;
        }

        if (src.skind() == SharedArrayObj::Storage::floats) {
            const auto &fv = src.flat_floats();
            SharedArrayObj::fvec_type nv(
                fv.cbegin() + src.offset(),
                fv.cbegin() + src.offset() + src.size()
            );
            SharedArrayObj arr(move(nv));
            arr.set_readonly();
            return arr;
        }

        if (src.skind() == SharedArrayObj::Storage::bools) {
            const auto &bv = src.flat_bools();
            SharedArrayObj::bvec_type nv(
                bv.cbegin() + src.offset(),
                bv.cbegin() + src.offset() + src.size()
            );
            SharedArrayObj arr(move(nv));
            arr.set_readonly();
            return arr;
        }

        /* Flat POD-struct array: a deep read-only byte copy (POD bytes hold no
         * nested references to recurse into). */
        if (src.skind() == SharedArrayObj::Storage::structs) {
            const auto &sv = src.flat_structs();
            std::vector<char> nb(
                sv.buf.cbegin() + src.offset() * sv.stride,
                sv.buf.cbegin() + (src.offset() + src.size()) * sv.stride
            );
            SharedArrayObj arr(
                SharedArrayObj::svec_type(move(nb), sv.def, sv.stride));
            arr.set_readonly();
            return arr;
        }

        const ArrayConstView &view = src.get_view();

        SharedArrayObj::vec_type vec;
        vec.reserve(view.size());

        for (unsigned i = 0; i < view.size(); i++)
            vec.emplace_back(make_const_clone(view[i].get()), false);

        SharedArrayObj arr(move(vec));
        arr.set_readonly();
        return arr;
    }

    if (v.is<intrusive_ptr<DictObject>>()) {

        const DictObject &src_obj = *v.get<intrusive_ptr<DictObject>>().get();
        DictObject::inner_type data;

        for (const auto &p : src_obj.get_ref()) {
            data.emplace(
                p.first,
                LValue(make_const_clone(p.second.get()), false)
            );
        }

        auto obj = make_intrusive<DictObject>(move(data));
        if (src_obj.get_has_default())   /* preserve the default-dict default */
            obj->set_default(make_const_clone(src_obj.get_default()));
        obj->set_readonly();
        return intrusive_ptr<DictObject>(obj);
    }

    if (v.is<intrusive_ptr<StructObject>>()) {

        const StructObject &src = *v.get<intrusive_ptr<StructObject>>().get();

        /* POD: bytes hold no references, so a byte copy is a deep copy. */
        if (src.is_pod()) {
            auto obj = make_intrusive<StructObject>(src);   /* copies bytes */
            obj->set_readonly();
            return intrusive_ptr<StructObject>(obj);
        }

        auto obj = make_intrusive<StructObject>(src.def);
        obj->fields.reserve(src.fields.size());
        for (const auto &f : src.fields)
            obj->fields.emplace_back(make_const_clone(f.get()), false);
        obj->set_readonly();
        return intrusive_ptr<StructObject>(obj);
    }

    return v;
}

/*
 * Build a fresh, mutable GENERAL array from a flat (unboxed int/float) array,
 * reading its scalar elements directly (no promotion of the source). Used to
 * materialize a folded array literal whose destination is dynamically typed
 * (arr_hint general): the array is born general so a later mixed write to it
 * never has to promote. Only a flat source reaches here - an already-general
 * baked value is handled by make_mutable_clone, which keeps it general.
 */
static EvalValue
make_general_array_clone(const SharedArrayObj &src)
{
    const size_type m = src.size();
    SharedArrayObj::vec_type gv;
    gv.reserve(m);

    if (src.skind() == SharedArrayObj::Storage::ints) {
        const auto &iv = src.flat_ints();
        for (size_type i = 0; i < m; i++)
            gv.emplace_back(EvalValue(iv[src.offset() + i]), false);
    } else if (src.skind() == SharedArrayObj::Storage::floats) {
        const auto &fv = src.flat_floats();
        for (size_type i = 0; i < m; i++)
            gv.emplace_back(EvalValue(fv[src.offset() + i]), false);
    } else {
        const auto &bv = src.flat_bools();
        for (size_type i = 0; i < m; i++)
            gv.emplace_back(EvalValue(static_cast<bool>(bv[src.offset() + i])),
                            false);
    }

    return SharedArrayObj(move(gv));
}

/*
 * Read array element i (slice-relative) as a boxed value without promoting flat
 * (unboxed int/float) storage - the eval.cpp counterpart of types/arr.cpp.h's
 * arr_elem_at (a separate translation unit). For a general array get_view()
 * doesn't promote.
 */
static EvalValue
arr_elem_boxed(const SharedArrayObj &a, size_type i)
{
    switch (a.skind()) {
        case SharedArrayObj::Storage::ints:
            return EvalValue(a.flat_ints()[a.offset() + i]);
        case SharedArrayObj::Storage::floats:
            return EvalValue(a.flat_floats()[a.offset() + i]);
        case SharedArrayObj::Storage::bools:
            return EvalValue(static_cast<bool>(a.flat_bools()[a.offset() + i]));
        case SharedArrayObj::Storage::structs: {
            const auto &sv = a.flat_structs();
            auto obj = make_intrusive<StructObject>(sv.def);
            std::memcpy(obj->bytes.data(),
                        sv.buf.data() + (a.offset() + i) * sv.stride,
                        sv.stride);
            return intrusive_ptr<StructObject>(obj);
        }
        default:
            return a.get_view()[i].get();
    }
}

EvalValue LiteralObj::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * A const-decl target (immutable) shares the baked value directly: it was
     * baked deep read-only (make_const_clone, in the parser), so it can't be
     * mutated and re-entry can safely observe the same object - no per-eval
     * copy, and the const symbol and this node hold one buffer, not two. Any
     * other target (a `var` or a read-only consumer) gets a fresh mutable deep
     * copy, so writes work and re-entry never sees a prior mutation.
     *
     * Type-driven: when the destination is dynamically typed (arr_hint general)
     * but the baked array is flat (a homogeneous literal like [1,2,3] later
     * widened to array<dyn> by a mixed write), materialize it general from the
     * start so that write never promotes.
     */
    if (!immutable && arr_hint == ArrHint::general && value.is<SharedArrayObj>()
        && value.get<SharedArrayObj>().skind()
               != SharedArrayObj::Storage::general)
        return make_general_array_clone(value.get<SharedArrayObj>());

    /* An empty baked array bound to an array<POD struct> destination starts
     * flat (the const-fold erased the element type, so the hint restores it),
     * so a built-up `var a = []; append(a, S(..))` stays unboxed. */
    if (arr_hint == ArrHint::flat_s && arr_hint_struct &&
        value.is<SharedArrayObj>() && value.get<SharedArrayObj>().size() == 0) {
        StructTypeDef *def = const_cast<StructTypeDef *>(arr_hint_struct);
        return SharedArrayObj(SharedArrayObj::svec_type({}, def, def->size));
    }

    return immutable ? value : make_mutable_clone(value);
}

/*
 * Attach `c`'s source location to an in-flight exception that has none, so a
 * caret points at the offending sub-expression (operand) instead of the whole
 * enclosing expression. Used wherever an operand's RValue or operator
 * application can throw (undefined variable, type mismatch, division by zero).
 */
static void
stamp_operand_loc(const Construct *c, Exception &e)
{
    if (!e.loc_start) {
        e.loc_start = c->start;
        e.loc_end = c->end;
    }

    /*
     * Operator-ladder errors are stamped at the offending operand, but the
     * inlined-at frames are emitted by the enclosing node's Construct::eval
     * (keyed off inline_origin_emitted, which the loc-stamp above does not
     * gate), so there's nothing to flush here.
     */
}

/* `acc OP= operand`, with operand-precise error locations. */
static void
num_binop_loc(EvalValue &acc, const Construct *operand, EvalContext *ctx,
              NumBinOp op)
{
    try {
        num_bin_op(acc, RValue(operand->eval(ctx)), op);
    } catch (Exception &e) {
        stamp_operand_loc(operand, e);
        throw;
    }
}

/* Same, for the short-circuiting logical operators (&& and ||). */
static void
logop_loc(EvalValue &acc, const Construct *operand, EvalContext *ctx, Op op)
{
    /*
     * `&&` / `||` operate on truthiness (the unchanged truthy rules:
     * 0/none/[]/{} are false, everything else true) and yield a bool. This
     * works for any operand type, not just int, and returns a real bool.
     */
    try {
        const bool a = acc.is_true();
        const bool b = RValue(operand->eval(ctx)).is_true();
        acc = EvalValue(op == Op::land ? (a && b) : (a || b));
    } catch (Exception &e) {
        stamp_operand_loc(operand, e);
        throw;
    }
}

EvalValue MultiOpConstruct::eval_first_rvalue(EvalContext *ctx) const
{
    assert(elems.size() >= 1 && elems[0].first == Op::invalid);

    if (elems.size() == 1)
        return elems[0].second->eval(ctx);

    /* Stamp the first operand's loc on an undefined-variable error. */
    try {
        return RValue(elems[0].second->eval(ctx));
    } catch (Exception &e) {
        stamp_operand_loc(elems[0].second.get(), e);
        throw;
    }
}

EvalValue Expr02::do_eval(EvalContext *ctx, bool rec) const
{
    assert(elems.size() == 1 || elems.size() == 2);
    const auto &[op, e] = elems[0];

    if (op == Op::invalid)
        return e->eval(ctx);

    try {

        EvalValue &&val = RValue(e->eval(ctx)).clone();

        switch (op) {
            case Op::plus:
                /* Unary operator '+': promote a bool to int 0/1, else no-op */
                if (val.is<bool>())
                    val = static_cast<int_type>(val.get<bool>() ? 1 : 0);
                break;
            case Op::minus:
                /* Unary operator '-': negate (a bool promotes to int first) */
                if (val.is<bool>())
                    val = static_cast<int_type>(val.get<bool>() ? 1 : 0);
                val.get_type()->opneg(val);
                break;
            case Op::lnot:
                /* Unary '!': logical not of the truthiness, yielding a bool */
                val = EvalValue(!val.is_true());
                break;
            case Op::bnot:
                /* Unary '~': bitwise NOT (a bool promotes to int 0/1 first) */
                if (val.is<bool>())
                    val = static_cast<int_type>(val.get<bool>() ? 1 : 0);
                val.get_type()->bnot(val);
                break;
            default:
                throw InternalErrorEx();
        }

        return move(val);

    } catch (Exception &ex) {
        stamp_operand_loc(e.get(), ex);
        throw;
    }
}

EvalValue Expr05::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::shl:  num_binop_loc(val, e.get(), ctx, &Type::shl);  break;
            case Op::shr:  num_binop_loc(val, e.get(), ctx, &Type::shr);  break;
            case Op::ushr: num_binop_loc(val, e.get(), ctx, &Type::ushr); break;
            default:       throw InternalErrorEx();
        }
    }

    return move(val);
}

EvalValue Expr08::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        if (op != Op::band)
            throw InternalErrorEx();

        num_binop_loc(val, e.get(), ctx, &Type::band);
    }

    return move(val);
}

EvalValue Expr09::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        if (op != Op::bxor)
            throw InternalErrorEx();

        num_binop_loc(val, e.get(), ctx, &Type::bxor);
    }

    return move(val);
}

EvalValue Expr10::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        if (op != Op::bor)
            throw InternalErrorEx();

        num_binop_loc(val, e.get(), ctx, &Type::bor);
    }

    return move(val);
}

EvalValue Expr03::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::times:
                num_binop_loc(val, e.get(), ctx, &Type::mul);
                break;
            case Op::div:
                num_binop_loc(val, e.get(), ctx, &Type::div);
                break;
            case Op::mod:
                num_binop_loc(val, e.get(), ctx, &Type::mod);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return move(val);
}

EvalValue Expr04::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx).clone();

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::plus:
                num_binop_loc(val, e.get(), ctx, &Type::add);
                break;
            case Op::minus:
                num_binop_loc(val, e.get(), ctx, &Type::sub);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return move(val);
}

EvalValue Expr06::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::lt:
                num_binop_loc(val, e.get(), ctx, &Type::lt);
                break;
            case Op::gt:
                num_binop_loc(val, e.get(), ctx, &Type::gt);
                break;
            case Op::le:
                num_binop_loc(val, e.get(), ctx, &Type::le);
                break;
            case Op::ge:
                num_binop_loc(val, e.get(), ctx, &Type::ge);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    /* an ordering comparison (<, >, <=, >=) yields a bool */
    return EvalValue(val.is_true());
}

EvalValue Expr07::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::eq:
                num_binop_loc(val, e.get(), ctx, &Type::eq);
                break;
            case Op::noteq:
                num_binop_loc(val, e.get(), ctx, &Type::noteq);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    /* `==` / `!=` yield a bool */
    return EvalValue(val.is_true());
}

EvalValue Expr11::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::land:
                logop_loc(val, e.get(), ctx, op);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return move(val);
}

EvalValue Expr12::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue &&val = eval_first_rvalue(ctx);

    for (auto &&it = elems.begin() + 1; it != elems.end(); it++) {

        const auto &[op, e] = *it;

        switch (op) {
            case Op::lor:
                logop_loc(val, e.get(), ctx, op);
                break;
            default:
                throw InternalErrorEx();
        }
    }

    return move(val);
}

/*
 * Evaluate a condition to a boolean. When the inferencer proved the condition
 * is a (non-null) int, take the unboxed eval_int() path - no LValue wrapper, no
 * EvalValue copy, no is_true() virtual. Otherwise the general path.
 */
static inline bool eval_cond(const Construct *c, EvalContext *ctx)
{
    if (c->th == TypeHint::i)
        return c->eval_int(ctx) != 0;
    return RValue(c->eval(ctx)).is_true();
}

/* --------------- TypedScalarExpr (M8 specialized scalar eval) ------------- */

template <class T>
static inline int_type typed_cmp(Op op, T a, T b)
{
    switch (op) {
        case Op::lt:    return a <  b;
        case Op::gt:    return a >  b;
        case Op::le:    return a <= b;
        case Op::ge:    return a >= b;
        case Op::eq:    return a == b;
        case Op::noteq: return a != b;
        default:        throw InternalErrorEx();
    }
}

int_type TypedScalarExpr::eval_int(EvalContext *ctx) const
{
    switch (cat) {

        case Cat::neg:
            if (kind == TypeHint::f)
                return static_cast<int_type>(-elems[0].second->eval_float(ctx));
            return -elems[0].second->eval_int(ctx);

        case Cat::lnot:
            return elems[0].second->eval_int(ctx) == 0 ? 1 : 0;

        case Cat::arith: {
            if (kind == TypeHint::f)
                return static_cast<int_type>(eval_float(ctx));
            int_type acc = elems[0].second->eval_int(ctx);
            for (size_t i = 1; i < elems.size(); i++) {
                const int_type r = elems[i].second->eval_int(ctx);
                switch (elems[i].first) {
                    case Op::plus:  acc += r; break;
                    case Op::minus: acc -= r; break;
                    case Op::times: acc *= r; break;
                    case Op::div:
                        if (r == 0) throw DivisionByZeroEx(start, end);
                        acc /= r; break;
                    case Op::mod:
                        if (r == 0) throw DivisionByZeroEx(start, end);
                        acc %= r; break;
                    case Op::band: acc &= r; break;
                    case Op::bor:  acc |= r; break;
                    case Op::bxor: acc ^= r; break;
                    case Op::shl:  acc = bit_shl(acc, r);  break;
                    case Op::shr:  acc = bit_shr(acc, r);  break;
                    case Op::ushr: acc = bit_ushr(acc, r); break;
                    default: throw InternalErrorEx();
                }
            }
            return acc;
        }

        case Cat::cmp:
            if (kind == TypeHint::f)
                return typed_cmp<float_type>(elems[1].first,
                                             elems[0].second->eval_float(ctx),
                                             elems[1].second->eval_float(ctx));
            return typed_cmp<int_type>(elems[1].first,
                                       elems[0].second->eval_int(ctx),
                                       elems[1].second->eval_int(ctx));

        case Cat::logical: {
            int_type acc = elems[0].second->eval_int(ctx);
            for (size_t i = 1; i < elems.size(); i++) {
                /* both sides always evaluated (no short-circuit) */
                const int_type r = elems[i].second->eval_int(ctx);
                acc = (elems[i].first == Op::land) ? (acc && r) : (acc || r);
            }
            return acc;
        }
    }

    return 0;
}

float_type TypedScalarExpr::eval_float(EvalContext *ctx) const
{
    switch (cat) {

        case Cat::neg:
            return -elems[0].second->eval_float(ctx);

        case Cat::arith: {
            float_type acc = elems[0].second->eval_float(ctx);
            for (size_t i = 1; i < elems.size(); i++) {
                const float_type r = elems[i].second->eval_float(ctx);
                switch (elems[i].first) {
                    case Op::plus:  acc += r; break;
                    case Op::minus: acc -= r; break;
                    case Op::times: acc *= r; break;
                    case Op::div:
                        if (r == 0.0) throw DivisionByZeroEx(start, end);
                        acc /= r; break;
                    case Op::mod:
                        if (r == 0.0) throw DivisionByZeroEx(start, end);
                        acc = fmod(acc, r); break;
                    default: throw InternalErrorEx();
                }
            }
            return acc;
        }

        default:   /* cmp / logical / lnot yield int */
            return static_cast<float_type>(eval_int(ctx));
    }
}

EvalValue TypedScalarExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /* Comparisons / logical / `!` yield a bool (computed unboxed as 0/1 by
     * eval_int, then boxed as a bool). Arithmetic / negation keep int/float. */
    if (cat == Cat::cmp || cat == Cat::logical || cat == Cat::lnot)
        return EvalValue(eval_int(ctx) != 0);
    if ((cat == Cat::arith || cat == Cat::neg) && kind == TypeHint::f)
        return EvalValue(eval_float(ctx));
    return EvalValue(eval_int(ctx));
}

/* Apply a compound-assignment op (`+=`, `-=`, ...) to `acc` in place. */
static inline void
apply_compound_op(EvalValue &acc, const EvalValue &rhs, Op op)
{
    switch (op) {
        case Op::addeq: num_bin_op(acc, rhs, &Type::add); break;
        case Op::subeq: num_bin_op(acc, rhs, &Type::sub); break;
        case Op::muleq: num_bin_op(acc, rhs, &Type::mul); break;
        case Op::diveq: num_bin_op(acc, rhs, &Type::div); break;
        case Op::modeq: num_bin_op(acc, rhs, &Type::mod); break;
        default:        throw InternalErrorEx();
    }
}

static EvalValue
doAssign(const EvalValue &lval, const EvalValue &rval, Op op)
{
    EvalValue newVal;

    if (lval.get<LValue *>()->is<Builtin>())
        throw CannotRebindBuiltinEx();

    if (op == Op::assign) {

        newVal = RValue(rval);

    } else {

        newVal = lval.get<LValue *>()->get();
        apply_compound_op(newVal, RValue(rval), op);
    }

    lval.get<LValue *>()->put(newVal);
    return newVal;
}

/* Return `lvalue` as an Identifier resolved to a slot, or nullptr otherwise.
 * The cheap ct tag (is_id) avoids a dynamic_cast on the hot assignment path. */
static inline const Identifier *
as_resolved_local(const Construct *lvalue)
{
    if (!lvalue->is_id())
        return nullptr;

    const Identifier *id = static_cast<const Identifier *>(lvalue);
    return id->sym.kind == SymKind::local ? id : nullptr;
}

/*
 * Fast path for `a[i] = v` / `a[i] OP= v` when `a` is a flat (unboxed) int or
 * float array: write the scalar straight into the unboxed vector, with no
 * promotion to vector<LValue> and no element-LValue round-trip. Without this,
 * the first store into a flat array (e.g. filling an array(N, 0)) would promote
 * it and undo the whole specialization.
 *
 * Returns true (and sets `out` to the stored value) when it handled the store;
 * false to fall through to the general lvalue->eval() -> doAssign() path. To
 * keep the fall-through sound it commits to the flat path only after deciding
 * on the BASE alone (which must be a side-effect-free lvalue - an id or a
 * nested subscript/member chain like `a[0][0]`, so re-eval on the general path
 * is harmless); the index is evaluated once, inside. Handling nested bases is
 * essential: a flat array nested in a general one (e.g. `[[1,2],[3,4]]`) has no
 * element LValue, so `a[0][0] = v` can only be written here.
 *
 * A value that doesn't fit the flat kind (a string into an int array, only
 * reachable through `dyn`) promotes the array and stores generally - same
 * result as the un-specialized path, just slower for that one cold write.
 */
static bool
no_side_effects(const Construct *c)
{
    if (c->is_id())
        return true;
    if (dynamic_cast<const Literal *>(c))   /* scalar literals are pure */
        return true;
    if (c->is_subscript()) {
        auto *s = static_cast<const Subscript *>(c);
        return no_side_effects(s->what.get()) &&
               no_side_effects(s->index.get());
    }
    if (auto *m = dynamic_cast<const MemberExpr *>(c))
        return no_side_effects(m->what.get());
    if (auto *mo = dynamic_cast<const MultiOpConstruct *>(c)) {
        for (const auto &pr : mo->elems)
            if (!no_side_effects(pr.second.get()))
                return false;
        return true;          /* pure arithmetic/comparison index, e.g. i+1 */
    }
    if (auto *t = dynamic_cast<const TypedScalarExpr *>(c)) {
        for (const auto &pr : t->elems)
            if (!no_side_effects(pr.second.get()))
                return false;
        return true;          /* specialized form of the above */
    }
    return false;
}

static bool
try_flat_subscript_store(EvalContext *ctx, Construct *lvalue, Op op,
                         const EvalValue &rval, EvalValue &out)
{
    if (!lvalue->is_subscript())
        return false;

    Subscript *sub = static_cast<Subscript *>(lvalue);

    /* Base must be a side-effect-free lvalue - see the note above. */
    if (!no_side_effects(sub->what.get()))
        return false;

    const EvalValue base_lv = sub->what->eval(ctx);
    if (!base_lv.is<LValue *>())
        return false;

    LValue *blv = base_lv.get<LValue *>();
    if (!blv->is<SharedArrayObj>())
        return false;

    SharedArrayObj &arr = blv->getval<SharedArrayObj>();

    if (arr.skind() == SharedArrayObj::Storage::general)
        return false;            /* not flat: let the general path handle it */

    /* A const/read-only array: defer so the general path raises the right error
     * (CannotChangeConstEx / NotLValueEx) with the proper loc. */
    if (blv->is_const_var() || arr.is_readonly())
        return false;

    /*
     * Flat POD-struct array: `a[i] = <matching POD struct>` stores the value's
     * bytes (a compound op falls through; structs have no `+=`). A non-matching
     * value is the dyn-launder case (errors like the scalar kinds).
     */
    if (arr.skind() == SharedArrayObj::Storage::structs) {

        if (op != Op::assign)
            return false;

        const EvalValue r = RValue(rval);
        const auto &sv0 = arr.flat_structs();

        const EvalValue idx_v = RValue(sub->index->eval(ctx));
        if (!idx_v.is<int_type>())
            throw TypeErrorEx("Expected integer as subscript",
                              sub->index->start, sub->index->end);
        int_type idx = idx_v.get<int_type>();
        if (idx < 0)
            idx += arr.size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
            throw OutOfBoundsEx(sub->start, sub->end);

        if (!r.is<intrusive_ptr<StructObject>>() ||
            !r.get<intrusive_ptr<StructObject>>()->is_pod() ||
            r.get<intrusive_ptr<StructObject>>()->def != sv0.def)
            throw TypeErrorEx(
                "Cannot store a value of a different type in a flat (typed) "
                "array; declare the array dyn for a polymorphic array",
                sub->start, sub->end);

        if (arr.is_slice())
            arr.clone_internal_vec();
        else if (arr.use_count() > 1)
            arr.clone_aliased_slices(arr.offset() + idx);

        auto &sv = arr.flat_structs();
        const StructObject &o = *r.get<intrusive_ptr<StructObject>>().get();
        std::memcpy(sv.buf.data() + (arr.offset() + idx) * sv.stride,
                    o.bytes.data(), sv.stride);
        out = r;
        return true;
    }

    const bool kind_int = arr.skind() == SharedArrayObj::Storage::ints;
    const bool kind_bool = arr.skind() == SharedArrayObj::Storage::bools;

    /* Committed to the flat path now: evaluate the index exactly once. */
    const EvalValue idx_v = RValue(sub->index->eval(ctx));
    if (!idx_v.is<int_type>())
        throw TypeErrorEx("Expected integer as subscript",
                          sub->index->start, sub->index->end);

    int_type idx = idx_v.get<int_type>();
    if (idx < 0)
        idx += arr.size();
    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx(sub->start, sub->end);

    const size_type at0 = arr.offset() + idx;
    const EvalValue r = RValue(rval);

    /* Compute the value to store (compound ops read the current element). */
    EvalValue newval;
    if (op == Op::assign) {
        newval = r;
    } else {
        newval = arr_elem_boxed(arr, idx);   /* read current element, any kind */
        apply_compound_op(newval, r, op);
    }

    const bool fits = kind_int
        ? newval.is<int_type>()
        : kind_bool
            ? newval.is<bool>()
            : (newval.is<float_type>() || newval.is<int_type>());

    if (!fits) {
        /*
         * The new element type doesn't fit the flat array's scalar kind. mylang
         * does not promote (flat->general) in place - the representation is
         * fixed at creation from the proven static type. This is reachable only
         * by laundering a typed array through `dyn` (declare it dyn from the
         * start for a polymorphic array). Same message as arr.cpp.h's
         * flat_array_violation_msg (a separate translation unit).
         */
        throw TypeErrorEx(
            "Cannot store a value of a different type in a flat (typed) array; "
            "declare the array dyn for a polymorphic array",
            sub->start, sub->end);
    }

    /*
     * COW, matching the general element-write semantics (get_value_for_put):
     * a slice clones itself; a non-slice that is aliased clones any live slices
     * so they don't observe the write, but writes in place otherwise (plain
     * handle aliases share the mutation - MyLang assignment aliases).
     */
    if (arr.is_slice())
        arr.clone_internal_vec();            /* keep-flat; now standalone */
    else if (arr.use_count() > 1)
        arr.clone_aliased_slices(at0);

    const size_type at = arr.offset() + idx;
    if (kind_int) {
        arr.flat_ints()[at] = newval.get<int_type>();
    } else if (kind_bool) {
        arr.flat_bools()[at] = newval.get<bool>() ? 1 : 0;
    } else {
        arr.flat_floats()[at] = newval.is<int_type>()
            ? static_cast<float_type>(newval.get<int_type>())
            : newval.get<float_type>();
    }

    arr.invalidate_hash();   /* an element write changes the array's hash */
    out = newval;
    return true;
}

/*
 * Fast path for `s.field = v` / `s.field OP= v` when `s` is a POD struct: a POD
 * field is bytes, not an LValue, so the general lvalue path can't target it -
 * write the (coerced/validated) scalar straight into the byte slot. Returns
 * false (and lets the general path run) for a boxed struct, a const/read-only
 * instance (so the right error fires), a non-field member, or a non-lvalue
 * base. No COW clone: a POD struct aliases like any value (a `var q = p` shares
 * it, and the write is shared - the same as the boxed path and arrays/dicts).
 */
static bool
try_pod_struct_store(EvalContext *ctx, Construct *lvalue, Op op,
                     const EvalValue &rval, EvalValue &out)
{
    auto *mem = dynamic_cast<MemberExpr *>(lvalue);
    if (!mem)
        return false;

    if (!no_side_effects(mem->what.get()))
        return false;

    const EvalValue base_lv = mem->what->eval(ctx);
    if (!base_lv.is<LValue *>())
        return false;                 /* temporary base: general path errors */

    LValue *blv = base_lv.get<LValue *>();
    if (!blv->is<intrusive_ptr<StructObject>>())
        return false;

    StructObject &obj = *blv->getval<intrusive_ptr<StructObject>>().get();
    if (!obj.is_pod())
        return false;                 /* boxed: general lvalue path handles */

    const int slot = obj.def->slot_of(mem->memUid);
    if (slot < 0)
        return false;                 /* a const member etc.: general path */

    if (blv->is_const_var() || obj.is_readonly())
        return false;                 /* const: defer for the right error/loc */

    const EvalValue r = RValue(rval);

    EvalValue newval;
    if (op == Op::assign) {
        newval = r;
    } else {
        newval = obj.pod_get(slot);
        apply_compound_op(newval, r, op);
    }

    /* coerce + runtime-validate to the field's scalar type (throws on mismatch,
     * e.g. a dyn-laundered wrong type) */
    newval = coerce_struct_field(obj.def->fields[slot], move(newval),
                                 mem->start, mem->end);
    obj.pod_set(slot, newval);

    out = newval;
    return true;
}

/*
 * Coerce a value to a declared scalar type's storage on a typed assignment /
 * declaration: a `float` variable stores an int/bool as a float, an `int`
 * variable stores a bool as an int (the numeric-widening coercions of the
 * `bool <= int <= float` chain). So `float f = 3;` and a later `f = 5;` actually
 * hold a float, not an int. Other declared types (str/bool/array/dict) and
 * non-widening values pass through unchanged; the inferencer has already
 * rejected anything not assignable to the declared type.
 */
static EvalValue coerce_to_decl_type(const EvalValue &v, DeclType dt)
{
    if (dt == DeclType::f) {
        if (v.is<int_type>())
            return EvalValue(static_cast<float_type>(v.get<int_type>()));
        if (v.is<bool>())
            return EvalValue(static_cast<float_type>(v.get<bool>() ? 1 : 0));
    } else if (dt == DeclType::i) {
        if (v.is<bool>())
            return EvalValue(static_cast<int_type>(v.get<bool>() ? 1 : 0));
    }
    return v;
}

static EvalValue
handle_single_expr14(EvalContext *ctx,
                     bool inDecl,
                     Op op,
                     Construct *lvalue,
                     const EvalValue &rval_in)
{
    /*
     * For a plain `=` to an explicitly-typed scalar variable, coerce a widening
     * value to the declared type (float <- int/bool, int <- bool). The decl's
     * type is on the Identifier (the resolver also copied it to every use, so a
     * later reassignment coerces too). Compound ops (`+=`) already produce the
     * right type via num_bin_op, so only `assign` is handled here.
     */
    EvalValue rval_storage;
    const EvalValue *rvalp = &rval_in;
    if (op == Op::assign && lvalue->is_id()) {
        const DeclType dt = static_cast<const Identifier *>(lvalue)->decl_type;
        if (dt == DeclType::f || dt == DeclType::i) {
            rval_storage = coerce_to_decl_type(RValue(rval_in), dt);
            rvalp = &rval_storage;
        }
    }
    const EvalValue &rval = *rvalp;

    /*
     * Fast path: declaring a resolved local. Write its slot and mark it live.
     * The resolver already rejected illegal same-block redeclarations and the
     * enclosing block cleared this slot's live bit on entry, so we just
     * (over)write - which is also exactly what a loop re-entry needs. The
     * dynamic_cast is gated on inDecl so plain assignments (the hot path, e.g.
     * `s += i`) never pay for it; an assignment to a resolved local instead
     * flows through the normal lvalue->eval() -> LValue* -> doAssign path
     * below.
     */
    if (inDecl) {

        if (const Identifier *id = as_resolved_local(lvalue)) {

            Frame *f = ctx->frame;
            f->slots[id->sym.slot] =
                LValue(RValue(rval), ctx->const_ctx || lvalue->is_const);
            f->live |= static_cast<uint64_t>(1) << id->sym.slot;
            return rval;
        }

    } else if (!ctx->const_ctx) {

        /*
         * Fast path: an assignment / compound-assignment to a resolved, live,
         * non-const local. The slot's LValue has no `container` (only array
         * elements do), so we read-modify-write it in place, skipping the
         * lvalue->eval() -> LValue* wrapping and the doAssign() dispatch the
         * general path below would run. Falls through to that path when the
         * slot is not live (so an undefined-variable error is still raised) or
         * is const (so the rebind error is still raised), keeping behavior
         * identical. The same in-place op (apply_compound_op / RValue) is used.
         */
        if (const Identifier *id = as_resolved_local(lvalue)) {

            Frame *f = ctx->frame;
            const uint64_t bit = static_cast<uint64_t>(1) << id->sym.slot;

            if (f && (f->live & bit)) {

                LValue &lv = f->slots[id->sym.slot];

                if (!lv.is_const_var()) {

                    if (op == Op::assign) {

                        lv.put(RValue(rval));

                    } else {

                        const EvalValue r = RValue(rval);

                        /* Direct int compound-assign (e.g. `j += i`): mutate
                         * the slot's int in place, skipping the copy in/out and
                         * the num_bin_op PMF dispatch. add/sub/mul can't fault;
                         * div/mod (and any non-int operand) take the general
                         * path, which keeps the zero check and int->float
                         * promotion. */
                        if (lv.is<int_type>() && r.is<int_type>() &&
                            (op == Op::addeq || op == Op::subeq ||
                             op == Op::muleq)) {

                            int_type &v = lv.getval<int_type>();
                            const int_type n = r.get<int_type>();

                            if (op == Op::addeq)      v += n;
                            else if (op == Op::subeq) v -= n;
                            else                      v *= n;

                        } else {

                            EvalValue nv = lv.get();
                            apply_compound_op(nv, r, op);
                            lv.put(move(nv));
                        }
                    }

                    return lv.get();
                }
            }
        }

        /*
         * Fast path: `a[i] = v` / `a[i] OP= v` into a flat (unboxed) int/float
         * array - write the scalar straight into the flat vector, no promotion.
         */
        EvalValue flat_out;
        if (try_flat_subscript_store(ctx, lvalue, op, rval, flat_out))
            return flat_out;
    }

    /*
     * Fast path: `s.field = v` / `s.field OP= v` into a POD struct - store the
     * scalar straight into the struct's byte slot (a POD field has no LValue).
     */
    {
        EvalValue pod_out;
        if (try_pod_struct_store(ctx, lvalue, op, rval, pod_out))
            return pod_out;
    }

    /*
     * Mark the lvalue eval as a plain-assignment target (op == assign) so a
     * dict subscript/member auto-vivifies a missing key instead of throwing
     * (`d[k] = v` inserts; a compound `d[k] += v` or a read does not - those
     * throw on a missing key in a plain dict). The outermost subscript/member
     * consumes the flag; reset it afterwards for non-subscript lvalues.
     */
    ctx->assign_target = (!inDecl && op == Op::assign);
    const EvalValue &lval = lvalue->eval(ctx);
    ctx->assign_target = false;

    if (lval.is<UndefinedId>()) {

        if (!inDecl)
            throw UndefinedVariableEx{ lval.get<UndefinedId>().id };

        ctx->emplace(
            lval.get<UndefinedId>().id,
            RValue(rval),
            ctx->const_ctx || lvalue->is_const
        );

    } else if (lval.is<LValue *>()) {

        if (ctx->const_ctx) {

            /*
             * We're in const ctx, and we're trying to change the value of a
             * symbol. That should not be possible. The parser won't try to evaluate
             * an assignment statement during const evaluation. If that happens,
             * a bug has been just introduced, in a recent changed.
             */
            throw InternalErrorEx();

        } else if (lval.get<LValue *>()->is_const_var()) {

            if (lval.get<LValue *>()->is<Builtin>())
                throw CannotRebindBuiltinEx(lvalue->start, lvalue->end);

            throw CannotRebindConstEx(lvalue->start, lvalue->end);
        }

        if (inDecl) {

            const EvalValue &local_lval = lvalue->eval(ctx, false);

            if (!local_lval.is<UndefinedId>()) {

                /* REPL: a re-declaration of an existing global rebinds it
                 * (fresh value + const-ness) - see
                 * EvalContext::allow_redeclare.
                 * Otherwise, re-defining the same variable in the same scope is
                 * an error (the script rule). */
                if (ctx->allow_redeclare && lvalue->is_id()) {
                    const Identifier *id =
                        static_cast<const Identifier *>(lvalue);
                    ctx->erase(id);
                    ctx->emplace(id, RValue(rval), lvalue->is_const);
                    return rval;
                }

                throw AlreadyDefinedEx(lvalue->start, lvalue->end);
            }

            /* We're re-declaring a symbol already declared outside */
            ctx->emplace(
                local_lval.get<UndefinedId>().id,
                RValue(rval),
                ctx->const_ctx
            );

        } else {
            return doAssign(lval, rval, op);
        }

    } else {
        throw NotLValueEx(lvalue->start, lvalue->end);
    }

    return rval;
}

EvalValue Expr14::do_eval(EvalContext *ctx, bool rec) const
{
    const bool inDecl = fl & pFlags::pInDecl;

    /*
     * Fast path for `local += N` / `-= N` / `*= N` where N is an int literal
     * and the slot currently holds a live, non-const int. Mutates the slot's
     * int in place, skipping both the rvalue-node eval (the literal) and the
     * num_bin_op dispatch. add/sub/mul can't fault (they wrap, -fwrapv), so no
     * check is needed; div/mod fall through to the general path (zero check).
     * This is what an `i++` / `i += 1` increment would compile to.
     */
    if (!inDecl && !ctx->const_ctx &&
        (op == Op::addeq || op == Op::subeq || op == Op::muleq)) {

        if (const Identifier *id = as_resolved_local(lvalue.get())) {

            /*
             * The rhs is an int when it is an int literal OR a node the
             * inferencer proved is a non-null int (th == i) - e.g. a POD struct
             * field `p.x`. The latter reads UNBOXED via eval_int() (no
             * pod_get/EvalValue), which is what makes `sx += p.x` over a flat
             * struct array a true unboxed reduction (plans/structs.md phase 8).
             */
            const bool rhs_int =
                rvalue->is_lit_int() || rvalue->th == TypeHint::i;

            if (rhs_int) {

                Frame *f = ctx->frame;
                const uint64_t bit = static_cast<uint64_t>(1) << id->sym.slot;

                if (f && (f->live & bit)) {

                    LValue &lv = f->slots[id->sym.slot];

                    if (!lv.is_const_var() && lv.is<int_type>()) {

                        const int_type n = rvalue->is_lit_int()
                            ? static_cast<const LiteralInt *>(
                                  rvalue.get())->ival()
                            : rvalue->eval_int(ctx);   /* unboxed */

                        int_type &v = lv.getval<int_type>();

                        if (op == Op::addeq)      v += n;
                        else if (op == Op::subeq) v -= n;
                        else                      v *= n;

                        return v;
                    }
                }
            }
        }
    }

    /* Evaluate the rhs first; point errors (undefined var, ...) at the rhs
     * itself, not at the whole `lhs = rhs` assignment. */
    EvalValue rval_storage;
    try {
        rval_storage = RValue(rvalue->eval(ctx));
    } catch (Exception &e) {
        stamp_operand_loc(rvalue.get(), e);
        throw;
    }
    const EvalValue &rval = rval_storage;
    IdList *idlist = nullptr;

    if (lvalue->is_idlist())
        idlist = static_cast<IdList *>(lvalue.get());

    if (idlist) {

        if (!rval.is<SharedArrayObj>()) {

            for (const auto &e: idlist->elems)
                handle_single_expr14(ctx, inDecl, op, e.get(), rval);

        } else {

            const SharedArrayObj &arr = rval.get<SharedArrayObj>();
            const size_type asz = arr.size();

            for (size_type i = 0; i < idlist->elems.size(); i++) {

                handle_single_expr14(
                    ctx,
                    inDecl,
                    op,
                    idlist->elems[i].get(),
                    i < asz ? arr_elem_boxed(arr, i) : none
                );
            }
        }

        return none;

    } else {

        return handle_single_expr14(ctx, inDecl, op, lvalue.get(), rval);
    }
}

/*
 * C-style ++ / -- on an int/float lvalue. Two paths, both evaluating the
 * operand exactly ONCE:
 *
 *  - statically int/float (th stamped by the inferencer - the usual case,
 *    incl. flat-array elements and POD struct fields, which have no LValue):
 *    route the mutation through handle_single_expr14 (`operand += 1`), which
 *    reuses every store fast path (slot, flat array, COW, struct). It returns
 *    the NEW value; postfix derives `old = new ∓ 1` (the delta is exactly 1),
 *    so we never re-read the operand;
 *
 *  - a `dyn` / un-hinted operand (always LValue-backed - a dyn value is never
 *    flat): read-modify-write through the LValue so the int/float requirement
 *    can be enforced at runtime (a `dyn` holding a string/bool throws here).
 *
 * The inferencer rejects a non-lvalue / const / non-numeric operand at compile
 * time; the runtime checks are the `dyn` safety net.
 */
EvalValue IncDecExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * Fast path: a resolved, live, non-const LOCAL holding an int or float -
     * mutate the slot's scalar straight in place and return old (postfix) / new
     * (prefix), with NO handle_single_expr14 call and NO num_bin_op. This is
     * the loop-counter / scalar-var case; it must be at least as fast as
     * `i += 1` (and is slightly faster - no literal-1 operand to read).
     */
    if (!ctx->const_ctx) {
        if (const Identifier *id = as_resolved_local(lvalue.get())) {
            Frame *f = ctx->frame;
            const uint64_t bit = static_cast<uint64_t>(1) << id->sym.slot;
            if (f && (f->live & bit)) {
                LValue &lv = f->slots[id->sym.slot];
                if (!lv.is_const_var()) {
                    if (lv.is<int_type>()) {
                        int_type &v = lv.getval<int_type>();
                        const int_type old = v;
                        v += is_inc ? 1 : -1;
                        return is_prefix ? EvalValue(v) : EvalValue(old);
                    }
                    if (lv.is<float_type>()) {
                        float_type &v = lv.getval<float_type>();
                        const float_type old = v;
                        v += is_inc ? 1.0 : -1.0;
                        return is_prefix ? EvalValue(v) : EvalValue(old);
                    }
                    /* another type (only reachable via a dyn alias): fall
                     * through to the general path, which raises the error. */
                }
            }
        }
    }

    const Op cop = is_inc ? Op::addeq : Op::subeq;
    const EvalValue one{static_cast<int_type>(1)};

    if (th == TypeHint::i || th == TypeHint::f) {

        const EvalValue nv =
            handle_single_expr14(ctx, false, cop, lvalue.get(), one);

        if (is_prefix)
            return nv;

        EvalValue old = nv;     /* old = new ∓ 1 (no re-read of the operand) */
        apply_compound_op(old, one, is_inc ? Op::subeq : Op::addeq);
        return old;
    }

    /* dyn / un-hinted: read-modify-write through the LValue. */
    EvalValue lref;
    try {
        lref = lvalue->eval(ctx);
    } catch (Exception &e) {
        stamp_operand_loc(lvalue.get(), e);
        throw;
    }

    if (lref.is<UndefinedId>())
        throw UndefinedVariableEx(lref.get<UndefinedId>().id, start, end);

    if (!lref.is<LValue *>())
        throw NotLValueEx(start, end);

    LValue *lv = lref.get<LValue *>();

    if (lv->is<Builtin>())
        throw CannotRebindBuiltinEx();

    if (lv->is_const_var())
        throw CannotChangeConstEx(start, end);

    EvalValue old = lv->get();

    if (!old.is<int_type>() && !old.is<float_type>())
        throw TypeErrorEx("'++'/'--' requires an int or float", start, end);

    EvalValue nv = old;
    apply_compound_op(nv, one, cop);
    lv->put(move(nv));

    return is_prefix ? lv->get() : old;
}

EvalValue IfStmt::do_eval(EvalContext *ctx, bool rec) const
{
    if (eval_cond(condExpr.get(), ctx)) {

        if (thenBlock)
            thenBlock->eval(ctx);

    } else {

        if (elseBlock)
            elseBlock->eval(ctx);
    }

    return none;
}

EvalValue BreakStmt::do_eval(EvalContext *ctx, bool rec) const
{
    ctx->flow->type = FlowState::brk;
    return none;
}

EvalValue ContinueStmt::do_eval(EvalContext *ctx, bool rec) const
{
    ctx->flow->type = FlowState::cont;
    return none;
}

EvalValue ReturnStmt::do_eval(EvalContext *ctx, bool rec) const
{
    /* RValue() throws UndefinedVariableEx (with this stmt's loc) if needed */
    ctx->flow->value = elem ? RValue(elem->eval(ctx)) : none;
    ctx->flow->type = FlowState::ret;
    return none;
}

EvalValue RethrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    throw RethrowEx{start, end};
}

EvalValue ThrowStmt::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &e = RValue(elem->eval(ctx));

    /*
     * Throwing a struct instance: the struct IS the exception. Wrap it as a
     * named exception whose name is the struct's type (so `catch (T)` matches
     * by type) carrying the instance as the payload (so `catch (T as v)` binds
     * the instance and `v.field` reads it).
     */
    if (e.is<intrusive_ptr<StructObject>>()) {
        throw ExceptionObject(
            string(e.get<intrusive_ptr<StructObject>>()->def->name->val),
            e
        );
    }

    /*
     * Re-throwing a caught built-in exception value (bound by `catch (X as e)`,
     * which hands back an exception object for a payload-less built-in).
     */
    if (e.is<shared_ptr<ExceptionObject>>())
        throw *e.get<shared_ptr<ExceptionObject>>().get();

    throw TypeErrorEx(
        "Can only throw a struct instance",
        elem->start,
        elem->end
    );
}

/* Live-bit mask for a block's contiguous slot range [start, start+count). */
static inline uint64_t
block_slot_mask(int slot_start, int slot_count)
{
    if (slot_count >= 64)
        return ~static_cast<uint64_t>(0);
    return ((static_cast<uint64_t>(1) << slot_count) - 1) << slot_start;
}

EvalValue Block::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * Scope-free fast path: this block declares only frame slots, so it never
     * uses the EvalContext map. Run its statements directly in the parent
     * context, skipping the per-entry EvalContext construction/destruction.
     * Only the slot range still needs clearing on entry (re-entry semantics).
     * The root block (ctx == nullptr) always takes the full path below, since
     * it owns the program's context and frame.
     */
    if (scope_free && ctx) {

        if (ctx->frame && slot_count)
            ctx->frame->live &= ~block_slot_mask(slot_start, slot_count);

        for (const auto &e : elems) {

            EvalValue &&tmp = e->eval(ctx);

            if (tmp.is<UndefinedId>())
                throw UndefinedVariableEx(
                    tmp.get<UndefinedId>().id, e->start, e->end);

            if (ctx->flow->type != FlowState::none)
                break;
        }

        return none;
    }

    EvalContext curr(ctx, ctx ? ctx->const_ctx : false);

    /*
     * The root block (ctx == nullptr) is the program's implicit "main": build
     * its Frame here so slotted top-level variables get O(1) slots. curr IS the
     * root context, so unslotted globals still live in curr's map where
     * functions reach them (via get_root_ctx). The Frame lives for the whole
     * program (this do_eval spans it).
     */
    unique_ptr<Frame> root_frame;

    if (!ctx && slot_count) {
        root_frame = make_unique<Frame>();
        root_frame->init(slot_count);
        curr.frame = root_frame.get();
    }

    /*
     * Reset this block's resolved locals to "undefined" on entry. The slots
     * persist for the whole call, so without this a re-entered block (a loop
     * body, say) would still see the previous iteration's bindings live. The
     * resolver guarantees a block's slots are a contiguous range, so one mask
     * op clears them; slot_count == 0 (no slotted locals / unresolved function)
     * makes this a no-op. Params live below the body block's range, so they are
     * never cleared here.
     */
    if (curr.frame && slot_count) {

        curr.frame->live &= ~block_slot_mask(slot_start, slot_count);
    }

    for (const auto &e: elems) {

        EvalValue &&tmp = e->eval(&curr);

        if (tmp.is<UndefinedId>())
            throw UndefinedVariableEx(tmp.get<UndefinedId>().id, e->start, e->end);

        /*
         * A return/break/continue fired in this statement (possibly nested in
         * ifs): stop running the block and let the signal propagate upward to
         * the enclosing loop or function boundary.
         */
        if (curr.flow->type != FlowState::none)
            break;
    }

    return none;
}

EvalValue WhileStmt::do_eval(EvalContext *ctx, bool rec) const
{
    FlowState &fs = *ctx->flow;

    while (eval_cond(condExpr.get(), ctx)) {

        if (body)
            body->eval(ctx);

        if (fs.type == FlowState::ret)
            break;                              /* propagate to the function */

        if (fs.type == FlowState::brk) {
            fs.type = FlowState::none;
            break;
        }

        if (fs.type == FlowState::cont)
            fs.type = FlowState::none;          /* consume; loop again */
    }

    return none;
}

EvalValue StructDeclStmt::do_eval(EvalContext *ctx, bool rec) const
{
    /* Bind the struct name to its type descriptor (a const t_structtype value
     * holding the AST-owned StructTypeDef*), like a func name. */
    EvalValue desc(def.get());

    if (id) {

        if (!id->eval(ctx).is<UndefinedId>()) {
            if (!ctx->allow_redeclare)        /* REPL: redefining replaces */
                throw AlreadyDefinedEx(id->start, id->end);
            ctx->erase(id.get());
        }

        ctx->emplace(id.get(), move(desc), true /* const */);
        return none;
    }

    return desc;
}

EvalValue FuncDeclStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalValue func(
        shared_ptr<FuncObject>(make_shared<FuncObject>(this, ctx))
    );

    if (id) {

        if (!id->eval(ctx).is<UndefinedId>()) {
            /* REPL: re-defining a function replaces it (the edit-and-resubmit
             * workflow); a script rejects the duplicate. */
            if (!ctx->allow_redeclare)
                throw AlreadyDefinedEx(id->start, id->end);
            ctx->erase(id.get());
        }

        ctx->emplace(
            id.get(),
            move(func),
            ctx->const_ctx
        );

        return none;
    }

    return func;
}

EvalValue Subscript::do_eval(EvalContext *ctx, bool rec) const
{
    /*
     * Consume the plain-assignment-target flag here (before evaluating `what`
     * and the index), so a nested base like `d[k1]` in `d[k1][k2] = v` is read
     * normally (throws/defaults on a missing key) while only this outermost
     * subscript may auto-vivify. See EvalContext::assign_target.
     */
    const bool for_write = ctx->assign_target;
    ctx->assign_target = false;

    const EvalValue &lval = what->eval(ctx);

    Type *t = lval.is<LValue *>()
        ? lval.get<LValue *>()->get().get_type()
        : lval.get_type();

    if (t->t == Type::t_undefid) {
        throw UndefinedVariableEx(
            lval.get<UndefinedId>().id, what->start, what->end
        );
    }

    return t->subscript(lval, RValue(index->eval(ctx)), for_write);
}

/*
 * Typed array element read: skip the virtual subscript dispatch and the
 * EvalValue result construction, reading the element's scalar directly. Only
 * for an array base (the inferencer proved array<int>/array<float>); anything
 * else (dict, str, dyn) falls back to the boxed path.
 */
/* The stored value of a present dict key, else nullptr (defined below near
 * MemberExpr). Shared by the typed dict fast paths of Subscript and MemberExpr;
 * a missing key falls back to do_eval. */
static const EvalValue *
dict_present_value(const intrusive_ptr<DictObject> &obj, const EvalValue &key);

int_type Subscript::eval_int(EvalContext *ctx) const
{
    const EvalValue &lval = what->eval(ctx);
    const EvalValue &base = lval.is<LValue *>()
        ? lval.get<LValue *>()->get() : lval;

    if (base.is<SharedArrayObj>()) {
        const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
        int_type idx = index->eval_int(ctx);
        if (idx < 0)
            idx += arr.size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
            throw OutOfBoundsEx(start, end);
        const size_type at = arr.offset() + idx;
        if (arr.skind() == SharedArrayObj::Storage::ints)
            return arr.flat_ints()[at];     /* unboxed: no promotion */
        if (arr.skind() == SharedArrayObj::Storage::bools)
            return arr.flat_bools()[at] ? 1 : 0;   /* bool elem -> int 0/1 */
        return arr.get_vec()[at].getval<int_type>();
    }
    /* typed dict read `d[k]` (present key): look the value up directly instead
     * of Construct::eval_int, which would re-evaluate `what` (the dict). */
    if (base.is<intrusive_ptr<DictObject>>()) {
        const EvalValue key = RValue(index->eval(ctx));
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), key)) {
            if (v->is<bool>())
                return v->get<bool>() ? 1 : 0;
            return v->get<int_type>();
        }
    }
    return Construct::eval_int(ctx);   /* missing key / non-dict: do_eval */
}

float_type Subscript::eval_float(EvalContext *ctx) const
{
    const EvalValue &lval = what->eval(ctx);
    const EvalValue &base = lval.is<LValue *>()
        ? lval.get<LValue *>()->get() : lval;

    if (base.is<SharedArrayObj>()) {
        const SharedArrayObj &arr = base.get_ref<SharedArrayObj>();
        int_type idx = index->eval_int(ctx);
        if (idx < 0)
            idx += arr.size();
        if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
            throw OutOfBoundsEx(start, end);
        const size_type at = arr.offset() + idx;
        if (arr.skind() == SharedArrayObj::Storage::floats)
            return arr.flat_floats()[at];   /* unboxed: no promotion */
        if (arr.skind() == SharedArrayObj::Storage::ints)
            return static_cast<float_type>(arr.flat_ints()[at]);
        if (arr.skind() == SharedArrayObj::Storage::bools)
            return arr.flat_bools()[at] ? 1.0 : 0.0;
        const LValue &el = arr.get_vec()[at];
        if (el.is<int_type>())
            return static_cast<float_type>(el.getval<int_type>());
        return el.getval<float_type>();
    }
    if (base.is<intrusive_ptr<DictObject>>()) {
        const EvalValue key = RValue(index->eval(ctx));
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), key)) {
            if (v->is<int_type>())
                return static_cast<float_type>(v->get<int_type>());
            if (v->is<bool>())
                return v->get<bool>() ? 1.0 : 0.0;
            return v->get<float_type>();
        }
    }
    return Construct::eval_float(ctx);   /* missing key / non-dict: do_eval */
}

EvalValue Slice::do_eval(EvalContext *ctx, bool rec) const
{
    const EvalValue &lval = what->eval(ctx);

    Type *t = lval.is<LValue *>()
        ? lval.get<LValue *>()->get().get_type()
        : lval.get_type();

    if (t->t == Type::t_undefid) {
        throw UndefinedVariableEx(
            lval.get<UndefinedId>().id, what->start, what->end
        );
    }

    return t->slice(
        lval,
        start_idx ? RValue(start_idx->eval(ctx)) : none,
        end_idx ? RValue(end_idx->eval(ctx)) : none
    );
}

static bool
do_catch(EvalContext *ctx,
         RuntimeException *saved_ex,
         IdList *exList,
         Identifier *asId,
         Construct *catchBody)
{
    if (!exList) {

        /* Catch-anything block */
        try {
            catchBody->eval(ctx);
        } catch (const RethrowEx &re) {
            saved_ex->loc_start = re.start;
            saved_ex->loc_end = re.end;
            saved_ex->rethrow();
        }

        return true;
    }

    ExceptionObject *exObj = dynamic_cast<ExceptionObject *>(saved_ex);
    string_view ex_name = exObj ? exObj->get_name() : saved_ex->name;

    for (const unique_ptr<Identifier> &id : exList->elems) {

        if (id->get_str() != ex_name)
            continue;

        try {

            EvalContext catch_ctx(ctx);

            if (asId) {

                /*
                 * Bind the catch variable. A thrown struct carries the
                 * instance as its payload, so `e` IS the struct (and `e.field`
                 * works). A payload-less built-in exception binds to a small
                 * exception object instead (printable, re-throwable).
                 */
                EvalValue bind_val =
                    (exObj && exObj->get_data()
                                  .is<intrusive_ptr<StructObject>>())
                        ? exObj->get_data()
                        : EvalValue(make_shared<ExceptionObject>(
                              exObj
                                  ? *exObj
                                  : ExceptionObject(saved_ex->name)));

                if (asId->sym.kind == SymKind::local && catch_ctx.frame) {

                    /* Resolved catch variable: bind it into its slot. */
                    Frame *f = catch_ctx.frame;
                    f->slots[asId->sym.slot] =
                        LValue(move(bind_val), ctx->const_ctx);
                    f->live |= static_cast<uint64_t>(1) << asId->sym.slot;

                } else {

                    catch_ctx.emplace(
                        asId,
                        move(bind_val),
                        ctx->const_ctx
                    );
                }
            }

            catchBody->eval(&catch_ctx);

        } catch (const RethrowEx &re) {
            saved_ex->loc_start = re.start;
            saved_ex->loc_end = re.end;
            saved_ex->rethrow();
        }

        return true;
    }

    return false;
}

EvalValue TryCatchStmt::do_eval(EvalContext *ctx, bool rec) const
{
    struct trivial_scope_guard {

        EvalContext *ctx;
        Construct *finallyBody;

        trivial_scope_guard(EvalContext *ctx, Construct *body)
            : ctx(ctx), finallyBody(body) { }

        ~trivial_scope_guard() {

            if (!finallyBody)
                return;

            FlowState &fs = *ctx->flow;

            /*
             * A return/break/continue may be in flight out of the try or catch
             * block. Suspend it so the finally body runs to completion, then
             * resume it - unless finally raised its own control-flow signal,
             * which then takes over (as in C#/Java).
             */
            const FlowState::Type saved_type = fs.type;
            EvalValue saved_val = move(fs.value);
            fs.type = FlowState::none;

            finallyBody->eval(ctx);

            if (fs.type == FlowState::none) {
                fs.type = saved_type;
                fs.value = move(saved_val);
            }
        }
    };

    trivial_scope_guard on_exit(ctx, finallyBody.get());
    unique_ptr<RuntimeException> saved_ex;

    try {

        tryBody->eval(ctx);

    } catch (const RuntimeException &e) {

        saved_ex.reset(e.clone());
    }

    if (!saved_ex)
        return none;

    for (const auto &p : catchStmts) {

        IdList *exList = p.first.exList.get();
        Identifier *asId = p.first.asId.get();
        Construct *catchBody = p.second.get();

        if (do_catch(ctx, saved_ex.get(), exList, asId, catchBody))
            return none;
    }

    saved_ex->rethrow();
    return none; /* Make compilers unaware of [[noreturn]] happy */
}

LValue LValue::clone()
{
    LValue nl(valtype()->clone(val), is_const);
    nl.container = container;
    nl.container_idx = container_idx;
    return nl;
}

EvalValue &LValue::get_value_for_put()
{
    if (!container)
        return val;

    assert(container->is<SharedArrayObj>());

    if (container->valtype()->is_slice(container->val)) {
        const size_type off = container->getval<SharedArrayObj>().offset();
        *container = container->clone();
        container_idx -= off;
        return container->getval<SharedArrayObj>().get_vec()[container_idx].val;
    }

    if (container->valtype()->use_count(container->val) > 1)
        container->getval<SharedArrayObj>().clone_aliased_slices(container_idx);

    /* an in-place element write changes the array's hash (the slice path above
     * returns a fresh clone, which is already hash-invalid). */
    container->getval<SharedArrayObj>().invalidate_hash();
    return val;
}

void LValue::put(const EvalValue &v)
{
    get_value_for_put() = v;
    type_checks();
}

void LValue::put(EvalValue &&v)
{
    get_value_for_put() = move(v);
    type_checks();
}

/*
 * Bind a foreach loop variable to `val`. Fast path: a resolved-local loop var
 * is a plain frame slot, so write it directly (mark live), skipping the general
 * lvalue->eval() + doAssign() machinery handle_single_expr14 would run on every
 * iteration. Correct for every iteration (not just the decl): the loop var is a
 * fresh `var` each pass, so overwriting the slot is exactly the semantics. The
 * loop var elements (`ids->elems`) are Identifiers, so no dynamic_cast is
 * needed. Non-resolved (map-based) loop vars fall back to the general path.
 */
static inline void
bind_loop_var(EvalContext *ctx, bool decl, Identifier *id, const EvalValue &val)
{
    if (id->sym.kind == SymKind::local && ctx->frame) {
        Frame *f = ctx->frame;
        f->slots[id->sym.slot] = LValue(val, id->is_const);
        f->live |= static_cast<uint64_t>(1) << id->sym.slot;
        return;
    }

    handle_single_expr14(ctx, decl, Op::assign, id, val);
}

bool
ForeachStmt::do_iter(EvalContext *ctx,
                     size_type index,
                     const EvalValue *elems,
                     size_type count) const
{
    const bool decl = index == 0 ? idsVarDecl : false;
    size_type id_start = 0;

    if (indexed) {

        bind_loop_var(
            ctx, decl, ids->elems[0].get(), static_cast<int_type>(index)
        );

        id_start++;
    }

    if (count == 1) {

        if (elems[0].is<SharedArrayObj>() && ids->elems.size() > (1 + id_start)) {

            const SharedArrayObj &arr = elems[0].get<SharedArrayObj>();
            const size_type asz = arr.size();

            for (size_type i = id_start; i < ids->elems.size(); i++) {

                const size_type val_i = i - id_start;

                bind_loop_var(
                    ctx,
                    decl,
                    ids->elems[i].get(),
                    val_i < asz ? arr_elem_boxed(arr, val_i) : none
                );
            }

        } else {

            bind_loop_var(ctx, decl, ids->elems[id_start].get(), elems[0]);

            for (size_type i = id_start+1; i < ids->elems.size(); i++)
                bind_loop_var(ctx, decl, ids->elems[i].get(), none);
        }

    } else {

        for (size_type i = id_start; i < ids->elems.size(); i++) {

            const size_type val_i = i - id_start;

            bind_loop_var(
                ctx,
                decl,
                ids->elems[i].get(),
                val_i < count ? elems[val_i] : none
            );
        }
    }

    if (body)
        body->eval(ctx);

    FlowState &fs = *ctx->flow;

    if (fs.type == FlowState::ret)
        return false;                       /* stop; propagate up */

    if (fs.type == FlowState::brk) {
        fs.type = FlowState::none;
        return false;                       /* stop iterating */
    }

    if (fs.type == FlowState::cont)
        fs.type = FlowState::none;          /* consume; advance to next item */

    return true;
}

EvalValue
ForeachStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext loopCtx(ctx, ctx->const_ctx);
    const EvalValue &cval = RValue(container->eval(ctx));

    if (cval.is<SharedArrayObj>()) {

        const SharedArrayObj &arr = cval.get<SharedArrayObj>();

        /*
         * Flat fast path: iterate the unboxed int/float vector directly, with
         * no promotion to vector<LValue>. Each element is materialized into a
         * scalar EvalValue per iteration (cheap, trivially-copyable).
         */
        if (arr.skind() == SharedArrayObj::Storage::ints) {

            const auto &iv = arr.flat_ints();
            const size_type off = arr.offset(), n = arr.size();

            for (size_type i = 0; i < n; i++) {
                const EvalValue elem(iv[off + i]);
                if (!do_iter(&loopCtx, i, &elem, 1))
                    break;
            }

        } else if (arr.skind() == SharedArrayObj::Storage::floats) {

            const auto &fv = arr.flat_floats();
            const size_type off = arr.offset(), n = arr.size();

            for (size_type i = 0; i < n; i++) {
                const EvalValue elem(fv[off + i]);
                if (!do_iter(&loopCtx, i, &elem, 1))
                    break;
            }

        } else if (arr.skind() == SharedArrayObj::Storage::bools) {

            const auto &bv = arr.flat_bools();
            const size_type off = arr.offset(), n = arr.size();

            for (size_type i = 0; i < n; i++) {
                const EvalValue elem(static_cast<bool>(bv[off + i]));
                if (!do_iter(&loopCtx, i, &elem, 1))
                    break;
            }

        } else if (arr.skind() == SharedArrayObj::Storage::structs) {

            /*
             * Flat POD-struct array: rather than heap-allocate a StructObject
             * per element, reuse ONE across iterations - overwrite its bytes in
             * place and hand it to the body. COW guard: if the previous body
             * captured the element (so something other than the loop var still
             * holds it: use_count() > 2 == this local + the loop var's slot +
             * a capture), allocate a fresh one so the capture keeps its value.
             */
            const auto &sv = arr.flat_structs();
            const size_type n = arr.size(), base = arr.offset();
            intrusive_ptr<StructObject> reuse;

            for (size_type i = 0; i < n; i++) {

                if (!reuse || reuse.use_count() > 2)
                    reuse = intrusive_ptr<StructObject>(
                        make_intrusive<StructObject>(sv.def));

                std::memcpy(reuse->bytes.data(),
                            sv.buf.data() + (base + i) * sv.stride, sv.stride);

                const EvalValue elem(reuse);
                if (!do_iter(&loopCtx, i, &elem, 1))
                    break;
            }

        } else {

            const ArrayConstView &view = arr.get_view();

            for (size_type i = 0; i < view.size(); i++) {

                const EvalValue &elem = view[i].get();

                if (!do_iter(&loopCtx, i, &elem, 1))
                    break;
            }
        }

    } else if (cval.is<SharedStr>()) {

        const string_view view = cval.get<SharedStr>().get_view();

        for (size_type i = 0; i < view.size(); i++) {

            const EvalValue &elem = SharedStr(string(&view[i], 1));

            if (!do_iter(&loopCtx, i, &elem, 1))
                break;
        }

    } else if (cval.is<intrusive_ptr<DictObject>>()) {

        const DictObject::inner_type &data
            = cval.get<intrusive_ptr<DictObject>>()->get_ref();

        size_type i = 0;

        for (const auto &p : data) {

            const EvalValue elems[2] = { p.first, p.second.get() };

            if (!do_iter(&loopCtx, i, elems, 2))
                break;

            i++;
        }

    } else {

        throw TypeErrorEx(
            "Unsupported container type by foreach()",
            container->start,
            container->end
        );
    }

    return none;
}

EvalValue LiteralDict::do_eval(EvalContext *ctx, bool rec) const
{
    DictObject::inner_type data;

    for (const auto &e : elems) {

        /* freeze the key (see TypeDict::subscript) so a mutable container key
         * cannot be mutated later and corrupt the dict. */
        data.emplace(
            make_const_clone(RValue(e->key->eval(ctx))),
            LValue(RValue(e->value->eval(ctx)), ctx->const_ctx)
        );
    }

    return intrusive_ptr<DictObject>(make_intrusive<DictObject>(move(data)));
}

/* True if `c` is rooted at a variable (an identifier, or a member/subscript
 * chain ending at one), so an lvalue derived from it outlives this evaluation;
 * a temporary (a call/literal result) is not. */
static bool is_lvalue_rooted(const Construct *c)
{
    if (c->is_id())
        return true;
    if (auto *m = dynamic_cast<const MemberExpr *>(c))
        return is_lvalue_rooted(m->what.get());
    if (auto *s = dynamic_cast<const Subscript *>(c))
        return is_lvalue_rooted(s->what.get());
    return false;
}

EvalValue MemberExpr::do_eval(EvalContext *ctx, bool rec) const
{
    /* Consume the plain-assignment-target flag (see Subscript::do_eval). */
    const bool for_write = ctx->assign_target;
    ctx->assign_target = false;

    EvalValue &&dval = RValue(what->eval(ctx));

    /*
     * A struct instance: `s.field` is a field (an lvalue when the instance is
     * mutable, so `s.f = v` / `s.f += v` work; an rvalue for a read-only/const
     * instance, so a write fails NotLValueEx), else `s.K` is a const member,
     * else field-not-found.
     */
    if (dval.is<intrusive_ptr<StructObject>>()) {

        const auto &obj = dval.get<intrusive_ptr<StructObject>>();
        const int slot = obj->def->slot_of(memUid);

        if (slot >= 0) {
            /*
             * A POD field has no per-field LValue (it is bytes), so a read
             * always returns the value; a write goes through
             * try_pod_struct_store (handle_single_expr14), never this lvalue
             * path.
             */
            if (obj->is_pod())
                return obj->pod_get(slot);
            /*
             * Boxed: hand out an assignable field lvalue ONLY when the base is
             * rooted at a variable (so the StructObject outlives this call).
             * For a read-only instance, or a *temporary* base (`Point(1,2).x`,
             * a call result), return the value by copy - a field lvalue into a
             * soon-freed temporary would dangle, and a temporary's field isn't
             * assignable anyway.
             */
            if (obj->is_readonly() || !is_lvalue_rooted(what.get()))
                return obj->fields[slot].get();
            return &obj->fields[slot];
        }

        if (const EvalValue *cv = obj->def->const_of(memUid))
            return *cv;

        throw TypeErrorEx(
            intern_msg("Struct '" + string(obj->def->name->val) +
                       "' has no member '" + string(memUid->val) + "'"),
            start, end);
    }

    /* A struct TYPE descriptor: only its `const` members (no instance). */
    if (dval.is<StructTypeDef *>()) {

        StructTypeDef *def = dval.get<StructTypeDef *>();

        if (const EvalValue *cv = def->const_of(memUid))
            return *cv;

        if (def->slot_of(memUid) >= 0)
            throw TypeErrorEx(
                intern_msg("Field '" + string(memUid->val) +
                           "' needs an instance"), start, end);

        throw TypeErrorEx(
            intern_msg("Struct '" + string(def->name->val) +
                       "' has no member '" + string(memUid->val) + "'"),
            start, end);
    }

    if (!dval.is<intrusive_ptr<DictObject>>())
        throw TypeErrorEx("Expected dict object", what->start, what->end);

    const auto &obj = dval.get<intrusive_ptr<DictObject>>();
    DictObject::inner_type &data = obj->get_ref();
    const auto &it = data.find(memId);

    /*
     * `d.key` mirrors `d[key]` (TypeDict::subscript): present -> the value
     * (lvalue when mutable, so `d.k = v` / `d.k += v` work); missing -> the
     * default (default dict), else throw on a read / auto-vivify on a plain-
     * assignment target. So `d.k` is non-opt (a value or an exception, never
     * none); use get()/get!() for explicit nullable / fail-fast lookup.
     */
    if (obj->is_readonly()) {
        if (it != data.end())
            return it->second.get();
        if (obj->get_has_default())
            return obj->get_default();
        if (for_write)
            return none;            /* write then fails NotLValueEx */
        throw KeyNotFoundEx(start, end);
    }

    if (it != data.end())
        return &it->second;

    if (obj->get_has_default())
        return &(*data.emplace(memId, LValue(obj->get_default(), false))
                      .first).second;

    if (for_write)
        return &(*data.emplace(memId, LValue(none, false)).first).second;

    throw KeyNotFoundEx(start, end);
}

/*
 * Typed (unboxed) reads of a POD struct field (the M8 fast path, plans/
 * structs.md phase 8): the inferencer stamps `th` on `s.x` when the field is a
 * non-null int/float, and the specializer / compound-assign fast path then call
 * these instead of do_eval() -> pod_get() -> a boxed EvalValue.
 *
 * Two cases: `a[i].field` on a flat POD-struct array reads the scalar STRAIGHT
 * from the array bytes, with NO per-element StructObject materialized at all
 * (guarded by no_side_effects so the array base is evaluated once); any other
 * base falls back to evaluating it to a StructObject and reading its bytes.
 */
template <class T>
static bool member_pod_array_scalar(const Subscript *sub, EvalContext *ctx,
                                    const UniqueId *memUid, Loc s, Loc e,
                                    T &out)
{
    if (!no_side_effects(sub->what.get()))
        return false;

    const EvalValue av = sub->what->eval(ctx);
    const EvalValue &arrv = av.is<LValue *>() ? av.get<LValue *>()->get() : av;
    if (!arrv.is<SharedArrayObj>())
        return false;

    const SharedArrayObj &arr = arrv.get_ref<SharedArrayObj>();
    if (arr.skind() != SharedArrayObj::Storage::structs)
        return false;

    const auto &sv = arr.flat_structs();
    const FieldDef *f = sv.def->field_of(memUid);
    if (!f || f->offset < 0)
        return false;

    int_type idx = sub->index->eval_int(ctx);
    if (idx < 0)
        idx += arr.size();
    if (idx < 0 || static_cast<size_t>(idx) >= arr.size())
        throw OutOfBoundsEx(s, e);

    const char *p =
        sv.buf.data() + (arr.offset() + idx) * sv.stride + f->offset;

    switch (f->kind) {
        case FieldKind::f_int: {
            int_type v;
            std::memcpy(&v, p, sizeof v);
            out = static_cast<T>(v);
            return true;
        }
        case FieldKind::f_float: {
            float_type v;
            std::memcpy(&v, p, sizeof v);
            out = static_cast<T>(v);
            return true;
        }
        case FieldKind::f_bool:
            out = static_cast<T>(static_cast<unsigned char>(*p) != 0 ? 1 : 0);
            return true;
        default:
            return false;
    }
}

/*
 * The stored value of a PRESENT dict key, or nullptr if absent. Returns a
 * pointer into the live dict (the caller's `base` keeps it alive). The typed
 * fast paths (Subscript/MemberExpr eval_int/eval_float) use this for the common
 * present-key case so a typed `d.key` / `d[k]` reads the value WITHOUT
 * re-evaluating the base (the old code fell through to Construct::eval_int,
 * which re-ran do_eval and re-fetched the dict). A missing key falls back to
 * do_eval, preserving the exact default-dict vivify / key-freeze / KeyNotFound
 * behavior unchanged.
 */
static const EvalValue *
dict_present_value(const intrusive_ptr<DictObject> &obj, const EvalValue &key)
{
    const DictObject::inner_type &data = obj->get_ref();
    const auto it = data.find(key);
    return it != data.end() ? &it->second.get() : nullptr;
}

int_type MemberExpr::eval_int(EvalContext *ctx) const
{
    if (auto *sub = dynamic_cast<const Subscript *>(what.get())) {
        int_type v;
        if (member_pod_array_scalar(sub, ctx, memUid, start, end, v))
            return v;
    }

    const EvalValue base = RValue(what->eval(ctx));
    if (base.is<intrusive_ptr<StructObject>>()) {
        const StructObject &o = *base.get<intrusive_ptr<StructObject>>().get();
        const int slot = o.def->slot_of(memUid);
        if (slot >= 0 && o.is_pod()) {
            const FieldDef &f = o.def->fields[slot];
            const char *p = o.bytes.data() + f.offset;
            if (f.kind == FieldKind::f_int) {
                int_type v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            if (f.kind == FieldKind::f_bool)
                return static_cast<unsigned char>(*p) != 0 ? 1 : 0;
        }
    }
    if (base.is<intrusive_ptr<DictObject>>()) {
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), memId)) {
            if (v->is<bool>())
                return v->get<bool>() ? 1 : 0;
            return v->get<int_type>();
        }
    }
    return Construct::eval_int(ctx);   /* missing key / non-dict: do_eval */
}

float_type MemberExpr::eval_float(EvalContext *ctx) const
{
    if (auto *sub = dynamic_cast<const Subscript *>(what.get())) {
        float_type v;
        if (member_pod_array_scalar(sub, ctx, memUid, start, end, v))
            return v;
    }

    const EvalValue base = RValue(what->eval(ctx));
    if (base.is<intrusive_ptr<StructObject>>()) {
        const StructObject &o = *base.get<intrusive_ptr<StructObject>>().get();
        const int slot = o.def->slot_of(memUid);
        if (slot >= 0 && o.is_pod()) {
            const FieldDef &f = o.def->fields[slot];
            const char *p = o.bytes.data() + f.offset;
            if (f.kind == FieldKind::f_float) {
                float_type v;
                std::memcpy(&v, p, sizeof v);
                return v;
            }
            if (f.kind == FieldKind::f_int) {
                int_type v;
                std::memcpy(&v, p, sizeof v);
                return static_cast<float_type>(v);
            }
            if (f.kind == FieldKind::f_bool)
                return static_cast<unsigned char>(*p) != 0 ? 1.0 : 0.0;
        }
    }
    if (base.is<intrusive_ptr<DictObject>>()) {
        if (const EvalValue *v = dict_present_value(
                base.get_ref<intrusive_ptr<DictObject>>(), memId)) {
            if (v->is<int_type>())
                return static_cast<float_type>(v->get<int_type>());
            if (v->is<bool>())
                return v->get<bool>() ? 1.0 : 0.0;
            return v->get<float_type>();
        }
    }
    return Construct::eval_float(ctx);   /* missing key / non-dict: do_eval */
}

EvalValue ForStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext loop_ctx(ctx, ctx->const_ctx);

    if (init)
        init->eval(&loop_ctx);

    FlowState &fs = *loop_ctx.flow;

    while (true) {

        if (cond && !eval_cond(cond.get(), &loop_ctx))
            break;

        if (body)
            body->eval(&loop_ctx);

        if (fs.type == FlowState::ret)
            break;                              /* propagate to the function */

        if (fs.type == FlowState::brk) {
            fs.type = FlowState::none;
            break;
        }

        if (fs.type == FlowState::cont)
            fs.type = FlowState::none;          /* consume; still run `inc` */

        if (inc)
            inc->eval(&loop_ctx);
    }

    return none;
}

/*
 * Specialized counted loop (see syntax.h). After `init` declares the int slot,
 * `bound` and `step` are evaluated ONCE; the per-iteration condition test and
 * increment are then plain C on the slot's int_type - no expression eval, no
 * num_bin_op, no TypedScalarExpr dispatch. The slot ref is re-fetched each
 * iteration so a body that reassigns `i` is still respected.
 */
EvalValue ForRangeStmt::do_eval(EvalContext *ctx, bool rec) const
{
    EvalContext loop_ctx(ctx, ctx->const_ctx);

    init->eval(&loop_ctx);                 /* declares i in frame slot i_slot */

    Frame *f = loop_ctx.frame;
    ML_CHECK(f);              /* i is a resolved slot -> the frame must exist */

    const int_type bound_val = RValue(bound->eval(&loop_ctx)).get<int_type>();
    const int_type step_val =
        step ? RValue(step->eval(&loop_ctx)).get<int_type>()
             : static_cast<int_type>(1);
    /* lt/le ascend (+step), ge/gt descend (-step). */
    const bool asc = (cmp_op == Op::lt || cmp_op == Op::le);
    const int_type delta = asc ? step_val : -step_val;

    FlowState &fs = *loop_ctx.flow;

    while (true) {

        const int_type iv = f->slots[i_slot].getval<int_type>();
        bool go;
        switch (cmp_op) {
            case Op::lt: go = iv <  bound_val; break;
            case Op::le: go = iv <= bound_val; break;
            case Op::ge: go = iv >= bound_val; break;
            default:     go = iv >  bound_val; break;   /* Op::gt */
        }
        if (!go)
            break;

        if (body)
            body->eval(&loop_ctx);

        if (fs.type == FlowState::ret)
            break;                          /* propagate to the function */
        if (fs.type == FlowState::brk) {
            fs.type = FlowState::none;
            break;
        }
        if (fs.type == FlowState::cont)
            fs.type = FlowState::none;      /* consume; still run the step */

        f->slots[i_slot].getval<int_type>() += delta;
    }

    return none;
}
