#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <string.h> /* _GNU_SOURCE for strcasestr() on linux */

#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>
#include <mrkcommon/bytes.h>

#include "diag.h"

#include "units.h"

void
mnhtest_unit_init(mnhtest_unit_t *unit, int ty, double mult)
{
    unit->ty = ty;
    unit->mult = mult;
}


double
mnhtest_unit_normalize(mnhtest_unit_t *dst, mnhtest_unit_t *src, double v)
{
    assert(dst->ty == src->ty);
    assert(isfinite(src->mult));

    if (isfinite(dst->mult)) {
        return v * src->mult / dst->mult;
    } else {
        if (dst->ty == MNHTEST_UREQ) {
            for (v *= src->mult, dst->mult = 1.0;
                 v >= 1000.0;
                 dst->mult *= 1000.0, v /= dst->mult) {
            }
            return v;

        } else if (dst->ty == MNHTEST_UBYTE) {
            for (v *= src->mult, dst->mult = 1.0;
                 v >= 1024.0;
                 dst->mult *= 1024.0, v /= dst->mult) {
            }
            return v;
        } else if (dst->ty == MNHTEST_USEC) {
            v *= src->mult;
            if (src->mult > 604800.0) {
                dst->mult = 604800.0;
            } else if (src->mult > 86400.0) {
                dst->mult = 86400.0;
            } else if (src->mult > 3600.0) {
                dst->mult = 3600.0;
            } else if (src->mult > 60.0) {
                dst->mult = 60.0;
            } else {
                dst->mult = 1.0;
            }
            return v / dst->mult;
        } else {
            dst->mult = src->mult;
            return v;
        }
    }
}


/**
 * Simplistic unit parser.
 *
 */
unsigned char *
mnhtest_unit_parse(mnhtest_unit_t *unit, mnbytes_t *s, double *v)
{
    char *endptr = NULL, *p;

    mnhtest_unit_init(unit, 0, 0.0);

    if ((*v = strtod((const char *)BDATA(s), &endptr)) == 0.0) {
        if (errno == ERANGE) {
            TRRETNULL(MNHTEST_UNIT_PARSE + 1);
        } else {
            /* unit */
            *v = 1.0;
        }
    }

    assert(endptr != NULL);
    while (*endptr == ' ') {
        ++endptr;
    }
    p = endptr;
    while (isalpha(*p)) {
        ++p;
    }
    if (strcasestr(endptr, "gb") == endptr ||
        strcasestr(endptr, "gbyte") == endptr ||
        strcasestr(endptr, "gbytes") == endptr) {
        unit->ty = MNHTEST_UBYTE;
        unit->mult = (double)(1024 * 1024 * 1024);

    } else if (strcasestr(endptr, "mb") == endptr ||
               strcasestr(endptr, "mbyte") == endptr ||
               strcasestr(endptr, "mbytes") == endptr) {
        unit->ty = MNHTEST_UBYTE;
        unit->mult = (double)(1024 * 1024);

    } else if (strcasestr(endptr, "kb") == endptr ||
               strcasestr(endptr, "kbyte") == endptr ||
               strcasestr(endptr, "kbytes") == endptr) {
        unit->ty = MNHTEST_UBYTE;
        unit->mult = (double)(1024);

    } else if (strcasestr(endptr, "byte") == endptr ||
               strcasestr(endptr, "bytes") == endptr) {
        unit->ty = MNHTEST_UBYTE;
        unit->mult = (double)(1);

    } else if (strcasestr(endptr, "req") == endptr ||
               strcasestr(endptr, "request") == endptr ||
               strcasestr(endptr, "requests") == endptr) {
        unit->ty = MNHTEST_UREQ;
        unit->mult = (double)(1);

    } else if (strcasestr(endptr, "sec") == endptr ||
               strcasestr(endptr, "second") == endptr ||
               strcasestr(endptr, "seconds") == endptr) {
        unit->ty = MNHTEST_USEC;
        unit->mult = (double)(1);

    } else if (strcasestr(endptr, "min") == endptr ||
               strcasestr(endptr, "minute") == endptr ||
               strcasestr(endptr, "minutes") == endptr) {
        unit->ty = MNHTEST_USEC;
        unit->mult = (double)(60);

    } else if (strcasestr(endptr, "hr") == endptr ||
               strcasestr(endptr, "hour") == endptr ||
               strcasestr(endptr, "hours") == endptr) {
        unit->ty = MNHTEST_USEC;
        unit->mult = (double)(3600);

    } else if (strcasestr(endptr, "d") == endptr ||
               strcasestr(endptr, "day") == endptr ||
               strcasestr(endptr, "days") == endptr) {
        unit->ty = MNHTEST_USEC;
        unit->mult = (double)(86400);

    } else if (strcasestr(endptr, "w") == endptr ||
               strcasestr(endptr, "week") == endptr ||
               strcasestr(endptr, "weeks") == endptr) {
        unit->ty = MNHTEST_USEC;
        unit->mult = (double)(604800);

    } else {
        /*
         * unknown unit, extension?
         */
        unit->mult = (double)(1);
    }

    return (unsigned char *)p;
}


mnbytes_t *
mnhtest_unit_str(mnhtest_unit_t *unit, double v, int flags)
{
    mnbytes_t *res = NULL;

    if (unit->ty == MNHTEST_UREQ) {
        v *= unit->mult;
        if (v == 1.0) {
            if (flags & MNHTEST_UNIT_STR_SHORT) {
                res = bytes_new_from_str("req");
            } else {
                res = bytes_printf("%lfreq", v);
            }
        } else {
            res = bytes_printf("%lfreq", v);
        }
    } else if (unit->ty == MNHTEST_UBYTE){
        if (unit->mult >= (double)(1 << 30)) {
            res = bytes_printf("%lfGB", v);
        } else if (unit->mult >= (double)(1 << 20)) {
            res = bytes_printf("%lfMB", v);
        } else if (unit->mult >= (double)(1 << 10)) {
            res = bytes_printf("%lfKB", v);
        } else {
            res = bytes_printf("%lfB", v);
        }
    } else if (unit->ty == MNHTEST_USEC){
        if (unit->mult >= 604800.0) {
            res = bytes_printf("%lfw", v);
        } else if (unit->mult >= 86400.0) {
            res = bytes_printf("%lfd", v);
        } else if (unit->mult >= 3600.0) {
            res = bytes_printf("%lfhr", v);
        } else if (unit->mult >= 60.0) {
            res = bytes_printf("%lfmin", v);
        } else {
            if (flags & MNHTEST_UNIT_STR_SHORT) {
                res = bytes_new_from_str("sec");
            } else {
                res = bytes_printf("%dsec", (int)v);
            }
        }
    } else {
        res = bytes_printf("%lf", v);
    }
    return res;
}
