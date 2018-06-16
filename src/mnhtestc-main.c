#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <libgen.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "config.h"

#ifdef HAVE_MALLOC_H
#   include <malloc.h>
#endif

#include <mrkcommon/array.h>
#include <mrkcommon/bytes.h>
#define TRRET_DEBUG
#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>

#include <mrkthr.h>
#include <mnhttpc.h>

#include "diag.h"

#ifndef NDEBUG
//const char *_malloc_options = "AJ";
#endif

typedef struct {
    mnbytes_t *key;
    mnbytes_t *value;
} mnhtestc_header_t;

static mnbytes_t _bsiz = BYTES_INITIALIZER("bsiz");
static mnbytes_t _dlay = BYTES_INITIALIZER("dlay");
static mnbytes_t _connection = BYTES_INITIALIZER("Connection");
static mnbytes_t _proxy_connection = BYTES_INITIALIZER("Proxy-Connection");
static mnbytes_t _close = BYTES_INITIALIZER("close");
static mnbytes_t _x_mnhtesto_quota = BYTES_INITIALIZER("x-mnhtesto-quota");
static mnbytes_t _retry_after = BYTES_INITIALIZER("retry-after");

static int develop = 0;
static int print_config = 0;
static int keepalive = 0;
#define MNHTEST_PARALLEL_MIN 1
#define MNHTEST_PARALLEL_MAX 100000
#define MNHTEST_PARALLEL_DEFAULT MNHTEST_PARALLEL_MIN
static int parallel = 0;
static int batch_pause;
static int use_bsize = 0;
static int use_delay = 0;
static int limit = 0;

/*
 * Runtime.
 */
static bool shutting_down = false;
static bool sigshutdown_sent = false;

/*
 * mnbytes_t *
 */
static mnarray_t urls;
/*
 * mnhtestc_header_t
 */
static mnarray_t headers;
/*
 * mnbytes_t *
 */
static mnarray_t quotas;
static mnbytes_t *proxy_host;
static mnbytes_t *proxy_port;
static mnbytes_t *quota_selector;

static unsigned long nreq[600];
static unsigned long nbytes[600];


static struct option optinfo[] = {
#define MNHTESTC_OPT_HELP           0
    {"help", no_argument, NULL, 'h'},
#define MNHTESTC_OPT_VERSION        1
    {"version", no_argument, NULL, 'V'},
#define MNHTESTC_OPT_DEVELOP        2
    {"develop", no_argument, &develop, 1},
#define MNHTESTC_OPT_PRINTCONFIG    3
    {"print-config", no_argument, &print_config, 1},
#define MNHTESTC_OPT_KEEPALIVE      4
    {"keepalive", no_argument, &keepalive, 'A'},
#define MNHTESTC_OPT_PARALLEL       5
    {"parallel", required_argument, &parallel, 'p'},
#define MNHTESTC_OPT_URL            6
    {"url", required_argument, NULL, 'u'},
#define MNHTESTC_OPT_PROXY          7
    {"proxy", required_argument, NULL, 'P'},
#define MNHTESTC_OPT_PAUSE          8
    {"pause", required_argument, NULL, 'z'},
#define MNHTESTC_OPT_HEADER         9
    {"header", required_argument, NULL, 'H'},
#define MNHTESTC_OPT_DELAY          10
    {"delay", required_argument, NULL, 'D'},
#define MNHTESTC_OPT_BSIZE          11
    {"bsize", required_argument, NULL, 'B'},
#define MNHTESTC_OPT_QUOTA          12
    {"quota", required_argument, NULL, 'Q'},
#define MNHTESTC_OPT_QUOTA_SELECTOR 13
    {"quota-selector", required_argument, NULL, 'S'},
#define MNHTESTC_OPT_LIMIT          14
    {"limit", required_argument, &limit, 'l'},

    {NULL, 0, NULL, 0},
};


#define BSIZE_MIN 10
#define BSIZE_MAX 21
#define DELAY_MIN 1
#define DELAY_MAX 14


static int
randombsize(void)
{
    return (random() % (BSIZE_MAX - BSIZE_MIN)) + BSIZE_MIN;
}


static int
randomdelay(void)
{
    return (random() % (DELAY_MAX - DELAY_MIN)) + DELAY_MIN;
}


