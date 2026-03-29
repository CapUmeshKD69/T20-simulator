#include "helpers.h"
#include "../globals.h"

#include <cerrno>
#include <cstdio>
#include <functional>
using namespace std;
// These are thin wrappers around the raw pthread functions.
// They print an error message to stderr if the operation fails, making
// locking bugs much easier to spot without cluttering every call site
// with error-checking boilerplate.

int LockChecked(pthread_mutex_t* m, const char* name) { // wrapper for pthread_mutex_lock that checks the return code and prints an error message if it fails
    int rc = pthread_mutex_lock(m);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int UnlockChecked(pthread_mutex_t* m, const char* name) { // wrapper for pthread_mutex_unlock that checks the return code and prints an error message if it fails
    int rc = pthread_mutex_unlock(m);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int SignalChecked(pthread_cond_t* c, const char* name) { // wrapper for pthread_cond_signal that checks the return code and prints an error message if it fails
    int rc = pthread_cond_signal(c);  // wakes exactly one thread waiting on this condition
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int BroadcastChecked(pthread_cond_t* c, const char* name) { // wrapper for pthread_cond_broadcast that checks the return code and prints an error message if it fails
    int rc = pthread_cond_broadcast(c);  // wakes ALL threads waiting on this condition
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}

// ── Thread-safe logging helpers ──────────────────────────────────────────────
// All log functions acquire log_mutex before printing.
// Without this, output from concurrent threads would interleave and become unreadable.
// Use the variant that matches the number and type of arguments you want to print.

void Log(const char* msg) { // simple log function for static messages with no formatting, it locks the log_mutex to ensure that the message is printed atomically without interleaving with other threads' output, it also flushes stdout to ensure timely display of logs
    LockChecked(&log_mutex, "log_mutex lock");
    fputs(msg, stdout);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf(const char* fmt, int a) { // log function for a single integer argument, locks log_mutex to ensure atomic printing , used for logging runs scored and wickets in the commentary
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, a);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf2(const char* fmt, int a, int b) { // log function for two integer arguments, locks log_mutex to ensure atomic printing , used for balls numbers and over numbers in the commentary
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, a, b);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf3(const char* fmt, int a, int b, int c) { 
    // Prints a visual separator line before the message to highlight major events
    // (e.g. end of match, over summary) in the output stream
    LockChecked(&log_mutex, "log_mutex lock");
    printf("------------------------------------------------------\n");
    printf(fmt, a, b, c);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void LogS(const char* fmt, const char* s) { // log function for a single string argument, locks log_mutex to ensure atomic printing , used for logging player names in the commentary
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, s);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogSI(const char* fmt, const char* s, int a) {
    // String + integer variant — e.g. "[PlayerName] scores N run(s)"
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, s, a);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogSS(const char* fmt, const char* s1, const char* s2) {
    // Two-string variant — e.g. "[BowlerManager] Over change: BowlerA -> BowlerB"
    LockChecked(&log_mutex, "log_mutex lock");
    printf(fmt, s1, s2);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogS3(const char* fmt, const char* s, int a, int b) {
    // String + two ints; also prints separator — used for per-delivery commentary ("over.ball")
    LockChecked(&log_mutex, "log_mutex lock");
    printf("------------------------------------------------------\n");
    printf(fmt, s, a, b);
    fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

// Appends one timestamped row to the Gantt chart CSV.
// Acquires file_mutex (separate from log_mutex) to avoid interleaving file writes.
// The thread ID is hashed to a portable size_t for CSV-safe output.
void LogGanttEvent(pthread_t tid, const char* role, const char* action, const char* resource) {
    LockChecked(&file_mutex, "file_mutex lock");
    if (gantt_log_file.is_open()) {
        size_t tv = hash<pthread_t>{}(tid);  // hash thread id to a portable numeric value for cross-platform CSV output
        gantt_log_file << get_timestamp() << ',' << tv << ','
                       << role << ',' << action << ',' << resource << '\n';
        gantt_log_file.flush();
    }
    UnlockChecked(&file_mutex, "file_mutex unlock");
}

// Returns true if the match is over — checks all three termination conditions:
//   1. All 20 overs bowled
//   2. 10 wickets fallen
//   3. match_completed flag set explicitly by umpire
bool MatchOver() {
    LockChecked(&score_mutex, "score_mutex lock");
    bool over = (g_match_context.current_over >= MAX_OVERS ||
                 g_match_context.total_wickets >= MAX_WICKETS ||
                 match_completed);
    UnlockChecked(&score_mutex, "score_mutex unlock");
    return over;
}

// Swaps striker and non-striker — both the integer IDs and the PCB pointers.
// IMPORTANT: caller must hold score_mutex before calling this ("Unsafe" suffix = no internal locking)
void SwapStrikeUnsafe() {
    int tmp_id = active_striker_id;
    active_striker_id = non_striker_id;
    non_striker_id = tmp_id;
    PlayerControlBlock* tmp = active_striker_pcb;
    active_striker_pcb = active_non_striker_pcb;
    active_non_striker_pcb = tmp;
}
