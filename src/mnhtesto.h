#ifndef MNHTESTO_H
#define MNHTESTO_H

#include <mnfcgi_app.h>
#include "units.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * quota specification syntax:
 *  quota           ::= qname ":" denom "/" divisor
 *                      [":" poena-factor [":" flags]]
 *  qname           ::= ALNUM
 *  denom           ::= num [s-unit]
 *  divisor         ::= num [t-unit]
 *  s-unit          ::= (s-mult "Bytes") / "Requests"
 *  s-mult          ::= "K" / "M" / "G"
 *  t-unit          ::= "sec" / "min" / "hour" / "day"
 *  poena-factor    ::= FLOATNUM ;; typically [0.0, 1.0], default 1.0
 *  flags           ::= any combination of:
 *                      - "h" send the retry-after: header
 */
typedef struct _mnhtesto_quota_spec {
    double denom;
    mnhtest_unit_t denom_unit;
    double divisor;
    mnhtest_unit_t divisor_unit;
    double poena_factor;
#define MNHTESTO_QF_SENDRA (0x01)
    unsigned flags;
} mnhtesto_quota_spec_t;


typedef struct _mnhtesto_quota {
    mnhtesto_quota_spec_t spec;
    uint64_t ts;
    double value;
    double prorated;
} mnhtesto_quota_t;


#define MNHTESTO_QUOTA_LIMIT(q) \
    ((q)->spec.denom * (q)->spec.denom_unit.mult)

#define MNHTESTO_QUOTA_UNITS(q) \
    ((q)->spec.divisor * (q)->spec.divisor_unit.mult)

#define MNHTESTO_QUOTAS(q, _ts) \
    (((double)(_ts - (q)->ts)) / MNHTESTO_QUOTA_UNITS(q))


#define MNHTESTO_QUOTA_PER_UNIT(q)  \
    (MNHTESTO_QUOTA_LIMIT(q) / MNHTESTO_QUOTA_UNITS(q))


#define MNHTESTO_IN_QUOTA(q, _ts)              \
    INB1((q)->ts,                              \
         (_ts),                                \
         ((q)->ts + MNHTESTO_QUOTA_UNITS(q)))  \


#define MNHTESTO_QUOTA_PRORATE_PER_UNIT(q, v, _ts)  \
    ((v) / ((double)(_ts - (q)->ts)))


#define MNHTESTO_QUOTA_PRORATE(q, v, _ts)   \
    (MNHTESTO_QUOTA_PRORATE_PER_UNIT(q, v, _ts) * MNHTESTO_QUOTA_UNITS(q))


extern mnbytes_t _x_mnhtesto_quota;

void mnhtesto_init(void);
void mnhtesto_fini(void);
int parse_quota(char *);
int mnhtesto_stdin_end(mnfcgi_request_t *, void *);
int mnhtesto_app_init(mnfcgi_app_t *);

#ifdef __cplusplus
}
#endif

#endif /* MNHTESTO_H*/