static mnbytes_t *
randomquota(void)
{
    mnbytes_t *res = NULL;

    if (quotas.elnum > 0) {
        mnbytes_t **p;
        if ((p = array_get(&quotas, random() % quotas.elnum)) != NULL) {
            res = *p;
        }
    }
    return res;
}

static int
url_item_fini(mnbytes_t **s)
{
    BYTES_DECREF(s);
    return 0;
}


static int
mnhtestc_header_item_fini(mnhtestc_header_t *header)
{
    BYTES_DECREF(&header->key);
    BYTES_DECREF(&header->value);
    return 0;
}


static int
quota_item_fini(mnbytes_t **s)
{
    BYTES_DECREF(s);
    return 0;
}


static void
usage(char *p)
{
    printf("Usage: %s OPTIONS\n"
"\n"
"Options:\n"
"  --help|-h                    Show this message and exit.\n"
"  --version|-V                 Print version and exit.\n"
"  --develop                    Run in develop mode.\n"
"  --print-config               Print configuration.\n"
"  --keepalive|-A               Keep the connection alive.\n"
"                               Default is false.\n"
"  --parallel=N|-p N            Parallel connections.\n"
"                               Default is %d.\n"
"  --url=URL|-u URL             URL to query. Required. Multiple.\n"
"  --proxy=HOST[:PORT]|-P HOST[:PORT]\n"
"                               Proxy host or IP address and optionally port\n"
"                               (default to URL port) to connect to.\n"
"                               No scheme prefix.\n"
"  --pause=MSEC|-z MSEC         Pause before sending a URL batch.\n"
"  --header=HEADER|-H HEADER    Send this header.  Multiple.\n"
"  --delay=NUM|-D NUM           Request delay in log msec.\n"
"  --bsize=NUM|-B NUM           Body size in log bytes.\n"
"  --quota=QUOTA|-Q QUOTA       Use thie quota.  Multiple.\n"
"  --quota-selector=NAME|-S NAME\n"
"                               Copy quota in this header.\n"
"  --limit|-l NUM               Limit the number of calls per thread.\n"
"                               Default is unlimited.\n"
        ,
        basename(p),
        MNHTEST_PARALLEL_DEFAULT
        );
}


#ifndef SIGINFO
UNUSED
#endif
static void
myinfo(UNUSED int sig)
{
    mrkthr_dump_all_ctxes();
}


static int
sigshutdown(UNUSED int argc, UNUSED void **argv)
{
    if (!shutting_down) {

        if (!sigshutdown_sent) {
            mrkthr_shutdown();

            /*
             * At this point mrkthr_loop() should get ready to terminate in
             * main(), and let it exit naturally.
             */
        } else {
            ++sigshutdown_sent;
        }
        shutting_down = true;
    } else {
        exit(0);
    }
    return 0;
}


static void
myterm(UNUSED int sig)
{
    (void)MRKTHR_SPAWN_SIG("sigshutdown", sigshutdown);
}


static int
stats0(UNUSED int argc, UNUSED void **argv)
{
    while (!shutting_down && mrkthr_sleep(1000) == 0) {
        unsigned i;

        //if (limit < 0) {
        //    break;
        //}
        //CTRACE("limit=%d", limit);

        for (i = 0; i < countof(nreq); ++i) {
            if (nreq[i] > 0) {
                TRACEC(" % 3d: % 6ld % 9ld", i, nreq[i], nbytes[i]);
                nreq[i] = 0;
                nbytes[i] = 0;
            }
        }
        TRACEC("\n");

        if ((MRKTHR_GET_NOW_SEC() % 60) == 0) {
            mrkthr_gc();
        }

    }
    return 0;
}


uint64_t
parse_retry_after(mnbytes_t *s)
{
    struct tm t;
    time_t tt;
    long seconds;

    assert(s != NULL);

    if (strptime((char *)BDATA(s), "%a, %d %b %Y %H:%M:%S %Z", &t) != NULL) {
        tt = mktime(&t);

    } else if ((seconds = strtol((char *)BDATA(s), NULL, 10)) != 0) {
        tt = MRKTHR_GET_NOW_SEC() + seconds;

    } else {
        tt = MRKTHR_GET_NOW_SEC();
    }

    return (uint64_t)tt;
}


