#ifndef MNHTEST_UNITS_H
#define MNHTEST_UNITS_H

#include <mrkcommon/bytes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif
#endif /* MNHTEST_UNITS_H */


typedef struct _mnhtest_unit {
#define MNHTEST_UREQ  1
#define MNHTEST_UBYTE 2
#define MNHTEST_USEC  3
    int ty;
    double mult;
} mnhtest_unit_t;

void mnhtest_unit_init(mnhtest_unit_t *, int, double);
double mnhtest_unit_normalize(mnhtest_unit_t *, mnhtest_unit_t *, double);
unsigned char *mnhtest_unit_parse(mnhtest_unit_t *, mnbytes_t *, double *);
#define MNHTEST_UNIT_STR_SHORT (0x01)
mnbytes_t *mnhtest_unit_str(mnhtest_unit_t *, double, int);
