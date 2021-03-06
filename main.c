// SPDX-License-Identifier: MIT

/* Check available memory and swap in a loop and start killing
 * processes if they get too low */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#include "globals.h"
#include "kill.h"
#include "meminfo.h"
#include "msg.h"
#include "config.h"

/* Don't fail compilation if the user has an old glibc that
 * does not define MCL_ONFAULT. The kernel may still be recent
 * enough to support the flag.
 */
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

#ifndef VERSION
#define VERSION "*** unknown version ***"
#endif

// Minimum time between invoking kill_emergency(), in milliseconds
#define EMERGENCY_TIMEOUT 30000
#define STATUS_FILENAME "/var/run/earlyoom/status"

/* Arbitrary identifiers for long options that do not have a short
 * version */
enum {
    LONG_OPT_PREFER = 513,
    LONG_OPT_AVOID,
    LONG_OPT_DRYRUN,
};

static int set_oom_score_adj(int);
static void poll_loop(const poll_loop_args_t* args);
static void update_status(int sig, bool emergency, bool high, double memavail, double setpoint);

// Prevent Golang / Cgo name collision when the test suite runs -
// Cgo generates it's own main function.
#ifdef CGO
#define main main2
#endif

double min(double x, double y)
{
    if (x < y)
        return x;
    return y;
}

void handle_sigchld(int sig)
{
    (void)sig; // unused
    waitpid(-1, NULL, WNOHANG);
}

void handle_sigpipe(int sig)
{
    (void)sig; // unused

    // we're gonna log here, just in case this is triggered erroneously.
    // this function will not return
    fatal(99, "SIGPIPE caught! aborting");
}