static int
mybodycb(mnhttp_ctx_t *ctx,
         UNUSED mnbytestream_t *bs,
         mnhttpc_request_t *req)
{
    int res = 0;
    uint64_t tts = 0;

    if (mnhttp_ctx_last_chunk(ctx)) {
        if (req->response.in.ctx.code.status == 429 || req->response.in.ctx.code.status == 503) {
            mnhash_item_t *hit;
            if ((hit = hash_get_item(&req->response.in.headers, &_retry_after)) != NULL) {
                mnbytes_t *v;
                uint64_t waketime, now;

                v = hit->value;
                //CTRACE("status=%d retry after %s", req->response.in.ctx.code.status, BDATASAFE(v));

                now = MRKTHR_GET_NOW_SEC();
                if ((waketime = parse_retry_after(v)) > now) {
                    tts = waketime - now;
                }

            } else {
                //CTRACE("status=%d", req->response.in.ctx.code.status);
            }

        } else {
            //CTRACE("status=%d", req->response.in.ctx.code.status);
        }

        //CTRACE("received %d bytes of body", ctx->bodysz);
        if ((unsigned)req->response.in.ctx.code.status < countof(nreq)) {
            ++nreq[req->response.in.ctx.code.status];
            nbytes[req->response.in.ctx.code.status] += ctx->bodysz;
        }

        if (tts > 0) {
            //CTRACE("sleeping for %"PRId64" seconds", tts);
            res = mrkthr_sleep(tts * 1000);
        }
    }

    return res;
}


static int
add_header_cb(mnhtestc_header_t *header, mnhttpc_request_t *req)
{
    if (header->key != NULL && header->value != NULL) {
        (void)mnhttpc_request_out_field_addb(req, header->key, header->value);
    }
    return 0;
}


static int
mycb2(mnbytes_t **s, UNUSED void *udata)
{
    int res = 0;
    mnhttpc_t *client = udata;
    mnhttpc_request_t *req = NULL;
    int bsize = -1, delay = -1;
    mnbytes_t *quota;

    if (shutting_down) {
        CTRACE("shutting down");
        goto end;
    }

    if ((req = mnhttpc_get_new(client,
                               proxy_host,
                               proxy_port,
                               *s,
                               mybodycb)) == NULL) {
        CTRACE("Failed to create a request %s with proxy %s:%s",
               BDATASAFE(*s),
               BDATASAFE(proxy_host),
               BDATASAFE(proxy_port));
        res = 1;
        goto end;
    }

    if (use_bsize) {
        if (use_bsize < 0) {
            bsize = randombsize();
        } else {
            bsize = use_bsize;
        }
        mnhttpc_request_out_qterm_addb(req, &_bsiz, bytes_printf("%d", bsize));
    }

    if (use_delay) {
        if (use_delay < 0) {
            delay = randomdelay();
        } else {
            delay = use_delay;
        }
        mnhttpc_request_out_qterm_addb(req, &_dlay, bytes_printf("%d", delay));
    }

    if ((quota = randomquota()) != NULL) {
        mnhttpc_request_out_field_addb(req, &_x_mnhtesto_quota, quota);
        if (quota_selector != NULL) {
            mnhttpc_request_out_field_addb(req, quota_selector, quota);
        }
    }

    //CTRACE("url=%s bsize=%d delay=%d", BDATASAFE(*s), bsize, delay);
    (void)mnhttpc_request_out_field_addb(req, &_connection, &_close);
    (void)mnhttpc_request_out_field_addb(req, &_proxy_connection, &_close);

    (void)array_traverse(&headers, (array_traverser_t)add_header_cb, req);

    if (shutting_down) {
        CTRACE("shutting down");
        goto end;
    }

    if ((res = mnhttpc_request_finalize(req)) != 0) {
        //CTRACE("mnhttpc_request_finalize failure");
        //TR(res);
        goto end;
    }

end:
    mnhttpc_request_destroy(&req);
    return res;
}


