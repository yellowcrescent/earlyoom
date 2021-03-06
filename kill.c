// SPDX-License-Identifier: MIT

/* Kill the most memory-hungy process */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h> // for PATH_MAX
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

#include "globals.h"
#include "kill.h"
#include "meminfo.h"
#include "msg.h"

#define BADNESS_PREFER 300
#define BADNESS_AVOID -300
#define BADNESS_AVOID_USER -150
#define BADNESS_AGE_DIV 600

#define SIGTERM_WAIT 6.0
#define EMERG_LIST_MAX 64
#define PROC_LEN_MAX 32

static int isnumeric(char* str)
{
    int i = 0;

    // Empty string is not numeric
    if (str[0] == 0)
        return 0;

    while (1) {
        if (str[i] == 0) // End of string
            return 1;

        if (isdigit(str[i]) == 0)
            return 0;

        i++;
    }
}

static void notify(const char* summary, const char* body)
{
    int pid = fork();
    if (pid > 0) {
        // parent
        return;
    }
    char summary2[1024] = { 0 };
    snprintf(summary2, sizeof(summary2), "string:%s", summary);
    char body2[1024] = "string:";
    if (body != NULL) {
        snprintf(body2, sizeof(body2), "string:%s", body);
    }
    // Complete command line looks like this:
    // dbus-send --system / net.nuetzlich.SystemNotifications.Notify 'string:summary text' 'string:and body text'
    execl("/usr/bin/dbus-send", "dbus-send", "--system", "/", "net.nuetzlich.SystemNotifications.Notify",
        summary2, body2, NULL);
    warn("notify: exec failed: %s\n", strerror(errno));
    exit(1);
}

/*
 * Send the selected signal to "pid" and wait for the process to exit
 * (max 10 seconds)
 */
int kill_wait(const poll_loop_args_t* args, pid_t pid, int sig)
{
    if (args->dryrun && sig != 0) {
        warn("dryrun, not actually sending any signal\n");
        return 0;
    }
    meminfo_t m = { 0 };
    const unsigned poll_ms = 100;
    int res = kill(pid, sig);
    if (res != 0) {
        return res;
    }
    /* signal 0 does not kill the process. Don't wait for it to exit */
    if (sig == 0) {
        return 0;
    }
    for (unsigned i = 0; i < 100; i++) {
        float secs = (float)(i * poll_ms) / 1000;
        // We have sent SIGTERM but now have dropped below SIGKILL limits.
        // Escalate to SIGKILL.
        if (sig != SIGKILL) {
            m = parse_meminfo();
            print_mem_stats(debug, m);
            if (secs >= SIGTERM_WAIT || (m.MemAvailablePercent <= args->mem_kill_percent && m.SwapFreePercent <= args->swap_kill_percent)) {
                sig = SIGKILL;
                res = kill(pid, sig);
                // kill first, print after
                warn("escalating to SIGKILL after %.1f seconds\n", secs);
                if (res != 0) {
                    return res;
                }
            }
        } else if (enable_debug) {
            m = parse_meminfo();
            print_mem_stats(printf, m);
        }
        if (!is_alive(pid)) {
            warn("process exited after %.1f seconds\n", secs);
            return 0;
        }
        usleep(poll_ms * 1000);
    }
    errno = ETIME;
    return -1;
}

/*
 * Find the process with the largest oom_score and kill it.
 */
