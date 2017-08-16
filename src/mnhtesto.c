#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <syslog.h>

#include <mrkcommon/bytes.h>
#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>

#include <mrkthr.h>

#include <mnfcgi_app.h>

#include "diag.h"
#include "config.h"
#include "mnhtesto.h"

static mnbytes_t _not_implemented = BYTES_INITIALIZER("Not Implemented");
static mnbytes_t _server = BYTES_INITIALIZER("Server");
static mnbytes_t _date = BYTES_INITIALIZER("Date");
static mnbytes_t _cache_control = BYTES_INITIALIZER("Cache-Control");
static mnbytes_t _private = BYTES_INITIALIZER("private");
static mnbytes_t _pragma = BYTES_INITIALIZER("Pragma");
static mnbytes_t _no_cache = BYTES_INITIALIZER("no-cache");
static mnbytes_t _content_length = BYTES_INITIALIZER("Content-Length");
static mnbytes_t _retry_after = BYTES_INITIALIZER("Retry-After");
static mnbytes_t __root = BYTES_INITIALIZER("/");
static mnbytes_t __qwe0 = BYTES_INITIALIZER("/qwe(0)=привіт");
static mnbytes_t __qwe1 = BYTES_INITIALIZER("/qwe 1");
static mnbytes_t __qwe2 = BYTES_INITIALIZER("/qwe02");
static mnbytes_t __qwe3 = BYTES_INITIALIZER("/qwe03");
static mnbytes_t __qwe4 = BYTES_INITIALIZER("/qwe04");
static mnbytes_t __qwe5 = BYTES_INITIALIZER("/qwe05");
static mnbytes_t __qwe6 = BYTES_INITIALIZER("/qwe06");
static mnbytes_t __qwe7 = BYTES_INITIALIZER("/qwe07");
static mnbytes_t __qwe8 = BYTES_INITIALIZER("/qwe08");
static mnbytes_t __qwe9 = BYTES_INITIALIZER("/qwe09");
static mnbytes_t __qwea = BYTES_INITIALIZER("/qwe0a");
static mnbytes_t __qweb = BYTES_INITIALIZER("/qwe0b");
static mnbytes_t _ok = BYTES_INITIALIZER("OK");
static mnbytes_t _too_much = BYTES_INITIALIZER("Too Much");

mnbytes_t _x_mnhtesto_quota = BYTES_INITIALIZER("x-mnhtesto-quota");
mnbytes_t _http_x_mnhtesto_quota = BYTES_INITIALIZER("HTTP_X_MNHTESTO_QUOTA");


#define BSIZE_MIN  10
#define BSIZE_MAX  21
#define BSIZE_DEFAULT BSIZE_MIN
#define DELAY_MIN   1
#define DELAY_MAX  14
#define DELAY_DEFAULT DELAY_MIN

#define MNHTESTO_DEFAULT_POENA_FACTOR   (0.0l)


static char d[1<<(BSIZE_MAX + 1)];

unsigned long nreq[600];
unsigned long nbytes[600];

mnhash_t quotas;


static void
update_stats(UNUSED mnfcgi_request_t *req, int code, int amount)
{
    if ((unsigned)code < countof(nreq)) {
        ++nreq[code];
        nbytes[code] += amount;
    }
}


static ssize_t
mnhtesto_body(mnfcgi_record_t *rec, mnbytestream_t *bs, void *udata)
{
    ssize_t res;
    UNUSED mnfcgi_request_t *req;
    struct {
        int bsize;
        int clen;
        int delay;
        int tts;
        int offset;
    } *params = mnfcgi_stdout_get_udata(rec);
    int sz;

    req = udata;
    sz = MIN(MNFCGI_MAX_PAYLOAD, params->clen - params->offset);
    res = mnfcgi_cat(bs, sz, d + sizeof(char) * params->offset);
    params->offset += sz;
    return res;
}


static void
quota_init(mnhtesto_quota_t *quota, uint64_t now)
{
    quota->ts = now;
    quota->ts -= quota->ts % (unsigned)MNHTESTO_QUOTA_UNITS(quota);
    quota->value = 0.0;
    quota->prorated = 0.0;
}