static int
mycb1(mnbytes_t **s, void *udata)
{
    int res = 0;
    mnhttpc_t *client = udata;
    mnhttpc_request_t *req = NULL;
    int bsize = -1, delay = -1;
    mnbytes_t *quota;

    if (shutting_down) {
        CTRACE("shutting down");
        goto end;
    }

    if ((req = mnhttpc_get_new(client,
                               proxy_host,
                               proxy_port,
                               *s,
                               mybodycb)) == NULL) {
        CTRACE("Failed to create a request %s with proxy %s:%s",
               BDATASAFE(*s),
               BDATASAFE(proxy_host),
               BDATASAFE(proxy_port));
        res = 1;
        goto end;
    }

    if (use_bsize) {
        if (use_bsize < 0) {
            bsize = randombsize();
        } else {
            bsize = use_bsize;
        }
        mnhttpc_request_out_qterm_addb(req, &_bsiz, bytes_printf("%d", bsize));
    }

    if (use_delay) {
        if (use_delay < 0) {
            delay = randomdelay();
        } else {
            delay = use_delay;
        }
        mnhttpc_request_out_qterm_addb(req, &_dlay, bytes_printf("%d", delay));
    }

    if ((quota = randomquota()) != NULL) {
        mnhttpc_request_out_field_addb(req, &_x_mnhtesto_quota, quota);
        if (quota_selector != NULL) {
            mnhttpc_request_out_field_addb(req, quota_selector, quota);
        }
    }

    //CTRACE("url=%s bsize=%d delay=%d", BDATASAFE(*s), bsize, delay);
    (void)array_traverse(&headers, (array_traverser_t)add_header_cb, req);
    if (shutting_down) {
        CTRACE("shutting down");
        goto end;
    }

    if ((res = mnhttpc_request_finalize(req)) != 0) {
        //CTRACE("mnhttpc_request_finalize failure");
        //TR(res);
        goto end;
    }

end:
    mnhttpc_request_destroy(&req);
    return res;
}


void mndiag_mrkapp_str(int, char *, size_t);


static int
run2(UNUSED int argc, UNUSED void **argv)
{
    int res = 0;
    mnhttpc_t client;
    array_traverser_t cb;

    mnhttpc_init(&client);

    if (keepalive) {
        cb = (array_traverser_t)mycb1;
    } else {
        cb = (array_traverser_t)mycb2;
    }

    while (!shutting_down && --limit) {
        if ((res = array_traverse(&urls, cb, &client)) != 0) {
            //char buf[64];
            //mndiag_mrkapp_str(res, buf, sizeof(buf));
            //CTRACE("client failure: %s", buf);
            res = 0; // we want to report continue
            break;
        }
        if (batch_pause > 0) {
            if (mrkthr_sleep(batch_pause) != 0) {
                res = -1;
                break;
            }
        }
    }
    mnhttpc_fini(&client);
    MRKTHRET(res);
}


void mndiag_mrkthr_str(int, char *, size_t);

static int
run1(UNUSED int argc, UNUSED void **argv)
{
    int res;

    while (!shutting_down) {
        mrkthr_ctx_t *child;
        uint64_t delay;
        char buf[64];

        //CTRACE("Running...");
        child = MRKTHR_SPAWN("run2", run2);
        if (mrkthr_join(child) != 0) {
            break;
        }
        delay = (1 << randomdelay()) * 20;
        //res = mrkthr_set_retval(0);
        //mndiag_mrkthr_str(res, buf, sizeof(buf));
        //CTRACE("sleeping for %"PRId64" ms with corc %s", delay, buf);
        if ((res = mrkthr_sleep(delay)) != 0) {
            mndiag_mrkthr_str(res, buf, sizeof(buf));
            CTRACE("breaking out res %s...", buf);
            break;
        }
    }
    return 0;
}


static int
run0(UNUSED int argc, UNUSED void **argv)
{
    int i;

    for (i = 0; i < parallel; ++i) {
        MRKTHR_SPAWN("run1", run1, i, mycb1);
    }
    MRKTHR_SPAWN("stats0", stats0);
    return 0;
}


static int
print_config_urls(mnbytes_t **url, mnbytestream_t *bs)
{
    return bytestream_nprintf(bs, 1024, " -u %s", BDATA(*url)) <= 0;
}


static int
print_config_quotas(mnbytes_t **quota, mnbytestream_t *bs)
{
    return bytestream_nprintf(bs, 1024, " -Q %s", BDATA(*quota)) <= 0;
}


