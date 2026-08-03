/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * The `.myv` stored-bytecode reader/writer (plans/myv-serializer.md).
 *
 * ONLY the VM image is stored - never native code. The AOT tier re-runs at
 * LOAD time, so a `.myv` is portable across machines and across the JIT's
 * own evolution (serializing fragments would need a relocating linker: the
 * library and own-text addresses differ per run - a later increment).
 *
 * ⛔ THE PAIRING RULE: every record's WRITE and READ live ADJACENT here, in
 * the same order, so the two can never drift. ANY format change bumps
 * MYV_FORMAT_VERSION (serialize.h) in the SAME commit.
 *
 * Cross-references are indices, never pointers: `UniqueId *` -> a string
 * index (the loader re-interns), `Builtin` -> its NAME (re-resolved against
 * the builtin table; an unknown name is refused), struct defs and function
 * descriptors -> table indices. Values go through the recursive codec below,
 * which ABORTS (loudly) on a type it does not know - the writer never emits
 * a silently-lossy image.
 */

#include "serialize.h"
#include "defs.h"
#include "errors.h"
#include "evalvalue.h"
#include "bytecode.h"
#include "funcdesc.h"
#include "structtype.h"
#include "vm.h"
#include "eval.h"
#include "jit.h"
#include "env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

/* getcwd(), for the source reference's project root */
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace {

const char MYV_MAGIC[4] = { 'M', 'Y', 'L', 'V' };
constexpr uint32_t MYV_ENDIAN_MARK = 0x01020304u;

[[noreturn]] void bad_image(const char *what)
{
    throw Exception("MyvError", what);
}

/* ------------------------------------------------------------------ */
/* The SOURCE REFERENCE: CRC32 + path math (no <filesystem>: it would  */
/* need -lstdc++fs on the older GCCs the CI still builds with)         */
/* ------------------------------------------------------------------ */

/*
 * CRC-32 (the reflected form: polynomial 0xedb88320, init/final inverted) -
 * computed a bit at a time, so there is no 1KB table to carry. It runs once
 * per compile and once per image load over a source file, so speed is
 * irrelevant; what matters is that a source edited after `-c` is DETECTED.
 */
uint32_t crc32_of(const std::string &data)
{
    uint32_t c = 0xffffffffu;

    for (unsigned char b : data) {
        c ^= b;
        for (int i = 0; i < 8; i++) {
            /* subtract the low bit as a MASK: branch-free, and it keeps the
             * conditional xor obvious */
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
    }

    return ~c;
}

/* Read a whole file; false if it cannot be opened. */
bool read_file(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);

    if (!f)
        return false;

    out.assign((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
    return true;
}

/* '/' is accepted as a separator by both platforms; Windows also takes '\'. */
bool is_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool is_absolute(const std::string &p)
{
    if (p.empty())
        return false;
#ifdef _WIN32
    if (p.size() >= 3 && p[1] == ':' && is_sep(p[2]))
        return true;                      /* C:\... or C:/... */
    return is_sep(p[0]);                  /* \\server\share, \abs */
#else
    return p[0] == '/';
#endif
}

std::string cwd_path()
{
    /* PATH_MAX is not portable (and Hurd has none), so grow until it fits -
     * but only while the failure IS "too small" (ERANGE); any other errno is
     * permanent and would just spin through every size. */
    for (size_t cap = 512; cap <= (1u << 16); cap *= 2) {
        std::vector<char> buf(cap);
        errno = 0;
#ifdef _WIN32
        if (_getcwd(buf.data(), static_cast<int>(cap)))
#else
        if (getcwd(buf.data(), cap))
#endif
            return std::string(buf.data());
        if (errno != ERANGE)
            break;
    }
    return std::string();   /* no root: the source's own dir is used */
}

std::string join_path(const std::string &dir, const std::string &rel)
{
    if (dir.empty())
        return rel;
    if (is_sep(dir.back()))
        return dir + rel;
    return dir + "/" + rel;
}

/*
 * Resolve + VERIFY the source of an image being loaded, per the reference and
 * the load options. The rules (README / plans/myv-serializer.md):
 *  - `--source ROOT` given: look ONLY under it (`ROOT/rel`) - an explicit
 *    root that does not have the file is a mistake worth reporting, not a
 *    reason to silently fall back to a stale absolute path.
 *  - else: the stored absolute path, then the stored `root/rel` (identical
 *    by construction, but explicit - and the pair is what a future relocation
 *    scheme would key on).
 * A file that is THERE but whose size/CRC32 differs is REFUSED (warning, no
 * text) unless `force`, because a caret drawn on the wrong text is worse than
 * no caret. A file that is simply ABSENT is silent: shipping an image without
 * its source is a supported, ordinary thing to do.
 */
void resolve_source(const MyvSourceRef &ref, const MyvLoadOpts &opts,
                    MyvSource &out)
{
    if (ref.rel.empty() && ref.abs.empty())
        return;                           /* --strip-source: no reference */

    out.name = !ref.rel.empty() ? ref.rel : ref.abs;

    std::vector<std::string> cands;

    if (!opts.root.empty()) {
        if (!ref.rel.empty())
            cands.push_back(join_path(opts.root, ref.rel));
    } else {
        if (!ref.abs.empty())
            cands.push_back(ref.abs);
        if (!ref.root.empty() && !ref.rel.empty()) {
            const std::string p = join_path(ref.root, ref.rel);
            if (cands.empty() || p != cands.front())
                cands.push_back(p);
        }
    }

    std::string text;
    std::string found;

    for (const std::string &c : cands)
        if (read_file(c, text)) { found = c; break; }

    if (found.empty())
        return;                           /* absent: plain errors, no fuss */

    /* `out.name` stays the RELATIVE path (set above), not `found`: it is what
     * a compiler prints, it is machine-independent, and it makes an image's
     * error output byte-identical to the same error from the source. Which
     * copy was read only matters when it MISMATCHES - the warning says so. */

    if (text.size() != ref.size || crc32_of(text) != ref.crc) {

        out.warning = "warning: '" + found + "' has changed since the image "
                      "was compiled";

        if (!opts.force) {
            out.warning += " - ignoring it (pass -f to use it anyway)";
            return;                       /* no text: no misplaced carets */
        }

        out.warning += " - using it anyway (-f): carets may be misplaced";
    }

    /* Split exactly as mylang.cpp's read_script does (getline semantics: a
     * trailing newline does NOT add an empty last line), and drop a CR so a
     * CRLF checkout renders the same carets as an LF one. */
    std::string line;

    for (char ch : text) {
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            out.lines.push_back(line);
            line.clear();
        } else {
            line.push_back(ch);
        }
    }

    if (!line.empty()) {
        if (line.back() == '\r')
            line.pop_back();
        out.lines.push_back(line);
    }
}

/* ------------------------------------------------------------------ */
/* Primitive writer / reader (the pairing rule starts here)             */
/* ------------------------------------------------------------------ */

struct Writer {
    std::string buf;
    /* the string TABLE: deduped, index-stable (insertion order) */
    std::map<std::string, uint32_t> str_ids;
    std::vector<std::string> strs;
    /* index maps for the cross-referenced tables */
    std::map<const StructTypeDef *, uint32_t> struct_ids;
    std::map<const FuncDescriptor *, uint32_t> desc_ids;

    void u8v(uint8_t v) { buf.push_back(static_cast<char>(v)); }
    void u32v(uint32_t v)
    {
        for (int i = 0; i < 4; i++)
            u8v(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
    void i64v(int64_t v)
    {
        const uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; i++)
            u8v(static_cast<uint8_t>((u >> (8 * i)) & 0xff));
    }
    void f64v(double v)
    {
        uint64_t u;
        static_assert(sizeof(u) == sizeof(v), "double is not 8 bytes");
        memcpy(&u, &v, sizeof(u));
        i64v(static_cast<int64_t>(u));
    }
    void boolv(bool b) { u8v(b ? 1 : 0); }
    void locv(const Loc &l) { u32v(static_cast<uint32_t>(l.line));
                              u32v(static_cast<uint32_t>(l.col)); }
    void raw(const std::string &s) { u32v(static_cast<uint32_t>(s.size()));
                                     buf += s; }

    /* a STRING-TABLE index (the loader re-interns; null uid -> ~0u) */
    uint32_t sid(const std::string &s)
    {
        auto it = str_ids.find(s);
        if (it != str_ids.end())
            return it->second;
        const uint32_t id = static_cast<uint32_t>(strs.size());
        strs.push_back(s);
        str_ids.emplace(s, id);
        return id;
    }
    void uidv(const UniqueId *u)
    {
        u32v(u ? sid(std::string(u->val)) : 0xffffffffu);
    }
    void strv(const std::string &s) { u32v(sid(s)); }
};

struct Reader {
    const std::string &buf;
    size_t p = 0;
    std::vector<const UniqueId *> uids;      /* the interned string table */
    std::vector<std::string> strs;
    std::vector<StructTypeDef *> structs;
    std::vector<FuncDescriptor *> descs;

    explicit Reader(const std::string &b) : buf(b) { }

    void need(size_t n) const
    {
        if (p + n > buf.size())
            bad_image("corrupt or incompatible .myv (truncated)");
    }
    uint8_t u8v() { need(1); return static_cast<uint8_t>(buf[p++]); }
    uint32_t u32v()
    {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(buf[p + i]))
                 << (8 * i);
        p += 4;
        return v;
    }
    int64_t i64v()
    {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(buf[p + i]))
                 << (8 * i);
        p += 8;
        return static_cast<int64_t>(v);
    }
    double f64v()
    {
        const int64_t i = i64v();
        double d;
        uint64_t u = static_cast<uint64_t>(i);
        memcpy(&d, &u, sizeof(d));
        return d;
    }
    bool boolv() { return u8v() != 0; }
    Loc locv()
    {
        const uint32_t line = u32v(), col = u32v();
        return Loc(static_cast<int>(line), static_cast<int>(col));
    }
    std::string raw()
    {
        const uint32_t n = u32v();
        need(n);
        std::string s = buf.substr(p, n);
        p += n;
        return s;
    }
    const UniqueId *uidv()
    {
        const uint32_t id = u32v();
        if (id == 0xffffffffu)
            return nullptr;
        if (id >= uids.size())
            bad_image("corrupt .myv (string index)");
        return uids[id];
    }
    const std::string &strv()
    {
        const uint32_t id = u32v();
        if (id >= strs.size())
            bad_image("corrupt .myv (string index)");
        return strs[id];
    }
    uint32_t idx(size_t limit, const char *what)
    {
        const uint32_t v = u32v();
        if (v != 0xffffffffu && v >= limit)
            bad_image(what);
        return v;
    }
};

/* ------------------------------------------------------------------ */
/* The VALUE codec (recursive; consts, literal_objs, struct consts)     */
/* ------------------------------------------------------------------ */

enum class VTag : uint8_t {
    none = 0, boolean, integer, flt, str, arr, dict, strct, structtype, func
};

void write_value(Writer &w, const EvalValue &v);
EvalValue read_value(Reader &r);

void write_array(Writer &w, const SharedArrayObj &a)
{
    const auto kind = a.skind();
    w.u8v(static_cast<uint8_t>(kind));
    w.boolv(a.is_readonly());
    const size_type n = a.size();
    w.u32v(static_cast<uint32_t>(n));
    /* Slices and offsets are NOT stored: a stored array is materialized as
     * its own storage (an on-disk const is a fresh value at load, exactly
     * as the in-memory const pool holds a self-contained value). */
    switch (kind) {
    case SharedArrayObj::Storage::ints:
        for (size_type i = 0; i < n; i++)
            w.i64v(a.flat_ints()[a.offset() + i]);
        break;
    case SharedArrayObj::Storage::floats:
        for (size_type i = 0; i < n; i++)
            w.f64v(a.flat_floats()[a.offset() + i]);
        break;
    case SharedArrayObj::Storage::bools:
        for (size_type i = 0; i < n; i++)
            w.boolv(a.flat_bools()[a.offset() + i] != 0);
        break;
    case SharedArrayObj::Storage::strs:
        for (size_type i = 0; i < n; i++)
            w.raw(std::string(a.flat_strs()[a.offset() + i].get_view()));
        break;
    case SharedArrayObj::Storage::structs: {
        const auto &sv = a.flat_structs();
        auto it = w.struct_ids.find(sv.def);
        if (it == w.struct_ids.end())
            bad_image("unserializable value (struct-array def)");
        w.u32v(it->second);
        w.u32v(static_cast<uint32_t>(sv.stride));
        for (size_type i = 0; i < n; i++)
            w.buf.append(&sv.buf[(a.offset() + i) * sv.stride], sv.stride);
        break;
    }
    default:                                  /* general */
        for (size_type i = 0; i < n; i++)
            write_value(w, a.get_view()[a.offset() + i].get());
        break;
    }
}

EvalValue read_array(Reader &r)
{
    const auto kind = static_cast<SharedArrayObj::Storage>(r.u8v());
    const bool ro = r.boolv();
    const uint32_t n = r.u32v();
    SharedArrayObj out;
    switch (kind) {
    case SharedArrayObj::Storage::ints: {
        std::vector<int_type> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; i++)
            v.push_back(static_cast<int_type>(r.i64v()));
        out = SharedArrayObj(std::move(v));
        break;
    }
    case SharedArrayObj::Storage::floats: {
        std::vector<float_type> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; i++)
            v.push_back(static_cast<float_type>(r.f64v()));
        out = SharedArrayObj(std::move(v));
        break;
    }
    case SharedArrayObj::Storage::bools: {
        std::vector<unsigned char> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; i++)
            v.push_back(r.boolv() ? 1 : 0);
        out = SharedArrayObj(std::move(v));
        break;
    }
    case SharedArrayObj::Storage::strs: {
        std::vector<SharedStr> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; i++)
            v.push_back(SharedStr(r.raw()));
        out = SharedArrayObj(std::move(v));
        break;
    }
    case SharedArrayObj::Storage::structs: {
        const uint32_t di = r.idx(r.structs.size(), "corrupt .myv (struct)");
        const uint32_t stride = r.u32v();
        const size_t bytes = static_cast<size_t>(n) * stride;
        r.need(bytes);
        std::vector<char> b(r.buf.begin() + static_cast<long>(r.p),
                            r.buf.begin() + static_cast<long>(r.p + bytes));
        r.p += bytes;
        out = SharedArrayObj(SharedArrayObj::svec_type(
            std::move(b), r.structs[di], static_cast<int>(stride)));
        break;
    }
    default: {
        std::vector<LValue> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n; i++)
            v.emplace_back(read_value(r), false);
        out = SharedArrayObj(std::move(v));
        break;
    }
    }
    if (ro)
        out.set_readonly();
    return EvalValue(std::move(out));
}

