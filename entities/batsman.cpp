#include "entities.h"
#include "helpers.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <random>
#include <unistd.h>
using namespace std;

// ═══════════════════════════════════════════════════
//  BATSMAN ROUTINE
//
//  The striker waits for delivery_bowled, plays a shot,
//  waits for fielding resolution, then handles runs/wickets.
//  The non-striker only activates during run exchanges.
// ═══════════════════════════════════════════════════
//
//  Every batsman (both openers and all incoming batsmen) runs
//  this exact same function. The thread's behavior at any moment
//  depends on whether it is currently the striker,
//  the non-striker, or waiting in an idle spin.
//  Two batsmen are always on pitch simultaneously
void* batsman_routine(void* arg) {
    auto* pcb = static_cast<PlayerControlBlock*>(arg);
    if (!pcb) return nullptr;
    const int pid = pcb->player_id;  

    
    thread_local mt19937 rng(random_device{}());
    uniform_int_distribution<int> shot_dist(1, 100);   // determines shot outcome every delivery
    uniform_int_distribution<int> crease_dist(1, 100); // determines if batsman makes the crease on a run exchange

    while (!MatchOver() && pcb->is_active_on_pitch) {
        // ── Determine role ──────────────────────────
        // Role can change ball-to-ball (e.g. after an odd-run swap or end of over),
        // so we re-read it at the top of every loop iteration under the score_mutex
        LockChecked(&score_mutex, "score_mutex lock (batsman role)");
        bool is_striker      = (pid == active_striker_id);
        bool is_non_striker  = (pid == non_striker_id);
        bool exchange_needed = run_exchange_needed;
        UnlockChecked(&score_mutex, "score_mutex unlock (batsman role)");

        // ── NON-STRIKER: participate in run exchange if needed ──
        // The non-striker is dormant most of the time. It only activates when the striker
        // hits an odd number of runs (1 or 3), meaning both batsmen must cross to the other end.
        // This is the run-exchange protocol: both threads race to their crease,
        // report the result, and the umpire adjudicates any run-out.
        if (is_non_striker && exchange_needed) {
            LockChecked(&score_mutex, "score_mutex lock (ns exchange)");
            if (!run_exchange_needed || non_striker_crease_done) {
                // Exchange already resolved or we already reported — skip this iteration
                UnlockChecked(&score_mutex, "score_mutex unlock (ns exchange)");
                usleep(2000);
                continue;
            }

            UnlockChecked(&score_mutex, "score_mutex unlock (ns exchange)");

            // Non-striker tries to reach the other crease
            // 3% chance (roll <= 3) of hesitating and failing → potential run-out
            int roll = crease_dist(rng);
            bool let_go = (roll <= 3);

            if (let_go) {
                LogS("  WARNING: [%s] (non-striker) hesitates, FAILS to make crease!\n", pcb->name);
            } else {
                LogS("  OK: [%s] (non-striker) makes it to the other crease.\n", pcb->name);
            }

            // Publish our crease result so the umpire can see both batsmen's outcomes
            LockChecked(&score_mutex, "score_mutex lock (ns result)");
            non_striker_let_go = let_go;
            non_striker_crease_done = true;  // signals umpire that non-striker has reported
            UnlockChecked(&score_mutex, "score_mutex unlock (ns result)");
            BroadcastChecked(&run_exchange_cond, "run_exchange_cond (ns done)");

            // Wait for umpire to resolve
            // Umpire checks both batsmen's flags and decides: full runs / runs-1 / run-out
            LockChecked(&score_mutex, "score_mutex lock (ns wait resolve)");
            while (!exchange_resolved && !match_completed) {
                timespec ts{};
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 2;  // 2s timeout prevents indefinite block if umpire misses the broadcast
                pthread_cond_timedwait(&run_exchange_cond, &score_mutex, &ts);
            }
            UnlockChecked(&score_mutex, "score_mutex unlock (ns wait resolve)");

            // If umpire ruled THIS batsman out, release crease slot and exit the thread
            if (exchange_wicket && exchange_runout_id == pid) {
                LogS("  OUT: [%s] RUN OUT by umpire decision!\n", pcb->name);
                strncpy(pcb->how_out, "run out", sizeof(pcb->how_out) - 1);
                pcb->is_active_on_pitch = false;
                psem_post(&crease_semaphore);  // frees one crease slot → umpire can send in next batsman
                return nullptr;
            }
            continue;
        }

        // ── NON-STRIKER idle ────────────────────────
        // No delivery to react to and no exchange in progress — just busy-wait
        if (!is_striker) {
            usleep(5000);
            continue;
        }

        // ═══════════════════════════════════════════
        //  STRIKER: wait for bowler to deliver
        // ═══════════════════════════════════════════
        // Blocks on delivery_cond, which the bowler broadcasts after setting delivery_bowled=true
        LockChecked(&ball_mutex, "ball_mutex lock (striker wait delivery)");
        while (!delivery_bowled && !match_completed) {
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 300L * 1000 * 1000;  // 300ms timeout: re-checks role frequently in case strike rotated
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&delivery_cond, &ball_mutex, &ts);

            // Re-check if we're still the striker
            // Strike could have rotated (odd run / end of over) while we were sleeping
            LockChecked(&score_mutex, "score_mutex lock (striker recheck)");
            bool still_striker = (pid == active_striker_id);
            UnlockChecked(&score_mutex, "score_mutex unlock (striker recheck)");
            if (!still_striker) {
                UnlockChecked(&ball_mutex, "ball_mutex unlock (not striker)");
                goto continue_outer;  // role changed mid-wait; skip to next iteration without consuming the delivery
            }
        }
        if (match_completed) {
            UnlockChecked(&ball_mutex, "ball_mutex unlock (match done)");
            break;
        }
        delivery_bowled = false;  // consume the delivery; bowler will not re-signal until next ball
        UnlockChecked(&ball_mutex, "ball_mutex unlock (striker got delivery)");

        {
            // ── Play the shot ────────────────────────
            int shot = shot_dist(rng);

            // Probabilities:
            // 1-77  : clean hit (fielders resolve)  (77%)
            // 78-95 : batsman misses, keeper collects (18%)
            // 96-97 : clean bowled (OUT)              ( 2%)
            // 98-100: both miss -> byes               ( 3%)
            if (shot <= 77) {
                // ── CLEAN HIT ────────────────────────
                // Ball is in the air; all fielder threads are woken and compete to claim it.
                // Only one fielder wins (guarded by current_ball_sequence).
                LogS("  >> [%s] smashes the ball into the field!\n", pcb->name);
                ++pcb->balls_faced;

                LockChecked(&ball_mutex, "ball_mutex lock (hit)");
                shared_hit_result = INVALID_HIT_RESULT;
                ++current_ball_sequence;  // unique sequence number per delivery; fielders check this to avoid double-handling
                ball_in_air = true;
                BroadcastChecked(&fielders_cond, "fielders_cond (hit)");

                // Wait for fielder to resolve
                // The winning fielder sets shared_hit_result and signals batsman_cond
                while (shared_hit_result == INVALID_HIT_RESULT && !match_completed) {
                    timespec ts{};
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 500L * 1000 * 1000;
                    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                    int rc = pthread_cond_timedwait(&batsman_cond, &ball_mutex, &ts);
                    if (rc == ETIMEDOUT && shared_hit_result == INVALID_HIT_RESULT) {
                        shared_hit_result = 0; // safety: dot ball if no fielder responded in time
                        Log("  TIMEOUT: No fielder resolved, dot ball.\n");
                    }
                }
                int result = shared_hit_result;  // -1=caught, 0=dot, 1/2/3=runs, 4=boundary, 6=six
                UnlockChecked(&ball_mutex, "ball_mutex unlock (hit resolved)");

                if (result == -1) {
                    // ── CAUGHT OUT ────────────────────
                    // Fielder caught the ball cleanly before it bounced → batsman is out
                    LogS("  OUT: [%s] OUT! Caught by fielder!\n", pcb->name);
                    strncpy(pcb->how_out, "Catch OUT!", sizeof(pcb->how_out) - 1);
                    LockChecked(&score_mutex, "score_mutex lock (caught)");
                    ++g_match_context.total_wickets;
                    ++g_match_context.balls_in_current_over;
                    --active_batsmen_count;
                    pcb->is_active_on_pitch = false;
                    wicket_fell = true;
                    // Track bowler wicket
                    if (current_bowler_pcb) {
                        ++current_bowler_pcb->wickets_taken;
                        ++current_bowler_pcb->total_balls_bowled;
                    }
                    UnlockChecked(&score_mutex, "score_mutex unlock (caught)");

                    // Mark delivery resolved for the bowler
                    // IMPORTANT: bowler is blocked on resolved_cond — must unblock it before returning
                    LockChecked(&ball_mutex, "ball_mutex lock (resolve caught)");
                    delivery_resolved = true;
                    UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve caught)");
                    BroadcastChecked(&resolved_cond, "resolved_cond (caught)");
                    SignalChecked(&umpire_cond, "umpire_cond (caught)");

                    psem_post(&crease_semaphore);  // release crease slot → umpire can send in next batsman
                    return nullptr;
                }

                // ── RUNS SCORED ──────────────────────
                LogSI("  RUNS: [%s] scores %d run(s)!\n", pcb->name, result);
                pcb->runs_scored += result;
                if (result == 4) ++pcb->fours;
                if (result == 6) ++pcb->sixes;
                // Track bowler runs conceded + ball
                LockChecked(&score_mutex, "score_mutex lock (bowler-stats-hit)");
                if (current_bowler_pcb) {
                    current_bowler_pcb->runs_conceded += result;
                    ++current_bowler_pcb->total_balls_bowled;
                }
                UnlockChecked(&score_mutex, "score_mutex unlock (bowler-stats-hit)");

                if (result > 0 && (result % 2) == 1) {
                    // ── ODD RUNS: run exchange needed ──
                    // 1 or 3 runs means both batsmen must cross to the other end.
                    // Set up all exchange flags, then both threads (striker here, non-striker above)
                    // independently report their crease result. The umpire adjudicates.
                    LockChecked(&score_mutex, "score_mutex lock (odd runs)");
                    run_exchange_needed     = true;
                    run_exchange_runs       = result;
                    striker_crease_done     = false;  // reset all flags for this fresh exchange
                    non_striker_crease_done = false;
                    striker_let_go          = false;
                    non_striker_let_go      = false;
                    exchange_resolved       = false;
                    exchange_wicket         = false;
                    exchange_runout_id      = -1;
                    exchange_credited_runs  = result;
                    UnlockChecked(&score_mutex, "score_mutex unlock (odd runs setup)");

                    // Striker tries to reach other end
                    // 6% chance of letting go
                    // 1% chance (roll<=1) striker fails to reach the crease → potential run-out
                    int roll = crease_dist(rng);
                    bool let_go = (roll <= 1);

                    if (let_go) {
                        LogS("  WARNING: [%s] (striker) slips! Fails to make crease!\n", pcb->name);
                    } else {
                        LogS("  OK: [%s] (striker) makes it to the other end.\n", pcb->name);
                    }

                    // Publish striker's result; umpire needs BOTH batsmen's results before resolving
                    LockChecked(&score_mutex, "score_mutex lock (striker crease)");
                    striker_let_go = let_go;
                    striker_crease_done = true;
                    UnlockChecked(&score_mutex, "score_mutex unlock (striker crease)");
                    BroadcastChecked(&run_exchange_cond, "run_exchange_cond (striker done)");

                    // Wait for umpire to resolve exchange
                    // Umpire will set exchange_resolved once both batsmen have reported
                    LockChecked(&score_mutex, "score_mutex lock (striker wait resolve)");
                    while (!exchange_resolved && !match_completed) {
                        timespec ts{};
                        clock_gettime(CLOCK_REALTIME, &ts);
                        ts.tv_sec += 2;
                        pthread_cond_timedwait(&run_exchange_cond, &score_mutex, &ts);
                    }
                    int credited = exchange_credited_runs;  // actual runs umpire decided to award
                    bool wicket  = exchange_wicket;         // true if a run-out was called
                    int  victim  = exchange_runout_id;      // player_id of run-out batsman (-1 if none)
                    UnlockChecked(&score_mutex, "score_mutex unlock (striker wait resolve)");

                    // Advance ball count
                    LockChecked(&score_mutex, "score_mutex lock (advance ball odd)");
                    g_match_context.global_score += credited;
                    ++g_match_context.balls_in_current_over;
                    UnlockChecked(&score_mutex, "score_mutex unlock (advance ball odd)");

                    // Mark delivery resolved
                    LockChecked(&ball_mutex, "ball_mutex lock (resolve odd)");
                    delivery_resolved = true;
                    UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve odd)");
                    BroadcastChecked(&resolved_cond, "resolved_cond (odd)");
                    SignalChecked(&umpire_cond, "umpire_cond (odd)");

                    // If this batsman (the striker) was the one run out, exit the thread
                    if (wicket && victim == pid) {
                        LogS("  OUT: [%s] RUN OUT!\n", pcb->name);
                        strncpy(pcb->how_out, "run out", sizeof(pcb->how_out) - 1);
                        pcb->is_active_on_pitch = false;
                        psem_post(&crease_semaphore);
                        return nullptr;
                    }
                } else {
                    // ── EVEN RUNS or 0: no exchange ──
                    // 0, 2, 4, 6 runs: batsmen don't swap, just update score and proceed
                    LockChecked(&score_mutex, "score_mutex lock (even runs)");
                    g_match_context.global_score += result;
                    ++g_match_context.balls_in_current_over;
                    UnlockChecked(&score_mutex, "score_mutex unlock (even runs)");

                    // Mark delivery resolved
                    LockChecked(&ball_mutex, "ball_mutex lock (resolve even)");
                    delivery_resolved = true;
                    UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve even)");
                    BroadcastChecked(&resolved_cond, "resolved_cond (even)");
                    SignalChecked(&umpire_cond, "umpire_cond (even)");
                }
            } else if (shot <= 95) {
                // ── BATSMAN MISSES, KEEPER COLLECTS ──
                // Ball goes past the bat; keeper thread handles it (may fumble for byes)
                LogS("  MISS: [%s] plays and misses! Ball goes to keeper.\n", pcb->name);
                ++pcb->balls_faced;

                LockChecked(&ball_mutex, "ball_mutex lock (miss)");
                keeper_event_pending = true;      // tells the keeper thread to wake up and resolve
                shared_hit_result = INVALID_HIT_RESULT;
                SignalChecked(&keeper_cond, "keeper_cond (miss)");

                // Wait for keeper to set shared_hit_result (0=dot, 1=bye on fumble)
                while (shared_hit_result == INVALID_HIT_RESULT && !match_completed) {
                    timespec ts{};
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 500L * 1000 * 1000;
                    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                    int rc = pthread_cond_timedwait(&batsman_cond, &ball_mutex, &ts);
                    if (rc == ETIMEDOUT && shared_hit_result == INVALID_HIT_RESULT) {
                        shared_hit_result = 0;  // keeper didn't respond in time; treat as clean gather/dot
                        Log("  TIMEOUT: Keeper didn't respond, dot ball.\n");
                    }
                }
                int bye_result = shared_hit_result;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (miss)");

                if (bye_result > 0) {
                    // Byes! Keeper fumbled
                    LogSI("  BYES: [%s] %d bye(s) scored!\n", pcb->name, bye_result);
                    LockChecked(&score_mutex, "score_mutex lock (byes)");
                    g_match_context.global_score += bye_result;
                    ++g_match_context.balls_in_current_over;
                    if (current_bowler_pcb) {
                        current_bowler_pcb->runs_conceded += bye_result;
                        ++current_bowler_pcb->total_balls_bowled;
                    }
                    if (bye_result % 2 == 1) SwapStrikeUnsafe();  // odd byes rotate strike just like odd runs
                    UnlockChecked(&score_mutex, "score_mutex unlock (byes)");

                    LogS("  -> Striker is now %s\n", active_striker_pcb ? active_striker_pcb->name : "?");
                } else {
                    // Dot ball
                    LogS("  DOT: [%s] dot ball.\n", pcb->name);
                    LockChecked(&score_mutex, "score_mutex lock (dot)");
                    ++g_match_context.balls_in_current_over;
                    if (current_bowler_pcb) ++current_bowler_pcb->total_balls_bowled;
                    UnlockChecked(&score_mutex, "score_mutex unlock (dot)");
                }

                // Unblock the bowler so it can send the next delivery
                LockChecked(&ball_mutex, "ball_mutex lock (resolve miss)");
                delivery_resolved = true;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve miss)");
                BroadcastChecked(&resolved_cond, "resolved_cond (miss)");
                SignalChecked(&umpire_cond, "umpire_cond (miss)");

            } else if (shot <= 97) {
                // ── CLEAN BOWLED (2%) ─────────────────
                // Stumps hit directly; no fielder or keeper involvement — wicket is immediate
                LogS("  WICKET: [%s] CLEAN BOWLED! OUT!\n", pcb->name);
                ++pcb->balls_faced;
                strncpy(pcb->how_out, "bowled", sizeof(pcb->how_out) - 1);

                LockChecked(&score_mutex, "score_mutex lock (bowled)");
                ++g_match_context.total_wickets;
                ++g_match_context.balls_in_current_over;
                --active_batsmen_count;
                pcb->is_active_on_pitch = false;
                wicket_fell = true;
                if (current_bowler_pcb) {
                    ++current_bowler_pcb->wickets_taken;
                    ++current_bowler_pcb->total_balls_bowled;
                }
                UnlockChecked(&score_mutex, "score_mutex unlock (bowled)");

                // IMPORTANT: must still signal bowler before returning — otherwise it waits forever
                LockChecked(&ball_mutex, "ball_mutex lock (resolve bowled)");
                delivery_resolved = true;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve bowled)");
                BroadcastChecked(&resolved_cond, "resolved_cond (bowled)");
                SignalChecked(&umpire_cond, "umpire_cond (bowled)");

                psem_post(&crease_semaphore);  // free crease slot for the next batsman
                return nullptr;
            } else {
                // ── BOTH MISS → BYES ─────────────────
                // Ball beats both batsman AND keeper; keeper still gets to decide if it fumbles
                LogS("  BYES: [%s] ball beats everyone!\n", pcb->name);
                ++pcb->balls_faced;

                LockChecked(&ball_mutex, "ball_mutex lock (byes)");
                keeper_event_pending = true;
                shared_hit_result = INVALID_HIT_RESULT;  // let keeper decide
                SignalChecked(&keeper_cond, "keeper_cond (byes)");

                while (shared_hit_result == INVALID_HIT_RESULT && !match_completed) {
                    timespec ts{};
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 500L * 1000 * 1000;
                    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                    int rc = pthread_cond_timedwait(&batsman_cond, &ball_mutex, &ts);
                    if (rc == ETIMEDOUT && shared_hit_result == INVALID_HIT_RESULT) {
                        shared_hit_result = 1; // default 1 bye if keeper times out
                    }
                }
                int bye_runs = shared_hit_result;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (byes)");

                if (bye_runs > 0) {
                    LogSI("  BYES: [%s] %d bye(s)! Keeper fumbles!\n", pcb->name, bye_runs);
                } else {
                    LogS("  DOT: [%s] keeper gathers, dot ball.\n", pcb->name);
                }

                LockChecked(&score_mutex, "score_mutex lock (byes2)");
                g_match_context.global_score += bye_runs;
                ++g_match_context.balls_in_current_over;
                if (current_bowler_pcb) {
                    current_bowler_pcb->runs_conceded += bye_runs;
                    ++current_bowler_pcb->total_balls_bowled;
                }
                if (bye_runs % 2 == 1) {
                    SwapStrikeUnsafe();  // odd byes also rotate strike, same rule as odd runs
                    LogS("  -> Strike rotated on bye. Striker is now %s\n",
                         active_striker_pcb ? active_striker_pcb->name : "?");
                }
                UnlockChecked(&score_mutex, "score_mutex unlock (byes2)");

                LockChecked(&ball_mutex, "ball_mutex lock (resolve byes)");
                delivery_resolved = true;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve byes)");
                BroadcastChecked(&resolved_cond, "resolved_cond (byes)");
                SignalChecked(&umpire_cond, "umpire_cond (byes)");
            }
        }

        // Check over completion
        // After every ball, if we've reached 6 deliveries, notify umpire to close the over
        {
            LockChecked(&score_mutex, "score_mutex lock (over check)");
            int balls = g_match_context.balls_in_current_over;
            UnlockChecked(&score_mutex, "score_mutex unlock (over check)");

            if (balls >= BALLS_PER_OVER) {
                SignalChecked(&umpire_cond, "umpire_cond (over done)");
            }
        }

        usleep(2000);  // brief pause before looping back for the next delivery
continue_outer:;
    }

    // Normal loop exit: match ended or batsman was retired — release crease slot
    psem_post(&crease_semaphore);
    return nullptr;
}