int main(int argc, char* argv[])
{
    poll_loop_args_t args = {
        .mem_high_percent = 15,
        .mem_term_percent = 10,
        .swap_term_percent = 10,
        .mem_kill_percent = 5,
        .swap_kill_percent = 5,
        .report_interval_ms = 1000,
        /* omitted fields are set to zero */
    };
    int set_my_priority = 0;
    char* prefer_cmds = NULL;
    char* avoid_cmds = NULL;
    char* config_path = NULL;
    regex_t _prefer_regex;
    regex_t _avoid_regex;

    /* request line buffering for stdout - otherwise the output
     * may lag behind stderr */
    setlinebuf(stdout);

    /* clean up dbus-send zombies */
    signal(SIGCHLD, handle_sigchld);
    signal(SIGPIPE, handle_sigpipe);

    fprintf(stderr, "earlyoom " VERSION "\n");

    if (chdir("/proc") != 0) {
        fatal(4, "Could not cd to /proc: %s", strerror(errno));
    }

    meminfo_t m = parse_meminfo();

    int c;
    const char* short_opt = "c:m:s:M:S:kinN:dvr:ph";
    struct option long_opt[] = {
        { "prefer", required_argument, NULL, LONG_OPT_PREFER },
        { "avoid", required_argument, NULL, LONG_OPT_AVOID },
        { "dryrun", no_argument, NULL, LONG_OPT_DRYRUN },
        { "help", no_argument, NULL, 'h' },
        { 0, 0, NULL, 0 } /* end-of-array marker */
    };
    bool have_m = 0, have_M = 0, have_s = 0, have_S = 0;
    double mem_term_kib = 0, mem_kill_kib = 0, swap_term_kib = 0, swap_kill_kib = 0;

    while ((c = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1) {
        float report_interval_f = 0;
        term_kill_tuple_t tuple;

        switch (c) {
        case -1: /* no more arguments */
        case 0: /* long option toggles */
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'm':
            // Use 99 as upper limit. Passing "-m 100" makes no sense.
            tuple = parse_term_kill_tuple(optarg, 99);
            if (strlen(tuple.err)) {
                fatal(15, "-m: %s", tuple.err);
            }
            args.mem_term_percent = tuple.term;
            args.mem_kill_percent = tuple.kill;
            have_m = 1;
            break;
        case 's':
            // Using "-s 100" is a valid way to ignore swap usage
            tuple = parse_term_kill_tuple(optarg, 100);
            if (strlen(tuple.err)) {
                fatal(16, "-s: %s", tuple.err);
            }
            args.swap_term_percent = tuple.term;
            args.swap_kill_percent = tuple.kill;
            have_s = 1;
            break;
        case 'M':
            tuple = parse_term_kill_tuple(optarg, m.MemTotalKiB * 100 / 99);
            if (strlen(tuple.err)) {
                fatal(15, "-M: %s", tuple.err);
            }
            mem_term_kib = tuple.term;
            mem_kill_kib = tuple.kill;
            have_M = 1;
            break;
        case 'S':
            tuple = parse_term_kill_tuple(optarg, m.SwapTotalKiB * 100 / 99);
            if (strlen(tuple.err)) {
                fatal(16, "-S: %s", tuple.err);
            }
            if (m.SwapTotalKiB == 0) {
                warn("warning: -S: total swap is zero, using default percentages\n");
                break;
            }
            swap_term_kib = tuple.term;
            swap_kill_kib = tuple.kill;
            have_S = 1;
            break;
        case 'k':
            fprintf(stderr, "Option -k is ignored since earlyoom v1.2\n");
            break;
        case 'i':
            args.ignore_oom_score_adj = 1;
            fprintf(stderr, "Ignoring positive oom_score_adj values (-i)\n");
            break;
        case 'n':
            args.notify = true;
            fprintf(stderr, "Notifying through D-Bus\n");
            break;
        case 'N':
            args.notify = true;
            fprintf(stderr, "Notifying through D-Bus, argument '%s' ignored for compatability\n", optarg);
            break;
        case 'd':
            enable_debug = 1;
            break;
        case 'v':
            // The version has already been printed above
            exit(0);
        case 'r':
            report_interval_f = strtof(optarg, NULL);
            if (report_interval_f < 0) {
                fatal(14, "-r: invalid interval '%s'\n", optarg);
            }
            args.report_interval_ms = (int)(report_interval_f * 1000);
            break;
        case 'p':
            set_my_priority = 1;
            break;
        case LONG_OPT_PREFER:
            prefer_cmds = optarg;
            break;
        case LONG_OPT_AVOID:
            avoid_cmds = optarg;
            break;
        case LONG_OPT_DRYRUN:
            warn("dryrun mode enabled, will not kill anything\n");
            args.dryrun = 1;
            break;
        case 'h':
            fprintf(stderr,
                "Usage: %s [OPTION]...\n"
                "\n"
                "  -m PERCENT[,KILL_PERCENT] set available memory minimum to PERCENT of total\n"
                "                            (default 10 %%).\n"
                "                            earlyoom sends SIGTERM once below PERCENT, then\n"
                "                            SIGKILL once below KILL_PERCENT (default PERCENT/2).\n"
                "  -s PERCENT[,KILL_PERCENT] set free swap minimum to PERCENT of total (default\n"
                "                            10 %%).\n"
                "                            Note: both memory and swap must be below minimum for\n"
                "                            earlyoom to act.\n"
                "  -M SIZE[,KILL_SIZE]       set available memory minimum to SIZE KiB\n"
                "  -S SIZE[,KILL_SIZE]       set free swap minimum to SIZE KiB\n"
                "  -i                        user-space oom killer should ignore positive\n"
                "                            oom_score_adj values\n"
                "  -n                        enable d-bus notifications\n"
                "  -d                        enable debugging messages\n"
                "  -v                        print version information and exit\n"
                "  -r INTERVAL               memory report interval in seconds (default 1), set\n"
                "                            to 0 to disable completely\n"
                "  -p                        set niceness of earlyoom to -20 and oom_score_adj to\n"
                "                            -100\n"
                "  -c CONFIG_FILE            use configuration file located at CONFIG_PATH\n"
                "                            Note: will override values specified via flags\n"
                "  --prefer REGEX            prefer to kill processes matching REGEX\n"
                "  --avoid REGEX             avoid killing processes matching REGEX\n"
                "  --dryrun                  dry run (do not kill any processes)\n"
                "  -h, --help                this help text\n",
                argv[0]);
            exit(0);
        case '?':
            fprintf(stderr, "Try 'earlyoom --help' for more information.\n");
            exit(13);
        }
    } /* while getopt */

    if (optind < argc) {
        fatal(13, "extra argument not understood: '%s'\n", argv[optind]);
    }
    // Merge "-M" with "-m" values
    if (have_M) {
        double M_term_percent = 100 * mem_term_kib / (double)m.MemTotalKiB;
        double M_kill_percent = 100 * mem_kill_kib / (double)m.MemTotalKiB;
        if (have_m) {
            // Both -m and -M were passed. Use the lower of both values.
            args.mem_term_percent = min(args.mem_term_percent, M_term_percent);
            args.mem_kill_percent = min(args.mem_kill_percent, M_kill_percent);
        } else {
            // Only -M was passed.
            args.mem_term_percent = M_term_percent;
            args.mem_kill_percent = M_kill_percent;
        }
    }
    // Merge "-S" with "-s" values
    if (have_S) {
        double S_term_percent = 100 * swap_term_kib / (double)m.SwapTotalKiB;
        double S_kill_percent = 100 * swap_kill_kib / (double)m.SwapTotalKiB;
        if (have_s) {
            // Both -s and -S were passed. Use the lower of both values.
            args.swap_term_percent = min(args.swap_term_percent, S_term_percent);
            args.swap_kill_percent = min(args.swap_kill_percent, S_kill_percent);
        } else {
            // Only -S was passed.
            args.swap_term_percent = S_term_percent;
            args.swap_kill_percent = S_kill_percent;
        }
    }
    if (prefer_cmds) {
        args.prefer_regex = &_prefer_regex;
        if (regcomp(args.prefer_regex, prefer_cmds, REG_EXTENDED | REG_NOSUB) != 0) {
            fatal(6, "could not compile regexp '%s'\n", prefer_cmds);
        }
        fprintf(stderr, "Preferring to kill process names that match regex '%s'\n", prefer_cmds);
    }
    if (avoid_cmds) {
        args.avoid_regex = &_avoid_regex;
        if (regcomp(args.avoid_regex, avoid_cmds, REG_EXTENDED | REG_NOSUB) != 0) {
            fatal(6, "could not compile regexp '%s'\n", avoid_cmds);
        }
        fprintf(stderr, "Will avoid killing process names that match regex '%s'\n", avoid_cmds);
    }
    if (config_path) {
        parse_config(config_path, &args);
        set_my_priority = args.nice;
    }
    if (set_my_priority) {
        bool fail = 0;
        if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
            warn("Could not set priority: %s. Continuing anyway\n", strerror(errno));
            fail = 1;
        }
        int ret = set_oom_score_adj(-100);
        if (ret != 0) {
            warn("Could not set oom_score_adj: %s. Continuing anyway\n", strerror(ret));
            fail = 1;
        }
        if (!fail) {
            fprintf(stderr, "Priority was raised successfully\n");
        }
    }

    // Print memory limits
    fprintf(stderr, "mem total: %4lld MiB, swap total: %4lld MiB\n",
        m.MemTotalMiB, m.SwapTotalMiB);
    fprintf(stderr, "sending SIGTERM when mem <= " PRIPCT " and swap <= " PRIPCT ",\n",
        args.mem_term_percent, args.swap_term_percent);
    fprintf(stderr, "        SIGKILL when mem <= " PRIPCT " and swap <= " PRIPCT "\n",
        args.mem_kill_percent, args.swap_kill_percent);
    if (args.mem_emerg_percent && args.emerg_kill) {
        fprintf(stderr, "        EMERGENCY when mem <= " PRIPCT " and swap <= " PRIPCT "\n",
            args.mem_emerg_percent, args.swap_kill_percent);
    }
    fprintf(stderr, "writing status to file: %s\n", STATUS_FILENAME);

    /* Dry-run oom kill to make sure stack grows to maximum size before
     * calling mlockall()
     */
    debug("dry-running kill_largest_process()...\n");
    kill_largest_process(&args, 0);

    int err = mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT);
    // kernels older than 4.4 don't support MCL_ONFAULT. Retry without it.
    if (err != 0) {
        err = mlockall(MCL_CURRENT | MCL_FUTURE);
    }
    if (err != 0) {
        perror("Could not lock memory - continuing anyway");
    }

    // Jump into main poll loop
    poll_loop(&args);
    return 0;
}

