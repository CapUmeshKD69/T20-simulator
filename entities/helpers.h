#ifndef ENTITIES_HELPERS_H
#define ENTITIES_HELPERS_H

#include <pthread.h>

int LockChecked(pthread_mutex_t* m, const char* name);
int UnlockChecked(pthread_mutex_t* m, const char* name);
int SignalChecked(pthread_cond_t* c, const char* name);
int BroadcastChecked(pthread_cond_t* c, const char* name);

void Log(const char* msg);
void Logf(const char* fmt, int a);
void Logf2(const char* fmt, int a, int b);
void Logf3(const char* fmt, int a, int b, int c);
void LogS(const char* fmt, const char* s);
void LogSI(const char* fmt, const char* s, int a);
void LogSS(const char* fmt, const char* s1, const char* s2);
void LogS3(const char* fmt, const char* s, int a, int b);

void LogGanttEvent(pthread_t tid, const char* role, const char* action, const char* resource);

bool MatchOver();
void SwapStrikeUnsafe();

#endif  // ENTITIES_HELPERS_H
