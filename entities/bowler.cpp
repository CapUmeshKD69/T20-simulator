#include "entities.h"
#include "helpers.h"

#include <ctime>
#include <unistd.h>

// ═══════════════════════════════════════════════════
//  BOWLER ROUTINE
//
//  Each ball:
//    1. Lock ball_mutex
//    2. Set delivery_bowled = true, signal delivery_cond
//    3. Wait for delivery_resolved (striker done with ball)
//    4. Unlock ball_mutex
//    5. Advance ball count under score_mutex
// ═══════════════════════════════════════════════════
void* bowler_routine(void* arg) {
    auto* pcb = static_cast<PlayerControlBlock*>(arg);
    if (!pcb) return nullptr;

    while (!MatchOver() && pcb->is_active_on_pitch) {
        if (pcb->deliveries_bowled >= BALLS_PER_OVER) {
            // This bowler's over is done; wait for manager to retire us.
            usleep(5000);
            continue;
        }

        // Wait for both batsmen to be on the pitch before bowling
        {
            LockChecked(&score_mutex, "score_mutex lock (bowler wait batsmen)");
            bool ready = (active_batsmen_count >= 2) || match_completed;
            UnlockChecked(&score_mutex, "score_mutex unlock (bowler wait batsmen)");
            if (!ready) {
                usleep(5000);
                continue;
            }
        }

        // ── Bowl a delivery ──────────────────────────────
        LockChecked(&ball_mutex, "ball_mutex lock (bowler)");

        // Wait until previous ball is fully resolved
        while (!delivery_resolved && !match_completed) {
            // First ball: delivery_resolved starts true in main
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500L * 1000 * 1000; // 500ms
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&resolved_cond, &ball_mutex, &ts);
        }
        if (match_completed) {
            UnlockChecked(&ball_mutex, "ball_mutex unlock (bowler)");
            break;
        }

        // Set up delivery
        delivery_resolved = false;
        delivery_bowled   = true;
        shared_hit_result = INVALID_HIT_RESULT;
        ++pcb->deliveries_bowled;

        UnlockChecked(&ball_mutex, "ball_mutex unlock (bowler)");

        // Read current over.ball for commentary
        LockChecked(&score_mutex, "score_mutex lock (bowler)");
        int ov = g_match_context.current_over;
        int bl = g_match_context.balls_in_current_over;
        UnlockChecked(&score_mutex, "score_mutex unlock (bowler)");
    
LogS3(">> [%s] bowls delivery %d.%d\n", pcb->name, ov, bl + 1);
        LogGanttEvent(pthread_self(), "Bowler", "Bowled", "Pitch");

        // Signal the striker that a ball has been bowled
        BroadcastChecked(&delivery_cond, "delivery_cond broadcast");

        // Wait for the ball to be fully resolved by striker
        LockChecked(&ball_mutex, "ball_mutex lock (bowler wait resolved)");
        while (!delivery_resolved && !match_completed) {
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500L * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&resolved_cond, &ball_mutex, &ts);
        }
        UnlockChecked(&ball_mutex, "ball_mutex unlock (bowler)");

        usleep(3000);  // brief pause between deliveries
    }

    return nullptr;
}
