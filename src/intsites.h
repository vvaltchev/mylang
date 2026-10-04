/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * THE INTRUSIVE-TEST SITE REGISTRY (#107, plans/intrusive-tests.md).
 *
 * The ONE list of every instrumented point. Each row is
 *
 *     X(name, "what an event here means", F(type, field) F(type, field)...)
 *
 * and inttest.h expands it into the site enum, a TYPED payload struct per
 * site (`IntPay_<name>`, filled positionally by `ML_INT(name, ...)`, so a
 * wrong field count or type is a compile error) and the canonical printer.
 *
 * Field types are restricted to `int64_t`, `bool` and `std::string`: a
 * printed event must be a deterministic function of the program, so no
 * pointer, address or hash ever goes in a payload.
 *
 * A row nobody's tests reach is a failure (tests/int_run's site census):
 * add a site together with the test that exercises it.
 */

#pragma once

#define ML_INT_SITES(X)                                                       \
    /* The AST inliner spliced a call: `engine` is expr / block / tail,    */ \
    /* `caller` the function whose body holds the call (`main` at the top  */ \
    /* level), `callee` the spliced function, line:col the call site.      */ \
    /* Display names, so a template instance reads as its base.            */ \
    X(inline_ast, "the AST inliner spliced a call",                           \
      F(std::string, engine) F(std::string, caller)                           \
      F(std::string, callee) F(int64_t, line) F(int64_t, col))             \
    /* The bytecode splice decided one (call site, candidate callee):     */ \
    /* `verdict` is `spliced` or the decline reason (codegen.cpp,         */ \
    /* bc_inline_chunk_splice), `kind` call / value, line:col the call.   */ \
    X(splice, "the bytecode splice decided a call site",                      \
      F(std::string, verdict) F(std::string, kind) F(std::string, callee)    \
      F(int64_t, line) F(int64_t, col)) \
    /* An AST loop transform FIRED on the loop at line:col: `pass` is      */ \
    /* slice_hoist / licm / for_range (inferencer.cpp, specialize).        */ \
    X(ast_transform, "an AST loop transform rewrote a loop",                  \
      F(std::string, pass) F(int64_t, line) F(int64_t, col)) \
    /* The JIT held a frame slot in a register somewhere in a function:   */ \
    /* `func` the function (`main` at the top level), `var` the slot's    */ \
    /* source name (`tN` for the Nth expression temp), `reg` the register */ \
    /* (`xmmN` for a float). One event per distinct (var, reg) per chunk, */ \
    /* from the FINAL emission only (a bet's discarded attempt records    */ \
    /* nothing). jit.cpp, jit_compile_chunk.                              */ \
    X(pin, "the JIT held a slot in a register",                               \
      F(std::string, func) F(std::string, var) F(std::string, reg)) \
    /* The JIT chose a call site's PROTOCOL at emit time: `tier` is       */ \
    /* frameless / push (the sync call, emit_sync_call_inline) or         */ \
    /* native_direct (a native leaf callee, CallV's #55 path); `callee`   */ \
    /* the named function(s) (`a|b` for a two-way site, `?` unknown).     */ \
    X(call_tier, "the JIT chose a call site's protocol",                      \
      F(std::string, tier) F(std::string, callee)                            \
      F(int64_t, line) F(int64_t, col)) \
    /* Lever A FORWARDED a value to its consumer in a register instead of */ \
    /* through its slot: `var` the slot (a local's name, `tN` a temp).    */ \
    X(forward, "the JIT forwarded a value in a register",                     \
      F(std::string, func) F(std::string, var))                              \
    /* The JIT ELIDED a reference guard: `kind` store (C5: a ref-listed   */ \
    /* slot's store dropped its release test) or member (C4d: a proven    */ \
    /* struct's member read dropped its type guard); `var` the slot.      */ \
    X(guard_elided, "the JIT elided a reference guard",                       \
      F(std::string, kind) F(std::string, func) F(std::string, var)) \
    /* An ENUMERATED decision (plan 4.2): the register allocator picked   */ \
    /* one of `n` legal registers - `dflt` the heuristic's index, `pick`  */ \
    /* the one taken (they differ only under MYLANG_INT_CHOOSE). `key` is */ \
    /* the instance: function, file (gp/fp) and the pick's ordinal in     */ \
    /* that function's emission; `reg` the register number picked (the  */ \
    /* diagnosis: an index says nothing about WHICH register broke).     */ \
    /* jit.cpp, RegAlloc::take / ftake.                                   */ \
    X(reg_choice, "the register allocator picked among legal registers",     \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick)  \
      F(int64_t, reg))                                                  \
    /* An ENUMERATED decision: a run's PIN BUDGET - how many values it  */ \
    /* may keep in registers, `n` = max + 1 legal answers (0..max),     */ \
    /* `dflt` the maximum. `key` is `<fn>@<run begin pc>/budget`.       */ \
    /* jit.cpp, jit_compile_chunk's run loop.                           */ \
    X(pin_budget, "a run's pin budget (how many values it pins)",         \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick))  \
    /* An ENUMERATED decision: the bytecode SPLICE of a call site that  */ \
    /* would be spliced - pick 0 splices, 1 declines (the call stays a  */ \
    /* call). `key` is `<caller>/splice@<line>:<col>`. codegen.cpp,     */ \
    /* bc_inline_chunk_splice.                                          */ \
    X(splice_choice, "the bytecode splice of a site (0 splice, 1 decline)", \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick))  \
    /* An ENUMERATED decision: the linear scan's PRESSURE CONTEST -    */ \
    /* which piece loses its register (spills) when the pool is full.  */ \
    /* 0 is the newcomer, 1+a the a-th active piece. `key` is          */ \
    /* `<run>/spill#<k>` (GP) or `<run>/fspill#<k>` (float), k the     */ \
    /* contest's ordinal in that scan. jit.cpp, jit_lsra_assign.        */ \
    X(spill_choice, "which value spills at register pressure",           \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick))  \
    /* An ENUMERATED decision: the C4b FLOAT LITERAL POOL of a run.     */ \
    /* `<run>/flitgate` - the cost gate refused the pool (a helper call */ \
    /* in a loop): 0 refuses, 1 takes it anyway (what FORCE=flit does - */ \
    /* the epilogues restore the pool). `<run>/flit#<i>` - the i-th    */ \
    /* ranked literal the pool would hold: 0 holds it, 1 leaves it     */ \
    /* inline. jit.cpp, pick_float_lits.                                */ \
    X(flit_choice, "the float literal pool (gate / admit one literal)",   \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick))  \
    /* An ENUMERATED decision: the AST INLINE of a call site an engine   */ \
    /* would inline - 0 inlines, 1 declines. `key` is                    */ \
    /* `<caller>/inline@<line>:<col>`. resolver.cpp, the Inliner.        */ \
    X(inline_choice, "the AST inline of a site (0 inline, 1 decline)",    \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick)) \
    /* An ENUMERATED decision: how many levels the inliner UNROLLS a     */ \
    /* pure tree-recursive function (0..REC_UNROLL_MAX; the cost model's */ \
    /* depth is the default). Every depth is legal - the node cap and   */ \
    /* the inline budget still bound the growth. `key` is `<fn>/unroll`. */ \
    /* resolver.cpp, rec_unroll_depth.                                   */ \
    X(unroll_choice, "the recursion unroll depth of a function",         \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick)) \
    /* An ENUMERATED decision: a call site the JIT would make FRAMELESS */ \
    /* - 0 frameless, 1 the general push. `key` is `frameless@L:C`, the */ \
    /* site's source position. jit.cpp, jit_frameless_candidates.      */ \
    X(frameless_choice, "a call site's frameless tier (0 take, 1 push)", \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick)) \
    /* An ENUMERATED decision: a guarded tier's DECLINE (a decline_jump  */ \
    /* or a reference check's helper arm) - 0 as emitted, 1 forced to   */ \
    /* the slow tier for every value. `key` is `<run>/<guard>@<pc>#<k>`. */ \
    /* jit.cpp, jit_int_force_decline (P7, the per-site forcing sweep).  */ \
    X(decline_choice, "a guarded tier's decline (0 as emitted, 1 forced)", \
      F(std::string, key) F(int64_t, n) F(int64_t, dflt) F(int64_t, pick))

