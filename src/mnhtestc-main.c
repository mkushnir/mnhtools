#include <err.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include <openssl/ssl.h>

#include "config.h"

#ifdef HAVE_MALLOC_H
#   include <malloc.h>
#endif

#include <mrkcommon/array.h>
#include <mrkcommon/bytes.h>
#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>

#include <mrkthr.h>
#include <mnhttpc.h>

#include "diag.h"

#ifndef NDEBUG
const char *_malloc_options = "AJ";
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

static int develop = 0;
static int keepalive = 0;
#define MNHTEST_PARALLEL_MIN 1
#define MNHTEST_PARALLEL_MAX 1000
#define MNHTEST_PARALLEL_DEFAULT MNHTEST_PARALLEL_MIN
static int parallel = 0;
static int batch_pause;

/*
 * Runtime.
 */
static bool shutting_down = false;
static bool sigshutdown_sent = false;

/*
 * mnbytes_t *
 */
static mnarray_t urls;
static mnarray_t headers;
static mnbytes_t *proxy_host;
static mnbytes_t *proxy_port;

static unsigned long nreq;
static unsigned long nbytes;


static struct option optinfo[] = {
#define MNHTESTC_OPT_HELP       0
    {"help", no_argument, NULL, 'h'},
#define MNHTESTC_OPT_VERSION    1
    {"version", no_argument, NULL, 'V'},
#define MNHTESTC_OPT_DEVELOP    2
    {"develop", no_argument, &develop, 1},
#define MNHTESTC_OPT_KEEPALIVE  3
    {"keepalive", no_argument, &keepalive, 'A'},
#define MNHTESTC_OPT_PARALLEL   4
    {"parallel", required_argument, &parallel, 'p'},
#define MNHTESTC_OPT_URL        5
    {"url", required_argument, NULL, 'u'},
#define MNHTESTC_OPT_PROXY      6
    {"proxy", required_argument, NULL, 'P'},
#define MNHTESTC_OPT_PAUSE      7
    {"pause", required_argument, &batch_pause, 'z'},
#define MNHTESTC_OPT_HEADER     8
    {"header", required_argument, NULL, 'H'},

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


static void
usage(char *p)
{
    printf("Usage: %s OPTIONS\n"
"\n"
"Options:\n"
"  --help|-h                    Show this message and exit.\n"
"  --version|-V                 Print version and exit.\n"
"  --develop                    Run in develop mode.\n"
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
    while (!shutting_down && mrkthr_sleep(2000) == 0) {
        CTRACE("nreq %ld nbytes %ld", nreq, nbytes);
        nreq = 0;
        nbytes = 0;
    }
    return 0;
}


static int
mybodycb(mnhttp_ctx_t *ctx,
         UNUSED mnbytestream_t *bs,
         UNUSED mnhttpc_request_t *req)
{
    if (mnhttp_ctx_last_chunk(ctx)) {
        ++nreq;
        nbytes += ctx->bodysz;
        //CTRACE("received %d bytes of body", ctx->bodysz);
    }
    return 0;
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
    int bsize, delay;

    if (shutting_down) {
        goto end;
    }
    bsize = randombsize();
    delay = randomdelay();
    //CTRACE("url=%s bsize=%d delay=%d", BDATASAFE(*s), bsize, delay);
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
    mnhttpc_request_out_qterm_addb(req, &_bsiz, bytes_printf("%d", bsize));
    mnhttpc_request_out_qterm_addb(req, &_dlay, bytes_printf("%d", delay));
    (void)mnhttpc_request_out_field_addb(req, &_connection, &_close);
    (void)mnhttpc_request_out_field_addb(req, &_proxy_connection, &_close);
    (void)array_traverse(&headers, (array_traverser_t)add_header_cb, req);
    if (shutting_down) {
        goto end;
    }
    if ((res = mnhttpc_request_finalize(req)) != 0) {
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
    int bsize, delay;

    if (shutting_down) {
        goto end;
    }
    bsize = randombsize();
    delay = randomdelay();
    //CTRACE("url=%s bsize=%d delay=%d", BDATASAFE(*s), bsize, delay);
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
    mnhttpc_request_out_qterm_addb(req, &_bsiz, bytes_printf("%d", bsize));
    mnhttpc_request_out_qterm_addb(req, &_dlay, bytes_printf("%d", delay));
    (void)array_traverse(&headers, (array_traverser_t)add_header_cb, req);
    if (shutting_down) {
        goto end;
    }
    if ((res = mnhttpc_request_finalize(req)) != 0) {
        goto end;
    }

end:
    mnhttpc_request_destroy(&req);
    return res;
}


static int
run1(UNUSED int argc, UNUSED void **argv)
{
    mnhttpc_t client;
    array_traverser_t cb;

    mnhttpc_init(&client);


    if (keepalive) {
        cb = (array_traverser_t)mycb1;
    } else {
        cb = (array_traverser_t)mycb2;
    }
    while (!shutting_down) {
        if (array_traverse(&urls, cb, &client) != 0) {
            break;
        }
        if (batch_pause > 0) {
            if (mrkthr_sleep(batch_pause)) {
                break;
            }
        }
    }
    mnhttpc_fini(&client);
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

    while ((ch = getopt_long(argc, argv, "AH:hP:p:u:Vz:", optinfo, &idx)) != -1) {
        switch (ch) {
        case 'A':
            keepalive = 1;
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

        case 1:
        case 'z':
            break;

        default:
            usage(argv[0]);
            exit(1);
        }
    }

    if (!INB0(MNHTEST_PARALLEL_MIN, parallel, MNHTEST_PARALLEL_MAX)) {
        parallel = MNHTEST_PARALLEL_DEFAULT;
    }

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
    (void)MRKTHR_SPAWN("run0", run0, argc, argv);
    (void)mrkthr_loop();
    (void)mrkthr_fini();

    return 0;
}