// Returns errno (success = 0)
static int set_oom_score_adj(int oom_score_adj)
{
    char buf[256];
    pid_t pid = getpid();

    snprintf(buf, sizeof(buf), "/proc/%d/oom_score_adj", pid);
    FILE* f = fopen(buf, "w");
    if (f == NULL) {
        return -1;
    }

    // fprintf returns a negative error code on failure
    int ret1 = fprintf(f, "%d", oom_score_adj);
    // fclose returns a non-zero value on failure and errno contains the error code
    int ret2 = fclose(f);

    if (ret1 < 0) {
        return -ret1;
    }
    if (ret2) {
        return errno;
    }
    return 0;
}

/* Calculate the time we should sleep based upon how far away from the memory and swap
 * limits we are (headroom). Returns a millisecond value between 100 and 1000 (inclusive).
 * The idea is simple: if memory and swap can only fill up so fast, we know how long we can sleep
 * without risking to miss a low memory event.
 */
static unsigned sleep_time_ms(const poll_loop_args_t* args, const meminfo_t* m)
{
    // Maximum expected memory/swap fill rate. In kiB per millisecond ==~ MiB per second.
    const long long mem_fill_rate = 6000; // 6000MiB/s seen with "stress -m 4 --vm-bytes 4G"
    const long long swap_fill_rate = 800; //  800MiB/s seen with membomb on ZRAM
    // Clamp calculated value to this range (milliseconds)
    const unsigned min_sleep = 100;
    const unsigned max_sleep = 1000;

    long long mem_headroom_kib = (long long)((m->MemAvailablePercent - args->mem_term_percent) * 10 * (double)m->MemTotalMiB);
    if (mem_headroom_kib < 0) {
        mem_headroom_kib = 0;
    }
    long long swap_headroom_kib = (long long)((m->SwapFreePercent - args->swap_term_percent) * 10 * (double)m->SwapTotalMiB);
    if (swap_headroom_kib < 0) {
        swap_headroom_kib = 0;
    }
    long long ms = mem_headroom_kib / mem_fill_rate + swap_headroom_kib / swap_fill_rate;
    if (ms < min_sleep) {
        return min_sleep;
    }
    if (ms > max_sleep) {
        return max_sleep;
    }
    return (unsigned)ms;
}