static int
print_config_headers(mnhtestc_header_t *header, mnbytestream_t *bs)
{
    return bytestream_nprintf(bs, 1024, " -H %s:%s",
            BDATA(header->key), BDATA(header->value)) <= 0;
}


static void
_print_config(char *prog)
{
    mnbytestream_t bs;

    bytestream_init(&bs, 1024);

    if (develop) {
        bytestream_nprintf(&bs, 1024, " -d");
    }

    if (keepalive) {
        bytestream_nprintf(&bs, 1024, " -A");
    }

    if (parallel != MNHTEST_PARALLEL_DEFAULT) {
        bytestream_nprintf(&bs, 1024, " -p %d", parallel);
    }

    array_traverse(&urls, (array_traverser_t)print_config_urls, &bs);

    if (proxy_host != NULL) {
        bytestream_nprintf(&bs, 1024, " -P %s", BDATA(proxy_host));
    }

    if (proxy_port != NULL) {
        bytestream_nprintf(&bs, 1024, ":%s", BDATA(proxy_port));
    }

    if (batch_pause > 0) {
        bytestream_nprintf(&bs, 1024, " -z %d", batch_pause);
    }

    array_traverse(&headers,
                   (array_traverser_t)print_config_headers, &bs);

    if (use_delay) {
        bytestream_nprintf(&bs, 1024, " -D %d", use_delay);
    }

    if (use_bsize) {
        bytestream_nprintf(&bs, 1024, " -B %d", use_bsize);
    }

    if (quota_selector != NULL) {
        bytestream_nprintf(&bs, 1024, " -S %s", BDATA(quota_selector));
    }


    array_traverse(&quotas,
                   (array_traverser_t)print_config_quotas, &bs);
    printf("%s %s\n", basename(prog), SPDATA(&bs));

    bytestream_fini(&bs);
}

