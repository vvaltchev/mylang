/* SPDX-License-Identifier: BSD-2-Clause */

#include "parser.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "eval.h"
#include "resolver.h"
#include "backtrace.h"
#include "inferencer.h"
#include "analyzer.h"
#include "repl.h"
#include "errfmt.h"
#include "trace.h"
#include "vm.h"
#include "codegen.h"   /* g_bc_inline_enabled (-nbi) */
#include "serialize.h"
#include "jit.h"
#include "disasm.h"

#include <initializer_list>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#ifndef _WIN32
#include <unistd.h>   /* isatty (REPL launch; Unix-only) */
#endif

using std::string;

static bool opt_show_tokens;
static bool opt_show_syntax_tree;
static bool opt_no_const_eval;
static bool opt_no_inline;
static int opt_inline_threshold = 24;  /* max inlined body size (nodes) */
static bool opt_no_run;
static bool opt_no_type_infer;
static bool opt_debug_ti;
static bool opt_analyze;
static bool opt_no_color;
static bool opt_repl;
static bool opt_vm;   /* -vm: explicit VM (the DEFAULT since 2026-07-18) */
static bool opt_tw;   /* -tw: run via the TREE-WALKER instead of the VM */
static bool opt_vm_disasm;   /* -vd: print the bytecode disassembly, no run */
static bool opt_compile;     /* -c: write a .myv image, then exit (no run) */
static bool opt_strip_source;/* -c --strip-source: store no source reference */
static string opt_out_path;  /* -o <path> (default: the source's .myv twin) */
static string opt_script_path;   /* the FILE argument (source or image) */
static bool opt_run_image;       /* the file argument is a .myv image */
static string opt_source_root;   /* --source ROOT: find an image's source
                                  * under THIS root, not the stored path */
static bool opt_force;           /* -f: use a source whose CRC32 mismatches */

static std::vector<string> lines;
/* The source file to NAME in errors ("" for -e / the REPL, which have none).
 * For a .myv image it is what the stored reference resolved to - the ONLY
 * source identity such a run has, and the whole point of storing it. */
static string src_name;
static std::vector<Tok> tokens;
/* The whole source as one buffer (lines re-joined with '\n'). The lexer scans
 * it in one pass so strings / block comments can span lines; token string_views
 * point into it, so it must outlive parsing (it is static => program lifetime).
 * `lines` is kept separately for per-line error carets. */
static string source;

static void
lex_all()
{
    source.clear();
    for (size_t i = 0; i < lines.size(); i++) {
        if (i)
            source += '\n';
        source += lines[i];
    }
    lexer(source, 1, tokens);
}

void run_tests(bool dump_syntax_tree);

/*
 * `-v`: report HOW THIS BINARY WAS BUILT.
 *
 * The two lines that matter are `opt` and `asserts`: a performance number is
 * only meaningful from an OPT=1 ASSERTS=0 build, and mixing configs silently
 * invalidates a comparison (assertion cost is not a uniform multiplier - it
 * sits unevenly across code paths, so it can flip the SIGN of an A/B). The
 * bench runner parses this and refuses to measure a wrongly-built binary.
 *
 * Everything here is read from a COMPILER-SET or build-system macro rather
 * than a hand-maintained string, so it cannot drift from what was actually
 * compiled. `__OPTIMIZE__` and `NDEBUG` in particular are set by the
 * toolchain itself, which is exactly the property the gate needs.
 *
 * Format: one `key value` line per fact, so it stays trivially parseable.
 */