void write_value(Writer &w, const EvalValue &v)
{
    if (v.get_type()->t == Type::t_none) {
        w.u8v(static_cast<uint8_t>(VTag::none));
    }
    else if (v.is<bool>()) {
        w.u8v(static_cast<uint8_t>(VTag::boolean));
        w.boolv(v.get<bool>());
    } else if (v.is<int_type>()) {
        w.u8v(static_cast<uint8_t>(VTag::integer));
        w.i64v(static_cast<int64_t>(v.get<int_type>()));
    } else if (v.is<float_type>()) {
        w.u8v(static_cast<uint8_t>(VTag::flt));
        w.f64v(static_cast<double>(v.get<float_type>()));
    } else if (v.is<SharedStr>()) {
        w.u8v(static_cast<uint8_t>(VTag::str));
        w.raw(std::string(v.get<SharedStr>().get_view()));
    } else if (v.is<SharedArrayObj>()) {
        w.u8v(static_cast<uint8_t>(VTag::arr));
        write_array(w, v.get_ref<SharedArrayObj>());
    } else if (v.is<intrusive_ptr<DictObject>>()) {
        w.u8v(static_cast<uint8_t>(VTag::dict));
        const DictObject &d = *v.get<intrusive_ptr<DictObject>>();
        w.boolv(d.is_readonly());
        w.boolv(d.get_has_default());
        if (d.get_has_default())
            write_value(w, d.get_default());
        w.u32v(static_cast<uint32_t>(d.get_ref().size()));
        for (const auto &kv : d.get_ref()) {
            write_value(w, kv.first);
            write_value(w, kv.second.get());
        }
    } else if (v.is<intrusive_ptr<StructObject>>()) {
        w.u8v(static_cast<uint8_t>(VTag::strct));
        const StructObject &so = *v.get<intrusive_ptr<StructObject>>();
        auto it = w.struct_ids.find(so.def);
        if (it == w.struct_ids.end())
            bad_image("unserializable value (struct def)");
        w.u32v(it->second);
        w.boolv(so.is_readonly());
        const bool pod = so.def->layout == StructTypeDef::Layout::pod;
        w.boolv(pod);
        if (pod) {
            w.u32v(static_cast<uint32_t>(so.bytes.size()));
            w.buf.append(so.bytes.data(), so.bytes.size());
        } else {
            w.u32v(static_cast<uint32_t>(so.fields.size()));
            for (const LValue &f : so.fields)
                write_value(w, f.get());
        }
    } else if (v.is<StructTypeDef *>()) {
        w.u8v(static_cast<uint8_t>(VTag::structtype));
        auto it = w.struct_ids.find(v.get<StructTypeDef *>());
        if (it == w.struct_ids.end())
            bad_image("unserializable value (struct type)");
        w.u32v(it->second);
    } else if (v.is<intrusive_ptr<FuncObject>>()) {
        /* A const-pool FuncObject is capture-free by construction (a
         * capturing closure is built at runtime by MakeClosureV). */
        const FuncObject &fo = *v.get<intrusive_ptr<FuncObject>>();
        if (!fo.capture_slots.empty())
            bad_image("unserializable value (capturing closure in a pool)");
        auto it = w.desc_ids.find(fo.func);
        if (it == w.desc_ids.end())
            bad_image("unserializable value (func descriptor)");
        w.u8v(static_cast<uint8_t>(VTag::func));
        w.u32v(it->second);
    } else {
        /* The loud abort: an image is never silently lossy. */
        bad_image("unserializable value type in a chunk pool");
    }
}

