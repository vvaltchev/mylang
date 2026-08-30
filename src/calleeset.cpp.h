/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * ⛔ THIS IS *NOT* A HEADER FILE. It is a C++ file in the form of a
 * header, `#include`d exactly ONCE into inferencer.cpp - the same
 * convention src/types/ and src/builtins/ use, and for the same reason:
 * it needs `TypeSym`, `FuncInfo` and the `Inferencer` private tables,
 * which live in inferencer.cpp's anonymous namespace. It will not
 * compile standalone and there is nothing to add to the Makefile.
 *
 * ============================================================
 * THE CALLEE-SET ANALYSIS - one answer to "which function is this?"
 * (#116 increment 1; the design record is plans/callee-set-analysis.md)
 * ============================================================
 *
 * There are two different questions about a callable value, and MyLang
 * had been answering them with one mechanism:
 *
 *  - SHAPE   - how do I emit the call, the window, the return? Asked by
 *              the JIT, answered by the `Func` StaticType. N functions
 *              assignable to one variable share ONE shape, and one
 *              answer is exactly what the emitter needs.
 *  - IDENTITY - is it always the SAME function? Asked by
 *              devirtualization, by value-template instantiation, and
 *              by #115 (typing a closure's parameters from its call
 *              sites).
 *
 * `StaticType::finfos` asks the IDENTITY question off the SHAPE object,
 * and that is a category error: two distinct functions routinely have
 * equal types, so every shape-level equivalence is a place identity
 * dies. `join`'s `if (static_type_equal(a,b)) return a;` was the first
 * one found; there is no argument that it was the last, because the
 * whole class is "operations that treat equal types as interchangeable"
 * - which is what a type system is FOR.
 *
 * This analysis answers IDENTITY on its own terms, keyed by PROGRAM
 * LOCATION rather than by type.
 *
 * ------------------------------------------------------------
 * THE DEEPER FLAW IT FIXES: there was no ⊤.
 * ------------------------------------------------------------
 *
 * `finfos` cannot distinguish "nothing flows here" from "I have no
 * idea" - both are the empty set. That is why `escaped_finfos` /
 * `FuncInfo::value_escaped` exist at all (a side-channel ledger
 * carrying the "unknown" the set itself cannot express), why that
 * ledger is structurally insufficient (it records members a join
 * DROPPED and says nothing about members that never ENTERED), and why
 * `finfos.size() == 1` was never a proof.
 *
 * Here ⊥ (the empty set) means "no function value can reach this
 * location" and ⊤ means "I do not know", and they are different values.
 * Every decline is then a decline for a stated reason.
 *
 * ------------------------------------------------------------
 * THE SHAPE OF THE ANALYSIS
 * ------------------------------------------------------------
 *
 * A monotone least fixpoint over FUNCTION VALUES ONLY - inclusion-based
 * (Andersen-style), flow-insensitive, with one abstract heap object per
 * container ALLOCATION SITE.
 *
 * Abstract values:   Func(FuncInfo *)   a function
 *                    Obj(int)           a container allocation site
 * Locations:         Sym(TypeSym *)     a variable / parameter / capture
 *                    Ret(FuncInfo *)    a function's return value
 *                    Elem(obj)          everything inside one container
 *
 * ⛔ WHY ALLOCATION SITES AND NOT A SINGLE "HEAP" LOCATION. One shared
 * heap location is trivially sound and was the first design; it is
 * ALSO useless, because it merges every container in the program. In
 * tests/functional/25_factory_closure_param.my that alone would drop
 * case (6) - a closure reached through `fs[0]` where `fs` holds exactly
 * one - from a MUST answer to a 5-way decline. Allocation sites cost a
 * few dozen lines and keep the answers that #115 already relies on.
 *
 * ⛔ AND WHY NOT PER-SYMBOL ELEMENT LOCATIONS (which look simpler).
 * Because two symbols can name ONE container without any copy the
 * analysis models - `f(p)` binds it to a parameter, `[p]` nests it, a
 * `dyn` alias re-reads it. A per-symbol element location is only sound
 * with an alias analysis on top; letting the OBJECT be the location
 * makes aliasing fall out of the ordinary `q = p` copy rule, because
 * both symbols then point at the same Obj.
 *
 * ------------------------------------------------------------
 * ⛔ SOUNDNESS: THE FAIL-CLOSED DIRECTION IS ⊤, AND IT IS NOT OPTIONAL
 * ------------------------------------------------------------
 *
 * A MISSING member is a wrong MUST answer. If `callee_set(e)` says
 * {F} where the truth is {F, G}, then #115 types F's parameters from
 * arguments G received - and every unboxed tier downstream is built on
 * that proof (see CLAUDE.md, "static types imply runtime guards"). So
 * an extra member costs an optimization; a missing one is a RULE 1
 * violation.
 *
 * Therefore EVERY path that cannot name what flows returns ⊤:
 *  - a builtin's result;
 *  - a value read out of a baked const pool (`const OPS = [sq];` folds
 *    to ONE LiteralObj at parse time - the FuncObject is in the VALUE,
 *    not in any tree the walker can see. See const_pool_func.my);
 *  - a struct field, a dict/array read through a ⊤ base;
 *  - an unknown AST node shape - the `esc_known_shape` rule verbatim:
 *    an unrecognised kind HIDES occurrences, and a hidden occurrence is
 *    how a wrong "safe" happens;
 *  - the REPL's open world (a global can be redefined between inputs).
 *
 * The one cheap precision rule that keeps this from swallowing the
 * program: an expression whose settled static type CANNOT hold a
 * function (`cs_type_can_hold_func`) is ⊥ rather than ⊤. That is a
 * TYPE fact, and it is the one place asking the type is right - we are
 * asking whether a function can be there at all, not which one.
 *
 * ------------------------------------------------------------
 * ITERATION ORDER DOES NOT MATTER
 * ------------------------------------------------------------
 *
 * Every constraint is `dst ⊇ src` and ⊤ is absorbing, so the transfer
 * functions are monotone and the least fixpoint is unique regardless of
 * visit order. Unlike the type fixpoint - which is deliberately Jacobi
 * (read the previous round's stable values) because a `join` CONFLICT
 * is order-sensitive - this one reads the current values (chaotic
 * iteration) and simply converges faster.
 */

/* ---------------------------------------------------------------- */

/* The universal container: everything we could not name is in here,
 * and its elements are permanently ⊤. Obj id 0 by construction. */
static const int CS_OBJ_UNKNOWN = 0;

/* Past this many members a set collapses to ⊤, so a pathological
 * program cannot make the sets large. Every real answer we consume is
 * of size 1, so the cap costs nothing a consumer wanted. */
static const size_t CS_MAX_SET = 16;

/* Rounds before we give up and answer ⊤ everywhere. The fixpoint
 * converges in 2-4 on the corpus; the bound exists so a construct
 * nobody anticipated cannot hang the compiler. */
static const int CS_MAX_ROUNDS = 64;

bool CsSet::add_func(FuncInfo *f)
{
    if (top)
        return false;
    if (std::find(funcs.begin(), funcs.end(), f) != funcs.end())
        return false;
    funcs.push_back(f);
    if (funcs.size() + objs.size() > CS_MAX_SET)
        return set_top();
    return true;
}

bool CsSet::add_obj(int o)
{
    if (top)
        return false;
    if (std::find(objs.begin(), objs.end(), o) != objs.end())
        return false;
    objs.push_back(o);
    if (funcs.size() + objs.size() > CS_MAX_SET)
        return set_top();
    return true;
}

bool CsSet::set_top()
{
    if (top)
        return false;
    top = true;
    funcs.clear();
    objs.clear();
    return true;
}

bool CsSet::merge(const CsSet &o)
{
    if (top)
        return false;
    if (o.top)
        return set_top();
    bool ch = false;
    for (FuncInfo *f : o.funcs)
        ch |= add_func(f);
    for (int x : o.objs)
        ch |= add_obj(x);
    return ch;
}

/* ---------------------------------------------------------------- */

/*
 * Can a value of this static type hold a function, directly or inside
 * a container/struct? A `false` here is the ONLY licence to answer ⊥
 * for something we did not analyse, so it must be conservative in the
 * other direction: Unknown and Dyn both say YES.
 *
 * ⛔ IT MUST BE CYCLE-SAFE. A struct may reference its own type
 * through an `opt` field (that is how a linked list is written - see
 * check_struct_no_recursion), and a Dict<K,V> can nest arbitrarily, so
 * a naive recursion is an infinite one. `seen` is the guard.
 */
bool Inferencer::cs_type_can_hold_func(StaticTypeRef t,
                                       std::set<const void *> &seen)
{
    t = static_type_resolve(t);
    if (!t)
        return true;                     /* no information -> assume yes */

    switch (t->kind) {
    case StaticTypeKind::Func:
    case StaticTypeKind::Dyn:
    case StaticTypeKind::Unknown:
        return true;

    /*
     * ⛔ `None` IS "NOT PINNED YET", NOT "EMPTY" - the inferencer's own
     * defer-on-Unknown/None invariant, and answering `false` here was a
     * real bug the -dcs dump caught on the corpus:
     *
     *     var fns = [];                       # the LITERAL is array<none>
     *     for (...) append(fns, mk(i));       # ...though `fns` is array<func>
     *     foreach (var fn in fns) tot += fn(1);
     *
     * The literal's own type has no element type yet, so the gate
     * pruned it, no abstract object was ever allocated for it, and the
     * call answered ⊥ - "no function value can reach here" - for a site
     * that reaches five closures. ⊥ is a MUST answer, so that is the
     * unsound direction. Treating None as "assume yes" costs precision
     * only on a genuinely empty container.
     */
    case StaticTypeKind::None:
        return true;

    case StaticTypeKind::Array:
        return cs_type_can_hold_func(t->elem, seen);
    case StaticTypeKind::Dict:
        return cs_type_can_hold_func(t->key, seen) ||
               cs_type_can_hold_func(t->val, seen);

    case StaticTypeKind::Struct:
        return cs_struct_can_hold_func(
            static_cast<const StructTypeDef *>(t->struct_def), seen);

    /* Bool/Int/Float/Str/Exception cannot carry one. */
    case StaticTypeKind::Bool:
    case StaticTypeKind::Int:
    case StaticTypeKind::Float:
    case StaticTypeKind::Str:
    case StaticTypeKind::Exception:
        return false;
    }
    return true;                          /* unreachable; fail closed */
}

/*
 * ⛔ RECURSES ON THE StructTypeDef, NEVER THROUGH THE ARENA. A first
 * version built an StaticType per nested field with
 * `A.struct_ty(def, nullptr)`, which ALLOCATES a fresh node every
 * call - so an analysis that is supposed to be observationally INERT
 * grew the inferencer's shared type arena with throwaway nodes on
 * every program. Nothing reads them, but "inert" should mean inert.
 *
 * ⛔ AND IT MUST BE CYCLE-SAFE: a struct may reference its own type
 * through an `opt` field (that is how a linked list is written - see
 * check_struct_no_recursion), so a naive recursion is an infinite
 * one. `seen` is the guard.
 */
bool Inferencer::cs_struct_can_hold_func(const StructTypeDef *def,
                                         std::set<const void *> &seen)
{
    if (!def)
        return true;                      /* unresolved: assume yes */
    if (!seen.insert(def).second)
        return false;                     /* already being examined */

    for (const FieldDef &f : def->fields) {
        switch (f.kind) {
        case FieldKind::f_dyn:
            return true;                  /* may hold anything */
        case FieldKind::f_array:
        case FieldKind::f_dict:
            /* a GENERIC `array`/`dict` field carries no element type,
             * so it may hold one */
            if (!f.annot || cs_annot_can_hold_func(f.annot.get(), seen))
                return true;
            break;
        case FieldKind::f_struct:
            if (cs_struct_can_hold_func(f.struct_def, seen))
                return true;
            break;
        case FieldKind::f_bool:
        case FieldKind::f_int:
        case FieldKind::f_float:
        case FieldKind::f_str:
            break;
        }
    }

    /* a folded `const` MEMBER is a value on the DEF, reachable with no
     * instance at all - `struct Ops { const F = sq; }` */
    for (const auto &kv : def->consts)
        if (kv.second.get_type()->t == Type::t_func ||
            kv.second.get_type()->t == Type::t_arr ||
            kv.second.get_type()->t == Type::t_dict ||
            kv.second.get_type()->t == Type::t_struct)
            return true;

    return false;
}

/* The TypeAnnot twin (a struct FIELD's declared type is an annotation,
 * not an StaticType, until the inferencer resolves it). */
bool Inferencer::cs_annot_can_hold_func(const TypeAnnot *a,
                                        std::set<const void *> &seen)
{
    if (!a)
        return true;
    switch (a->kind) {
    case DeclType::none:
    case DeclType::dyn:
        return true;
    case DeclType::arr:
        return !a->elem || cs_annot_can_hold_func(a->elem.get(), seen);
    case DeclType::dict:
        return (!a->key || cs_annot_can_hold_func(a->key.get(), seen)) ||
               (!a->val || cs_annot_can_hold_func(a->val.get(), seen));
    case DeclType::strct:
        return cs_struct_can_hold_func(a->strct, seen);
    case DeclType::b:
    case DeclType::i:
    case DeclType::f:
    case DeclType::s:
        return false;                  /* a scalar annotation */
    }
    return true;                       /* unreachable; fail closed */
}

bool Inferencer::cs_can_hold_func(const Construct *e)
{
    if (!e)
        return false;
    std::set<const void *> seen;
    return cs_type_can_hold_func(type_of(e), seen);
}

/* ---------------------------------------------------------------- */

int Inferencer::cs_loc(CsLocKind k, const void *p, int obj)
{
    CsLocKey key { k, p, obj };
    auto it = cs_loc_ids.find(key);
    if (it != cs_loc_ids.end())
        return it->second;
    const int id = static_cast<int>(cs_pts.size());
    cs_loc_ids[key] = id;
    cs_pts.emplace_back();
    return id;
}

/* One abstract object per ALLOCATION SITE - the node that builds the
 * container. Two evaluations of the same literal (a loop body) share
 * an object, which is the flow-insensitive answer and is sound. */
int Inferencer::cs_obj_for(const Construct *site)
{
    auto it = cs_obj_ids.find(site);
    if (it != cs_obj_ids.end())
        return it->second;
    const int id = static_cast<int>(cs_obj_ids.size()) + 1;  /* 0 = UNKNOWN */
    cs_obj_ids[site] = id;
    return id;
}

bool Inferencer::cs_write(int loc, const CsSet &v)
{
    const bool ch = cs_pts[loc].merge(v);
    cs_changed |= ch;
    return ch;
}

/* Read "everything inside" a set of abstract values: the union of each
 * object's element location. A ⊤ base reads ⊤ - it may be any
 * container, including ones we never saw. */
CsSet Inferencer::cs_read_elems(const CsSet &base)
{
    CsSet out;
    if (base.top) {
        out.set_top();
        return out;
    }
    for (int o : base.objs) {
        if (o == CS_OBJ_UNKNOWN) {
            out.set_top();
            return out;
        }
        out.merge(cs_pts[cs_loc(CsLocKind::elem, nullptr, o)]);
    }
    return out;
}

/* Write into "everything inside" a base - a WEAK update (merge, never
 * replace), which is what flow-insensitivity requires: the old
 * contents may still be there on some path. */
void Inferencer::cs_write_elems(const CsSet &base, const CsSet &v)
{
    if (base.top) {
        /* We do not know which container was written, so the value can
         * later come out of any of them. Marking the UNKNOWN object's
         * elements ⊤ is not enough - a subsequent read of a KNOWN
         * object would miss it - so every function in `v` escapes. */
        cs_escape_set(v);
        return;
    }
    for (int o : base.objs) {
        if (o == CS_OBJ_UNKNOWN) {
            cs_escape_set(v);
            continue;
        }
        cs_write(cs_loc(CsLocKind::elem, nullptr, o), v);
    }
}

/*
 * A function value reached somewhere this pass cannot follow. Recorded
 * per FuncInfo, because the question a consumer asks is "are ALL calls
 * to F sites I attributed?" - and once F's value is out of view, the
 * answer is no, whatever any individual call site's set says.
 */
void Inferencer::cs_escape_set(const CsSet &v)
{
    if (v.top) {
        cs_escaped_all = true;           /* an unnameable value escaped */
        return;
    }
    for (FuncInfo *f : v.funcs)
        if (cs_escaped.insert(f).second)
            cs_changed = true;
    /* A container that escapes takes its contents with it. */
    for (int o : v.objs) {
        if (o == CS_OBJ_UNKNOWN) {
            cs_escaped_all = true;
            continue;
        }
        const int el = cs_loc(CsLocKind::elem, nullptr, o);
        if (cs_escaping_objs.insert(o).second)
            cs_changed = true;
        cs_escape_flat(cs_pts[el]);
    }
}

/* The non-recursive half of cs_escape_set: escape the functions in a
 * set without re-descending into objects (cs_escaping_objs is drained
 * each round, so nesting is covered by the fixpoint rather than by
 * recursion - a container may contain itself). */
void Inferencer::cs_escape_flat(const CsSet &v)
{
    if (v.top) {
        cs_escaped_all = true;
        return;
    }
    for (FuncInfo *f : v.funcs)
        if (cs_escaped.insert(f).second)
            cs_changed = true;
    for (int o : v.objs) {
        if (o == CS_OBJ_UNKNOWN)
            cs_escaped_all = true;
        else if (cs_escaping_objs.insert(o).second)
            cs_changed = true;
    }
}

/* ---------------------------------------------------------------- */
/*                      the expression evaluator                     */
/* ---------------------------------------------------------------- */

CsSet Inferencer::cs_eval(Construct *e)
{
    CsSet out;
    if (!e)
        return out;                       /* ⊥ */

    /*
     * THE CHEAP PRECISION RULE, and the only place a TYPE decides
     * anything here: if no function can be in this expression at all,
     * the answer is ⊥ and no shape analysis is needed. Everything
     * below may therefore assume a function is possible, which is why
     * the unknown-shape default at the bottom can be ⊤ without
     * swallowing the program.
     */
    if (!cs_can_hold_func(e))
        return out;                       /* ⊥ */

    /*
     * ⛔ NO `default:` - EXHAUSTIVE BY -Werror=switch, the same
     * discipline `for_each_child` and `verify_chunk` use, and for the
     * strongest form of the reason. This switch decides, for every
     * node kind in the language, whether a FUNCTION VALUE can come out
     * of it. A kind that silently fell through to "no" would HIDE
     * occurrences, and a hidden occurrence is exactly how a wrong MUST
     * answer happens (the `esc_known_shape` rule). A new node kind
     * fails the BUILD here until someone decides.
     */
    switch (e->ct) {

    /* ---- kinds that can PRODUCE a function value ---- */

    case ConstructType::id: {
        auto *id = static_cast<Identifier *>(e);
        auto it = id_sym.find(id);
        if (it == id_sym.end() || !it->second) {
            out.set_top();                /* a builtin, or unresolved */
            return out;
        }
        TypeSym *s = it->second;
        if (s->func) {
            out.add_func(s->func);        /* the name of a function */
            return out;
        }
        if (s->struct_type)
            return out;                   /* a struct descriptor: ⊥ */
        /*
         * THE REPL'S OPEN WORLD. A committed global may be redefined
         * by a later input, so what a name holds is not decided by the
         * text this pass can see.
         */
        if (s->pinned) {
            out.set_top();
            return out;
        }
        out = cs_pts[cs_loc(CsLocKind::sym, s, 0)];
        return out;
    }

    case ConstructType::func_decl: {
        /* a lambda literal in expression position */
        auto *fd = static_cast<FuncDeclStmt *>(e);
        auto it = func_of_decl.find(fd);
        if (it != func_of_decl.end() && it->second)
            out.add_func(it->second);
        else
            out.set_top();
        return out;
    }

    case ConstructType::lit_arr:
    case ConstructType::lit_dict:
        /* an ALLOCATION SITE - its own abstract object. cs_visit fills
         * the object's element location from the literal's elements. */
        out.add_obj(cs_obj_for(e));
        return out;

    case ConstructType::lit_obj:
        /*
         * ⛔ A BAKED CONST VALUE IS NOT IN THE TREE, SO IT IS WALKED
         * AS A VALUE. `const OPS = [sq];` is folded by the PARSER into
         * ONE LiteralObj carrying the whole array, FuncObject and all,
         * so no Identifier names `sq` any anymore and no walk over the
         * tree can find it - the shape that let a dead base template
         * be dropped while a const pool still held it
         * (const_pool_func.my).
         *
         * A blanket ⊤ here is sound but was far too coarse, and the
         * -dcs dump proved it on the corpus: `var fns = [];` is a
         * CONST expression too, so the everyday empty-array
         * initialiser folds to a LiteralObj - and ⊤ there makes the
         * container it starts untraceable for the rest of the program.
         * cs_eval_value chases the FuncObjects out instead, and falls
         * back to ⊤ only for what it cannot name.
         */
        cs_eval_value(static_cast<LiteralObj *>(e)->literal_value(), e, out);
        return out;

    case ConstructType::call: {
        auto *c = static_cast<CallExpr *>(e);
        if (FuncInfo *direct = callee_funcinfo(c->what.get())) {
            if (direct->is_template) {
                /* An un-instantiated template's return type is a
                 * fallback, not inference; its clones carry the real
                 * one and the call is redirected to them. */
                out.set_top();
                return out;
            }
            out = cs_pts[cs_loc(CsLocKind::ret, direct, 0)];
            return out;
        }
        if (cs_struct_callee(c->what.get())) {
            /* a fresh struct instance: one object per construction
             * site, exactly like a container literal - its FIELDS are
             * that object's elements */
            out.add_obj(cs_obj_for(e));
            return out;
        }
        if (cs_callee_is_builtin(c->what.get())) {
            /*
             * EVERY builtin is opaque here - see cs_call for why this
             * analysis refuses to inherit `esc_builtin_*`'s allowlist
             * and its known map/filter/sort hole.
             *
             * ⛔ AND THIS BRANCH IS A PURE SHORT-CIRCUIT, PROVEN
             * REDUNDANT. An unshadowed builtin name has no TypeSym, so
             * the `id` case above already answers ⊤ and the generic
             * path below would reach the same ⊤ - deleting it fails no
             * test, which is exactly what the sabotage run found. It
             * stays for legibility, with the redundancy ASSERTED, so
             * the day a builtin acquires a symbol this fires instead
             * of the branch quietly becoming load-bearing. (Same
             * treatment as #93's reassignment guard.)
             */
            ML_CHECK(cs_eval(c->what.get()).top);
            out.set_top();
            return out;
        }
        const CsSet cal = cs_eval(c->what.get());
        if (cal.top) {
            out.set_top();
            return out;
        }
        for (FuncInfo *f : cal.funcs) {
            if (f->is_template) {
                out.set_top();
                return out;
            }
            out.merge(cs_pts[cs_loc(CsLocKind::ret, f, 0)]);
        }
        return out;
    }

    case ConstructType::subscript:
        return cs_read_elems(cs_eval(static_cast<Subscript *>(e)->what.get()));

    case ConstructType::member:
        return cs_read_elems(cs_eval(static_cast<MemberExpr *>(e)->what.get()));

    case ConstructType::slice:
        /* A slice is a VIEW: it denotes the parent's storage, so it
         * holds the same objects. */
        return cs_eval(static_cast<Slice *>(e)->what.get());

    case ConstructType::inlined_call:
        /* a spliced call body. The inliner runs AFTER inference, so
         * this reaches us only from a retained REPL body; its value is
         * the body's return, which this walk does not model. */
        out.set_top();
        return out;

    case ConstructType::other:
        /* the TRANSITIONAL sentinel - no shipped class carries it, and
         * the Construct ctor ML_CHECKs that. Fail closed anyway. */
        out.set_top();
        return out;

    /* ---- kinds that only PASS THROUGH what their children hold ----
     *
     * ⛔ THE CLAIM EACH OF THESE MAKES, and it is a LANGUAGE fact, not
     * a type fact: no MyLang operator manufactures a function value.
     * `+` on two arrays yields a container holding the operands'
     * elements, which is precisely the union of their objects;
     * everything else here yields a scalar. So the union of the
     * children is a superset of what can come out, which is the sound
     * direction. Anything that could CREATE a function belongs above.
     */
    case ConstructType::expr01:
    case ConstructType::expr02:
    case ConstructType::expr03:
    case ConstructType::expr04:
    case ConstructType::expr05:
    case ConstructType::expr06:
    case ConstructType::expr07:
    case ConstructType::expr08:
    case ConstructType::expr09:
    case ConstructType::expr10:
    case ConstructType::expr11:
    case ConstructType::expr12:
    case ConstructType::ternary:
    case ConstructType::coalesce:
    case ConstructType::expr_list:
    case ConstructType::lit_dict_kv:
    case ConstructType::typed_scalar:
    case ConstructType::incdec:
        for_each_child(e, [&](Construct *c) { out.merge(cs_eval(c)); });
        return out;

    case ConstructType::expr14:
        /* an assignment used as a value: the value is the rvalue's */
        return cs_eval(static_cast<Expr14 *>(e)->rvalue.get());

    /* ---- kinds that are not expressions, or are scalar literals ----
     * ⊥: a function value cannot come out of any of them. */
    case ConstructType::nop:
    case ConstructType::brk:
    case ConstructType::cont:
    case ConstructType::rethrow:
    case ConstructType::lit_int:
    case ConstructType::lit_bool:
    case ConstructType::lit_float:
    case ConstructType::lit_none:
    case ConstructType::lit_str:
    case ConstructType::idlist:
    case ConstructType::block:
    case ConstructType::if_stmt:
    case ConstructType::ret:
    case ConstructType::while_stmt:
    case ConstructType::struct_decl:
    case ConstructType::try_catch:
    case ConstructType::foreach_stmt:
    case ConstructType::throw_stmt:
    case ConstructType::for_stmt:
    case ConstructType::for_range:
        return out;
    }

    return out;                           /* unreachable */
}

/*
 * The points-to set of a BAKED VALUE - the LiteralObj case above.
 *
 * A container becomes an abstract object anchored at `site` (the
 * LiteralObj node), exactly like a written literal, and its elements
 * are walked into that object's element location. A FuncObject is
 * resolved back to its FuncInfo through the descriptor's `decl`
 * back-pointer, which is still live here (inference runs long before
 * the `-vm` teardown nulls it). Anything else - a value kind this
 * function does not know - is ⊤.
 */
void Inferencer::cs_eval_value(const EvalValue &v, const Construct *site,
                               CsSet &out)
{
    Type *t = v.get_type();
    switch (t->t) {

    /* a scalar cannot carry a function */
    case Type::t_bool:
    case Type::t_int:
    case Type::t_float:
    case Type::t_str:
    case Type::t_none:
        return;

    case Type::t_func: {
        const intrusive_ptr<FuncObject> &fo =
            v.get<intrusive_ptr<FuncObject>>();
        const FuncDescriptor *d = fo ? fo->func : nullptr;
        const Construct *decl = d ? d->decl : nullptr;
        auto it = decl ? func_of_decl.find(decl) : func_of_decl.end();
        if (it != func_of_decl.end() && it->second)
            out.add_func(it->second);
        else
            out.set_top();     /* a function this pass never declared */
        return;
    }

    case Type::t_arr: {
        const SharedArrayObj &arr = v.get<SharedArrayObj>();
        const int o = cs_obj_for(site);
        out.add_obj(o);
        /*
         * ⛔ FLAT STORAGE IS READ FROM THE KIND, NEVER THROUGH
         * get_view() - which PROMOTES a flat const array to general
         * (static_type_from_value carries the same warning). A flat
         * array is int/float/bool/str/struct-typed and so cannot hold
         * a function anyway, which is why skipping it is also right.
         */
        if (arr.skind() != SharedArrayObj::Storage::general)
            return;
        ArrayConstView view = arr.get_view();
        CsSet elems;
        for (size_type i = 0; i < view.size(); i++)
            cs_eval_value(view[i].get(), site, elems);
        cs_write(cs_loc(CsLocKind::elem, nullptr, o), elems);
        return;
    }

    case Type::t_dict: {
        const auto &m = v.get<intrusive_ptr<DictObject>>()->get_ref();
        const int o = cs_obj_for(site);
        out.add_obj(o);
        CsSet elems;
        for (const auto &kv : m) {
            cs_eval_value(kv.first, site, elems);
            cs_eval_value(kv.second.get(), site, elems);
        }
        cs_write(cs_loc(CsLocKind::elem, nullptr, o), elems);
        return;
    }

    case Type::t_struct: {
        const intrusive_ptr<StructObject> &so =
            v.get<intrusive_ptr<StructObject>>();
        const int o = cs_obj_for(site);
        out.add_obj(o);
        const StructTypeDef *def = so->def;
        if (!def)
            return;
        /* A POD instance holds only scalars by construction, so only
         * the boxed form can carry a function - and its folded `const`
         * MEMBERS can too (they are values on the DEF, reachable
         * without any instance). */
        CsSet elems;
        if (def->layout != StructTypeDef::Layout::pod)
            for (const LValue &f : so->fields)
                cs_eval_value(f.get(), site, elems);
        for (const auto &kv : def->consts)
            cs_eval_value(kv.second, site, elems);
        cs_write(cs_loc(CsLocKind::elem, nullptr, o), elems);
        return;
    }

    default:
        out.set_top();
        return;
    }
}

/* Is this callee expression a struct TYPE (so the call constructs)? */
const StructTypeDef *Inferencer::cs_struct_callee(Construct *e)
{
    if (!e || e->ct != ConstructType::id)
        return nullptr;
    auto it = id_sym.find(static_cast<Identifier *>(e));
    if (it == id_sym.end() || !it->second)
        return nullptr;
    return it->second->struct_type;
}

/* ---------------------------------------------------------------- */
/*                      the constraint walker                        */
/* ---------------------------------------------------------------- */

/*
 * Apply the STORE constraints of one node and recurse. Reads are done
 * by cs_eval on demand; this half is only about where a value LANDS.
 *
 * `fn` is the enclosing FuncInfo (null at the top level) - a `return`
 * needs it and there is no other way to know it.
 */
void Inferencer::cs_visit(Construct *n, FuncInfo *fn)
{
    if (!n)
        return;

    switch (n->ct) {

    case ConstructType::expr14: {
        auto *e = static_cast<Expr14 *>(n);
        cs_assign(e->lvalue.get(), e->rvalue.get());
        break;
    }

    case ConstructType::ret: {
        auto *r = static_cast<ReturnStmt *>(n);
        if (fn && r->elem)
            cs_write(cs_loc(CsLocKind::ret, fn, 0), cs_eval(r->elem.get()));
        break;
    }

    case ConstructType::call:
        cs_call(static_cast<CallExpr *>(n));
        break;

    case ConstructType::lit_arr: {
        /* the ALLOCATION's initial contents. Without this the object
         * exists and is empty, so `var p = [f]; p[0]()` reads ⊥ -
         * "nothing can reach here" for a call that plainly reaches
         * something. Found by the -dcs dump on its first run. */
        CsSet obj;
        obj.add_obj(cs_obj_for(n));
        for (auto &up : static_cast<MultiElemConstruct<> *>(n)->elems)
            cs_write_elems(obj, cs_eval(up.get()));
        break;
    }

    case ConstructType::lit_dict: {
        /* a dict's VALUES *and* its keys - a key can be a function
         * too, since hash() is total. Both land in the one element
         * location: this analysis is key/value-insensitive, which
         * costs precision on a dict keyed by functions and nothing
         * else.
         *
         * ⛔ NOT a MultiElemConstruct<> - LiteralDict's elements are
         * LiteralDictKVPair, so the plain downcast is a different type
         * and UBSan says so (caught on the probe's first run). */
        CsSet obj;
        obj.add_obj(cs_obj_for(n));
        for (auto &up : static_cast<LiteralDict *>(n)->elems) {
            cs_write_elems(obj, cs_eval(up->key.get()));
            cs_write_elems(obj, cs_eval(up->value.get()));
        }
        break;
    }

    case ConstructType::foreach_stmt: {
        auto *f = static_cast<ForeachStmt *>(n);
        const CsSet elems = cs_read_elems(cs_eval(f->container.get()));
        if (f->ids)
            for (auto &up : f->ids->elems)
                cs_bind_target(up.get(), elems);
        break;
    }

    case ConstructType::try_catch: {
        /*
         * A catch variable binds the thrown payload, which comes from
         * a `throw` anywhere in the program (or from the runtime). We
         * do not model the throw/catch edge, so it is ⊤ - and cheaply
         * so, since cs_bind_target's own type test makes it ⊥ for the
         * overwhelmingly common non-func payload.
         */
        auto *t = static_cast<TryCatchStmt *>(n);
        CsSet unknown;
        unknown.set_top();
        for (auto &cs : t->catchStmts)
            if (cs.first.asId)
                cs_bind_target(cs.first.asId.get(), unknown);
        break;
    }

    case ConstructType::func_decl: {
        auto *fd = static_cast<FuncDeclStmt *>(n);
        auto it = func_of_decl.find(fd);
        FuncInfo *inner = it != func_of_decl.end() ? it->second : nullptr;
        /*
         * ⛔ A CAPTURE LIST IS A READ, NOT A SINK. `func [base] (x)`
         * snapshots `base` BY VALUE at closure creation, and the body
         * resolves the captured name to the SAME TypeSym as the outer
         * variable (the inferencer's scoping is lexical - captures are
         * not separate symbols here). So this analysis, being
         * flow-insensitive, already reads a superset of what the
         * snapshot can hold, and there is nothing to write.
         */
        cs_visit(fd->body.get(), inner);
        return;                          /* body walked with the new fn */
    }

    default:
        break;
    }

    for_each_child(n, [&](Construct *c) { cs_visit(c, fn); });
}

/*
 * `lv = rv`. The lvalue decides the LOCATION; the rvalue is read with
 * cs_eval. A compound assignment (`f += g`) is the same store as far
 * as reachability goes - arithmetic on functions is a type error the
 * check pass already refuses, and merging is the safe direction.
 */
void Inferencer::cs_assign(Construct *lv, Construct *rv)
{
    if (!lv)
        return;
    cs_bind_target(lv, cs_eval(rv));
}

/* Write `v` into whatever `lv` denotes. */
void Inferencer::cs_bind_target(Construct *lv, const CsSet &v)
{
    if (!lv)
        return;

    switch (lv->ct) {
    case ConstructType::id: {
        auto *id = static_cast<Identifier *>(lv);
        if (id->is_underscore())
            return;                       /* the placeholder binds nothing */
        auto it = id_sym.find(id);
        if (it == id_sym.end() || !it->second) {
            /* an unresolved / builtin target: the value left our view */
            cs_escape_set(v);
            return;
        }
        if (!cs_can_hold_func(lv))
            return;
        cs_write(cs_loc(CsLocKind::sym, it->second, 0), v);
        return;
    }

    case ConstructType::subscript:
        cs_write_elems(cs_eval(static_cast<Subscript *>(lv)->what.get()), v);
        return;

    case ConstructType::member:
        cs_write_elems(cs_eval(static_cast<MemberExpr *>(lv)->what.get()), v);
        return;

    case ConstructType::idlist: {
        /* destructuring: each target receives an ELEMENT of the rvalue */
        auto *il = static_cast<IdList *>(lv);
        const CsSet elems = cs_read_elems(v);
        for (auto &up : il->elems)
            cs_bind_target(up.get(), elems);
        return;
    }

    default:
        /* a slice / arithmetic target is not an lvalue (the parser
         * refuses it by shape - the ASSIGNABLE-SHAPE rule), so this is
         * unreachable for a well-formed tree. Fail closed anyway. */
        cs_escape_set(v);
        return;
    }
}

/*
 * A call site: bind the arguments to the callee's parameters, and
 * account for what a callee we cannot name does with them.
 */
void Inferencer::cs_call(CallExpr *c)
{
    if (!c->args)
        return;

    /* a STRUCT construction: the arguments land in the instance's
     * fields, which this analysis models as its object's elements */
    if (const StructTypeDef *def = cs_struct_callee(c->what.get())) {
        (void) def;
        CsSet inst;
        inst.add_obj(cs_obj_for(c));
        for (auto &a : c->args->elems)
            cs_write_elems(inst, cs_eval(a.get()));
        return;
    }

    FuncInfo *direct = callee_funcinfo(c->what.get());
    CsSet cal;
    if (direct)
        cal.add_func(direct);
    else
        cal = cs_eval(c->what.get());

    /*
     * ⛔ A BUILTIN IS A SINK, AND THE ENUMERATION IS THE TRAP.
     * `esc_builtin_transparent` (the escape analysis) is an ALLOWLIST
     * of builtins that neither store nor invoke their arguments, and
     * its own note records that the grep it was derived from MISSES
     * map/filter/sort - they reach their callback through a shared
     * helper not named `builtin_*`. Rather than inherit that list and
     * its known hole, this analysis treats EVERY builtin as opaque:
     * any function-carrying argument escapes, and the result is ⊤.
     * That is one enumeration fewer to keep correct, and its cost is
     * precision on `print(f)`, which nothing consumes.
     */
    if (cs_callee_is_builtin(c->what.get())) {
        for (auto &a : c->args->elems)
            cs_escape_set(cs_eval(a.get()));
        cs_builtin_container_effect(c);
        return;
    }

    if (cal.top) {
        /* a callee we cannot name may do anything with them */
        cs_top_callee_sites++;
        for (auto &a : c->args->elems)
            cs_escape_set(cs_eval(a.get()));
        return;
    }

    for (FuncInfo *f : cal.funcs) {
        if (f->is_template) {
            /* the base is a shell; its clones get the real arguments
             * through their own redirected call sites */
            for (auto &a : c->args->elems)
                cs_escape_set(cs_eval(a.get()));
            continue;
        }
        const size_t np = f->params.size();
        for (size_t i = 0; i < c->args->elems.size(); i++) {
            const CsSet av = cs_eval(c->args->elems[i].get());
            if (i < np)
                cs_write(cs_loc(CsLocKind::sym, f->params[i], 0), av);
            else
                cs_escape_set(av);   /* an arity error the check pass
                                      * reports; be safe until it does */
        }
    }
}

/*
 * WHAT A BUILTIN DOES TO A CONTAINER IT WAS HANDED.
 *
 * ⛔ TREATING A BUILTIN AS PURELY OPAQUE IS NOT CONSERVATIVE - IT IS
 * WRONG, and the -dcs dump caught it on the corpus. `append` is how an
 * array is BUILT in MyLang:
 *
 *     var fns = [];
 *     for (var i = 0; i < 5; i++) append(fns, mk(i * 100));
 *     foreach (var fn in fns) tot += fn(1);      # <- what is `fn`?
 *
 * Escaping the arguments and stopping there loses the WRITE, so `fns`
 * object stays EMPTY and the call answered ⊥ - "no function value can
 * reach here" - for a site that reaches five closures. ⊥ is a MUST
 * answer; that is the unsound direction, not a precision loss.
 *
 * So the rule is inverted from `esc_builtin_transparent`'s: a builtin
 * handed a container makes that container's contents UNKNOWN, and only
 * a builtin whose write we model EXACTLY is exempt. A missing entry
 * below therefore costs precision (the container goes ⊤) and can never
 * cost soundness - which is the whole difference from the escape
 * analysis's allowlist, where a missing entry meant "transparent" and
 * a wrong entry was a use-after-free.
 */
void Inferencer::cs_builtin_container_effect(CallExpr *c)
{
    Construct *callee = c->what.get();
    const std::string name =
        callee->ct == ConstructType::id
            ? std::string(static_cast<Identifier *>(callee)->uid->val)
            : std::string();

    /* MODELLED EXACTLY: arg0's container gains arg1 (append/push), or
     * arg2 (insert, whose arg1 is the index). Everything else about
     * them is already covered by the argument escape above. */
    if ((name == "append" || name == "push") && c->args->elems.size() >= 2) {
        cs_write_elems(cs_eval(c->args->elems[0].get()),
                       cs_eval(c->args->elems[1].get()));
        return;
    }
    if (name == "insert" && c->args->elems.size() >= 3) {
        cs_write_elems(cs_eval(c->args->elems[0].get()),
                       cs_eval(c->args->elems[2].get()));
        return;
    }

    /* NOT MODELLED: whatever it did to the container, we cannot say -
     * so what comes out of it later is unknown. */
    for (auto &a : c->args->elems) {
        const CsSet av = cs_eval(a.get());
        if (av.top)
            continue;                    /* already unknown */
        for (int o : av.objs) {
            if (o == CS_OBJ_UNKNOWN)
                continue;
            CsSet unknown;
            unknown.set_top();
            cs_write(cs_loc(CsLocKind::elem, nullptr, o), unknown);
        }
    }
}

bool Inferencer::cs_callee_is_builtin(Construct *e)
{
    if (!e || e->ct != ConstructType::id)
        return false;
    auto *id = static_cast<Identifier *>(e);
    auto it = id_sym.find(id);
    if (it != id_sym.end() && it->second)
        return false;                     /* a user symbol shadows it */
    return is_builtin(id->uid);
}

/* ---------------------------------------------------------------- */
/*                            the solver                             */
/* ---------------------------------------------------------------- */

/*
 * Chaotic iteration to the least fixpoint. Every transfer is `dst ⊇
 * src` with ⊤ absorbing, so the result does not depend on visit order
 * (see the header note) - only the round count does.
 */
void Inferencer::cs_run(Block *rootBlock)
{
    cs_pts.clear();
    cs_loc_ids.clear();
    cs_obj_ids.clear();
    cs_escaped.clear();
    cs_escaping_objs.clear();
    cs_escaped_all = false;
    cs_top_callee_sites = 0;
    cs_ran = false;

    /* id 0 must be the UNKNOWN object's element location, so that a
     * bare `cs_loc(elem, nullptr, CS_OBJ_UNKNOWN)` is always ⊤. */
    cs_pts[cs_loc(CsLocKind::elem, nullptr, CS_OBJ_UNKNOWN)].set_top();

    int round = 0;
    for (; round < CS_MAX_ROUNDS; round++) {
        cs_changed = false;
        cs_top_callee_sites = 0;
        for (auto &e : rootBlock->elems)
            cs_visit(e.get(), nullptr);
        /* drain the escape worklist: a container that escaped takes
         * whatever landed in it this round with it */
        std::vector<int> objs(cs_escaping_objs.begin(),
                              cs_escaping_objs.end());
        for (int o : objs)
            cs_escape_flat(cs_pts[cs_loc(CsLocKind::elem, nullptr, o)]);
        if (!cs_changed)
            break;
    }

    /*
     * ⛔ AND IF IT DID NOT CONVERGE, SAY SO RATHER THAN ANSWER. An
     * un-converged monotone analysis is not "mostly right": the sets
     * are a SUBSET of the truth, which is precisely the unsound
     * direction. `cs_ran` false makes every query answer ⊤.
     */
    cs_ran = round < CS_MAX_ROUNDS;

    /*
     * The derived escape closure. A call site whose callee is ⊤ may
     * reach any function whose VALUE ever became reachable - so every
     * function that is anywhere in a points-to set escapes. Functions
     * only ever called by name are untouched, which is the common case
     * and the reason this is not simply "everything".
     */
    if (cs_escaped_all || cs_top_callee_sites > 0) {
        for (const CsSet &s : cs_pts)
            for (FuncInfo *f : s.funcs)
                cs_escaped.insert(f);
    }
}

/* ---------------------------------------------------------------- */
/*                            the query                              */
/* ---------------------------------------------------------------- */

/*
 * The one public question: which functions can this expression
 * evaluate to? A set of size 1 that is not ⊤ is a MUST answer - the
 * only thing that licenses devirtualization, or typing that one
 * function's parameters from this site.
 */
CsSet Inferencer::callee_set(Construct *e)
{
    CsSet out;
    if (!cs_ran) {
        out.set_top();
        return out;
    }
    if (FuncInfo *direct = callee_funcinfo(e)) {
        out.add_func(direct);
        return out;
    }
    return cs_eval(e);
}

/* Has this function's value left the part of the program we can see?
 * A consumer that reasons over ALL of a function's call sites must ask
 * this too: a set of size 1 at every site it CAN see proves nothing if
 * a site it cannot see exists. */
bool Inferencer::callee_escaped(FuncInfo *f)
{
    return !cs_ran || cs_escaped.count(f) != 0;
}

/* ---------------------------------------------------------------- */
/*                    -dcs: the inspectable dump                     */
/* ---------------------------------------------------------------- */

/*
 * A human name for a FuncInfo. A lambda has no `id`, so it is named by
 * where it was WRITTEN - which is both stable across runs and exactly
 * what a test wants to assert.
 */
std::string Inferencer::cs_func_name(FuncInfo *f)
{
    if (!f)
        return "?";
    std::string base;
    if (f->decl && f->decl->id)
        base = std::string(f->decl->id->uid->val);
    else if (f->decl)
        base = "lambda@" + std::to_string(f->decl->start.line) + ":" +
               std::to_string(f->decl->start.col);
    else
        return "?";

    /*
     * ⛔ A SOURCE LOCATION IS NOT AN IDENTITY HERE. Instantiating a
     * template CLONES its body, lambdas and all, and a clone's nodes
     * keep the ORIGINAL locs - so `make_adder` and `make_adder$0` each
     * contain a `lambda@28:10` and the dump printed one name for two
     * different functions. (Exactly the failure scripts/jitprofile.py
     * had with fragment labels.) Disambiguate by creation order, which
     * is stable: `all_funcs` is append-only and a clone is always made
     * after its base.
     */
    int seen = 0;
    for (auto &up : all_funcs) {
        if (up.get() == f)
            break;
        FuncInfo *o = up.get();
        if (!o->decl)
            continue;
        const bool same = o->decl->id
            ? (f->decl->id && o->decl->id->uid == f->decl->id->uid)
            : (!f->decl->id && o->decl->start.line == f->decl->start.line &&
               o->decl->start.col == f->decl->start.col);
        if (same)
            seen++;
    }
    return seen ? base + "~" + std::to_string(seen) : base;
}

std::string Inferencer::cs_set_str(const CsSet &s)
{
    if (s.top)
        return "-";
    std::vector<std::string> names;
    for (FuncInfo *f : s.funcs)
        names.push_back(cs_func_name(f));
    std::sort(names.begin(), names.end());
    std::string out;
    for (size_t i = 0; i < names.size(); i++) {
        if (i)
            out += ",";
        out += names[i];
    }
    return out.empty() ? "-" : out;
}

/*
 * ⛔ THE DUMP IS THE PRIMARY NET, WHICH IS WHY IT EXISTS BEFORE ANY
 * CONSUMER DOES. The engine differential is blind to this analysis by
 * construction - a better-analysed program computes the same answers
 * (CLAUDE.md, *Testing an AST TRANSFORM*) - so the SET itself has to
 * be asserted, per constraint form and per ⊤ sink.
 *
 * One `dcs` record per call site, tab-separated in the `-dti` spirit:
 *
 *   dcs <line> <col> <answer> <names>
 *
 * answer: direct  the callee is a named function / inline lambda
 *         one     exactly one function, named through a value
 *         many    a known set of 2+
 *         top     unknown (⊤)
 *         none    ⊥ - no function value can reach this callee
 * names : comma-separated, sorted; "-" for top/none
 *
 * plus one `dcs-esc` record per function whose value left our view.
 */
void Inferencer::dump_callee_sets(std::ostream &os)
{
    os << "# dcs\tline\tcol\tin\tanswer\tnames\n";

    /*
     * ⛔ COLLECTED WITH THEIR ENCLOSING FUNCTION, not by
     * `collect_calls`. A template's INSTANCE is a CLONE whose nodes
     * keep the original's source locs, so `apply` and `apply$0` each
     * hold a call at the same line:col - and they legitimately have
     * DIFFERENT answers (the base's parameter is never written,
     * because every call was redirected to the clone). Two rows with
     * one address are not assertable, so the row carries the function
     * it is in.
     */
    struct Site { CallExpr *call; FuncInfo *in; };
    std::vector<Site> sites;
    std::function<void(Construct *, FuncInfo *)> walk =
        [&](Construct *n, FuncInfo *fn) {
            if (!n)
                return;
            if (n->ct == ConstructType::call)
                sites.push_back({ static_cast<CallExpr *>(n), fn });
            FuncInfo *inner = fn;
            if (n->ct == ConstructType::func_decl) {
                auto it = func_of_decl.find(static_cast<FuncDeclStmt *>(n));
                inner = it != func_of_decl.end() ? it->second : nullptr;
            }
            for_each_child(n, [&](Construct *c) { walk(c, inner); });
        };
    if (auto *b = dynamic_cast<Block *>(root))
        for (auto &e : b->elems)
            walk(e.get(), nullptr);

    struct Row { int line, col; std::string in, answer, names; };
    std::vector<Row> rows;
    for (const Site &st : sites) {
        CallExpr *c = st.call;
        if (cs_struct_callee(c->what.get()))
            continue;                     /* a construction, not a call */
        const bool direct = callee_funcinfo(c->what.get()) != nullptr;
        const CsSet s = callee_set(c->what.get());
        /* A BUILTIN is ⊤ to the analysis (it has no FuncInfo), but it
         * is a KNOWN ⊤ - reporting it as one would bury every real
         * unknown among the print()s and make the ⊤-sink cases
         * unreadable. */
        const char *answer = direct ? "direct"
                           : cs_callee_is_builtin(c->what.get()) ? "builtin"
                           : s.top   ? "top"
                           : s.funcs.size() == 1 ? "one"
                           : s.funcs.empty() ? "none"
                           : "many";
        rows.push_back({ c->start.line, c->start.col,
                         st.in ? cs_func_name(st.in) : "-",
                         answer, cs_set_str(s) });
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.line != b.line) return a.line < b.line;
        if (a.col != b.col)   return a.col < b.col;
        return a.in < b.in;
    });
    for (const Row &r : rows)
        os << "dcs\t" << r.line << "\t" << r.col << "\t" << r.in
           << "\t" << r.answer << "\t" << r.names << "\n";

    /* Asked through the PUBLIC query, not off the set, so the dump and
     * a consumer cannot disagree about who escaped. */
    std::vector<std::string> esc;
    for (auto &up : all_funcs)
        if (callee_escaped(up.get()))
            esc.push_back(cs_func_name(up.get()));
    std::sort(esc.begin(), esc.end());
    for (const std::string &n : esc)
        os << "dcs-esc\t" << n << "\n";
}