static void show_build_config()
{
#if defined(__OPTIMIZE__)
    const char *opt = "1";
#elif defined(_MSC_VER)
    const char *opt = "unknown";     /* MSVC sets no such macro */
#else
    const char *opt = "0";
#endif

#ifdef NDEBUG
    const char *asserts = "0";
#else
    const char *asserts = "1";
#endif

#if defined(_GLIBCXX_ASSERTIONS) || defined(_LIBCPP_HARDENING_MODE)
    const char *stdlib_hardening = "1";
#else
    const char *stdlib_hardening = "0";
#endif

#if defined(__SANITIZE_ADDRESS__)
    const char *asan = "1";
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
    const char *asan = "1";
#  else
    const char *asan = "0";
#  endif
#else
    const char *asan = "0";
#endif

#if defined(__SANITIZE_UNDEFINED__)
    const char *ubsan = "1";
#elif defined(__has_feature)
#  if __has_feature(undefined_behavior_sanitizer)
    const char *ubsan = "1";
#  else
    const char *ubsan = "0";
#  endif
#else
    const char *ubsan = "0";
#endif

#ifdef ML_LTO
    const char *lto = ML_LTO ? "1" : "0";
#else
    const char *lto = "unknown";     /* no macro (e.g. a CMake build) */
#endif

    cout << "mylang build configuration" << endl;
    cout << "  opt               " << opt
         << "        (optimized: -O3)" << endl;
    cout << "  asserts           " << asserts
         << "        (C assert + ML_CHECK)" << endl;
    cout << "  stdlib_hardening  " << stdlib_hardening
         << "        (bounds-checked container access)" << endl;
    cout << "  lto               " << lto << endl;
    cout << "  asan              " << asan << endl;
    cout << "  ubsan             " << ubsan << endl;
    cout << "  vm_hardening      "
#if defined(ML_VM_HARDENING) && ML_VM_HARDENING
         << "1"
#else
         << "0"
#endif
         << "        (per-op slot/type checks)" << endl;
    cout << "  cgoto             "
#ifdef ML_NO_CGOTO
         << "0"
#else
         << "1"
#endif
         << "        (computed-goto dispatch)" << endl;
    cout << "  tests             "
#ifdef TESTS
         << "1"
#else
         << "0"
#endif
         << "        (-rt suite compiled in)" << endl;
    cout << "  recycle           "
#ifdef RECYCLE_ALLOC
         << "1"
#else
         << "0"
#endif
         << "        (adversarial node allocator)" << endl;
    cout << "  jit               "
#if defined(__x86_64__) && !defined(_WIN32)
         << "1"
#else
         << "0"
#endif
         << "        (native x86-64 codegen)" << endl;
    cout << "  compiler          "
#if defined(__clang__)
         << "clang " << __clang_major__ << "." << __clang_minor__
#elif defined(__GNUC__)
         << "gcc " << __GNUC__ << "." << __GNUC_MINOR__
#elif defined(_MSC_VER)
         << "msvc " << _MSC_VER
#else
         << "unknown"
#endif
         << endl;
    cout << endl;
    if (strcmp(opt, "1") || strcmp(asserts, "0"))
        cout << "NOT a performance build - measure only with "
                "OPT=1 ASSERTS=0." << endl;
    else
        cout << "This is a performance build (OPT=1 ASSERTS=0)." << endl;
}

