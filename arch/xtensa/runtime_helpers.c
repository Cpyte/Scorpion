/*
 * Scorpion / ESP32-S3 freestanding runtime helpers.
 *
 * The crosstool-NG toolchain ships libgcc only for the default windowed
 * ABI (and the psram variants) — there is no call0 multilib. Rather than
 * building a custom libgcc, Scorpion carries the handful of compiler-rt
 * style primitives the kernel actually needs. All are trivial, portable
 * C; the optimizer turns most of them into branchless code.
 */

#include <stddef.h>
#include <stdint.h>

/* ---- bit tricks ------------------------------------------------------ */

int __clzsi2(uint32_t x)
{
    int n = 0;
    if (x == 0) return 32;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8;  x <<= 8;  }
    if (!(x & 0xF0000000u)) { n += 4;  x <<= 4;  }
    if (!(x & 0xC0000000u)) { n += 2;  x <<= 2;  }
    if (!(x & 0x80000000u)) { n += 1; }
    return n;
}

/* ---- division / modulo (no hardware divider on LX7) ---------------- */

static uint32_t udivmod(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0;
    int s;

    if (d == 0) {
        if (rem) *rem = n;
        return 0xFFFFFFFFu;           /* divide-by-zero: deterministic */
    }

    /* Restoring division over the dividend's bit positions. A shifted
     * divisor that wrapped to 0 cannot divide anything and is skipped,
     * which keeps every intermediate inside 32 bits. */
    for (s = 31 - __clzsi2(n | 1u); s >= 0; s--) {
        uint32_t dd = d << s;
        if (dd != 0 && n >= dd) {
            n -= dd;
            q |= 1u << s;
        }
    }

    if (rem) *rem = n;
    return q;
}

uint32_t __udivsi3(uint32_t n, uint32_t d)
{
    return udivmod(n, d, NULL);
}

uint32_t __umodsi3(uint32_t n, uint32_t d)
{
    uint32_t r;
    udivmod(n, d, &r);
    return r;
}

int32_t __divsi3(int32_t n, int32_t d)
{
    int neg = ((n ^ d) < 0);
    uint32_t q = __udivsi3((uint32_t)(n < 0 ? -n : n),
                           (uint32_t)(d < 0 ? -d : d));
    return neg ? -(int32_t)q : (int32_t)q;
}

int32_t __modsi3(int32_t n, int32_t d)
{
    uint32_t m = __umodsi3((uint32_t)(n < 0 ? -n : n),
                           (uint32_t)(d < 0 ? -d : d));
    return n < 0 ? -(int32_t)m : (int32_t)m;
}

typedef union { struct { uint32_t lo, hi; } s; uint64_t v; } udi;

uint64_t __udivmodsi4(uint64_t num, uint64_t den, uint64_t *rem)
{
    uint64_t quot = 0, bit = 1;

    if (den == 0) {
        if (rem) *rem = num;
        return 0xFFFFFFFFFFFFFFFFull;
    }

    while (den < num && !(bit >> 63)) {
        den <<= 1;
        bit <<= 1;
    }
    while (bit) {
        if (num >= den) {
            num -= den;
            quot |= bit;
        }
        den >>= 1;
        bit >>= 1;
    }
    if (rem) *rem = num;
    return quot;
}

uint64_t __divmodsi4(int64_t num, int64_t den, int64_t *rem)
{
    int neg_q = ((num < 0) != (den < 0));
    int neg_r = (num < 0);
    uint64_t un = num < 0 ? -(uint64_t)num : (uint64_t)num;
    uint64_t ud = den < 0 ? -(uint64_t)den : (uint64_t)den;
    uint64_t urem;
    uint64_t uq = __udivmodsi4(un, ud, &urem);

    if (rem) *rem = neg_r ? -(int64_t)urem : (int64_t)urem;
    return neg_q ? -(uint64_t)(int64_t)uq : uq;
}

/* ---- bit tricks ------------------------------------------------------ */

int __ctzsi2(uint32_t x)
{
    if (x == 0) return 32;
    int n = 0;
    if (!(x & 0x0000FFFFu)) { n += 16; x >>= 16; }
    if (!(x & 0x000000FFu)) { n += 8;  x >>= 8;  }
    if (!(x & 0x0000000Fu)) { n += 4;  x >>= 4;  }
    if (!(x & 0x00000003u)) { n += 2;  x >>= 2;  }
    if (!(x & 0x00000001u)) { n += 1; }
    return n;
}

int __popcountsi2(uint32_t x)
{
    int c = 0;
    while (x) {
        x &= x - 1;
        c++;
    }
    return c;
}

/* ---- 64-bit shifts ---------------------------------------------------
 * Emitted whenever uint64_t tick arithmetic meets 32-bit constants
 * (timer.c does exactly that every tick).
 */

uint64_t __ashldi3(uint64_t v, int s)
{
    udi u;
    u.v = v;

    if ((unsigned)s >= 64) return 0;
    if ((unsigned)s >= 32) {
        u.s.hi = u.s.lo << (s - 32);
        u.s.lo = 0;
        return u.v;
    }
    u.s.hi = (u.s.hi << s) | (u.s.lo >> (32 - s));
    u.s.lo <<= s;
    return u.v;
}

uint64_t __lshrdi3(uint64_t v, int s)
{
    udi u;
    u.v = v;

    if ((unsigned)s >= 64) return 0;
    if ((unsigned)s >= 32) {
        u.s.lo = u.s.hi >> (s - 32);
        u.s.hi = 0;
        return u.v;
    }
    u.s.lo = (u.s.lo >> s) | (u.s.hi << (32 - s));
    u.s.hi >>= s;
    return u.v;
}

int64_t __ashrdi3(int64_t v, int s)
{
    udi u;
    u.v = (uint64_t)v;

    if ((unsigned)s >= 64) return u.s.hi & 0x80000000u ? -1ll : 0ll;
    if ((unsigned)s >= 32) {
        u.s.lo = (uint32_t)((int32_t)u.s.hi >> (s - 32));
        u.s.hi = (u.s.hi & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        return (int64_t)u.v;
    }
    u.s.lo = (u.s.lo >> s) | (u.s.hi << (32 - s));
    u.s.hi = (uint32_t)((int32_t)u.s.hi >> s);
    return (int64_t)u.v;
}

/* ---- misc ------------------------------------------------------------- */

int64_t __muldi3(int64_t a, int64_t b)
{
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    uint64_t result = (uint64_t)((uint32_t)ua) * ((uint32_t)ub);

    result += ((ua >> 32) * (ub & 0xFFFFFFFFull) +
               (ub >> 32) * (ua & 0xFFFFFFFFull)) << 32;
    return (int64_t)result;
}
