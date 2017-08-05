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

static void quota_init(mnhtesto_quota_t *, uint64_t);

static mnbytes_t _not_implemented = BYTES_INITIALIZER("Not Implemented");
static mnbytes_t _server = BYTES_INITIALIZER("Server");
static mnbytes_t _date = BYTES_INITIALIZER("Date");
static mnbytes_t _cache_control = BYTES_INITIALIZER("Cache-Control");
static mnbytes_t _private = BYTES_INITIALIZER("private");
static mnbytes_t _pragma = BYTES_INITIALIZER("Pragma");
static mnbytes_t _no_cache = BYTES_INITIALIZER("no-cache");
static mnbytes_t _content_length = BYTES_INITIALIZER("Content-Length");
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


#define BSIZE_MIN 10
#define BSIZE_MAX 21
#define BSIZE_DEFAULT BSIZE_MIN
#define DELAY_MIN 1
#define DELAY_MAX 14
#define DELAY_DEFAULT DELAY_MIN


static char d[1<<(BSIZE_MAX + 1)];

extern unsigned long nreq;
extern unsigned long nbytes;

static mnhash_t quotas;

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


static int
mnhtesto_update_quota(mnfcgi_request_t *req, int amount)
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
                if (quota->value < MNHTESTO_QUOTA_LIMIT(quota)) {
                    /*
                     * 200
                     */
                } else {
                    /*
                     * 429
                     */
                    res = -1;
                }

            } else {
                double v, vv;

                v = quota->value + (double)amount;
                vv = MNHTESTO_QUOTA_PRORATE_PER_UNIT(quota, v, now);

                quota_init(quota, now);
                assert(MNHTESTO_IN_QUOTA(quota, now));

                if (vv > (MNHTESTO_QUOTA_PER_UNIT(quota) * 2.0)) {
                    /*
                     * previous or current quota overuse, 429
                     */
                    quota->value = v;
                    res = -1;

                } else {
                    /*
                     * 200
                     */
                    quota->value = (double)amount;
                }
            }
        }
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

    if (mnhtesto_update_quota(req, params.clen) != 0) {
        mnfcgi_app_error(req, 429, &_too_much);
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
    ++nreq;
    nbytes += params.clen;

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


static void
quota_init(mnhtesto_quota_t *quota, uint64_t now)
{
    quota->ts = now;
    quota->ts -= quota->ts % (unsigned)MNHTESTO_QUOTA_UNIT(quota);
    quota->value = 0.0;
}


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


int
parse_quota(char *s)
{
    int res = 0;
    char *p;
    mnbytes_t *qname = NULL, *denom = NULL, *divisor = NULL;
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
    divisor = bytes_new_from_str(s);

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

    if ((hit = hash_get_item(&quotas, qname)) != NULL) {
        /*
         * duplicate quota
         */
        goto err;
    }

    BYTES_INCREF(qname);
    hash_set_item(&quotas, qname, quota);

end:
    BYTES_DECREF(&denom);
    BYTES_DECREF(&divisor);
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
