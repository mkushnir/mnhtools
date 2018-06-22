#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

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
static int suppress_quotas = 0;

extern unsigned long nreq[600];
extern unsigned long nbytes[600];
extern mnhash_t quotas;


static struct option optinfo[] = {
#define MNHTESTO_OPT_HELP           0
    {"help", no_argument, NULL, 'h'},
#define MNHTESTO_OPT_VERSION        1
    {"version", no_argument, NULL, 'V'},
#define MNHTESTO_OPT_DEVELOP        2
    {"develop", no_argument, &develop, 1},
#define MNHTESTO_OPT_HOST           3
    {"host", required_argument, NULL, 'H'},
#define MNHTESTO_OPT_PORT           4
    {"port", required_argument, NULL, 'P'},
#define MNHTESTO_MAX_CONN           5
    {"max-conn", required_argument, NULL, 'C'},
#define MNHTESTO_MAX_REQ            6
    {"max-req", required_argument, NULL, 'R'},
#define MNHTESTO_QUOTA              7
    {"quota", required_argument, NULL, 'Q'},
#define MNHTESTO_SUPPRESS_QUOTAS    8
    {"suppress-quotas", no_argument, &suppress_quotas, 1},

    {NULL, 0, NULL, 0},
};


/*
 * Run-time contxt.
 */
bool shutting_down = false;
int sigshutdown_sent = 0;

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
    mnhtesto_fini();
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
    printf("Usage: %s [OPTIONS]\n"
"\n"
"Options:\n"
"  --help|-h        Show this message and exit.\n"
"  --version|-V     Print version and exit.\n"
"  --develop        Run in develop mode.\n"
"  --host|-H        Host to listen on. Default %s\n"
"  --port|-P        Port to listen on. Required.\n"
"  --max-conn|-C    Max concurrent connections. Default %d.\n"
"  --max-req|-R     Max concurrent requests. Default %d.\n"
"  --quota|-Q       Apply this quota. Multiple. Quota selector is\n"
"                   %s: HTTP header.\n"
        ,
        basename(p),
        MNHTESTO_DEFAULT_HOST,
        MNHTESTO_DEFAULT_MAX_CONN,
        MNHTESTO_DEFAULT_MAX_REQ,
        BDATA(&_x_mnhtesto_quota));
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
print_quotas(mnbytes_t *qname, mnhtesto_quota_t *quota, UNUSED void *udata)
{
    mnbytes_t *what;
    //mnbytes_t *prorated;
    mnbytes_t *per;

    what = mnhtest_unit_str(&quota->spec.denom_unit, quota->value, MNHTEST_UNIT_STR_VBASE);
    //prorated = mnhtest_unit_str(&quota->spec.denom_unit, quota->prorated, MNHTEST_UNIT_STR_VBASE);
    per = mnhtest_unit_str(&quota->spec.divisor_unit, quota->spec.divisor, 0);
    TRACEC("%s: %s per %s\n",
           BDATA(qname),
           BDATA(what),
           BDATA(per));
    BYTES_DECREF(&what);
    //BYTES_DECREF(&prorated);
    BYTES_DECREF(&per);
    return 0;
}


static void
print_stats(void)
{
    mnfcgi_stats_t *stats;
    unsigned i;

    stats = mnfcgi_app_get_stats(fcgi_app);

    TRACEC("nthreads %d", stats->nthreads);

    for (i = 0; i < countof(nreq); ++i) {
        if (nreq[i] > 0) {
            TRACEC(" % 3d: % 6ld % 9ld", i, nreq[i], nbytes[i]);
            nreq[i] = 0;
            nbytes[i] = 0;
        }
    }
    TRACEC("\n");
    if (!suppress_quotas) {
        hash_traverse(&quotas, (hash_traverser_t)print_quotas, NULL);
        TRACEC("\n");
    }
}


static int
stats0(UNUSED int argc, UNUSED void **argv)
{
    while (mrkthr_sleep(1000) == 0) {
        print_stats();
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

    mnhtesto_init();

    while ((ch = getopt_long(argc,
                             argv,
                             "C:hH:P:Q:R:V",
                             optinfo,
                             &idx)) != -1) {
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

        case 'Q':
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

                    if (parse_quota(s0) != 0) {
                        usage(argv[0]);
                        exit(1);
                    }
                }
                free(buf);
            } else {
                if (parse_quota(optarg) != 0) {
                    usage(argv[0]);
                    exit(1);
                }
            }
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