static int
mnhtesto_update_quota(mnfcgi_request_t *req, int amount, double *ra)
{
    int res = 0;
    mnbytes_t *qname;

    if ((qname = mnfcgi_request_get_param(req,
                                          &_http_x_mnhtesto_quota)) != NULL) {
        mnhash_item_t *hit;

        if ((hit = hash_get_item(&quotas, qname)) != NULL) {
            mnhtesto_quota_t *quota;
            uint64_t now;

            quota = hit->value;
            if (quota->spec.denom_unit.ty == MNHTEST_UREQ) {
                amount = 1;
            }
            /*
             * quota update
             */
            now = MRKTHR_GET_NOW_SEC();
            if (MNHTESTO_IN_QUOTA(quota, now)) {
                quota->value += (double)amount;
                if (quota->value <= MNHTESTO_QUOTA_LIMIT(quota)) {
                    /*
                     * 200
                     */
                } else {
                    mnbytes_t *s, *ss, *sss;

                    /*
                     * 429
                     */
                    res = -1;
                    *ra = (quota->value / MNHTESTO_QUOTA_LIMIT(quota)) *
                            MNHTESTO_QUOTA_UNITS(quota);

                    s = mnhtest_unit_str(&quota->spec.denom_unit,
                                         quota->value, MNHTEST_UNIT_STR_VBASE);
                    ss = mnhtest_unit_str(&quota->spec.denom_unit,
                                          quota->spec.denom, 0);
                    sss = mnhtest_unit_str(&quota->spec.divisor_unit,
                                           quota->spec.divisor, 0);
                    CTRACE("current quota %s overuse: %s (over %s) per %s ra %lf sec",
                           BDATA(qname),
                           BDATA(s),
                           BDATA(ss),
                           BDATA(sss),
                           *ra);

                    BYTES_DECREF(&s);
                    BYTES_DECREF(&ss);
                    BYTES_DECREF(&sss);

                    if (!(quota->spec.flags & MNHTESTO_QF_SENDRA)) {
                        *ra = 0.0;
                    }
                }

            } else {
                double normvalue;

                normvalue = quota->spec.poena_factor * quota->value;

                quota->prorated = MNHTESTO_QUOTA_PRORATE(
                    quota, normvalue + (double)amount, now);

                if (quota->prorated <= MNHTESTO_QUOTA_LIMIT(quota)) {
                    /*
                     * 200
                     */
                    quota_init(quota, now);
                    assert(MNHTESTO_IN_QUOTA(quota, now));
                    quota->value = quota->prorated;

                } else {
                    mnbytes_t *xtot, *ytot, *xp, *xnom, *ynom;

                    /*
                     * previous quota overuse, 429
                     */
                    quota->value = normvalue + (double)amount;

                    res = -1;
                    *ra = (quota->prorated / MNHTESTO_QUOTA_LIMIT(quota)) *
                            MNHTESTO_QUOTA_UNITS(quota) * MNHTESTO_QUOTAS(quota, now);

                    xtot = mnhtest_unit_str(&quota->spec.denom_unit,
                                         quota->value, MNHTEST_UNIT_STR_VBASE);
                    ytot = mnhtest_unit_str(&quota->spec.divisor_unit,
                                           (double)(now - quota->ts), 0);
                    xp = mnhtest_unit_str(&quota->spec.denom_unit,
                                          quota->prorated, MNHTEST_UNIT_STR_VBASE);
                    xnom = mnhtest_unit_str(&quota->spec.denom_unit,
                                           quota->spec.denom, 0);
                    ynom = mnhtest_unit_str(&quota->spec.divisor_unit,
                                           quota->spec.divisor, 0);
                    CTRACE("previous quota %s (%" PRId64 ") overuse: "
                           "%s per %s (prorated %s) (over %s per %s) ra %lf sec",
                           BDATA(qname),
                           quota->ts,
                           BDATA(xtot),
                           BDATA(ytot),
                           BDATA(xp),
                           BDATA(xnom),
                           BDATA(ynom),
                           *ra);

                    BYTES_DECREF(&xtot);
                    BYTES_DECREF(&ytot);
                    BYTES_DECREF(&xp);
                    BYTES_DECREF(&xnom);
                    BYTES_DECREF(&ynom);

                    if (!(quota->spec.flags & MNHTESTO_QF_SENDRA)) {
                        *ra = 0.0;
                    }
                }
            }
        }
    } else {
        //CTRACE("no quota");
    }

    return res;
}

