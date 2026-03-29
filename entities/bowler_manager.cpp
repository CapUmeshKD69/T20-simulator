#include "entities.h"
#include "helpers.h"

#include <cerrno>
#include <cstdio>

// ═══════════════════════════════════════════════════
//  BOWLER MANAGER (Round-Robin over changes)
//
//  Waits for umpire to signal over_change_requested,
//  then retires the current bowler thread and starts
//  the next one from the pool in round-robin order.
// ═══════════════════════════════════════════════════
//
//  We can't create/join threads from within the umpire routine
//  (it would block the umpire while joining the old bowler).
//  The bowler manager runs independently and handles the
//  thread lifecycle — retire old, spawn new — without
//  stalling the umpire's polling loop.
void* bowler_manager_routine(void* arg) {
    (void)arg;
    while (true) {
        // Block until umpire signals that an over has ended, or the match is done
        LockChecked(&score_mutex, "score_mutex lock (bm)");
        while (!over_change_requested && !match_completed) {
            pthread_cond_wait(&bowler_manager_cond, &score_mutex);
        }
        if (match_completed) {
            UnlockChecked(&score_mutex, "score_mutex unlock (bm)");
            break;
        }
        over_change_requested = false;  // consume the signal before releasing the lock
        PlayerControlBlock* prev = current_bowler_pcb;  // save pointer to bowler being retired

        if (bowler_pool_size <= 0) {
            // No bowlers registered in the pool — nothing to swap in
            UnlockChecked(&score_mutex, "score_mutex unlock (bm)");
            continue;
        }
        // Advance index in round-robin: each bowler gets one over at a time
        current_bowler_index = (current_bowler_index + 1) % bowler_pool_size;
        PlayerControlBlock* next = bowler_pool[current_bowler_index];
        current_bowler_pcb = next;  // update shared pointer so striker/umpire see the new bowler
        UnlockChecked(&score_mutex, "score_mutex unlock (bm)");

        // Retire previous bowler thread
        // Set is_active_on_pitch=false so the bowler's loop exits, then join to free its stack
        if (prev && prev->thread_id != static_cast<pthread_t>(0)) {
            prev->is_active_on_pitch = false;
            BroadcastChecked(&resolved_cond, "resolved_cond (retire bowler)");  // wake bowler if it's blocked waiting for resolution
            pthread_join(prev->thread_id, nullptr);
            prev->thread_id = static_cast<pthread_t>(0);  // mark as joined so we don't double-join
        }

        // Start new bowler: reset per-over ball count and spawn its thread
        if (next) {
            next->is_active_on_pitch    = true;
            next->is_waiting_in_pavilion = false;
            next->deliveries_bowled     = 0;  // fresh ball count for the new over
            int rc = pthread_create(&next->thread_id, nullptr, bowler_routine, next);
            if (rc != 0) { errno = rc; perror("pthread_create(bowler)"); }
            LogSS("  [BowlerManager] Over change: %s -> %s\n",
                   prev ? prev->name : "none", next->name);
        }
    }
    return nullptr;
}
