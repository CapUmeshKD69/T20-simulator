#include "helpers.h"
#include "../globals.h"

#include <cerrno>
#include <cstdio>
#include <functional>
using namespace std;

int LockChecked(pthread_mutex_t* m, const char* name) {
    int rc = pthread_mutex_lock(m);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int UnlockChecked(pthread_mutex_t* m, const char* name) {
    int rc = pthread_mutex_unlock(m);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int SignalChecked(pthread_cond_t* c, const char* name) {
    int rc = pthread_cond_signal(c);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int BroadcastChecked(pthread_cond_t* c, const char* name) {
    int rc = pthread_cond_broadcast(c);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}

void Log(const char* msg) {
    LockChecked(&log_mutex, "log_mutex lock");
    fputs(msg, stdout);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf(const char* fmt, int a) {
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, a);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf2(const char* fmt, int a, int b) {
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, a, b);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf3(const char* fmt, int a, int b, int c) {
    LockChecked(&log_mutex, "log_mutex lock");
    printf("------------------------------------------------------\n");
    printf(fmt, a, b, c);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void LogS(const char* fmt, const char* s) {
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, s);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogSI(const char* fmt, const char* s, int a) {
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, s, a);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogSS(const char* fmt, const char* s1, const char* s2) {
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, s1, s2);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogS3(const char* fmt, const char* s, int a, int b) {
    LockChecked(&log_mutex, "log_mutex lock");
    printf("------------------------------------------------------\n");
    printf(fmt, s, a, b);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void LogGanttEvent(pthread_t tid, const char* role, const char* action, const char* resource) {
    LockChecked(&file_mutex, "file_mutex lock");
    if (gantt_log_file.is_open()) {
        size_t tv = hash<pthread_t>{}(tid);
        gantt_log_file << get_timestamp() << ',' << tv << ','
                       << role << ',' << action << ',' << resource << '\n';
        gantt_log_file.flush();
    }
    UnlockChecked(&file_mutex, "file_mutex unlock");
}

bool MatchOver() {
    LockChecked(&score_mutex, "score_mutex lock");
    bool over = (g_match_context.current_over >= MAX_OVERS ||
                 g_match_context.total_wickets >= MAX_WICKETS ||
                 match_completed);
    UnlockChecked(&score_mutex, "score_mutex unlock");
    return over;
}

void SwapStrikeUnsafe() {
    int tmp_id = active_striker_id;
    active_striker_id = non_striker_id;
    non_striker_id = tmp_id;
    PlayerControlBlock* tmp = active_striker_pcb;
    active_striker_pcb = active_non_striker_pcb;
    active_non_striker_pcb = tmp;
}
