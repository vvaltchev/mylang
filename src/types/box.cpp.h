/* SPDX-License-Identifier: BSD-2-Clause */

/*
 * This is NOT a header file: it is a C++ file in the form of a header,
 * just because it's faster to compile it this way (types.cpp includes it).
 *
 * TypeBox - a box of a scalar or a string, `box<int>` .. `box<str>`
 * (BoxObj, structtype.h; plans/class-and-box.md, step 3). A REFERENCE:
 * every copy of a box value shares the one cell, `==` is identity and the
 * hash its identity number (as for a class instance), so a box is a dict
 * key by identity. `*b` reads and writes the cell (box_load / box_store,
 * eval.cpp); nothing else does.
 */

class TypeBox : public TypeImpl<intrusive_ptr<BoxObj>> {

public:

    TypeBox() : TypeImpl<intrusive_ptr<BoxObj>>(Type::t_box) { }

    void eq(EvalValue &a, const EvalValue &b) override;
    void noteq(EvalValue &a, const EvalValue &b) override;
    size_t hash(const EvalValue &a) override;
    bool is_true(const EvalValue &a) override { return true; }
    string to_string(const EvalValue &a) override;
    EvalValue clone(const EvalValue &a) override;
    int_type use_count(const EvalValue &a) override;
    EvalValue intptr(const EvalValue &a) override;
};

static bool box_same(const EvalValue &a, const EvalValue &b)
{
    return b.is<intrusive_ptr<BoxObj>>()
        && a.get_ref<intrusive_ptr<BoxObj>>().get()
               == b.get_ref<intrusive_ptr<BoxObj>>().get();
}

void TypeBox::eq(EvalValue &a, const EvalValue &b)
{
    a = box_same(a, b);
}

void TypeBox::noteq(EvalValue &a, const EvalValue &b)
{
    a = !box_same(a, b);
}

size_t TypeBox::hash(const EvalValue &a)
{
    size_t seed = hash_salt_struct;
    hash_combine(seed, std::hash<std::string_view>()("box"));
    hash_combine(seed,
        std::hash<uint64_t>()(a.get_ref<intrusive_ptr<BoxObj>>()->ident));
    return seed;
}

string TypeBox::to_string(const EvalValue &a)
{
    return "box(" + a.get_ref<intrusive_ptr<BoxObj>>()->v.to_string_repr()
         + ")";
}

EvalValue TypeBox::clone(const EvalValue &a)
{
    /* a NEW box holding the same value: a new identity, mutable */
    auto copy = make_intrusive<BoxObj>(*a.get_ref<intrusive_ptr<BoxObj>>());
    copy->readonly = false;
    return intrusive_ptr<BoxObj>(copy);
}

int_type TypeBox::use_count(const EvalValue &a)
{
    return a.get_ref<intrusive_ptr<BoxObj>>().use_count();
}

EvalValue TypeBox::intptr(const EvalValue &a)
{
    return reinterpret_cast<int_type>(
        a.get_ref<intrusive_ptr<BoxObj>>().get());
}