static void update_status(int sig, bool emergency, bool high, double memavail, double setpoint)
{
    FILE* sfile;

    if ((sfile = fopen(STATUS_FILENAME, "w"))) {
        // Write status file, one value per line:
        // StatusCode
        if (high) {
            fprintf(sfile, "high\n");
        } else if (emergency) {
            fprintf(sfile, "emergency\n");
        } else if (sig == SIGTERM) {
            fprintf(sfile, "term\n");
        } else if (sig == SIGKILL) {
            fprintf(sfile, "kill\n");
        } else {
            fprintf(sfile, "ok\n");
        }

        // MemAvailable
        fprintf(sfile, "%0.02f\n", memavail);

        // TriggeredSetpoint
        fprintf(sfile, "%0.02f\n", setpoint);

        // LastUpdate
        fprintf(sfile, "%ld\n", time(NULL));

        fclose(sfile);
    } else {
        warn("failed to write to status file (%s)\n", STATUS_FILENAME);
    }
}

static void poll_loop(const poll_loop_args_t* args)
{
    // Print a a memory report when this reaches zero. We start at zero so
    // we print the first report immediately.
    unsigned int sleep_ms;
    int report_countdown_ms = 0;
    int hystis = 0;
    bool emergency_invoked = false;
    int emergency_timeout_ms = 0;
    double current_setpoint = 0;

    while (1) {
        int sig = 0;
        bool high = false;
        meminfo_t m = parse_meminfo();
        if (args->emerg_kill && emergency_timeout_ms <= 0 &&
            m.MemAvailablePercent <= args->mem_emerg_percent && m.SwapFreePercent <= args->swap_kill_percent) {
            sig = SIGKILL;
            emergency_invoked = true;
            current_setpoint = args->mem_emerg_percent;
            warn("EMERGENCY! at or below emergency limit: mem " PRIPCT ", swap " PRIPCT "\n",
                args->mem_emerg_percent, args->swap_kill_percent);
        } else if (m.MemAvailablePercent <= args->mem_kill_percent && m.SwapFreePercent <= args->swap_kill_percent) {
            print_mem_stats(warn, m);
            warn("low memory! at or below SIGKILL limits: mem " PRIPCT ", swap " PRIPCT "\n",
                args->mem_kill_percent, args->swap_kill_percent);
            sig = SIGKILL;
            current_setpoint = args->mem_kill_percent;
        } else if (m.MemAvailablePercent <= args->mem_term_percent && m.SwapFreePercent <= args->swap_term_percent) {
            print_mem_stats(warn, m);
            warn("low memory! at or below SIGTERM limits: mem " PRIPCT ", swap " PRIPCT "\n",
                args->mem_term_percent, args->swap_term_percent);
            sig = SIGTERM;
            current_setpoint = args->mem_term_percent;
        } else if (hystis > 0) {
            if (m.MemAvailablePercent <= args->mem_high_percent) {
                warn("below high watermark (" PRIPCT "), continuing to kill processes\n",
                    args->mem_high_percent);
                sig = hystis;
                high = true;
                current_setpoint = args->mem_high_percent;
            } else {
                hystis = 0;
                current_setpoint = 0;
                print_mem_stats(warn, m);
                warn("recovery complete (MemAvailable > mem_high_percent)\n");
            }
        }

        // write updated status file
        update_status(sig, emergency_invoked, high, m.MemAvailablePercent, current_setpoint);

        if (sig) {
            if (emergency_invoked) {
                kill_emergency(args);
                sleep_ms = 2000;
                emergency_timeout_ms = EMERGENCY_TIMEOUT;
                emergency_invoked = false;
            } else {
                kill_largest_process(args, sig);
                sleep_ms = (hystis == SIGKILL) ? 50 : 500;
            }
            hystis = sig;
        } else if (args->report_interval_ms && report_countdown_ms <= 0) {
            print_mem_stats(printf, m);
            report_countdown_ms = args->report_interval_ms;
            sleep_ms = sleep_time_ms(args, &m);
        }
        debug("adaptive sleep time: %d ms\n", sleep_ms);
        usleep(sleep_ms * 1000);
        report_countdown_ms -= (int)sleep_ms;
        if (emergency_timeout_ms > 0) {
            emergency_timeout_ms -= (int)sleep_ms;
        }
    }
}
