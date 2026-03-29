#ifndef ENTITIES_HELPERS_H
#define ENTITIES_HELPERS_H

#include <pthread.h>

// ── Mutex / Condvar wrappers ─────────────────────────────────────────────────
// Error-checking wrappers around pthread primitives.
// They print to stderr on failure so locking bugs are immediately visible.
int LockChecked(pthread_mutex_t* m, const char* name);
int UnlockChecked(pthread_mutex_t* m, const char* name);
int SignalChecked(pthread_cond_t* c, const char* name);    // wakes ONE waiting thread
int BroadcastChecked(pthread_cond_t* c, const char* name); // wakes ALL waiting threads

// ── Thread-safe logging helpers ──────────────────────────────────────────────
// All variants acquire log_mutex internally before printing to stdout.
// Choose the variant that matches the types of your format arguments.
void Log(const char* msg);                               // plain string, no format args
void Logf(const char* fmt, int a);                      // one integer arg
void Logf2(const char* fmt, int a, int b);              // two integer args
void Logf3(const char* fmt, int a, int b, int c);       // three ints; also prints a separator line before output
void LogS(const char* fmt, const char* s);              // one string arg
void LogSI(const char* fmt, const char* s, int a);      // string + integer (e.g. "[Name] scores N runs")
void LogSS(const char* fmt, const char* s1, const char* s2); // two strings (e.g. "BowlerA -> BowlerB")
void LogS3(const char* fmt, const char* s, int a, int b);    // string + two ints; also prints separator line

// Writes one timestamped row to the Gantt chart CSV file (thread-safe via file_mutex)
void LogGanttEvent(pthread_t tid, const char* role, const char* action, const char* resource);

// Returns true if match has ended: 20 overs bowled, 10 wickets fallen, or match_completed flag set
bool MatchOver();

// Swaps striker/non-striker IDs and PCB pointers in-place.
// CALLER MUST HOLD score_mutex — "Unsafe" means no internal locking
void SwapStrikeUnsafe();

#endif  // ENTITIES_HELPERS_H