EvalValue read_value(Reader &r)
{
    switch (static_cast<VTag>(r.u8v())) {
    case VTag::none:    return EvalValue();
    case VTag::boolean: return EvalValue(r.boolv());
    case VTag::integer: return EvalValue(static_cast<int_type>(r.i64v()));
    case VTag::flt:     return EvalValue(static_cast<float_type>(r.f64v()));
    case VTag::str:     return EvalValue(SharedStr(r.raw()));
    case VTag::arr:     return read_array(r);
    case VTag::dict: {
        const bool ro = r.boolv();
        auto d = make_intrusive<DictObject>();
        if (r.boolv())
            d->set_default(read_value(r));
        const uint32_t n = r.u32v();
        for (uint32_t i = 0; i < n; i++) {
            EvalValue k = read_value(r);
            EvalValue val = read_value(r);
            d->get_ref().emplace(std::move(k), LValue(std::move(val), false));
        }
        if (ro)
            d->set_readonly();
        return EvalValue(std::move(d));
    }
    case VTag::strct: {
        const uint32_t di = r.idx(r.structs.size(), "corrupt .myv (struct)");
        const bool ro = r.boolv();
        const bool pod = r.boolv();
        auto so = make_intrusive<StructObject>();
        so->def = r.structs[di];
        if (pod) {
            const uint32_t n = r.u32v();
            r.need(n);
            so->bytes.assign(r.buf.begin() + static_cast<long>(r.p),
                             r.buf.begin() + static_cast<long>(r.p + n));
            r.p += n;
        } else {
            const uint32_t n = r.u32v();
            so->fields.reserve(n);
            for (uint32_t i = 0; i < n; i++)
                so->fields.emplace_back(read_value(r), false);
        }
        if (ro)
            so->set_readonly();
        return EvalValue(std::move(so));
    }
    case VTag::structtype: {
        const uint32_t di = r.idx(r.structs.size(), "corrupt .myv (struct)");
        return EvalValue(r.structs[di]);
    }
    case VTag::func: {
        const uint32_t fi = r.idx(r.descs.size(), "corrupt .myv (descriptor)");
        /* a pool FuncObject is capture-free (asserted on write), so the
         * root ctx it links to is the one the run installs */
        return EvalValue(make_intrusive<FuncObject>(r.descs[fi], nullptr));
    }
    }
    bad_image("corrupt .myv (value tag)");
}

