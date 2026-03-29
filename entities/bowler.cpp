#include "entities.h"
#include "helpers.h"

#include <ctime>
#include <random>
#include <unistd.h>
using namespace std;

//  BOWLER ROUTINE
//
//  Each ball:
//    1. Lock ball_mutex
//    2. Set delivery_bowled = true, signal delivery_cond
//    3. Wait for delivery_resolved (striker done with ball)
//    4. Unlock ball_mutex
//    5. Advance ball count under score_mutex
//  One bowler thread is active at a time, spawned by the
//  bowler_manager when an over begins. It bowls exactly
//  BALLS_PER_OVER (6) deliveries, then spins until the
//  bowler_manager retires it and starts the next bowler.
//  The key handshake:
//    bowler sets delivery_bowled=true  → striker wakes up
//    striker/fielder/keeper sets delivery_resolved=true → bowler wakes up
void* bowler_routine(void* arg) {
    auto* pcb = static_cast<PlayerControlBlock*>(arg);
    if (!pcb) return nullptr;

    while (!MatchOver() && pcb->is_active_on_pitch) {
        if (pcb->deliveries_bowled >= BALLS_PER_OVER) {
            // This bowler's over is done; wait for manager to retire us.
            // We spin here instead of exiting because the manager needs to pthread_join us
            usleep(5000);
            continue;
        }

        // Wait for both batsmen to be on the pitch before bowling
        // Bowling with only one batsman would leave the run-exchange logic in a broken state
        {
            LockChecked(&score_mutex, "score_mutex lock (bowler wait batsmen)"); // lock for checking if both batsmen are on pitch before bowling
            bool ready = (active_batsmen_count >= 2) || match_completed;
            UnlockChecked(&score_mutex, "score_mutex unlock (bowler wait batsmen)"); // unlock after checking if both batsmen are on pitch
             if (!ready) {
                usleep(5000);  // poll every 5ms until both batsmen are on pitch
                continue;
            }
            if (!ready) {
                usleep(5000);  // poll every 5ms until both batsmen are on pitch
                continue;
            }
        }

        // ── Bowl a delivery ──────────────────────────────
        LockChecked(&ball_mutex, "ball_mutex lock (bowler)"); // lock ball_mutex to set up delivery state and signal striker

        // Wait until previous ball is fully resolved
        // First ball: delivery_resolved starts true in main, so this passes immediately
        while (!delivery_resolved && !match_completed) { 
            // First ball: delivery_resolved starts true in main
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500L * 1000 * 1000; // 500ms timeout — prevents permanent block if striker exits early
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&resolved_cond, &ball_mutex, &ts);
        }
        if (match_completed) {
            UnlockChecked(&ball_mutex, "ball_mutex unlock (bowler)");
            break;
        }

        // Set up delivery: reset shared state so striker/fielder get a clean slate
        delivery_resolved = false;   // will be set back to true by whoever resolves this ball
        delivery_bowled   = true;    // striker is watching for this flag
        shared_hit_result = INVALID_HIT_RESULT;  // -999: no result yet for this delivery

        // 5% chance of bowling a wide — penalty run, not a legal delivery
        thread_local mt19937 wide_rng(random_device{}());
        uniform_int_distribution<int> wide_dist(1, 100);
        bool is_wide = (wide_dist(wide_rng) <= 5);
        delivery_is_wide = is_wide;

        if (!is_wide) {
            ++pcb->deliveries_bowled;  // only count legal deliveries toward the over
        }

        UnlockChecked(&ball_mutex, "ball_mutex unlock (bowler)");// unlock ball_mutex after setting up delivery state and signaling striker

        // Read current over.ball for commentary
        LockChecked(&score_mutex, "score_mutex lock (bowler)"); // lock score_mutex to read current over and ball count for commentary
        int ov = g_match_context.current_over; // read current over for commentary
        int bl = g_match_context.balls_in_current_over;// read current ball count for commentary
        UnlockChecked(&score_mutex, "score_mutex unlock (bowler)"); // unlock score_mutex after reading current over and ball count

        LogS3(">> [%s] bowls delivery %d.%d\n", pcb->name, ov, bl + 1);
        LogGanttEvent(pthread_self(), "Bowler", is_wide ? "Wide" : "Bowled", "Pitch");

        if (is_wide) {
            LogS("   ~~ [%s] bowls a WIDE! +1 penalty run.\n", pcb->name);
        }

        // Signal the striker that a ball has been bowled
        // Broadcast (not signal) in case multiple batsmen are checking; only the real striker acts
        BroadcastChecked(&delivery_cond, "delivery_cond broadcast");

        // Wait for the ball to be fully resolved by striker/fielder/keeper before bowling the next one
        // "Resolved" means score updated, wicket processed, or exchange completed — ball is dead
        LockChecked(&ball_mutex, "ball_mutex lock (bowler wait resolved)");
        while (!delivery_resolved && !match_completed) {
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500L * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&resolved_cond, &ball_mutex, &ts);
        }
        UnlockChecked(&ball_mutex, "ball_mutex unlock (bowler)");

        usleep(3000);  // brief pause between deliveries to avoid thundering-herd on the next ball
    }

    return nullptr;
}