void help()
{
    cout << "Syntax:" << endl;
    cout << "   mylang [-t] [-s] [-nc] FILE | -e EXPR" << endl;
    cout << endl;
    cout << "   -t      Show all tokens" << endl;
    cout << "   -s      Dump the syntax tree" << endl;
    cout << "   -v      Show how this binary was BUILT (opt, asserts, ...)"
         << endl;
    cout << "  -nc      No const eval (debug)" << endl;
    cout << "  -ni      No function inlining (debug)" << endl;
    cout << "  --no-opt L  Disable AST transforms (comma-separated): "
         << opt_pass_names() << endl;
    cout << "  -it N    Inline threshold: max inlined body size (default 24)"
         << endl;
    cout << "  -nr      Don't run, just validate" << endl;
    cout << "  -c       Compile to a .myv bytecode file, then exit" << endl;
    cout << "  -o FILE  Output path for -c (default: the source's .myv twin)"
         << endl;
    cout << "  --strip-source  With -c: store no source reference at all"
         << endl;
    cout << "                  (errors then print no source line / caret,"
         << endl;
    cout << "                  and the image carries no local paths)" << endl;
    cout << "  --source ROOT   Find a .myv image's source under ROOT instead"
         << endl;
    cout << "                  of the path stored when it was compiled"
         << endl;
    cout << "  -f       Render carets from a source whose CRC32 does not"
         << endl;
    cout << "           match the image (--force; a warning is printed)"
         << endl;
    cout << "  -vm      Execute via the bytecode VM (the DEFAULT)." << endl;
    cout << "  -nj      Disable the native x86-64 AOT tier (also"
         << endl;
    cout << "           MYLANG_JIT=0). See plans/native-aot.md" << endl;
    cout << "  -tw      Execute via the tree-walking interpreter instead"
         << endl;
    cout << "           of the VM. See plans/archived/bytecode-vm.md" << endl;
    cout << "  -vd      Dump the VM bytecode disassembly, then exit" << endl;
    cout << "  -vdj     Like -vd, plus the native x86-64 disassembly of"
         << endl;
    cout << "           each JIT fragment interleaved with the VM ops"
         << endl;
    cout << " -nti      No type inference / checking (debug)" << endl;
    cout << " -dti      Dump inferred types of all identifiers, then exit"
         << endl;
    cout << "  -a       Analyze: reprint the source with colors showing which"
         << endl;
    cout << "           optimizations fired (--analyze; --no-color for plain)"
         << endl;
    cout << "  -T CATS  Trace the compiler's reasoning to stderr (--trace);"
         << endl;
    cout << "           CATS is comma-separated: infer,inline,specialize,"
         << endl;
    cout << "           template,autoconst,autopure,arrays,fold, or all"
         << endl;

#ifdef TESTS
    cout << "  -rt      Run unit tests" << endl;
#endif
}

void
read_script(const char *filename)
{
    {
        string line;
        std::ifstream filestream(filename);

        if (filestream.is_open()) {

            while (getline(filestream, line)) {
                lines.push_back(std::move(line));
                line.clear(); /* Put the string is a known state */
            }

        } else {

            cout << "Failed to open file '" << filename << "'\n";
            exit(1);
        }
    }

    lex_all();
}