/* ------------------------------------------------------------------ */
/* Small shared records                                                 */
/* ------------------------------------------------------------------ */

void write_operand(Writer &w, const Operand &o)
{
    w.boolv(o.is_lit);
    w.u8v(static_cast<uint8_t>(o.lit_kind));
    w.u32v(static_cast<uint32_t>(o.slot));
    if (o.is_lit && o.lit_kind == Operand::LitKind::f)
        w.f64v(static_cast<double>(o.flit));
    else
        w.i64v(static_cast<int64_t>(o.lit));
}

Operand read_operand(Reader &r)
{
    Operand o;
    o.is_lit = r.boolv();
    o.lit_kind = static_cast<Operand::LitKind>(r.u8v());
    o.slot = static_cast<int>(static_cast<int32_t>(r.u32v()));
    if (o.is_lit && o.lit_kind == Operand::LitKind::f)
        o.flit = static_cast<float_type>(r.f64v());
    else
        o.lit = static_cast<int_type>(r.i64v());
    return o;
}

void write_arglocs(Writer &w, const std::vector<ArgLoc> &v)
{
    w.u32v(static_cast<uint32_t>(v.size()));
    for (const auto &a : v) { w.locv(a.start); w.locv(a.end); }
}

std::vector<ArgLoc> read_arglocs(Reader &r)
{
    const uint32_t n = r.u32v();
    std::vector<ArgLoc> v;
    v.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        ArgLoc a;
        a.start = r.locv();
        a.end = r.locv();
        v.push_back(a);
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Chunks                                                               */
/* ------------------------------------------------------------------ */

/* dev-only byte accounting (env MYLANG_MYVSTATS=1): which sections the
 * image's bytes actually go to - the ROI surface for shrinking the format. */
void myv_stat(const char *what, size_t bytes)
{
    /* env_get, not getenv: MSVC deprecates getenv and the build is
     * warnings-as-errors on all three compilers (CLAUDE.md) */
    static bool on = env_get("MYLANG_MYVSTATS").has_value();
    if (on)
        fprintf(stderr, "MYVSTAT %-22s %8zu\n", what, bytes);
}

void myv_stat_ext(const char *what, size_t bytes) { myv_stat(what, bytes); }

void write_chunk(Writer &w, const Chunk &c)
{
    size_t cm = w.buf.size();
    const auto ct = [&](const char *what) {
        myv_stat_ext(what, w.buf.size() - cm);
        cm = w.buf.size();
    };
    /* code: FIELD-WISE (Instr has padding whose content is indeterminate) */
    w.u32v(static_cast<uint32_t>(c.code.size()));
    for (const Instr &in : c.code) {
        w.u8v(static_cast<uint8_t>(in.op));
        w.u8v(static_cast<uint8_t>(in.aop));
        w.u8v(in.opflags);
        w.u32v(static_cast<uint32_t>(in.target));
        w.u32v(static_cast<uint32_t>(in.target2));
        w.i64v(in.pa);
        w.i64v(in.pb);
    }
    ct("  code");
    w.u32v(static_cast<uint32_t>(c.slot_count));
    w.u32v(static_cast<uint32_t>(c.n_temps));
    w.u32v(static_cast<uint32_t>(c.n_dict_iters));
    w.u32v(static_cast<uint32_t>(c.n_dyn_iters));

    w.u32v(static_cast<uint32_t>(c.ref_slots.size()));
    for (int32_t s : c.ref_slots)
        w.u32v(static_cast<uint32_t>(s));

    w.u32v(static_cast<uint32_t>(c.consts.size()));
    for (const EvalValue &v : c.consts)
        write_value(w, v);

    ct("  consts+refslots");
    w.u32v(static_cast<uint32_t>(c.locs.size()));
    for (const auto &l : c.locs) {
        w.u32v(l.pc); w.locv(l.start); w.locv(l.end);
    }

    ct("  locs");
    w.u32v(static_cast<uint32_t>(c.inline_frames.size()));
    for (const auto &f : c.inline_frames) {
        w.strv(f.callee_name);
        w.u32v(static_cast<uint32_t>(f.params.size()));
        for (const auto &p : f.params)
            w.strv(p);
        w.locv(f.call_site);
        w.u32v(static_cast<uint32_t>(f.parent));
    }
    w.u32v(static_cast<uint32_t>(c.inline_ctxs.size()));
    for (const auto &e : c.inline_ctxs) {
        w.u32v(e.pc); w.u32v(static_cast<uint32_t>(e.frame));
    }

    ct("  inline_frames");
    w.u32v(static_cast<uint32_t>(c.member_keys.size()));
    for (const auto &m : c.member_keys) {
        write_value(w, m.memId);
        w.uidv(m.memUid);
        w.boolv(m.optional);
        w.locv(m.mstart); w.locv(m.mend);
        w.locv(m.bstart); w.locv(m.bend);
        auto it = w.struct_ids.find(m.bake_def);
        w.u32v(m.bake_def && it != w.struct_ids.end() ? it->second
                                                      : 0xffffffffu);
        w.u32v(static_cast<uint32_t>(m.bake_slot));
    }

    ct("  member_keys");
    w.u32v(static_cast<uint32_t>(c.boxed_ops.size()));
    for (const auto &b : c.boxed_ops) {
        w.u32v(static_cast<uint32_t>(b.target));
        w.u8v(static_cast<uint8_t>(b.aop));
        write_operand(w, b.a);
        write_operand(w, b.b);
        w.locv(b.start); w.locv(b.end);
    }

    w.u32v(static_cast<uint32_t>(c.ctor_plans.size()));
    for (const auto &p : c.ctor_plans) {
        w.u32v(static_cast<uint32_t>(p.f.size()));
        for (const auto &f : p.f) {
            w.u32v(static_cast<uint32_t>(f.off));
            w.u32v(static_cast<uint32_t>(f.src));
            w.u8v(f.act);
        }
    }

    w.u32v(static_cast<uint32_t>(c.incdec_sites.size()));
    for (const auto &s : c.incdec_sites) {
        w.locv(s.lstart); w.locv(s.lend);
        w.locv(s.istart); w.locv(s.iend);
        write_value(w, s.memId);
        w.uidv(s.memUid);
    }

    const auto wsteps = [&](const std::vector<Chunk::ChainStep> &v) {
        w.u32v(static_cast<uint32_t>(v.size()));
        for (const auto &s : v) {
            w.boolv(s.is_member);
            w.u32v(static_cast<uint32_t>(s.operand));
            w.locv(s.lstart); w.locv(s.lend);
        }
    };
    w.u32v(static_cast<uint32_t>(c.chain_steps.size()));
    for (const auto &v : c.chain_steps)
        wsteps(v);

    w.u32v(static_cast<uint32_t>(c.incdec_chains.size()));
    for (const auto &ch : c.incdec_chains) {
        wsteps(ch.steps);
        w.boolv(ch.tier2); w.boolv(ch.is_prefix);
        w.boolv(ch.allow_flat); w.boolv(ch.allow_pod);
        w.locv(ch.id_start); w.locv(ch.id_end);
        w.locv(ch.kstart); w.locv(ch.kend);
    }

    w.u32v(static_cast<uint32_t>(c.chain_locs.size()));
    for (const auto &v : c.chain_locs) {
        w.u32v(static_cast<uint32_t>(v.size()));
        for (const auto &pr : v) { w.locv(pr.first); w.locv(pr.second); }
    }

    w.u32v(static_cast<uint32_t>(c.catch_types.size()));
    for (const auto &v : c.catch_types) {
        w.u32v(static_cast<uint32_t>(v.size()));
        for (const auto &s : v)
            w.strv(s);
    }

    w.u32v(static_cast<uint32_t>(c.literal_objs.size()));
    for (const auto &lo : c.literal_objs) {
        write_value(w, lo.value);
        w.boolv(lo.immutable);
        w.u8v(static_cast<uint8_t>(lo.arr_hint));
        auto it = w.struct_ids.find(lo.arr_hint_struct);
        w.u32v(lo.arr_hint_struct && it != w.struct_ids.end()
                   ? it->second : 0xffffffffu);
    }

    w.u32v(static_cast<uint32_t>(c.closure_defs.size()));
    for (const FuncDescriptor *d : c.closure_defs) {
        auto it = w.desc_ids.find(d);
        if (it == w.desc_ids.end())
            bad_image("unserializable image (closure descriptor)");
        w.u32v(it->second);
    }
    w.u32v(static_cast<uint32_t>(c.struct_defs.size()));
    for (const StructTypeDef *d : c.struct_defs) {
        auto it = w.struct_ids.find(d);
        if (it == w.struct_ids.end())
            bad_image("unserializable image (struct def ref)");
        w.u32v(it->second);
    }

    w.u32v(static_cast<uint32_t>(c.boxed_ctors.size()));
    for (const auto &bc : c.boxed_ctors) {
        auto it = w.struct_ids.find(bc.def);
        w.u32v(it != w.struct_ids.end() ? it->second : 0xffffffffu);
        write_arglocs(w, bc.arg_locs);
    }

    w.u32v(static_cast<uint32_t>(c.emplace_sites.size()));
    for (const auto &es : c.emplace_sites) {
        auto it = w.struct_ids.find(es.def);
        w.u32v(it != w.struct_ids.end() ? it->second : 0xffffffffu);
        w.uidv(es.bname);
        w.locv(es.a0_start); w.locv(es.a0_end);
        write_arglocs(w, es.field_locs);
    }

    w.u32v(static_cast<uint32_t>(c.throws.size()));
    for (const auto &t : c.throws) {
        w.u8v(static_cast<uint8_t>(t.kind));
        w.locv(t.start); w.locv(t.end);
        w.uidv(t.name);
    }

    w.u32v(static_cast<uint32_t>(c.unpack_targets.size()));
    for (const auto &v : c.unpack_targets) {
        w.u32v(static_cast<uint32_t>(v.size()));
        for (int32_t s : v)
            w.u32v(static_cast<uint32_t>(s));
    }
    w.u32v(static_cast<uint32_t>(c.unpack_coerce.size()));
    for (const auto &v : c.unpack_coerce) {
        w.u32v(static_cast<uint32_t>(v.size()));
        for (unsigned char b : v)
            w.u8v(b);
    }

    ct("  pools(mid)");
    w.u32v(static_cast<uint32_t>(c.builtin_calls.size()));
    for (const auto &bc : c.builtin_calls) {
        w.uidv(bc.name);                 /* re-resolved at load */
        w.locv(bc.start); w.locv(bc.end);
        w.u8v(static_cast<uint8_t>(bc.arr_hint));
        write_arglocs(w, bc.args);
        w.uidv(bc.member);
    }

    w.u32v(static_cast<uint32_t>(c.call_sites.size()));
    for (const auto &cs : c.call_sites) {
        w.locv(cs.start); w.locv(cs.end);
        write_arglocs(w, cs.args);
        w.u8v(static_cast<uint8_t>(cs.arr_hint));
        w.u8v(static_cast<uint8_t>(cs.a0_form));
        w.u32v(static_cast<uint32_t>(cs.a0_kind));
        w.u32v(static_cast<uint32_t>(cs.a0_slot));
        w.u32v(static_cast<uint32_t>(cs.a0_operand));
        w.uidv(cs.a0_name);
    }

    ct("  builtin+callsites");
    /* slot_names: DEBUG only (-vd); kept so a loaded image dumps identically */
    w.u32v(static_cast<uint32_t>(c.slot_names.size()));
    for (const auto &s : c.slot_names)
        w.strv(s);
    ct("  slot_names");
}

void read_chunk(Reader &r, Chunk &c)
{
    const uint32_t ncode = r.u32v();
    c.code.resize(ncode);
    for (uint32_t i = 0; i < ncode; i++) {
        Instr &in = c.code[i];
        in.op = static_cast<OpCode>(r.u8v());
        in.aop = static_cast<Op>(r.u8v());
        in.opflags = r.u8v();
        in.target = static_cast<int>(static_cast<int32_t>(r.u32v()));
        in.target2 = static_cast<int>(static_cast<int32_t>(r.u32v()));
        in.pa = r.i64v();
        in.pb = r.i64v();
        if (static_cast<size_t>(in.op) >= static_cast<size_t>(OpCode::OpCount_))
            bad_image("corrupt .myv (opcode)");
    }
    c.slot_count = static_cast<int>(static_cast<int32_t>(r.u32v()));
    c.n_temps = static_cast<int>(static_cast<int32_t>(r.u32v()));
    c.n_dict_iters = static_cast<int>(static_cast<int32_t>(r.u32v()));
    c.n_dyn_iters = static_cast<int>(static_cast<int32_t>(r.u32v()));

    uint32_t n = r.u32v();
    c.ref_slots.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        c.ref_slots.push_back(static_cast<int32_t>(r.u32v()));

    n = r.u32v();
    c.consts.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        c.consts.push_back(read_value(r));

    n = r.u32v();
    c.locs.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::LocEntry l;
        l.pc = r.u32v();
        l.start = r.locv();
        l.end = r.locv();
        if (l.pc > c.code.size())
            bad_image("corrupt .myv (loc pc)");
        c.locs.push_back(l);
    }

    n = r.u32v();
    c.inline_frames.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::InlineFrame f;
        f.callee_name = r.strv();
        const uint32_t np = r.u32v();
        f.params.reserve(np);
        for (uint32_t j = 0; j < np; j++)
            f.params.push_back(r.strv());
        f.call_site = r.locv();
        f.parent = static_cast<int32_t>(r.u32v());
        c.inline_frames.push_back(std::move(f));
    }
    n = r.u32v();
    c.inline_ctxs.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::InlineEntry e;
        e.pc = r.u32v();
        e.frame = static_cast<int32_t>(r.u32v());
        c.inline_ctxs.push_back(e);
    }

    n = r.u32v();
    c.member_keys.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::MemberKey m;
        m.memId = read_value(r);
        m.memUid = r.uidv();
        m.optional = r.boolv();
        m.mstart = r.locv(); m.mend = r.locv();
        m.bstart = r.locv(); m.bend = r.locv();
        const uint32_t bd = r.idx(r.structs.size(), "corrupt .myv (bake def)");
        m.bake_def = bd == 0xffffffffu ? nullptr : r.structs[bd];
        m.bake_slot = static_cast<int>(static_cast<int32_t>(r.u32v()));
        c.member_keys.push_back(std::move(m));
    }

    n = r.u32v();
    c.boxed_ops.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::BoxedOp b;
        b.target = static_cast<int>(static_cast<int32_t>(r.u32v()));
        b.aop = static_cast<Op>(r.u8v());
        b.a = read_operand(r);
        b.b = read_operand(r);
        b.start = r.locv(); b.end = r.locv();
        c.boxed_ops.push_back(b);
    }

    n = r.u32v();
    c.ctor_plans.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::CtorPlan p;
        const uint32_t nf = r.u32v();
        p.f.reserve(nf);
        for (uint32_t j = 0; j < nf; j++) {
            Chunk::CtorPlanField f;
            f.off = static_cast<int32_t>(r.u32v());
            f.src = static_cast<int32_t>(r.u32v());
            f.act = r.u8v();
            p.f.push_back(f);
        }
        c.ctor_plans.push_back(std::move(p));
    }

    n = r.u32v();
    c.incdec_sites.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::IncDecSite s;
        s.lstart = r.locv(); s.lend = r.locv();
        s.istart = r.locv(); s.iend = r.locv();
        s.memId = read_value(r);
        s.memUid = r.uidv();
        c.incdec_sites.push_back(std::move(s));
    }

    const auto rsteps = [&]() {
        std::vector<Chunk::ChainStep> v;
        const uint32_t ns = r.u32v();
        v.reserve(ns);
        for (uint32_t j = 0; j < ns; j++) {
            Chunk::ChainStep s;
            s.is_member = r.boolv();
            s.operand = static_cast<int32_t>(r.u32v());
            s.lstart = r.locv(); s.lend = r.locv();
            v.push_back(s);
        }
        return v;
    };
    n = r.u32v();
    c.chain_steps.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        c.chain_steps.push_back(rsteps());

    n = r.u32v();
    c.incdec_chains.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::IncDecChain ch;
        ch.steps = rsteps();
        ch.tier2 = r.boolv(); ch.is_prefix = r.boolv();
        ch.allow_flat = r.boolv(); ch.allow_pod = r.boolv();
        ch.id_start = r.locv(); ch.id_end = r.locv();
        ch.kstart = r.locv(); ch.kend = r.locv();
        c.incdec_chains.push_back(std::move(ch));
    }

    n = r.u32v();
    c.chain_locs.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        std::vector<std::pair<Loc, Loc>> v;
        const uint32_t m = r.u32v();
        v.reserve(m);
        for (uint32_t j = 0; j < m; j++) {
            const Loc a = r.locv(), b = r.locv();
            v.emplace_back(a, b);
        }
        c.chain_locs.push_back(std::move(v));
    }

    n = r.u32v();
    c.catch_types.reserve(n);
    c.catch_uids.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        std::vector<std::string> v;
        std::vector<const UniqueId *> u;
        const uint32_t m = r.u32v();
        v.reserve(m); u.reserve(m);
        for (uint32_t j = 0; j < m; j++) {
            v.push_back(r.strv());
            u.push_back(UniqueId::get(v.back()));   /* the derived twin */
        }
        c.catch_types.push_back(std::move(v));
        c.catch_uids.push_back(std::move(u));
    }

    n = r.u32v();
    c.literal_objs.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::LiteralObjEntry lo;
        lo.value = read_value(r);
        lo.immutable = r.boolv();
        lo.arr_hint = static_cast<ArrHint>(r.u8v());
        const uint32_t si = r.idx(r.structs.size(), "corrupt .myv (hint)");
        lo.arr_hint_struct = si == 0xffffffffu ? nullptr : r.structs[si];
        c.literal_objs.push_back(std::move(lo));
    }

    n = r.u32v();
    c.closure_defs.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        c.closure_defs.push_back(
            r.descs[r.idx(r.descs.size(), "corrupt .myv (closure)")]);
    n = r.u32v();
    c.struct_defs.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        c.struct_defs.push_back(
            r.structs[r.idx(r.structs.size(), "corrupt .myv (struct)")]);

    n = r.u32v();
    c.boxed_ctors.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::BoxedCtor bc;
        const uint32_t di = r.idx(r.structs.size(), "corrupt .myv (ctor)");
        bc.def = di == 0xffffffffu ? nullptr : r.structs[di];
        bc.arg_locs = read_arglocs(r);
        c.boxed_ctors.push_back(std::move(bc));
    }

    n = r.u32v();
    c.emplace_sites.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::EmplaceSite es;
        const uint32_t di = r.idx(r.structs.size(), "corrupt .myv (emplace)");
        es.def = di == 0xffffffffu ? nullptr : r.structs[di];
        es.bname = r.uidv();
        es.a0_start = r.locv(); es.a0_end = r.locv();
        es.field_locs = read_arglocs(r);
        c.emplace_sites.push_back(std::move(es));
    }

    n = r.u32v();
    c.throws.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::ThrowSite t;
        t.kind = static_cast<Chunk::ThrowKind>(r.u8v());
        t.start = r.locv(); t.end = r.locv();
        t.name = r.uidv();
        c.throws.push_back(t);
    }

    n = r.u32v();
    c.unpack_targets.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        std::vector<int32_t> v;
        const uint32_t m = r.u32v();
        v.reserve(m);
        for (uint32_t j = 0; j < m; j++)
            v.push_back(static_cast<int32_t>(r.u32v()));
        c.unpack_targets.push_back(std::move(v));
    }
    n = r.u32v();
    c.unpack_coerce.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        std::vector<unsigned char> v;
        const uint32_t m = r.u32v();
        v.reserve(m);
        for (uint32_t j = 0; j < m; j++)
            v.push_back(r.u8v());
        c.unpack_coerce.push_back(std::move(v));
    }

    n = r.u32v();
    c.builtin_calls.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::BuiltinCall bc;
        bc.name = r.uidv();
        if (!bc.name || !vm_lookup_builtin(bc.name, bc.builtin))
            bad_image("corrupt or incompatible .myv (unknown builtin)");
        bc.start = r.locv(); bc.end = r.locv();
        bc.arr_hint = static_cast<ArrHint>(r.u8v());
        bc.args = read_arglocs(r);
        bc.member = r.uidv();
        c.builtin_calls.push_back(std::move(bc));
    }

    n = r.u32v();
    c.call_sites.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        Chunk::CallSite cs;
        cs.start = r.locv(); cs.end = r.locv();
        cs.args = read_arglocs(r);
        cs.arr_hint = static_cast<ArrHint>(r.u8v());
        cs.a0_form = static_cast<Chunk::CallSite::A0>(r.u8v());
        cs.a0_kind = static_cast<unsigned char>(r.u32v());
        cs.a0_slot = static_cast<int32_t>(r.u32v());
        cs.a0_operand = static_cast<int32_t>(r.u32v());
        cs.a0_name = r.uidv();
        c.call_sites.push_back(std::move(cs));
    }

    n = r.u32v();
    c.slot_names.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        c.slot_names.push_back(r.strv());
}

}  /* anon namespace */

