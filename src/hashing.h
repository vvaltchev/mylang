/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Hash combiners for the deep, by-value hash() of containers and structs.
 * Smarter than a naive xor (which cancels duplicates and ignores order).
 *
 * Everything here is built on the SplitMix64 *finalizer* - a public-domain
 * (CC0) bit-avalanche by Sebastiano Vigna (splitmix64.c at
 * https://prng.di.unimi.it/ dedicates it to the public domain). Magic numbers
 * used: SplitMix64's two finalizer constants, and the golden-ratio constant
 * 0x9e3779b97f4a7c15 (= 2^64 / phi). Both are public-domain facts, not
 * copyrightable. The combiners themselves are original - written from scratch
 * around that finalizer (see CLAUDE.md's policy on third-party code).
 *
 *   - hash_combine:   order-DEPENDENT, for SEQUENCES - arrays and struct fields
 *                     (declaration order matters);
 *   - hash_unordered: order-INDEPENDENT (commutative) for DICTS - their
 *                     iteration order is unspecified, so two equal dicts built
 *                     in a different order MUST hash equal; the SplitMix64
 *                     avalanche keeps the commutative accumulate from being a
 *                     weak xor/add of raw hashes.
 *
 * Distinct per-kind salts keep `[1,2]`, `struct {1,2}` and a dict from
 * colliding when they happen to share leaf hashes.
 */

/* SplitMix64 finalizer (public domain): a strong avalanche of one word */
inline size_t hash_mix(size_t z)
{
    uint64_t x = static_cast<uint64_t>(z);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return static_cast<size_t>(x);
}

/*
 * Order-dependent combine: fold `h` into the running sequence hash and
 * avalanche it. Order-dependent because `seed` carries the whole prefix into
 * the next step; the golden-ratio constant decorrelates successive inputs.
 */
inline void hash_combine(size_t &seed, size_t h)
{
    seed = hash_mix(seed + static_cast<size_t>(0x9e3779b97f4a7c15ULL) + h);
}

/* order-independent (commutative) combine for dicts */
inline void hash_unordered(size_t &acc, size_t h)
{
    acc += hash_mix(h);
}

/* per-kind salts: distinct, self-documenting (ASCII of the kind), arbitrary */
constexpr size_t hash_salt_array  = static_cast<size_t>(0x41525259ULL); /*ARRY*/
constexpr size_t hash_salt_dict   = static_cast<size_t>(0x44494354ULL); /*DICT*/
constexpr size_t hash_salt_struct = static_cast<size_t>(0x53545243ULL); /*STRC*/
constexpr size_t hash_salt_none   = static_cast<size_t>(0x4E4F4E45ULL); /*NONE*/
