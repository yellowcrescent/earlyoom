/* SPDX-License-Identifier: MIT */
#ifndef MEMINFO_H
#define MEMINFO_H

#define PATH_LEN 256
#define MAX_USERLEN 33

#include <stdbool.h>

typedef struct {
    // Values from /proc/meminfo, in KiB or converted to MiB.
    long long MemTotalKiB;
    long long MemTotalMiB;
    long long MemAvailableMiB; // -1 means no data available
    long long SwapTotalMiB;
    long long SwapTotalKiB;
    long long SwapFreeMiB;
    // Calculated percentages
    double MemAvailablePercent; // percent of total memory that is available
    double SwapFreePercent; // percent of total swap that is free
} meminfo_t;

struct procinfo {
    int pid;
    int uid;
    int badness;
    long long VmRSSkiB;
    unsigned long utime;
    unsigned long stime;
    unsigned long rtime;
    char name[PATH_LEN];
    char username[MAX_USERLEN];
};

typedef struct {
    // all times are in clock ticks
    // divide by sysconf(_SC_CLK_TCK) to get seconds
    unsigned long utime;
    unsigned long stime;
    unsigned long cutime;
    unsigned long cstime;
    // divide by sysconf(_SC_CLK_TCK) to get seconds
    // then subtract from uptime to get absolute runtime duration
    unsigned long long starttime;
    // calculated times, rounded to nearest second
    unsigned long c_utime;
    unsigned long c_stime;
    unsigned long c_cutime;
    unsigned long c_cstime;
    unsigned long c_runtime;
    bool valid;
} proctime_t;

meminfo_t parse_meminfo();
proctime_t get_process_times(int pid);
float get_uptime();
bool is_alive(int pid);
void print_mem_stats(int (*out_func)(const char* fmt, ...), const meminfo_t m);
int get_oom_score(int pid);
int get_oom_score_adj(const int pid, int* out);
long long get_vm_rss_kib(int pid);
int get_comm(int pid, char* out, size_t outlen);
int get_uid(int pid);

#endif