/* ------------------------------------------------------------------ */
/* The whole image                                                      */
/* ------------------------------------------------------------------ */

void myv_write(const VmProgram &prog, const std::string &path,
               const MyvSourceRef &src)
{
    Writer w;
    size_t mark = 0;
    const auto tick = [&](const char *what) {
        myv_stat(what, w.buf.size() - mark);
        mark = w.buf.size();
    };

    /* Index tables FIRST: values and pools reference defs/descs by index,
     * and the writer must be able to resolve any of them (dependency order
     * is guaranteed by construction - inline struct fields only reference
     * EARLIER defs, a parser invariant). */
    for (size_t i = 0; i < prog.structs.size(); i++)
        w.struct_ids.emplace(prog.structs[i].get(),
                             static_cast<uint32_t>(i));
    for (size_t i = 0; i < prog.funcs.size(); i++)
        w.desc_ids.emplace(prog.funcs[i].get(), static_cast<uint32_t>(i));

    /* structs */
    w.u32v(static_cast<uint32_t>(prog.structs.size()));
    for (const auto &sd : prog.structs) {
        w.uidv(sd->name);
        w.u32v(static_cast<uint32_t>(sd->fields.size()));
        for (const FieldDef &f : sd->fields) {
            w.uidv(f.name);
            w.u8v(static_cast<uint8_t>(f.kind));
            w.uidv(f.struct_ty);
            auto it = w.struct_ids.find(f.struct_def);
            w.u32v(f.struct_def && it != w.struct_ids.end() ? it->second
                                                            : 0xffffffffu);
            w.boolv(f.is_opt);
            w.u32v(static_cast<uint32_t>(f.slot));
        }
        w.u32v(static_cast<uint32_t>(sd->consts.size()));
        for (const auto &kv : sd->consts) {
            w.uidv(kv.first);
            write_value(w, kv.second);
        }
    }

    tick("structs");

    /* descriptors (their chunks follow, same order) */
    w.u32v(static_cast<uint32_t>(prog.funcs.size()));
    for (const auto &d : prog.funcs) {
        w.uidv(d->name);
        w.strv(d->display_name);
        w.u32v(static_cast<uint32_t>(d->params.size()));
        for (const auto &p : d->params) {
            w.uidv(p.name);
            w.boolv(p.opt); w.boolv(p.cnst); w.boolv(p.dyn_mod);
            w.u8v(static_cast<uint8_t>(p.decl_type));
        }
        w.u32v(static_cast<uint32_t>(d->captures.size()));
        for (const auto &cp : d->captures) {
            w.uidv(cp.name);
            w.u8v(static_cast<uint8_t>(cp.kind));
            w.u32v(static_cast<uint32_t>(cp.slot));
        }
        w.boolv(d->resolved);
        w.u32v(static_cast<uint32_t>(d->frame_size));
        w.u32v(static_cast<uint32_t>(d->min_args));
        w.boolv(d->explicit_pure); w.boolv(d->effective_pure);
        w.boolv(d->cache_results); w.boolv(d->pure_ctx);
        w.boolv(d->is_template_base);
        w.boolv(d->fast_bind);
        /* has a chunk? (a dead template base has none) */
        w.boolv(d->vm_chunk != nullptr);
    }

    tick("descriptors");

    /* chunks: the root, then one per descriptor that has one */
    write_chunk(w, prog.root);
    tick("chunk:root");
    for (const auto &d : prog.funcs)
        if (d->vm_chunk) {
            write_chunk(w, *static_cast<const Chunk *>(d->vm_chunk));
            tick("chunk:func");
        }

    /* globals */
    w.u32v(static_cast<uint32_t>(prog.global_slot_reassigned.size()));
    for (char c : prog.global_slot_reassigned)
        w.u8v(static_cast<uint8_t>(c));
    w.u32v(static_cast<uint32_t>(prog.root_slot_count));
    w.u32v(static_cast<uint32_t>(prog.global_func_names.size()));
    for (const UniqueId *u : prog.global_func_names)
        w.uidv(u);

    tick("globals");

    /* The header + string table go in FRONT: the body's writes populated
     * the table, so it can only be emitted now (the loader reads it first
     * and every later index resolves). */
    Writer h;
    h.buf.append(MYV_MAGIC, 4);
    h.u32v(MYV_FORMAT_VERSION);
    h.u32v(MYV_ENDIAN_MARK);
    h.i64v(static_cast<int64_t>(builtin_set_fingerprint()));

    /* the SOURCE REFERENCE (v2): where the text WAS + a CRC32 to prove the
     * file on disk is still it. NOT the text itself. Read side: myv_read. */
    const size_t ref_mark = h.buf.size();
    h.raw(src.root);
    h.raw(src.rel);
    h.raw(src.abs);
    h.u32v(src.crc);
    h.i64v(static_cast<int64_t>(src.size));
    const size_t ref_bytes = h.buf.size() - ref_mark;

    h.u32v(static_cast<uint32_t>(w.strs.size()));
    for (const std::string &s : w.strs)
        h.raw(s);

    myv_stat("header+strings", h.buf.size());
    myv_stat("source-ref", ref_bytes);
    myv_stat("TOTAL", h.buf.size() + w.buf.size());

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        throw Exception("MyvError", "cannot open the output file");
    f.write(h.buf.data(), static_cast<long>(h.buf.size()));
    f.write(w.buf.data(), static_cast<long>(w.buf.size()));
    if (!f)
        throw Exception("MyvError", "cannot write the .myv image");
}