int
main(int argc, char **argv)
{
    int ch;
    int idx;

#ifdef HAVE_MALLOC_H
#   ifndef NDEBUG
    /*
     * malloc options
     */
    if (mallopt(M_CHECK_ACTION, 1) != 1) {
        FAIL("mallopt");
    }
    if (mallopt(M_PERTURB, 0x5a) != 1) {
        FAIL("mallopt");
    }
#   endif
#endif

    /*
     * install signal handlers
     */
    if (signal(SIGINT, myterm) == SIG_ERR) {
        return 1;
    }
    if (signal(SIGTERM, myterm) == SIG_ERR) {
        return 1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return 1;
    }
#ifdef SIGINFO
    if (signal(SIGINFO, myinfo) == SIG_ERR) {
        return 1;
    }
#endif

    if (array_init(&urls,
                   sizeof(mnbytes_t *),
                   0,
                   NULL,
                   (array_finalizer_t)url_item_fini) != 0) {
        FAIL("array_init");
    }

    if (array_init(&headers,
                   sizeof(mnhtestc_header_t),
                   0,
                   NULL,
                   (array_finalizer_t)mnhtestc_header_item_fini) != 0) {
        FAIL("array_init");
    }

    if (array_init(&quotas,
                   sizeof(mnbytes_t *),
                   0,
                   NULL,
                   (array_finalizer_t)quota_item_fini) != 0) {
        FAIL("array_init");
    }

    while ((ch = getopt_long(argc,
                             argv,
                             "AB:D:H:hl:P:p:Q:S:u:Vz:",
                             optinfo,
                             &idx)) != -1) {
        switch (ch) {
        case 'A':
            keepalive = 1;
            break;

        case 'B':
            use_bsize = strtol(optarg, NULL, 10);
            break;

        case 'D':
            use_delay = strtol(optarg, NULL, 10);
            break;

        case 'H':
            {
                char *p;

                if ((p = strchr(optarg, ':')) != NULL) {
                    mnhtestc_header_t *h;

                    *p = '\0';
                    ++p;

                    if ((h = array_incr(&headers)) == NULL) {
                        FAIL("array_incr");
                    }

                    h->key = bytes_new_from_str(optarg);
                    BYTES_INCREF(h->key);
                    h->value = bytes_new_from_str(p);
                    BYTES_INCREF(h->value);
                }
            }
            break;

        case 'h':
            usage(argv[0]);
            exit(0);

        case 'l':
            limit = strtol(optarg, NULL, 10);
            break;

        case 'p':
            parallel = strtol(optarg, NULL, 10);
            break;

        case 'P':
            {
                char *p;

                BYTES_DECREF(&proxy_host);
                BYTES_DECREF(&proxy_port);
                if ((p = strchr(optarg, ':')) != NULL) {
                    *p = '\0';
                    ++p;
                    proxy_host = bytes_new_from_str(optarg);
                    BYTES_INCREF(proxy_host);
                    proxy_port = bytes_new_from_str(p);
                    BYTES_INCREF(proxy_port);
                } else {
                    proxy_host = bytes_new_from_str(optarg);
                    BYTES_INCREF(proxy_host);
                }
            }
            break;

        case 'Q':
            {
                if (*optarg == '@') {
                    int fd;
                    struct stat sb;
                    char *buf;
                    char *s0, *s1;

                    if ((fd = open(optarg + 1, O_RDONLY)) == -1) {
                        FAIL("open");
                    }
                    if (fstat(fd, &sb) == -1) {
                        FAIL("fstat");
                    }
                    if ((buf = malloc(sb.st_size + 1)) == NULL) {
                        FAIL("malloc");
                    }
                    if (read(fd, buf, sb.st_size) != sb.st_size) {
                        FAIL("read");
                    }
                    buf[sb.st_size] = '\0';
                    (void)close(fd);
                    for (s0 = buf, s1 = strchr(s0, '\n');
                         s1 != NULL;
                         s0 = s1 + 1, s1 = strchr(s0, '\n')) {

                        mnbytes_t **quota;

                        *s1 = '\0';
                        if (MRKUNLIKELY(
                                (quota = array_incr(&quotas)) == NULL)) {
                            FAIL("array_incr");
                        }
                        *quota = bytes_new_from_str(s0);
                        BYTES_INCREF(*quota);
                    }
                    free(buf);

                } else {
                    mnbytes_t **quota;
                    if (MRKUNLIKELY((quota = array_incr(&quotas)) == NULL)) {
                        FAIL("array_incr");
                    }
                    *quota = bytes_new_from_str(optarg);
                    BYTES_INCREF(*quota);
                }
            }
            break;

        case 'S':
            BYTES_DECREF(&quota_selector);
            quota_selector = bytes_new_from_str(optarg);
            BYTES_INCREF(quota_selector);
            break;

        case 'u':
            {
                mnbytes_t **url;
                if (MRKUNLIKELY((url = array_incr(&urls)) == NULL)) {
                    FAIL("array_incr");
                }
                *url = bytes_new_from_str(optarg);
                BYTES_INCREF(*url);
            }
            break;

        case 'V':
            printf("%s\n", VERSION);
            exit(0);

        case 'z':
            batch_pause = strtol(optarg, NULL, 10);
            break;

        case 0:
            break;

        default:
            usage(argv[0]);
            exit(1);
        }
    }

    if (!INB0(MNHTEST_PARALLEL_MIN, parallel, MNHTEST_PARALLEL_MAX)) {
        parallel = MNHTEST_PARALLEL_DEFAULT;
    }

    limit = MAX(0, limit);
    assert(limit >= 0);

    if (batch_pause < 0) {
        CTRACE("--pause cannot be negative.");
        usage(argv[0]);
        exit(1);
    }

    if (urls.elnum == 0) {
        CTRACE("URLs cannot be empty.");
        usage(argv[0]);
        exit(1);
    }

    if (print_config) {
        _print_config(argv[0]);
        exit(0);
    }

    argc -= optind;
    argv += optind;

    if (develop) {
        CTRACE("will run in develop mode");
    } else {
        /*
         * daemonize
         */
        //daemon_ize();
    }

    srandom(time(NULL));
    SSL_load_error_strings();
    SSL_library_init();
    (void)mrkthr_init();
    mrkthr_set_stacksize(4096 * 6);
    (void)MRKTHR_SPAWN("run0", run0, argc, argv);
    (void)mrkthr_loop();
    (void)mrkthr_fini();

    return 0;
}