void kill_largest_process(const poll_loop_args_t* args, int sig)
{
    struct procinfo victim = { 0 };
    struct timespec t0 = { 0 }, t1 = { 0 };
    char ppath[PATH_LEN] = { 0 };
    struct stat pstat;
    struct passwd* puser;
    proctime_t ptimes;

    if (enable_debug) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
    }

    DIR* procdir = opendir("/proc");
    if (procdir == NULL) {
        fatal(5, "Could not open /proc: %s", strerror(errno));
    }

    int candidates = 0;
    while (1) {
        errno = 0;
        struct dirent* d = readdir(procdir);
        if (d == NULL) {
            if (errno != 0)
                warn("userspace_kill: readdir error: %s", strerror(errno));
            break;
        }

        // proc contains lots of directories not related to processes,
        // skip them
        if (!isnumeric(d->d_name))
            continue;

        struct procinfo cur = {
            .pid = (int)strtol(d->d_name, NULL, 10),
            .uid = -1,
            .badness = -1,
            .VmRSSkiB = -1,
            .utime = 0,
            .stime = 0,
            .rtime = 0
        };

        if (cur.pid <= 1)
            // Let's not kill init.
            continue;

        debug("pid %5d:", cur.pid);

        {
            int res = get_oom_score(cur.pid);
            if (res < 0) {
                debug(" error reading oom_score: %s\n", strerror(-res));
                continue;
            }
            cur.badness = res;
        }
        if (args->ignore_oom_score_adj) {
            int oom_score_adj = 0;
            int res = get_oom_score_adj(cur.pid, &oom_score_adj);
            if (res < 0) {
                debug(" error reading oom_score_adj: %s\n", strerror(-res));
                continue;
            }
            if (oom_score_adj > 0) {
                cur.badness -= oom_score_adj;
            }
        }

        // get times for all processes if prefer_old is enabled
        if (args->prefer_old) {
            ptimes = get_process_times(cur.pid);
            if (ptimes.valid) {
                debug(" [process times: %lu user, %lu sys, %lu real] ", ptimes.c_utime, ptimes.c_stime, ptimes.c_runtime);
                cur.utime = ptimes.c_utime;
                cur.stime = ptimes.c_stime;
                cur.rtime = ptimes.c_runtime;
            }
        }

        if ((args->prefer_regex || args->avoid_regex || args->prefer_old)) {
            int res = get_comm(cur.pid, cur.name, sizeof(cur.name));
            if (res < 0) {
                debug(" error reading process name: %s\n", strerror(-res));
                continue;
            }
            if (args->prefer_regex && regexec(args->prefer_regex, cur.name, (size_t)0, NULL, 0) == 0) {
                cur.badness += BADNESS_PREFER;
            }
            if (args->avoid_regex && regexec(args->avoid_regex, cur.name, (size_t)0, NULL, 0) == 0) {
                cur.badness += BADNESS_AVOID;
            }
            if (args->prefer_old && regexec(args->prefer_old, cur.name, (size_t)0, NULL, 0) == 0) {
                if (ptimes.valid) {
                    cur.badness += (int)(ptimes.c_runtime / BADNESS_AGE_DIV);
                }
            }
        }

        if (args->avoid_users) {
            snprintf(ppath, sizeof(ppath), "/proc/%d", cur.pid);
            if (stat(ppath, &pstat) == 0) {
                if ((puser = getpwuid(pstat.st_uid)) != NULL) {
                    strncpy(cur.username, puser->pw_name, MAX_USERLEN);
                    if (regexec(args->avoid_users, cur.username, (size_t)0, NULL, 0) == 0) {
                        cur.badness += BADNESS_AVOID_USER;
                    }
                } else {
                    debug(" error looking up user with uid %d\n", pstat.st_uid);
                    continue;
                }
            } else {
                debug(" error stat'ing file: %s: %s\n", ppath, strerror(errno));
                continue;
            }
        }

        debug(" badness %3d", cur.badness);
        candidates++;

        if (cur.badness < victim.badness) {
            // skip "type 1", encoded as 1 space
            debug(" \n");
            continue;
        }

        {
            long long res = get_vm_rss_kib(cur.pid);
            if (res < 0) {
                debug(" error reading rss: %s\n", strerror((int)-res));
                continue;
            }
            cur.VmRSSkiB = res;
        }
        debug(" vm_rss %7llu", cur.VmRSSkiB);
        if (cur.VmRSSkiB == 0) {
            // Kernel threads have zero rss
            // skip "type 2", encoded as 2 spaces
            debug("  \n");
            continue;
        }
        if (cur.badness == victim.badness && cur.VmRSSkiB <= victim.VmRSSkiB) {
            // skip "type 3", encoded as 3 spaces
            debug("   \n");
            continue;
        }

        // Skip processes with oom_score_adj = -1000, like the
        // kernel oom killer would.
        int oom_score_adj = 0;
        {
            int res = get_oom_score_adj(cur.pid, &oom_score_adj);
            if (res < 0) {
                debug(" error reading oom_score_adj: %s\n", strerror(-res));
                continue;
            }
            if (oom_score_adj == -1000) {
                // skip "type 4", encoded as 3 spaces
                debug("    \n");
                continue;
            }
        }

        // Fill out remaining fields
        if (strlen(cur.name) == 0) {
            int res = get_comm(cur.pid, cur.name, sizeof(cur.name));
            if (res < 0) {
                debug(" error reading process name: %s\n", strerror(-res));
                continue;
            }
        }
        {
            int res = get_uid(cur.pid);
            if (res < 0) {
                debug(" error reading uid: %s\n", strerror(-res));
                continue;
            }
            cur.uid = res;
        }

        // Save new victim
        victim = cur;
        debug(" uid %4d oom_score_adj %4d \"%s\" <--- new victim\n", victim.uid, oom_score_adj, victim.name);

    } // end of while(1) loop
    closedir(procdir);

    if (candidates <= 1 && victim.pid == getpid()) {
        warn("Only found myself (pid %d) in /proc. Do you use hidpid? See https://github.com/rfjakob/earlyoom/wiki/proc-hidepid\n",
            victim.pid);
        victim.pid = 0;
    }

    if (victim.pid <= 0) {
        warn("Could not find a process to kill. Sleeping 1 second.\n");
        if (args->notify) {
            notify("earlyoom", "Error: Could not find a process to kill. Sleeping 1 second.");
        }
        sleep(1);
        return;
    }

    if (enable_debug) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long delta = (t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000;
        debug("selecting victim took %ld.%03ld ms\n", delta / 1000, delta % 1000);
    }

    char* sig_name = "?";
    if (sig == SIGTERM) {
        sig_name = "SIGTERM";
    } else if (sig == SIGKILL) {
        sig_name = "SIGKILL";
    } else if (sig == 0) {
        sig_name = "0 (no-op signal)";
    }
    // sig == 0 is used as a self-test during startup. Don't notifiy the user.
    if (sig != 0 || enable_debug) {
        warn("sending %s to process %d uid %d/%s \"%s\": badness %d, VmRSS %lld MiB, %lu re / %lu u / %lu s\n",
            sig_name, victim.pid, victim.uid, victim.username, victim.name, victim.badness, victim.VmRSSkiB / 1024,
            victim.rtime, victim.utime, victim.stime);
    }

    int res = kill_wait(args, victim.pid, sig);
    int saved_errno = errno;

    // Send the GUI notification AFTER killing a process. This makes it more likely
    // that there is enough memory to spawn the notification helper.
    if (sig != 0) {
        char notif_args[PATH_MAX + 1000];
        snprintf(notif_args, sizeof(notif_args),
            "Low memory! Killing process %d %s", victim.pid, victim.name);
        if (args->notify) {
            notify("earlyoom", notif_args);
        }
    }

    if (sig == 0) {
        return;
    }

    if (res != 0) {
        warn("kill failed: %s\n", strerror(saved_errno));
        if (args->notify) {
            notify("earlyoom", "Error: Failed to kill process");
        }
        // Killing the process may have failed because we are not running as root.
        // In that case, trying again in 100ms will just yield the same error.
        // Throttle ourselves to not spam the log.
        if (saved_errno == EPERM) {
            warn("sleeping 1 second\n");
            sleep(1);
        }
    }
}

