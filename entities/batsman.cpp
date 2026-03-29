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
void* batsman_routine(void* arg) {
    auto* pcb = static_cast<PlayerControlBlock*>(arg);
    if (!pcb) return nullptr;
    const int pid = pcb->player_id;

    thread_local mt19937 rng(random_device{}());
    uniform_int_distribution<int> shot_dist(1, 100);
    uniform_int_distribution<int> crease_dist(1, 100);

    while (!MatchOver() && pcb->is_active_on_pitch) {
        // ── Determine role ──────────────────────────
        LockChecked(&score_mutex, "score_mutex lock (batsman role)");
        bool is_striker      = (pid == active_striker_id);
        bool is_non_striker  = (pid == non_striker_id);
        bool exchange_needed = run_exchange_needed;
        UnlockChecked(&score_mutex, "score_mutex unlock (batsman role)");

        // ── NON-STRIKER: participate in run exchange if needed ──
        if (is_non_striker && exchange_needed) {
            LockChecked(&score_mutex, "score_mutex lock (ns exchange)");
            if (!run_exchange_needed || non_striker_crease_done) {
                UnlockChecked(&score_mutex, "score_mutex unlock (ns exchange)");
                usleep(2000);
                continue;
            }

            UnlockChecked(&score_mutex, "score_mutex unlock (ns exchange)");

            // Non-striker tries to reach the other crease
            int roll = crease_dist(rng);
            bool let_go = (roll <=3);

            if (let_go) {
                LogS("  WARNING: [%s] (non-striker) hesitates, FAILS to make crease!\n", pcb->name);
            } else {
                LogS("  OK: [%s] (non-striker) makes it to the other crease.\n", pcb->name);
            }

            LockChecked(&score_mutex, "score_mutex lock (ns result)");
            non_striker_let_go = let_go;
            non_striker_crease_done = true;
            UnlockChecked(&score_mutex, "score_mutex unlock (ns result)");
            BroadcastChecked(&run_exchange_cond, "run_exchange_cond (ns done)");

            // Wait for umpire to resolve
            LockChecked(&score_mutex, "score_mutex lock (ns wait resolve)");
            while (!exchange_resolved && !match_completed) {
                timespec ts{};
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 2;
                pthread_cond_timedwait(&run_exchange_cond, &score_mutex, &ts);
            }
            UnlockChecked(&score_mutex, "score_mutex unlock (ns wait resolve)");

            if (exchange_wicket && exchange_runout_id == pid) {
                LogS("  OUT: [%s] RUN OUT by umpire decision!\n", pcb->name);
                strncpy(pcb->how_out, "run out", sizeof(pcb->how_out) - 1);
                pcb->is_active_on_pitch = false;
                psem_post(&crease_semaphore);
                return nullptr;
            }
            continue;
        }

        // ── NON-STRIKER idle ────────────────────────
        if (!is_striker) {
            usleep(5000);
            continue;
        }

        // ═══════════════════════════════════════════
        //  STRIKER: wait for bowler to deliver
        // ═══════════════════════════════════════════
        LockChecked(&ball_mutex, "ball_mutex lock (striker wait delivery)");
        while (!delivery_bowled && !match_completed) {
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 300L * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&delivery_cond, &ball_mutex, &ts);

            // Re-check if we're still the striker
            LockChecked(&score_mutex, "score_mutex lock (striker recheck)");
            bool still_striker = (pid == active_striker_id);
            UnlockChecked(&score_mutex, "score_mutex unlock (striker recheck)");
            if (!still_striker) {
                UnlockChecked(&ball_mutex, "ball_mutex unlock (not striker)");
                goto continue_outer;
            }
        }
        if (match_completed) {
            UnlockChecked(&ball_mutex, "ball_mutex unlock (match done)");
            break;
        }
        delivery_bowled = false;
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
                LogS("  >> [%s] smashes the ball into the field!\n", pcb->name);
                ++pcb->balls_faced;

                LockChecked(&ball_mutex, "ball_mutex lock (hit)");
                shared_hit_result = INVALID_HIT_RESULT;
                ++current_ball_sequence;
                ball_in_air   = true;
                BroadcastChecked(&fielders_cond, "fielders_cond (hit)");

                // Wait for fielder to resolve
                while (shared_hit_result == INVALID_HIT_RESULT && !match_completed) {
                    timespec ts{};
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 500L * 1000 * 1000;
                    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                    int rc = pthread_cond_timedwait(&batsman_cond, &ball_mutex, &ts);
                    if (rc == ETIMEDOUT && shared_hit_result == INVALID_HIT_RESULT) {
                        shared_hit_result = 0; // safety: dot ball
                        Log("  TIMEOUT: No fielder resolved, dot ball.\n");
                    }
                }
                int result = shared_hit_result;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (hit resolved)");

                if (result == -1) {
                    // ── CAUGHT OUT ────────────────────
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
                    LockChecked(&ball_mutex, "ball_mutex lock (resolve caught)");
                    delivery_resolved = true;
                    UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve caught)");
                    BroadcastChecked(&resolved_cond, "resolved_cond (caught)");
                    SignalChecked(&umpire_cond, "umpire_cond (caught)");

                    psem_post(&crease_semaphore);
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
                    LockChecked(&score_mutex, "score_mutex lock (odd runs)");
                    run_exchange_needed     = true;
                    run_exchange_runs       = result;
                    striker_crease_done     = false;
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
                    int roll = crease_dist(rng);
                    bool let_go = (roll <= 1);

                    if (let_go) {
                        LogS("  WARNING: [%s] (striker) slips! Fails to make crease!\n", pcb->name);
                    } else {
                        LogS("  OK: [%s] (striker) makes it to the other end.\n", pcb->name);
                    }

                    LockChecked(&score_mutex, "score_mutex lock (striker crease)");
                    striker_let_go = let_go;
                    striker_crease_done = true;
                    UnlockChecked(&score_mutex, "score_mutex unlock (striker crease)");
                    BroadcastChecked(&run_exchange_cond, "run_exchange_cond (striker done)");

                    // Wait for umpire to resolve exchange
                    LockChecked(&score_mutex, "score_mutex lock (striker wait resolve)");
                    while (!exchange_resolved && !match_completed) {
                        timespec ts{};
                        clock_gettime(CLOCK_REALTIME, &ts);
                        ts.tv_sec += 2;
                        pthread_cond_timedwait(&run_exchange_cond, &score_mutex, &ts);
                    }
                    int credited = exchange_credited_runs;
                    bool wicket  = exchange_wicket;
                    int  victim  = exchange_runout_id;
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

                    if (wicket && victim == pid) {
                        LogS("  OUT: [%s] RUN OUT!\n", pcb->name);
                        strncpy(pcb->how_out, "run out", sizeof(pcb->how_out) - 1);
                        pcb->is_active_on_pitch = false;
                        psem_post(&crease_semaphore);
                        return nullptr;
                    }
                } else {
                    // ── EVEN RUNS or 0: no exchange ──
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
                LogS("  MISS: [%s] plays and misses! Ball goes to keeper.\n", pcb->name);
                ++pcb->balls_faced;

                LockChecked(&ball_mutex, "ball_mutex lock (miss)");
                keeper_event_pending = true;
                shared_hit_result = INVALID_HIT_RESULT;
                SignalChecked(&keeper_cond, "keeper_cond (miss)");

                while (shared_hit_result == INVALID_HIT_RESULT && !match_completed) {
                    timespec ts{};
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 500L * 1000 * 1000;
                    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                    int rc = pthread_cond_timedwait(&batsman_cond, &ball_mutex, &ts);
                    if (rc == ETIMEDOUT && shared_hit_result == INVALID_HIT_RESULT) {
                        shared_hit_result = 0;
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
                    if (bye_result % 2 == 1) SwapStrikeUnsafe();
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

                LockChecked(&ball_mutex, "ball_mutex lock (resolve miss)");
                delivery_resolved = true;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve miss)");
                BroadcastChecked(&resolved_cond, "resolved_cond (miss)");
                SignalChecked(&umpire_cond, "umpire_cond (miss)");

            } else if (shot <= 97) {
                // ── CLEAN BOWLED (2%) ─────────────────
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

                LockChecked(&ball_mutex, "ball_mutex lock (resolve bowled)");
                delivery_resolved = true;
                UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve bowled)");
                BroadcastChecked(&resolved_cond, "resolved_cond (bowled)");
                SignalChecked(&umpire_cond, "umpire_cond (bowled)");

                psem_post(&crease_semaphore);
                return nullptr;
            } else {
                // ── BOTH MISS → BYES ─────────────────
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
                        shared_hit_result = 1; // default 1 bye
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
                    SwapStrikeUnsafe();
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
        {
            LockChecked(&score_mutex, "score_mutex lock (over check)");
            int balls = g_match_context.balls_in_current_over;
            UnlockChecked(&score_mutex, "score_mutex unlock (over check)");

            if (balls >= BALLS_PER_OVER) {
                SignalChecked(&umpire_cond, "umpire_cond (over done)");
            }
        }

        usleep(2000);
continue_outer:;
    }

    psem_post(&crease_semaphore);
    return nullptr;
}
