#include <err.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

#include "config.h"

#ifdef HAVE_MALLOC_H
#   include <malloc.h>
#endif

#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>

#include <mrkthr.h>

#include <mnfcgi_app.h>

#include "diag.h"
#include "mnhtesto.h"

#ifndef NDEBUG
const char *_malloc_options = "AJ";
#endif

static int develop = 0;
#define MNHTESTO_DEFAULT_HOST "localhost"
static char *host;
static char *port;
#define MNHTESTO_DEFAULT_MAX_CONN 5
static int max_conn;
#define MNHTESTO_DEFAULT_MAX_REQ 1
static int max_req;

unsigned long nreq;
unsigned long nbytes;


static struct option optinfo[] = {
#define MNHTESTO_OPT_HELP    0
    {"help", no_argument, NULL, 'h'},
#define MNHTESTO_OPT_VERSION 1
    {"version", no_argument, NULL, 'V'},
#define MNHTESTO_OPT_DEVELOP 2
    {"develop", no_argument, &develop, 1},
#define MNHTESTO_OPT_HOST    3
    {"host", required_argument, NULL, 'H'},
#define MNHTESTO_OPT_PORT    4
    {"port", required_argument, NULL, 'P'},
#define MNHTESTO_MAX_CONN    5
    {"max-conn", required_argument, NULL, 'C'},
#define MNHTESTO_MAX_REQ     6
    {"max-req", required_argument, NULL, 'R'},

    {NULL, 0, NULL, 0},
};


/*
 * Run-time contxt.
 */
bool shutting_down = false;
bool sigshutdown_sent = false;

static mnfcgi_app_t *fcgi_app = NULL;


static int
initall(void)
{
    int res;

    res = 0;
    if (host == NULL) {
        host = strdup(MNHTESTO_DEFAULT_HOST);
    }
    if (port == NULL) {
        TRACE("Port cannot be null.  Missing --port argument.");
        res = 1;
    }
    if (max_conn <= 0) {
        max_conn = MNHTESTO_DEFAULT_MAX_CONN;
    }
    if (max_req <= 0) {
        max_req = MNHTESTO_DEFAULT_MAX_REQ;
    }

    return res;
}


static void
finiall(void)
{
    assert(host != NULL);
    free(host);
    host = NULL;
    if (port != NULL) {
        free(port);
        port = NULL;
    }
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
        "  --host|-H                    Host to listen on. Default %s\n"
        "  --port|-P                    Port to listen on. Required.\n"
        "  --max-conn|-C                Max concurrent connections.\n"
        "                               Default %d.\n"
        "  --max-req|-R                 Max concurrent requests.\n"
        "                               Default %d.\n"
        ,
        basename(p),
        MNHTESTO_DEFAULT_HOST,
        MNHTESTO_DEFAULT_MAX_CONN,
        MNHTESTO_DEFAULT_MAX_REQ);
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
        finiall();
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
        mnfcgi_stats_t *stats;

        stats = mnfcgi_app_get_stats(fcgi_app);
        CTRACE("nthreads %d nreq %ld nbytes %ld", stats->nthreads, nreq, nbytes);
        nreq = 0;
        nbytes = 0;
    }
    return 0;
}


static int
run0(UNUSED int argc, UNUSED void **argv)
{
    MRKTHR_SPAWN("stats0", stats0);

    while (true) {
        int res;

        mnfcgi_app_callback_table_t t = {
            .init_app = mnhtesto_app_init,
            .params_complete = mnfcgi_app_params_complete_select_exact,
            .stdin_end = mnhtesto_stdin_end,
        };
        fcgi_app = mnfcgi_app_new(host, port, max_conn, max_req, &t);

        if (fcgi_app != NULL) {
            mnfcgi_app_serve(fcgi_app);
        }
        mnfcgi_app_destroy(&fcgi_app);

        if ((res = mrkthr_sleep(1000)) != 0) {
            break;
        }
    }
    return 0;
}


int
main(int argc, char **argv)
{
    int res;
    int ch;
    int idx;

    res = 0;

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


    while ((ch = getopt_long(argc, argv, "C:hH:P:R:V", optinfo, &idx)) != -1) {
        switch (ch) {
        case 'C':
            max_conn = strtol(optarg, NULL, 10);
            break;

        case 'h':
            usage(argv[0]);
            exit(0);

        case 'H':
            host = strdup(optarg);
            break;

        case 'P':
            port = strdup(optarg);
            break;

        case 'R':
            max_req = strtol(optarg, NULL, 10);
            break;

        case 'V':
            printf("%s\n", VERSION);
            exit(0);

        case 0:
        case 1:
            /*
             * other options
             */
            break;

        default:
            usage(argv[0]);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    if ((res = initall()) != 0) {
        goto end;
    }

    if (develop) {
        CTRACE("will run in develop mode");
    } else {
        /*
         * daemonize
         */
        //daemon_ize();
    }

    (void)mrkthr_init();
    (void)MRKTHR_SPAWN("run0", run0, argc, argv);
    (void)mrkthr_loop();
    (void)mrkthr_fini();

end:
    finiall();
    return res;
}