/*
 * THE VALUE CLASSES (plan section 9.1): each row a declared BOUNDARY of
 * an operation - a shift by 63 and by 64, an index of -1 and of len, the
 * most negative int, an empty container. Branch coverage cannot see an
 * off-by-one there: a shift by 63 and one by 64 can take the same C++
 * branch in a saturating implementation that is wrong at 64. Declared,
 * each is a countable element: tests/int_run fails when no test exercises
 * one, and with --gcov each joins the coverage universe as `vc:<name>`.
 *
 * A class is recorded where its operation is DEFINED for the reference
 * engine: the shared helper when there is one (bitops.h, a builtin), else
 * the tree-walker's evaluation - the other engines are checked against it,
 * since a value-class test runs under the tree-walker AND the fast engines
 * and asserts each result from the README. Counters only, no event: a
 * recording point sits in hot code, so it costs an increment and throws
 * nothing (the noexcept int_vc_* classifiers, inttest.cpp).
 *
 *     V(name, "the boundary")
 */
#define ML_INT_VCLASSES(V)                                                    \
    /* shifts (bitops.h): the count at each edge of [0, 64) */             \
    V(shl_neg, "x << n, n < 0: InvalidValueEx")                               \
    V(shl_zero, "x << 0")                                                     \
    V(shl_63, "x << 63, the top bit")                                         \
    V(shl_64, "x << 64, the width: 0")                                        \
    V(shl_over, "x << n, n > 64: 0")                                          \
    V(shr_neg, "x >> n, n < 0: InvalidValueEx")                               \
    V(shr_zero, "x >> 0")                                                     \
    V(shr_63, "x >> 63")                                                      \
    V(shr_64, "x >> 64, the width: a full sign fill")                        \
    V(shr_over, "x >> n, n > 64: a full sign fill")                          \
    V(shr_fill, "x >> n, x < 0 and n >= 64: -1")                              \
    V(ushr_neg, "x >>> n, n < 0: InvalidValueEx")                             \
    V(ushr_zero, "x >>> 0")                                                   \
    V(ushr_63, "x >>> 63")                                                    \
    V(ushr_64, "x >>> 64, the width: 0")                                      \
    V(ushr_over, "x >>> n, n > 64: 0")                                        \
    V(ushr_negval, "x >>> n, x < 0 and 0 < n < 64: zero-filled")             \
    /* integer / and % (TypeInt, the M8 typed loop) */                     \
    V(div_zero, "x / 0: DivisionByZeroEx")                                    \
    V(mod_zero, "x % 0: DivisionByZeroEx")                                    \
    V(div_min_neg1, "INT_MIN / -1: InvalidValueEx")                           \
    V(mod_min_neg1, "INT_MIN % -1: InvalidValueEx")                           \
    V(div_neg_trunc, "a / b, inexact and negative: truncates toward 0")      \
    V(mod_neg, "a % b, a < 0 and inexact: a negative remainder")             \
    V(mod_neg_divisor, "a % b, a > 0, b < 0, inexact: a positive remainder") \
    /* integer wraparound (TypeInt, the M8 typed loop) */                  \
    V(add_wrap, "a + b overflows 64 bits: wraps")                             \
    V(sub_wrap, "a - b overflows 64 bits: wraps")                             \
    V(mul_wrap, "a * b overflows 64 bits: wraps")                             \
    V(neg_min, "-INT_MIN: INT_MIN")                                           \
    /* a[i] / s[i] (the tree-walker's subscript paths): each edge of     */ \
    /* [-len, len), and any index of an empty container                  */ \
    V(arr_idx_empty, "a[i] on an empty array: OutOfBoundsEx")                 \
    V(arr_idx_below, "a[-len-1]: OutOfBoundsEx")                              \
    V(arr_idx_neg_first, "a[-len]: the first element")                        \
    V(arr_idx_neg_last, "a[-1]: the last element")                            \
    V(arr_idx_first, "a[0]")                                                  \
    V(arr_idx_last, "a[len-1]")                                               \
    V(arr_idx_len, "a[len]: OutOfBoundsEx")                                   \
    V(str_idx_empty, "s[i] on an empty string: OutOfBoundsEx")                \
    V(str_idx_below, "s[-len-1]: OutOfBoundsEx")                              \
    V(str_idx_neg_first, "s[-len]: the first char")                           \
    V(str_idx_neg_last, "s[-1]: the last char")                               \
    V(str_idx_first, "s[0]")                                                  \
    V(str_idx_last, "s[len-1]")                                               \
    V(str_idx_len, "s[len]: OutOfBoundsEx")                                   \
    /* container builtins at zero and one element (shared by all engines) */\
    V(sum_empty, "sum([]): the additive identity, or InvalidArgumentEx")     \
    V(sum_one, "sum([x])")                                                    \
    V(min_empty, "min([]): InvalidArgumentEx")                                \
    V(min_one, "min([x])")                                                    \
    V(max_empty, "max([]): InvalidArgumentEx")                                \
    V(max_one, "max([x])")                                                    \
    V(pop_empty, "pop([]): OutOfBoundsEx")                                    \
    V(pop_one, "pop([x]): leaves []")                                         \
    V(top_empty, "top([]): OutOfBoundsEx")                                    \
    V(top_one, "top([x])")                                                    \
    V(join_empty, "join([], d): \"\"")                                        \
    V(join_one, "join([s], d): s, no delimiter")                              \
    V(sort_empty, "sort([])")                                                 \
    V(sort_one, "sort([x])")