int kill_emergency(const poll_loop_args_t* args)
{
    char victim_list[EMERG_LIST_MAX][PROC_LEN_MAX] = { 0 };
    meminfo_t m = { 0 };
    int kills = 0;
    const char vtok[2] = ",";
    char emerg_kill_str[EMERG_KILL_MAXLEN];

    // create a copy of args->emerg_kill, since strtok
    // will screw up our data by injecting NULLs into it
    strncpy(emerg_kill_str, args->emerg_kill, EMERG_KILL_MAXLEN);

    int num_victim = 0;
    char* ttok = strtok(emerg_kill_str, vtok);

    while (ttok != NULL) {
        strncpy(victim_list[num_victim], ttok, PROC_LEN_MAX);
        ttok = strtok(NULL, vtok);
        num_victim++;
    }

    for (int i = 0; i < num_victim; i++) {
        m = parse_meminfo();
        if (m.MemAvailablePercent > args->mem_high_percent) {
            break;
        }

        char* victim_name = victim_list[i];
        warn("kill_emergency: killing all processes with name '%s'\n", victim_name);

        DIR* procdir = opendir("/proc");
        if (procdir == NULL) {
            fatal(5, "Could not open /proc: %s", strerror(errno));
        }

        while (1) {
            errno = 0;
            struct dirent* d = readdir(procdir);
            if (d == NULL) {
                if (errno != 0) warn("kill_emergency: readdir error: %s", strerror(errno));
                break;
            }

            // proc contains lots of directories not related to processes,
            // skip them
            if (!isnumeric(d->d_name)) continue;

            struct procinfo cur = {
                .pid = (int)strtol(d->d_name, NULL, 10),
                .uid = -1,
                .badness = -1,
                .VmRSSkiB = -1,
                .utime = 0,
                .stime = 0,
                .rtime = 0
            };

            if (cur.pid <= 1)
                // Let's not kill init.
                continue;

            if (strlen(cur.name) == 0) {
                int res = get_comm(cur.pid, cur.name, sizeof(cur.name));
                if (res < 0) {
                    debug(" error reading process name: %s\n", strerror(-res));
                    continue;
                }
            }

            if (!strcmp(cur.name, victim_name)) {
                debug("kill_emergency: sending SIGKILL to process %d (%s)\n", cur.pid, cur.name);
                kill(cur.pid, SIGKILL);
                kills++;
            }
        }
        closedir(procdir);
    }

    warn("kill_emergency: finished after killing %d victims\n", kills);
    return kills;
}
