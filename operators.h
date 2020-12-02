/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

enum class Op : int {

    invalid     = 0,

    plus        = 1,
    minus       = 2,
    times       = 3,
    div         = 4,
    parenL      = 5,
    parenR      = 6,
    lt          = 7,
    gt          = 8,
    le          = 9,
    ge          = 10,
    semicolon   = 11,
    comma       = 12,
    mod         = 13,
    opnot       = 14,
    assign      = 15,
    eq          = 16,
    noteq       = 17,
    braceL      = 18,
    braceR      = 19,
};
