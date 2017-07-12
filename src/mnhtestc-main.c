#include <err.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include <openssl/ssl.h>

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
#include "config.h"

#ifndef NDEBUG
const char *_malloc_options = "AJ";
#endif

static mnbytes_t _bsiz = BYTES_INITIALIZER("bsiz");
static mnbytes_t _dlay = BYTES_INITIALIZER("dlay");
static mnbytes_t _connection = BYTES_INITIALIZER("Connection");
static mnbytes_t _close = BYTES_INITIALIZER("close");

static int develop = 0;
static int keepalive = 0;
#define MNHTEST_PARALLEL_MIN 1
#define MNHTEST_PARALLEL_MAX 1000
#define MNHTEST_PARALLEL_DEFAULT MNHTEST_PARALLEL_MIN
static int parallel = 0;

/*
 * Runtime.
 */
static bool shutting_down = false;
static bool sigshutdown_sent = false;

/*
 * mnbytes_t *
 */
static mnarray_t urls;

static unsigned long nreq;
static unsigned long nbytes;


static struct option optinfo[] = {
#define MNHTESTC_OPT_HELP 0
    {"help", no_argument, NULL, 'h'},
#define MNHTESTC_OPT_VERSION 1
    {"version", no_argument, NULL, 'V'},
#define MNHTESTC_OPT_DEVELOP 2
    {"develop", no_argument, &develop, 1},
#define MNHTESTC_OPT_KEEPALIVE 3
    {"keepalive", no_argument, &keepalive, 'A'},
#define MNHTESTC_OPT_PARALLEL 4
    {"parallel", required_argument, &parallel, 'p'},
#define MNHTESTC_OPT_URL 5
    {"url", required_argument, NULL, 'u'},
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
        " --parallel=N|-p N             Parallel connections.\n"
        "                               Default is %d.\n"
        " --url=URL|-u URL              URL to query. Required. Multiple.\n"
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
    while (mrkthr_sleep(2000) == 0) {
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
mycb2(mnbytes_t **s, UNUSED void *udata)
{
    int res;
    mnhttpc_t *client = udata;
    mnhttpc_request_t *req;
    int bsize, delay;

    bsize = randombsize();
    delay = randomdelay();
    //CTRACE("url=%s bsize=%d delay=%d", BDATASAFE(*s), bsize, delay);
    req = mnhttpc_get_new(client, *s, mybodycb);
    mnhttpc_request_out_qterm_addb(req, &_bsiz, bytes_printf("%d", bsize));
    mnhttpc_request_out_qterm_addb(req, &_dlay, bytes_printf("%d", delay));
    (void)mnhttpc_request_out_field_addb(req, &_connection, &_close);
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
    int res;
    mnhttpc_t *client = udata;
    mnhttpc_request_t *req;
    int bsize, delay;

    bsize = randombsize();
    delay = randomdelay();
    //CTRACE("url=%s bsize=%d delay=%d", BDATASAFE(*s), bsize, delay);
    req = mnhttpc_get_new(client, *s, mybodycb);
    mnhttpc_request_out_qterm_addb(req, &_bsiz, bytes_printf("%d", bsize));
    mnhttpc_request_out_qterm_addb(req, &_dlay, bytes_printf("%d", delay));
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
    while (true) {
        if (array_traverse(&urls, cb, &client) != 0) {
            break;
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

    while ((ch = getopt_long(argc, argv, "Ahp:u:V", optinfo, &idx)) != -1) {
        switch (ch) {
        case 'A':
            keepalive = 1;
            break;

        case 'h':
            usage(argv[0]);
            exit(0);

        case 'p':
            parallel = strtol(optarg, NULL, 10);
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
            break;

        default:
            usage(argv[0]);
            exit(1);
        }
    }

    if (!INB0(MNHTEST_PARALLEL_MIN, parallel, MNHTEST_PARALLEL_MAX)) {
        parallel = MNHTEST_PARALLEL_DEFAULT;
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