MyvSourceRef myv_source_ref(const std::string &path)
{
    MyvSourceRef ref;
    std::string text;

    if (!read_file(path, text))
        return ref;                       /* empty ref: no reference stored */

    ref.crc = crc32_of(text);
    ref.size = text.size();
    ref.abs = is_absolute(path) ? path : join_path(cwd_path(), path);

    /*
     * The ROOT is the compile-time CWD when the source lives under it (the
     * usual `mylang -c samples/phonebook` run from the project root), else
     * the source's OWN directory. Either way `rel` ends up non-empty, which
     * is what lets `--source ROOT` relocate the image: a `rel` full of ".."
     * hops would not survive relocation, so we never build one.
     */
    const std::string cwd = cwd_path();
    const std::string under = join_path(cwd, "");

    if (!cwd.empty() && ref.abs.size() > under.size()
            && ref.abs.compare(0, under.size(), under) == 0) {

        ref.root = cwd;
        ref.rel = ref.abs.substr(under.size());

    } else {

        size_t slash = std::string::npos;

        for (size_t i = ref.abs.size(); i-- > 0; )
            if (is_sep(ref.abs[i])) { slash = i; break; }

        if (slash == std::string::npos) {
            ref.root = ".";
            ref.rel = ref.abs;
        } else {
            /* keep the root's leading separator when it IS the root dir */
            ref.root = ref.abs.substr(0, slash ? slash : 1);
            ref.rel = ref.abs.substr(slash + 1);
        }
    }

    return ref;
}

