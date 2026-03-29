#include "entities.h"
#include "helpers.h"

#include <cerrno>
#include <cstdio>

// ═══════════════════════════════════════════════════
//  BOWLER MANAGER (Round-Robin over changes)
// ═══════════════════════════════════════════════════
void* bowler_manager_routine(void* arg) {
    (void)arg;
    while (true) {
        LockChecked(&score_mutex, "score_mutex lock (bm)");
        while (!over_change_requested && !match_completed) {
            pthread_cond_wait(&bowler_manager_cond, &score_mutex);
        }
        if (match_completed) {
            UnlockChecked(&score_mutex, "score_mutex unlock (bm)");
            break;
        }
        over_change_requested = false;
        PlayerControlBlock* prev = current_bowler_pcb;

        if (bowler_pool_size <= 0) {
            UnlockChecked(&score_mutex, "score_mutex unlock (bm)");
            continue;
        }
        current_bowler_index = (current_bowler_index + 1) % bowler_pool_size;
        PlayerControlBlock* next = bowler_pool[current_bowler_index];
        current_bowler_pcb = next;
        UnlockChecked(&score_mutex, "score_mutex unlock (bm)");

        // Retire previous bowler thread
        if (prev && prev->thread_id != static_cast<pthread_t>(0)) {
            prev->is_active_on_pitch = false;
            BroadcastChecked(&resolved_cond, "resolved_cond (retire bowler)");
            pthread_join(prev->thread_id, nullptr);
            prev->thread_id = static_cast<pthread_t>(0);
        }

        // Start new bowler
        if (next) {
            next->is_active_on_pitch = true;
            next->is_waiting_in_pavilion = false;
            next->deliveries_bowled = 0;
            int rc = pthread_create(&next->thread_id, nullptr, bowler_routine, next);
            if (rc != 0) { errno = rc; perror("pthread_create(bowler)"); }
            LogSS("  [BowlerManager] Over change: %s -> %s\n",
                   prev ? prev->name : "none", next->name);
        }
    }
    return nullptr;
}