void
parse_args(int argc, char **argv)
{
    string inline_text;
    bool in_tokens = false;

    if (!argc) {

        /* That should *never* happen */
        cout << "Unexpected (system) error: zero arguments" << endl;
        exit(1);

    } else if (argc == 1) {

        /* No FILE / -e: drop into the interactive REPL on a terminal (Ruby-
         * like); otherwise print help. The REPL is Unix-only (see plans/
         * repl.md), so on Windows the no-args case just prints help. */
#ifndef _WIN32
        if (isatty(STDIN_FILENO)) {
            opt_repl = true;
            return;
        }
#endif

        help();
        exit(0);
    }

    for (argc--, argv++; argc > 0; argc--, argv++) {

        char *arg = argv[0];

        if (in_tokens) {
            /*
             * Join multiple arguments after -e with a space, so that an
             * unquoted expression split by the shell (e.g. -e var x = 5)
             * is not glued back into a single mangled token (varx=5).
             */
            if (!inline_text.empty())
                inline_text += ' ';
            inline_text += arg;
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {

            help(); exit(0);

        } else if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) {

            show_build_config(); exit(0);

        } else if (!strcmp(arg, "-rt")) {

            run_tests(opt_show_syntax_tree); exit(0);

        } else if (!strcmp(arg, "--weights")) {

            run_weight_bench(); exit(0);

        } else if (!strcmp(arg, "-t")) {

            opt_show_tokens = true;

        } else if (!strcmp(arg, "-s")) {

            opt_show_syntax_tree = true;

        } else if (!strcmp(arg, "-nc")) {

            opt_no_const_eval = true;

        } else if (!strcmp(arg, "-ni")) {

            opt_no_inline = true;

        } else if (!strcmp(arg, "-npc")) {

            g_pure_cache_enabled = false;   /* recursion unroll, no per-frame cache */

        } else if (!strcmp(arg, "--no-opt")) {

            /* Turn OFF one or more AST transforms - the same-binary A/B lever
             * for an optimizer that rewrites the tree before either engine
             * runs it (see OptPass in inferencer.h). */
            if (argc < 2) {
                cout << "error: --no-opt requires a pass list ("
                     << opt_pass_names() << ")" << endl;
                exit(1);
            }
            argc--; argv++;
            std::string spec(*argv), one;
            for (size_t i = 0; i <= spec.size(); i++) {
                if (i == spec.size() || spec[i] == ',') {
                    unsigned bit;
                    if (!one.empty() && !opt_pass_bit(one, bit)) {
                        cout << "error: unknown --no-opt pass '" << one
                             << "' (known: " << opt_pass_names() << ")" << endl;
                        exit(1);
                    }
                    if (!one.empty())
                        g_opt_disabled |= bit;
                    one.clear();
                } else {
                    one += spec[i];
                }
            }

        } else if (!strcmp(arg, "-it")) {

            if (argc < 2) {
                cout << "error: -it requires a value (max inlined body size)"
                     << endl;
                exit(1);
            }

            opt_inline_threshold = atoi(argv[1]);
            argc--; argv++;   /* consume the value; the loop skips it too */

        } else if (!strcmp(arg, "-c")) {

            /* .myv: compile to stored bytecode and EXIT (implies the VM) */
            opt_compile = true;

        } else if (!strcmp(arg, "-o")) {

            if (argc < 2) {
                cout << "-o requires an output path" << endl;
                exit(1);
            }
            opt_out_path = argv[1];
            argc--; argv++;

        } else if (!strcmp(arg, "--strip-source")) {

            opt_strip_source = true;

        } else if (!strcmp(arg, "--source")) {

            /* .myv: resolve the stored RELATIVE source path under this root
             * instead of the compile-time paths (a moved / re-checked-out
             * tree, or a build machine's paths on a different host) */
            if (argc < 2) {
                cout << "--source requires a project-root path" << endl;
                exit(1);
            }
            opt_source_root = argv[1];
            argc--; argv++;

        } else if (!strcmp(arg, "-f") || !strcmp(arg, "--force")) {

            opt_force = true;   /* trust a CRC32-mismatched source anyway */

        } else if (!strcmp(arg, "-nr")) {

            opt_no_run = true;

        } else if (!strcmp(arg, "-nti")) {

            opt_no_type_infer = true;

        } else if (!strcmp(arg, "-vm")) {

            opt_vm = true;   /* explicit VM - the default; kept for scripts/
                              * CI written before the 2026-07-18 flip */

            if (opt_vm && opt_tw) {
                cout << "-vm and -tw are mutually exclusive" << endl;
                exit(1);
            }

        } else if (!strcmp(arg, "-nj")) {

            g_jit_enabled = false;   /* native-AOT off (plans/native-aot.md);
                                      * also MYLANG_JIT=0 */

        } else if (!strcmp(arg, "-bi")) {

            g_bc_inline_enabled = true;   /* the bytecode SPLICE on - OPT-IN
                                           * while it is unfinished, see
                                           * plans/bytecode-inliner.md; also
                                           * MYLANG_BCINLINE=1 */

        } else if (!strcmp(arg, "-tw")) {

            opt_tw = true;   /* run via the tree-walker (the pre-flip
                              * default; also the const-eval/REPL engine) */

            if (opt_vm && opt_tw) {
                cout << "-vm and -tw are mutually exclusive" << endl;
                exit(1);
            }

        } else if (!strcmp(arg, "-vd")) {

            opt_vm_disasm = true;   /* dump the bytecode disassembly, no run */

        } else if (!strcmp(arg, "-vdj")) {

            opt_vm_disasm = true;      /* -vd + the native-AOT disasm: the
                                        * dump is taken AFTER the JIT and
                                        * interleaves each fragment's x86-64
                                        * disassembly under its enter.nat */
            g_jit_annotate = true;     /* record op marks during codegen */

        } else if (!strcmp(arg, "-dti")) {

            opt_debug_ti = true;

        } else if (!strcmp(arg, "-a") || !strcmp(arg, "--analyze")) {

            opt_analyze = true;

        } else if (!strcmp(arg, "-T") || !strcmp(arg, "--trace")) {

            /* Enable diagnostic trace categories (comma-separated, or "all")
             * BEFORE compilation, so a script run narrates the optimizer
             * reasoning to stderr. See trace.h / plans/repl-introspection. */
            if (argc < 2) {
                cout << "error: --trace requires a category list "
                        "(e.g. infer,inline or all)" << endl;
                exit(1);
            }
            const string cats = argv[1];
            argc--; argv++;   /* consume the value */
            size_t pos = 0;
            while (pos <= cats.size()) {
                const size_t comma = cats.find(',', pos);
                const size_t end =
                    (comma == string::npos) ? cats.size() : comma;
                const string c = cats.substr(pos, end - pos);
                if (!c.empty() && !trace_set(c, true))
                    cout << "warning: unknown trace category '" << c << "'"
                         << endl;
                if (comma == string::npos)
                    break;
                pos = comma + 1;
            }

        } else if (!strcmp(arg, "--no-color")) {

            opt_no_color = true;

        } else if (!strcmp(arg, "--repl")) {

            opt_repl = true;   /* force the REPL even off a TTY (for testing) */
            return;

        } else if (!strcmp(arg, "-e")) {

            if (argc > 1) {
                in_tokens = true;
                continue;
            }

            help(); exit(1);

        } else {

            /* .myv: an IMAGE is detected by CONTENT (magic), not extension -
             * it is loaded, not parsed (no source, no optimizer passes). */
            opt_script_path = arg;
            if (myv_is_image(arg)) {
                opt_run_image = true;
            } else {
                read_script(arg);
                src_name = arg;   /* name it in errors, like any compiler */
            }

            /* Under -c the remaining args are the COMPILER's, not the
             * script's (there is no run): `mylang -c file.my -o out.myv
             * [--strip-source]`, the documented form. */
            if (opt_compile) {
                /* register `argv` (empty) FIRST: it is a builtin, and the
                 * builtin SET decides every baked builtin slot - a compile
                 * must see exactly what a run sees. */
                EvalContext::builtins.emplace(
                    make_pair(UniqueId::get("argv"),
                              LValue(SharedArrayObj(
                                         SharedArrayObj::vec_type()), true)));
                for (int i = 1; i < argc; i++) {
                    if (!strcmp(argv[i], "-o") && i + 1 < argc)
                        opt_out_path = argv[++i];
                    else if (!strcmp(argv[i], "--strip-source"))
                        opt_strip_source = true;
                    else {
                        cout << "unexpected argument after -c: "
                             << argv[i] << endl;
                        exit(1);
                    }
                }
                return;
            }

            SharedArrayObj::vec_type vec;

            for (int i = 1; i < argc; i++)
                vec.emplace_back(SharedStr(string(argv[i])), false);

            EvalContext::builtins.emplace(

                make_pair(
                    UniqueId::get("argv"),
                    LValue(
                        EvalValue(SharedArrayObj(std::move(vec))),
                        false
                    )
                )
            );

            break;
        }
    }

    if (in_tokens) {
        lines.emplace_back(std::move(inline_text));
        lex_all();
    }
}

