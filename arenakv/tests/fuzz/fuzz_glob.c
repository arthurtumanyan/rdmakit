/*
 * fuzz_glob.c - libFuzzer harness: glob matching (acl_glob); the input is split into pattern and subject
 * at its first zero byte. The result is compared with a recursive reference matcher on short inputs.
 */
#include <string.h>

#include "acl.h"

#include <stdlib.h>

/* Reference: the exponential recursive definition of the same pattern language. */
static int ref(const char *p, size_t pn, const char *s, size_t sn) {
    if (pn == 0) {
        return sn == 0;
    }
    if (p[0] == '*') {
        for (size_t i = 0; i <= sn; i++) {
            if (ref(p + 1, pn - 1, s + i, sn - i)) {
                return 1;
            }
        }
        return 0;
    }
    if (sn == 0) {
        return 0;
    }
    size_t adv = 1;
    int ok;
    if (p[0] == '?') {
        ok = 1;
    } else if (p[0] == '\\' && pn >= 2) {
        ok = p[1] == s[0];
        adv = 2;
    } else if (p[0] == '[') {
        size_t i = 1;
        int neg = i < pn && p[i] == '^';
        i += (size_t)neg;
        int m = 0;
        while (i < pn && p[i] != ']') {
            if (p[i] == '\\' && i + 1 < pn) {
                m |= p[i + 1] == s[0];
                i += 2;
            } else if (i + 2 < pn && p[i + 1] == '-' && p[i + 2] != ']') {
                char lo = p[i] < p[i + 2] ? p[i] : p[i + 2];
                char hi = p[i] < p[i + 2] ? p[i + 2] : p[i];
                m |= s[0] >= lo && s[0] <= hi;
                i += 3;
            } else {
                m |= p[i] == s[0];
                i++;
            }
        }
        adv = i < pn ? i + 1 : i;
        ok = m != neg;
    } else {
        ok = p[0] == s[0];
    }
    return ok && ref(p + adv, pn - adv, s + 1, sn - 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
    const uint8_t *z = memchr(data, 0, n);
    size_t plen = z != NULL ? (size_t)(z - data) : n;
    const char *s = z != NULL ? (const char *)z + 1 : "";
    size_t slen = z != NULL ? n - plen - 1 : 0;
    int got = acl_glob((const char *)data, plen, s, slen);
    if (plen <= 12 && slen <= 24 && got != ref((const char *)data, plen, s, slen)) {
        abort();
    }
    return 0;
}