static int
mnhtesto_root_get(mnfcgi_request_t *req, RESERVED void *__udata)
{
    int res = 0;
    BYTES_ALLOCA(_op, "op");
    BYTES_ALLOCA(_bsiz, "bsiz");
    BYTES_ALLOCA(_dlay, "dlay");
    mnbytes_t *op, *bsiz, *dlay;
    struct {
        int bsize;
        int clen;
        int delay;
        int tts;
        int offset;
    } params;
    double ra = 0.0l;

    /*
     *
     */
    if ((op = mnfcgi_request_get_query_term(req, _op)) == NULL) {
    } else {
        //CTRACE("op=%s", BDATA(op));
    }

    if ((bsiz = mnfcgi_request_get_query_term(req, _bsiz)) == NULL) {
        params.bsize = BSIZE_DEFAULT;
    } else {
        //CTRACE("bsiz=%s", BDATA(bsiz));
        params.bsize = strtol((char *)BDATA(bsiz), NULL, 10);
        if (!INB0(BSIZE_MIN, params.bsize, BSIZE_MAX)) {
            params.bsize = BSIZE_DEFAULT;
        }
    }

    params.offset = 0;
    params.clen = (int)(1 << params.bsize);

    if ((dlay = mnfcgi_request_get_query_term(req, _dlay)) == NULL) {
        params.delay = DELAY_DEFAULT;
    } else {
        //CTRACE("dlay=%s", BDATA(dlay));
        params.delay = strtol((char *)BDATA(dlay), NULL, 10);
        if (!INB0(DELAY_MIN, params.delay, DELAY_MAX)) {
            params.delay = DELAY_DEFAULT;
        }
    }

    params.tts = (int)(1 << params.delay);

    if (mnhtesto_update_quota(req, params.clen, &ra) != 0) {
        if (ra > 0.0l) {
            if (MRKUNLIKELY((res = mnfcgi_request_field_addf(
                                req,
                                MNFCGI_FADD_OVERRIDE,
                                &_retry_after,
                                "%d",
                                (int)(ra + 1.0l))) != 0)) {
                goto end;
            }
        }
        mnfcgi_app_error(req, 429, &_too_much);
        update_stats(req, 429, 0);
        goto end;
    }


    if (MRKUNLIKELY((res = mnfcgi_request_status_set(req, 200, &_ok)) != 0)) {
        goto end;
    }

    if (MRKUNLIKELY((res = mnfcgi_request_field_addf(
                        req,
                        MNFCGI_FADD_OVERRIDE,
                        &_content_length,
                        "%d",
                        params.clen)) != 0)) {
        goto end;
    }

    if (MRKUNLIKELY((res = mnfcgi_request_headers_end(req)) != 0)) {
        goto end;
    }

    //CTRACE("bsize=%d delay=%d clen=%d(%08x) tts=%d",
    //       params.bsize,
    //       params.delay,
    //       params.clen,
    //       params.clen,
    //       params.tts);


    if (mrkthr_sleep(params.tts) != 0) {
        return 0;
    }

    while (params.offset < params.clen) {
        if ((res = mnfcgi_render_stdout(req, mnhtesto_body, &params)) != 0) {
            break;
        }
    }
    update_stats(req, 200, params.clen);


end:
    return res;
}


int
mnhtesto_stdin_end(mnfcgi_request_t *req, void *udata)
{
    mnfcgi_app_callback_t cb;

    (void)mnfcgi_request_field_addf(req, 0,
            &_server, "%s/%s", PACKAGE, VERSION);
    (void)mnfcgi_request_field_addt(req, 0,
            &_date, MRKTHR_GET_NOW_SEC());
    (void)mnfcgi_request_field_addb(req, 0,
            &_cache_control, &_private);
    (void)mnfcgi_request_field_addb(req, 0,
            &_pragma, &_no_cache);

    if ((cb = req->udata) != NULL) {
        (void)cb(req, udata);

    } else {
        mnfcgi_app_error(req, 501, &_not_implemented);
    }

    if (MRKUNLIKELY(mnfcgi_finalize_request(req) != 0)) {
    }
    /*
     * at this point no operation on req is possible any more.
     */
    return 0;
}


static mnfcgi_app_endpoint_table_t endpoints[] = {
    { &__root, {mnhtesto_root_get, NULL,} },
    { &__qwe0, {mnhtesto_root_get, NULL,} },
    { &__qwe1, {mnhtesto_root_get, NULL,} },
    { &__qwe2, {mnhtesto_root_get, NULL,} },
    { &__qwe3, {mnhtesto_root_get, NULL,} },
    { &__qwe4, {mnhtesto_root_get, NULL,} },
    { &__qwe5, {mnhtesto_root_get, NULL,} },
    { &__qwe6, {mnhtesto_root_get, NULL,} },
    { &__qwe7, {mnhtesto_root_get, NULL,} },
    { &__qwe8, {mnhtesto_root_get, NULL,} },
    { &__qwe9, {mnhtesto_root_get, NULL,} },
    { &__qwea, {mnhtesto_root_get, NULL,} },
    { &__qweb, {mnhtesto_root_get, NULL,} },
};


