#ifndef MNHTESTO_H
#define MNHTESTO_H

#include <mnfcgi_app.h>
#include "units.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * quota specification syntax:
 *  quota   ::= qname ":" denom "/" divisor
 *  qname   ::= ALNUM
 *  denom   ::= num [s-unit]
 *  divisor ::= num [t-unit]
 *  s-unit  ::= (s-mult "Bytes") / "Requests"
 *  s-mult  ::= "K" / "M" / "G"
 *  t-unit  ::= "sec" / "min" / "hour" / "day"
 */
typedef struct _mnhtesto_quota {
    double denom;
    mnhtest_unit_t denom_unit;
    double divisor;
    mnhtest_unit_t divisor_unit;
} mnhtesto_quota_t;

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