/* anno_code + render_analysis now live in analyzer.cpp (shared with the REPL's
 * :analyze meta-command). */

int main(int argc, char **argv)
{
    /*
     * An uncaught struct exception's payload references its StructTypeDef (to
     * print field names), and values unwinding out of the run may reference
     * FuncDescriptors. Under the tree-walker the AST owns both; under -vm
     * vm_compile MOVES them into the VmProgram. So BOTH `root` and `prog`
     * must outlive the catch handlers below: declared here, not inside the
     * try, or unwinding would free the defs before format_exception() renders
     * the value.
     */
    unique_ptr<Construct> root;
    VmProgram prog;

    try {

        parse_args(argc, argv);

        /* Color the trace tags on a stderr TTY (the trace sink is stderr for a
         * script), unless --no-color. Harmless when no category is enabled. */
#ifndef _WIN32
        trace_set_color(!opt_no_color && isatty(STDERR_FILENO));
#endif

        if (opt_repl)
            return run_repl();

        if (opt_run_image) {
            /* .myv: LOAD AND GO - no lexer, no parser, no optimizer passes.
             * The AOT native tier runs inside the loader (only the VM image
             * is stored). `prog` is declared outside the try for the same
             * reason a compiled one is: exception payloads and lazy
             * backtrace frames reference its defs/descriptors. */
            g_exec_engine = ExecEngine::Vm;

            /* The image stores a source REFERENCE, not the text: the loader
             * looks for the file (under --source ROOT if given) and uses it
             * only when its CRC32 still matches. Absent or changed => errors
             * render without the quoted line + caret, naming file/line and
             * the backtrace. */
            MyvLoadOpts lo;
            lo.root = opt_source_root;
            lo.force = opt_force;
            MyvSource msrc;
            prog = myv_read(opt_script_path, msrc, lo);
            lines = std::move(msrc.lines);
            src_name = msrc.name;
            if (!msrc.warning.empty())
                cerr << msrc.warning << endl;

            if (opt_vm_disasm) {
                cout << disassemble_image(prog);
                return 0;
            }
            vm_run(prog);
            return 0;
        }

        ParseContext ctx(TokenStream(tokens), !opt_no_const_eval);

        /* -a: the parser records parse-time folds/DCE it would otherwise erase
         * (magenta folded calls, dim dead branches) into this collector; the
         * later passes add to it. Set before pBlock so the parser records. */
        AnalysisInfo analyze_info;
        if (opt_analyze)
            ctx.analysis = &analyze_info;

        if (opt_show_tokens) {
            cout << "Tokens" << endl;
            cout << "--------------------------" << endl;

            for (const auto &tok : tokens) {
                cout << tok << endl;
            }

            cout << endl;
        }

        root = pBlock(ctx);

        if (opt_show_syntax_tree) {
            cout << "Syntax tree" << endl;
            cout << "--------------------------" << endl;
            cout << *root << endl;
            cout << "--------------------------" << endl;
        }

        if (!ctx.eoi())
            throw SyntaxErrorEx(
                Loc(ctx.get_tok().loc),
                "Unexpected token at the end",
                &ctx.get_tok()
            );

        /* Implicit top-level `var`: a bare `name = expr` to an undeclared name
         * at the outermost scope is a declaration. Runs before inference so all
         * later passes see it as an ordinary var decl. (Script: no prior
         * globals.) */
        mark_implicit_globals(root.get(), {});

        /* -dti: dump the inferred type of every identifier + its use
         * sites (machine-readable) and exit, without running. */
        if (opt_debug_ti) {
            dump_type_info(root.get(), cout);
            return 0;
        }

        /* -a/--analyze: collect optimization decisions and reprint the source
         * with colors, then exit. Array-storage colors come from inference (on
         * the clean tree); the resolver passes run next and record auto-const /
         * dead-code / inlined / specialized / folded as the tree mutates. */
        if (opt_analyze) {
            /* analyze_info already holds the parser's records; the shared
             * pipeline adds the inference (array storage) and resolver
             * (auto-const/inline/etc.) decisions, then reprints colored. */
            analyze_and_render(cout, root.get(), analyze_info, lines,
                               !opt_no_color, /*repl_mode=*/false,
                               !opt_no_inline, opt_inline_threshold);
            return 0;
        }

        /* Static type inference + checking (compile-time). Runs before
         * resolve_names, on the clean source tree. A type violation throws a
         * compile-time exception here. Validation-only (-nr) still runs it. */
        infer_types(root.get(), !opt_no_type_infer);

        if (!opt_no_run) {
            /* Run the optimizer pipeline (resolve_names + specialize_types -
             * the SAME helper the REPL uses), then run the script. The root
             * block builds its own "main" Frame for slotted top-level vars. */
            run_optimizers(root.get(), !opt_no_inline, opt_inline_threshold,
                           !opt_no_type_infer);

            /* -s also dumps the tree AFTER the optimizer (inlining, unroll,
             * specialization) so the actual optimized AST is inspectable. */
            if (opt_show_syntax_tree) {
                cout << "Optimized syntax tree" << endl;
                cout << "--------------------------" << endl;
                cout << *root << endl;
                cout << "--------------------------" << endl;
            }

            /* -vd: dump the bytecode disassembly (the bytecode analogue of -s)
             * and do NOT run. See disasm.{h,cpp}. */
            if (opt_vm_disasm) {
                std::string dump = disassemble_program(
                    static_cast<const Block *>(root.get()));
                /* 256-color syntax highlight on a TTY only; a piped/redirected
                 * dump (or NO_COLOR / --no-color) stays plain for diffing.
                 * Unix-only (isatty/unistd), like the REPL - Windows dumps
                 * plain. */
#ifndef _WIN32
                if (!opt_no_color && !getenv("NO_COLOR")
                    && isatty(STDOUT_FILENO))
                    dump = highlight_disasm(dump);
#endif
                cout << dump;
                return 0;
            }

            /* Execution engine: the bytecode VM by DEFAULT (flipped
             * 2026-07-18 - full parity + ~2.2x the tree-walker on the
             * bench geomean, the two documented flip conditions), or the
             * tree-walker under -tw. The VM runs the SAME optimized AST
             * (it lowers `root`), so behavior is identical - see
             * plans/archived/bytecode-vm.md. g_exec_engine tells do_func_call to
             * run function bodies via the VM too, not just the top-level
             * chunk. */
            if (opt_compile) {
                /* .myv (plans/myv-serializer.md): the full pipeline, then
                 * SERIALIZE and exit - no execution. Only the VM image is
                 * stored; the native tier re-runs at load. */
                g_exec_engine = ExecEngine::Vm;
                prog = vm_compile(root.get(), /*jit=*/false);
                string out = opt_out_path;
                if (out.empty()) {
                    out = opt_script_path;
                    const size_t dot = out.find_last_of('.');
                    const size_t slash = out.find_last_of('/');
                    if (dot != string::npos
                            && (slash == string::npos || dot > slash))
                        out = out.substr(0, dot);
                    out += ".myv";
                }
                /* Store a source REFERENCE (path + CRC32), not the text:
                 * error carets then cost ~100 bytes instead of the whole
                 * program, and a run verifies the file is still the one
                 * compiled. --strip-source stores nothing at all. */
                MyvSourceRef ref;
                if (!opt_strip_source)
                    ref = myv_source_ref(opt_script_path);
                myv_write(prog, out, ref);
                return 0;
            }
            if (!opt_tw) {
                g_exec_engine = ExecEngine::Vm;
                prog = vm_compile(root.get());
                /* ASSERTS builds: free + zero the WHOLE AST and assert
                 * zero live nodes (no-op under ASSERTS=0) - the ZERO-AST
                 * proof; the VM runs without the tree. */
                vm_ast_teardown(root, prog);
                vm_run(prog);
            } else {
                root->eval(nullptr);
            }
        }

    } catch (const SyntaxErrorEx &caught) {

        /* An "unexpected EOF" syntax error carries the EOF sentinel; point it
         * just past the last real token so the caret lands at end-of-input.
         * (Copy first - loc adjusts, the other fields are const.) */
        SyntaxErrorEx e = caught;
        if (e.tok == &invalid_tok && !tokens.empty()) {
            e.loc_start = tokens.back().loc + 1;
            e.loc_end = tokens.back().loc + 2;
        }

        format_exception(cerr, e, lines, src_name);
        return 1;

    } catch (const Exception &e) {

        format_exception(cerr, e, lines, src_name);
        return 1;
    }

    jit_cache_audit_report();   /* no-op unless MYLANG_CACHEAUDIT=1 */
    return 0;
}
