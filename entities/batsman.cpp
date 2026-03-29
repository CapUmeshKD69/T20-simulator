#include "entities.h"
#include "helpers.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <random>
#include <unistd.h>
using namespace std;
//  BATSMAN ROUTINE
//
//  The striker waits for delivery_bowled, plays a shot,
//  waits for fielding resolution, then handles runs/wickets.
//  The non-striker only activates during run exchanges.
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
        bool is_wide = false;  // set later under ball_mutex when consuming a delivery
        // ── Determine role ──────────────────────────
        // Role can change ball-to-ball (e.g. after an odd-run swap or end of over),
        // so we re-read it at the top of every loop iteration under the score_mutex
        LockChecked(&score_mutex, "score_mutex lock (batsman role)");
        bool is_striker      = (pid == active_striker_id);// whether this thread is currently the striker
        bool is_non_striker  = (pid == non_striker_id); // whether this thread is currently the non-striker
        bool exchange_needed = run_exchange_needed;
        UnlockChecked(&score_mutex, "score_mutex unlock (batsman role)"); // unlocking score_mutex after reading role and exchange state

        // ── NON-STRIKER: participate in run exchange if needed ──
        // The non-striker is dormant most of the time. It only activates when the striker
        // hits an odd number of runs (1 or 3), meaning both batsmen must cross to the other end.
        // This is the run-exchange protocol: both threads race to their crease,
        // report the result, and the umpire adjudicates any run-out.
        if (is_non_striker && exchange_needed) {
            LockChecked(&score_mutex, "score_mutex lock (ns exchange)"); // Check if exchange still needed (could have been resolved while we were waiting for the lock)
            if (!run_exchange_needed || non_striker_crease_done) {
                // Exchange already resolved or we already reported 
                UnlockChecked(&score_mutex, "score_mutex unlock (ns exchange)");
                usleep(2000);
                continue;
            }

            UnlockChecked(&score_mutex, "score_mutex unlock (ns exchange)");

            // Non-striker tries to reach the other crease
            // 3% chance (roll <= 3) of hesitating and failing → potential run-out
            int roll = crease_dist(rng);// random roll to determine if non-striker makes the crease
            bool let_go = (roll <= 3); // if true, non-striker fails to make the crease

            if (let_go) {
                LogS(" Hesitate :  [%s] (non-striker) hesitates, FAILS to make crease!\n", pcb->name);
            } else {
                LogS("  SUCCESS: [%s] (non-striker) makes it to the other crease.\n", pcb->name);
            }

            // Publish our crease result so the umpire can see both batsmen's outcomes
            LockChecked(&score_mutex, "score_mutex lock (ns result)"); // lock score_mutex to update shared state about the run exchange result
            non_striker_let_go = let_go; // set flag indicating whether non-striker failed to make crease
            non_striker_crease_done = true;  // signals umpire that non-striker has reported
            UnlockChecked(&score_mutex, "score_mutex unlock (ns result)"); // unlock score_mutex after updating run exchange result
            BroadcastChecked(&run_exchange_cond, "run_exchange_cond (ns done)");// wake umpire in case it's waiting for the non-striker's report

            // Wait for umpire to resolve
            // Umpire checks both batsmen's flags and decides: full runs / runs-1 / run-out
            LockChecked(&score_mutex, "score_mutex lock (ns wait resolve)"); // wait for umpire to adjudicate the run exchange based on the reports from both batsmen
            while (!exchange_resolved && !match_completed) {
                timespec ts{};
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 2;  // 2s timeout prevents indefinite block if umpire misses the broadcast
                pthread_cond_timedwait(&run_exchange_cond, &score_mutex, &ts);
            }
            UnlockChecked(&score_mutex, "score_mutex unlock (ns wait resolve)"); // after umpire has resolved the exchange, check if non-striker was run out

            // If umpire ruled THIS batsman out, release crease slot and exit the thread
            if (exchange_wicket && exchange_runout_id == pid) { // non-striker was run out in the exchange
                LogS("  OUT: [%s] RUN OUT by umpire decision!\n", pcb->name);
                strncpy(pcb->how_out, "run out", sizeof(pcb->how_out) - 1);
                pcb->is_active_on_pitch = false; // mark this batsman as no longer active on the pitch
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

         // striker waits for the bowler to bowl the delivery
        // Blocks on delivery_cond, which the bowler broadcasts after setting delivery_bowled=true
        LockChecked(&ball_mutex, "ball_mutex lock (striker wait delivery)"); // wait for the bowler to signal that the delivery has been bowled
        while (!delivery_bowled && !match_completed) { // wait until the bowler has bowled the delivery or match is completed
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
        is_wide = delivery_is_wide;            // read wide flag under ball_mutex
        delivery_is_wide = false;              // consume
        UnlockChecked(&ball_mutex, "ball_mutex unlock (striker got delivery)");

        // ── WIDE BALL: bowler's error, 1 penalty run, not a legal delivery ──
        if (is_wide) {
            Log("  WIDE: Wide ball called! +1 run (extras).\n");

            LockChecked(&score_mutex, "score_mutex lock (wide)");
            g_match_context.global_score += 1;
            g_match_context.total_wides  += 1;
            // DO NOT increment balls_in_current_over — wide is not a legal delivery
            if (current_bowler_pcb) {
                current_bowler_pcb->runs_conceded += 1;
                ++current_bowler_pcb->wides;
            }
            UnlockChecked(&score_mutex, "score_mutex unlock (wide)");

            // Mark delivery resolved so bowler can re-bowl
            LockChecked(&ball_mutex, "ball_mutex lock (resolve wide)");
            delivery_resolved = true;
            UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve wide)");
            BroadcastChecked(&resolved_cond, "resolved_cond (wide)");
            SignalChecked(&umpire_cond, "umpire_cond (wide)");

            continue;  // loop back — batsman didn't face the ball
        }

        {
            // striker plays the shot 
            int shot = shot_dist(rng); // random shot outcome for this delivery

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

                LockChecked(&ball_mutex, "ball_mutex lock (hit)");// set shared state for fielders to resolve this hit
                shared_hit_result = INVALID_HIT_RESULT; // reset shared variable where the winning fielder will write the result (runs/wicket)
                ++current_ball_sequence;  // unique sequence number per delivery; fielders check this to avoid double-handling
                ball_in_air = true; // flag to indicate ball is in the air and needs fielding resolution
                BroadcastChecked(&fielders_cond, "fielders_cond (hit)");

                // Wait for fielder to resolve
                // The winning fielder sets shared_hit_result and signals batsman_cond ,  below we wait for that signal and read the result
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
                 //  if the result is -1, it means the batsman is caught out by a fielder, we log the event, update the PCB to reflect the how_out status and mark the batsman as inactive
                if (result == -1) {
                    // ── CAUGHT OUT ────────────────────
                    // Fielder caught the ball cleanly before it bounced → batsman is out
                    LogS("  OUT: [%s] OUT! Caught by fielder!\n", pcb->name);
                    strncpy(pcb->how_out, "Catch OUT!", sizeof(pcb->how_out) - 1);
                    LockChecked(&score_mutex, "score_mutex lock (caught)");
                    ++g_match_context.total_wickets; // for scorecard , atomic increment of total wickets in the match context
                    ++g_match_context.balls_in_current_over; // for over progression, caught is still a legal delivery
                    --active_batsmen_count; // for the match flow, decrement active batsmen count so umpire can send next one in
                    pcb->is_active_on_pitch = false;// mark this batsman as no longer active on the pitch
                    wicket_fell = true;
                    // Track bowler wicket , if we have a valid current bowler, increment their wicket count and ball count for stats
                    if (current_bowler_pcb) {
                        ++current_bowler_pcb->wickets_taken;
                        ++current_bowler_pcb->total_balls_bowled;
                    }
                    UnlockChecked(&score_mutex, "score_mutex unlock (caught)");

                    // Mark delivery resolved for the bowler
                    // IMPORTANT: bowler is blocked on resolved_cond — must unblock it before returning , we set delivery_resolved=true to indicate to the bowler that the delivery has been fully resolved and it can proceed to the next ball, then we broadcast on resolved_cond to wake the bowler thread in case it's waiting for this signal
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
                    // setting up the state for a run exchange, we lock the score_mutex to update the shared state that indicates a run exchange is needed and to set the number of runs for the exchange, we also reset all the flags related to the crease attempts and exchange resolution to prepare for this new exchange, then we unlock the score_mutex to allow the striker and non-striker threads to proceed with their respective crease attempts and reporting
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
                    LockChecked(&score_mutex, "score_mutex lock (striker crease)"); // locking score_mutex to update shared state about striker's crease attempt result for the umpire
                    striker_let_go = let_go;
                    striker_crease_done = true;
                    UnlockChecked(&score_mutex, "score_mutex unlock (striker crease)"); 
                    BroadcastChecked(&run_exchange_cond, "run_exchange_cond (striker done)"); // wake umpire in case it's waiting for the striker's report

                    // Wait for umpire to resolve exchange
                    // Umpire will set exchange_resolved once both batsmen have reported
                    LockChecked(&score_mutex, "score_mutex lock (striker wait resolve)"); // locking score_mutex to wait for the umpire to resolve the run exchange based on the reports from both batsmen
                    while (!exchange_resolved && !match_completed) {
                        timespec ts{};
                        clock_gettime(CLOCK_REALTIME, &ts);
                        ts.tv_sec += 2;
                        pthread_cond_timedwait(&run_exchange_cond, &score_mutex, &ts);
                    }
                    int credited = exchange_credited_runs;  // actual runs umpire decided to award
                    bool wicket  = exchange_wicket;         // true if a run-out was called
                    int  victim  = exchange_runout_id;      // player_id of run-out batsman (-1 if none)
                    UnlockChecked(&score_mutex, "score_mutex unlock (striker wait resolve)"); // after umpire has resolved the exchange, we read the final decision on how many runs to credit and whether there was a wicket

                    // Advance ball count
                    LockChecked(&score_mutex, "score_mutex lock (advance ball odd)"); // update the global score and ball count for this delivery, we add the credited runs to the global score and increment the ball count for the current over
                    g_match_context.global_score += credited;
                    ++g_match_context.balls_in_current_over;
                    UnlockChecked(&score_mutex, "score_mutex unlock (advance ball odd)"); // after updating the match context for the runs and ball count, we check if the credited runs are odd, if so we call SwapStrikeUnsafe to rotate the strike between the two batsmen

                    // Mark delivery resolved
                    LockChecked(&ball_mutex, "ball_mutex lock (resolve odd)"); // locking ball_mutex to update delivery state and signal the bowler that the delivery has been fully resolved and it can proceed to the next ball, we set delivery_resolved=true to indicate that the delivery has been resolved and the bowler can proceed, then we broadcast on resolved_cond to wake the bowler thread in case it's waiting for this signal, and we also signal umpire_cond in case the umpire is waiting for the delivery to be resolved before it can send in the next batsman or proceed with the match flow
                    delivery_resolved = true;
                    UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve odd)"); // unclock ball_mutex after updating delivery state for odd runs
                    BroadcastChecked(&resolved_cond, "resolved_cond (odd)"); // wake bowler
                    SignalChecked(&umpire_cond, "umpire_cond (odd)"); // wake umpire in case it's waiting for this delivery to be resolved before it can proceed with the match flow

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
                    LockChecked(&ball_mutex, "ball_mutex lock (resolve even)"); // locking ball_mutex to update delivery state and signal the bowler that the delivery has been fully resolved and it can proceed to the next ball, we set delivery_resolved=true to indicate that the delivery has been resolved and the bowler can proceed, then we broadcast on resolved_cond to wake the bowler thread in case it's waiting for this signal, and we also signal umpire_cond in case the umpire is waiting for the delivery to be resolved before it can send in the next batsman or proceed with the match flow
                    delivery_resolved = true;
                    UnlockChecked(&ball_mutex, "ball_mutex unlock (resolve even)"); // for even
                    BroadcastChecked(&resolved_cond, "resolved_cond (even)");
                    SignalChecked(&umpire_cond, "umpire_cond (even)");
                }
            } else if (shot <= 95) {
                // ── BATSMAN MISSES, KEEPER COLLECTS ──
                // Ball goes past the bat; keeper thread handles it (may fumble for byes)
                LogS("  MISS: [%s] plays and misses! Ball goes to keeper.\n", pcb->name);
                ++pcb->balls_faced;

                LockChecked(&ball_mutex, "ball_mutex lock (miss)"); // lock ball_mutex for  keepr interaction 
                keeper_event_pending = true;      // tells the keeper thread to wake up and resolve
                shared_hit_result = INVALID_HIT_RESULT;
                SignalChecked(&keeper_cond, "keeper_cond (miss)");

                // Wait for keeper to set shared_hit_result (0=dot, 4=bye on fumble)
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
                int bye_result = shared_hit_result; // 0 if keeper gathered cleanly, 4 if keeper fumbled and allowed a bye
                UnlockChecked(&ball_mutex, "ball_mutex unlock (miss)"); // unlock ball_mutex after keeper interaction

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
                LockChecked(&ball_mutex, "ball_mutex lock (resolve miss)"); // lock for keeper interaction to update delivery state and signal the bowler that the delivery has been resolved and it can proceed to the next ball, we set delivery_resolved=true to indicate that the delivery has been resolved and the bowler can proceed, then we broadcast on resolved_cond to wake the bowler thread in case it's waiting for this signal, and we also signal umpire_cond in case the umpire is waiting for the delivery to be resolved before it can proceed with the match flow
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