static int
quota_item_init(UNUSED mnbytes_t *qname,
                mnhtesto_quota_t *quota,
                UNUSED void *udata)
{
    quota_init(quota, MRKTHR_GET_NOW_SEC());
    return 0;
}


int
mnhtesto_app_init(mnfcgi_app_t *app)
{
    unsigned i;

    for (i = 0; i < countof(endpoints); ++i) {
        if (MRKUNLIKELY(mnfcgi_app_register_endpoint(app,
                                                     &endpoints[i])) != 0) {
            FAIL("mnhtesto_app_init");
        }
    }

    for (i = 0; i < countof(d); ++i) {
        d[i] = 'Y';
    }

    hash_traverse(&quotas, (hash_traverser_t)quota_item_init, NULL);
    return 0;
}


/**
 * Quota specification:
 *  name ":" denominator "/"
 *
 */
int
parse_quota(char *s)
{
    int res = 0;
    char *p;
    mnbytes_t *qname = NULL;
    mnbytes_t *denom = NULL;
    mnbytes_t *divisor = NULL;
    mnbytes_t *poena_factor = NULL;
    mnbytes_t *flags = NULL;
    mnhtesto_quota_t *quota;
    mnhash_item_t *hit;

    if ((p = strchr(s, ':')) == NULL) {
        goto err;
    }
    *p = '\0';
    qname = bytes_new_from_str(s);

    s = ++p;
    if ((p = strchr(s, '/')) == NULL) {
        goto err;
    }
    *p = '\0';
    denom = bytes_new_from_str(s);

    s = ++p;
    if ((p = strchr(s, ':')) != NULL) {
        *p = '\0';
        divisor = bytes_new_from_str(s);
        s = ++p;
        if ((p = strchr(s, ':')) != NULL) {
            *p = '\0';
            poena_factor = bytes_new_from_str(s);
            s = ++p;
            flags = bytes_new_from_str(s);
        } else {
            poena_factor = bytes_new_from_str(s);
        }
    } else {
        divisor = bytes_new_from_str(s);
    }

    if ((quota = malloc(sizeof(mnhtesto_quota_t))) == NULL) {
        FAIL("malloc");
    }

    if (mnhtest_unit_parse(&quota->spec.denom_unit,
                           denom,
                           &quota->spec.denom) == NULL) {
        goto err;
    }

    if (mnhtest_unit_parse(&quota->spec.divisor_unit,
                           divisor,
                           &quota->spec.divisor) == NULL) {
        goto err;
    }

    if (poena_factor != NULL) {
        double pf;

        if ((pf = strtod((char *)BDATA(poena_factor), NULL)) == 0.0) {
            if (errno != ERANGE) {
                quota->spec.poena_factor = pf;
            }
        } else {
            quota->spec.poena_factor = pf;
        }
    } else {
        /* default */
        quota->spec.poena_factor = MNHTESTO_DEFAULT_POENA_FACTOR;
    }

    quota->spec.flags = 0;
    if (flags != NULL) {
        unsigned char *p;

        for (p = BDATA(flags); *p != '\0'; ++p) {
            switch (*p) {
            case 'h':
                quota->spec.flags |= MNHTESTO_QF_SENDRA;
                break;

            default:
                break;
            }
        }
    }

    if ((hit = hash_get_item(&quotas, qname)) != NULL) {
        /*
         * duplicate quota
         */
        goto err;
    }

    BYTES_INCREF(qname);
    hash_set_item(&quotas, qname, quota);

    //CTRACE("pf %s/%lf fl %s/%08x",
    //       BDATASAFE(poena_factor),
    //       quota->spec.poena_factor,
    //       BDATASAFE(flags),
    //       quota->spec.flags);

end:
    BYTES_DECREF(&denom);
    BYTES_DECREF(&divisor);
    BYTES_DECREF(&poena_factor);
    BYTES_DECREF(&flags);
    TRRET(res);

err:
    BYTES_DECREF(&qname);
    res = PARSE_QUOTA + 1;
    goto end;
}


static void
mnhtesto_quota_destroy(mnhtesto_quota_t **quota)
{
    if (*quota != NULL) {
        free(*quota);
        *quota = NULL;
    }
}


static int
quota_item_finalizer(mnbytes_t *key, mnhtesto_quota_t *quota)
{
    BYTES_DECREF(&key);
    mnhtesto_quota_destroy(&quota);
    return 0;
}


void
mnhtesto_init(void)
{
    hash_init(&quotas,
              17,
              (hash_hashfn_t)bytes_hash,
              (hash_item_comparator_t)bytes_cmp,
              (hash_item_finalizer_t)quota_item_finalizer);

}


void
mnhtesto_fini(void)
{
    hash_fini(&quotas);
}