bool myv_is_image(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    char m[4] = { 0, 0, 0, 0 };
    f.read(m, 4);
    return f.gcount() == 4 && memcmp(m, MYV_MAGIC, 4) == 0;
}

VmProgram myv_read(const std::string &path, MyvSource &out_src,
                   const MyvLoadOpts &opts)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw Exception("MyvError", "cannot open the .myv file");
    std::string data((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    Reader r(data);

    r.need(4);
    if (memcmp(data.data(), MYV_MAGIC, 4) != 0)
        bad_image("not a .myv image (bad magic)");
    r.p = 4;
    if (r.u32v() != MYV_FORMAT_VERSION)
        bad_image("incompatible .myv version - recompile from source");
    if (r.u32v() != MYV_ENDIAN_MARK)
        bad_image("incompatible .myv endianness - recompile from source");
    if (static_cast<uint64_t>(r.i64v()) != builtin_set_fingerprint())
        bad_image("incompatible .myv (builtin set) - recompile from source");

    /* the SOURCE REFERENCE (write side: myv_write's header block) */
    MyvSourceRef ref;
    ref.root = r.raw();
    ref.rel = r.raw();
    ref.abs = r.raw();
    ref.crc = r.u32v();
    ref.size = static_cast<uint64_t>(r.i64v());

    out_src = MyvSource();
    resolve_source(ref, opts, out_src);

    /* strings -> interned uids */
    uint32_t n = r.u32v();
    r.strs.reserve(n);
    r.uids.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        r.strs.push_back(r.raw());
        r.uids.push_back(UniqueId::get(r.strs.back()));
    }

    VmProgram prog;

    /* structs: construct all first (so a field can reference any earlier
     * def), then wire + compute the layout - which is RECOMPUTED, never
     * stored (deterministic from the field kinds; storing it would only
     * add drift surface). */
    n = r.u32v();
    for (uint32_t i = 0; i < n; i++)
        prog.structs.push_back(std::unique_ptr<StructTypeDef>(
            new StructTypeDef()));
    for (uint32_t i = 0; i < n; i++)
        r.structs.push_back(prog.structs[i].get());
    for (uint32_t i = 0; i < n; i++) {
        StructTypeDef &sd = *prog.structs[i];
        sd.name = r.uidv();
        const uint32_t nf = r.u32v();
        sd.fields.reserve(nf);
        for (uint32_t j = 0; j < nf; j++) {
            FieldDef fd;
            fd.name = r.uidv();
            fd.kind = static_cast<FieldKind>(r.u8v());
            fd.struct_ty = r.uidv();
            const uint32_t si = r.idx(n, "corrupt .myv (field struct)");
            fd.struct_def = si == 0xffffffffu ? nullptr : r.structs[si];
            fd.is_opt = r.boolv();
            fd.slot = static_cast<int>(static_cast<int32_t>(r.u32v()));
            sd.fields.push_back(std::move(fd));
        }
        const uint32_t nc = r.u32v();
        sd.consts.reserve(nc);
        for (uint32_t j = 0; j < nc; j++) {
            const UniqueId *cn = r.uidv();
            sd.consts.emplace_back(cn, read_value(r));
        }
        sd.compute_layout();
    }

    /* descriptors: construct all, then fill (a chunk's closure_defs may
     * reference any of them) */
    n = r.u32v();
    std::vector<bool> has_chunk(n, false);
    for (uint32_t i = 0; i < n; i++)
        prog.funcs.push_back(std::unique_ptr<FuncDescriptor>(
            new FuncDescriptor()));
    for (uint32_t i = 0; i < n; i++)
        r.descs.push_back(prog.funcs[i].get());
    for (uint32_t i = 0; i < n; i++) {
        FuncDescriptor &d = *prog.funcs[i];
        d.name = r.uidv();
        d.display_name = r.strv();
        const uint32_t np = r.u32v();
        d.params.reserve(np);
        for (uint32_t j = 0; j < np; j++) {
            FuncDescriptor::ParamDesc p;
            p.name = r.uidv();
            p.opt = r.boolv(); p.cnst = r.boolv(); p.dyn_mod = r.boolv();
            p.decl_type = static_cast<DeclType>(r.u8v());
            d.params.push_back(p);
        }
        const uint32_t ncp = r.u32v();
        d.captures.reserve(ncp);
        for (uint32_t j = 0; j < ncp; j++) {
            FuncDescriptor::CaptureDesc cp;
            cp.name = r.uidv();
            cp.kind = static_cast<SymKind>(r.u8v());
            cp.slot = static_cast<int>(static_cast<int32_t>(r.u32v()));
            d.captures.push_back(cp);
        }
        d.resolved = r.boolv();
        d.frame_size = static_cast<int>(static_cast<int32_t>(r.u32v()));
        d.min_args = static_cast<int>(static_cast<int32_t>(r.u32v()));
        d.explicit_pure = r.boolv(); d.effective_pure = r.boolv();
        d.cache_results = r.boolv(); d.pure_ctx = r.boolv();
        d.is_template_base = r.boolv();
        d.fast_bind = r.boolv();
        has_chunk[i] = r.boolv();
        d.decl = nullptr;                      /* compile-only back-pointer */
    }

    /* chunks: root, then the descriptors' (same order as written) */
    read_chunk(r, prog.root);
    for (uint32_t i = 0; i < n; i++) {
        if (!has_chunk[i]) {
            prog.funcs[i]->vm_chunk_tried = true;   /* a dead template base */
            continue;
        }
        Chunk ck;
        read_chunk(r, ck);
        vm_install_func_chunk(prog.funcs[i].get(), std::move(ck));
    }

    uint32_t nre = r.u32v();
    prog.global_slot_reassigned.reserve(nre);
    for (uint32_t i = 0; i < nre; i++)
        prog.global_slot_reassigned.push_back(static_cast<char>(r.u8v()));

    prog.root_slot_count = static_cast<int>(static_cast<int32_t>(r.u32v()));
    n = r.u32v();
    prog.global_func_names.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        prog.global_func_names.push_back(r.uidv());

    /* the AOT native tier, re-run at LOAD over the WHOLE image (only the
     * VM image is stored) - the same two passes a fresh compile runs. */
    vm_jit_loaded_image(prog);
    return prog;
}
