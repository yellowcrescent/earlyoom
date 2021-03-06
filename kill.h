/* SPDX-License-Identifier: MIT */
#ifndef KILL_H
#define KILL_H

#include <regex.h>
#include <stdbool.h>

#define EMERG_KILL_MAXLEN 512

typedef struct {
    /* kill processes until we reach the upper watermark */
    double mem_high_percent;
    /* if the available memory AND swap goes below these percentages,
     * we start killing processes */
    double mem_term_percent;
    double mem_kill_percent;
    double mem_emerg_percent;
    double swap_term_percent;
    double swap_kill_percent;
    /* ignore /proc/PID/oom_score_adj? */
    bool ignore_oom_score_adj;
    /* send d-bus notifications? */
    bool notify;
    /* prefer/avoid killing these processes. NULL = no-op. */
    regex_t* prefer_regex;
    regex_t* avoid_regex;
    regex_t* avoid_users;
    regex_t* prefer_old;
    /* memory report interval, in milliseconds */
    int report_interval_ms;
    /* Flag --dryrun was passed */
    bool dryrun;
    bool nice;
    /* comma-delimited list of processes to kill in case of emergency */
    char* emerg_kill;
} poll_loop_args_t;

void kill_largest_process(const poll_loop_args_t* args, int sig);
int kill_emergency(const poll_loop_args_t* args);

#endif
