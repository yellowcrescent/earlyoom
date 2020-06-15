// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "globals.h"
#include "kill.h"
#include "msg.h"


regex_t _c_prefer_regex;
regex_t _c_avoid_regex;
regex_t _c_user_regex;
regex_t _c_old_regex;
char _c_emerg_kill[EMERG_KILL_MAXLEN];


int parse_config(char* filename, poll_loop_args_t* confdata)
{
    FILE* f;
    char* tpos;
    char* line = NULL;
    char ckey[64];
    char cvalue[512];
    int regerr = 0;
    size_t buflen = 0;
    ssize_t read;

    fprintf(stderr, "Loading configuration from %s\n", filename);

    if ((f = fopen(filename, "r")) == NULL) {
        fatal(7, "failed to read configuration file '%s': %s\n", filename, strerror(errno));
    }

    while ((read = getline(&line, &buflen, f)) != -1) {
        if (read < 2) {
            continue;
        } else if(line[0] == '#' || line[0] == ';') {
            continue;
        }

        if ((tpos = strchr(line, '=')) == NULL) {
            continue;
        }

        *tpos = (char)'\0';
        strncpy(ckey, line, sizeof(ckey));
        strncpy(cvalue, tpos + 1, sizeof(cvalue));
        cvalue[strlen(cvalue) - 1] = (char)'\0';

        if (!strcmp(ckey, "report_interval")) {
            confdata->report_interval_ms = atoi(cvalue) * 1000;
        } else if (!strcmp(ckey, "nice")) {
            confdata->nice = (cvalue[0] == 'y' || cvalue[0] == '1') ? true : false;
        } else if (!strcmp(ckey, "ignore_oom_score_adj")) {
            confdata->ignore_oom_score_adj = (cvalue[0] == 'y' || cvalue[0] == '1') ? true : false;
        } else if (!strcmp(ckey, "notify_dbus")) {
            confdata->notify = (cvalue[0] == 'y' || cvalue[0] == '1') ? true : false;
        } else if (!strcmp(ckey, "memory_high")) {
            confdata->mem_high_percent = atof(cvalue);
        } else if (!strcmp(ckey, "memory_low")) {
            confdata->mem_term_percent = atof(cvalue);
        } else if (!strcmp(ckey, "memory_kill")) {
            confdata->mem_kill_percent = atof(cvalue);
        } else if (!strcmp(ckey, "memory_emerg")) {
            confdata->mem_emerg_percent = atof(cvalue);
        } else if (!strcmp(ckey, "swap_low")) {
            confdata->swap_term_percent = atof(cvalue);
        } else if (!strcmp(ckey, "swap_kill")) {
            confdata->swap_kill_percent = atof(cvalue);
        } else if (!strcmp(ckey, "prefer_regex")) {
            confdata->prefer_regex = &_c_prefer_regex;
            if ((regerr = regcomp(confdata->prefer_regex, cvalue, REG_EXTENDED | REG_NOSUB)) != 0) {
                fatal(6, "could not compile regexp '%s'\n", cvalue);
            }
            fprintf(stderr, "Preferring to kill process names that match regex '%s'\n", cvalue);
        } else if (!strcmp(ckey, "avoid_regex")) {
            confdata->avoid_regex = &_c_avoid_regex;
            if ((regerr = regcomp(confdata->avoid_regex, cvalue, REG_EXTENDED | REG_NOSUB)) != 0) {
                fatal(6, "could not compile regexp '%s'\n", cvalue);
            }
            fprintf(stderr, "Will avoid killing process names that match regex '%s'\n", cvalue);
        } else if (!strcmp(ckey, "avoid_users")) {
            confdata->avoid_users = &_c_user_regex;
            if ((regerr = regcomp(confdata->avoid_users, cvalue, REG_EXTENDED | REG_NOSUB)) != 0) {
                fatal(6, "could not compile regexp '%s'\n", cvalue);
            }
            fprintf(stderr, "Will avoid killing process owned by users that match regex '%s'\n", cvalue);
        } else if (!strcmp(ckey, "prefer_old")) {
            confdata->prefer_old = &_c_old_regex;
            if ((regerr = regcomp(confdata->prefer_old, cvalue, REG_EXTENDED | REG_NOSUB)) != 0) {
                fatal(6, "could not compile regexp '%s'\n", cvalue);
            }
            fprintf(stderr, "Preferring to kill old processes by age that match regex '%s'\n", cvalue);
        } else if (!strcmp(ckey, "emerg_kill")) {
            confdata->emerg_kill = _c_emerg_kill;
            strncpy(confdata->emerg_kill, cvalue, EMERG_KILL_MAXLEN);
            fprintf(stderr, "In case of emergency, will kill the following processes: %s\n", confdata->emerg_kill);
        } else {
            warn("warning: unrecognized config parameter '%s'\n", ckey);
            continue;
        }

        // if we successfully got a good key/value pair, then print it
        // for debugging purposes
        debug("parse_config: set %s = '%s'\n", ckey, cvalue);
    }

    fclose(f);
    debug("parse_config: configuration loaded\n");
    return 0;
}
