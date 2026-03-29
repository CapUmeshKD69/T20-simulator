#include "entities.h"
#include "helpers.h"

#include <cerrno>
#include <cstdio>
#include <unistd.h>

//  UMPIRE ROUTINE
//
//  Manages:
//    - Over completion + strike rotation end of over
//    - Run exchange resolution (deadlock detection)
//    - Wicket → bring in next batsman from pavilion (SJF)
//    - Match termination
//

//  The umpire is the central coordinator of the match.
//  It runs in a tight polling loop (every 3ms) and handles
//  everything that no individual player thread can handle alone:
//    - Adjudicating run-exchange outcomes (who gets run out)
//    - Triggering over changes (signals bowler_manager)
//    - Sending the next batsman from the pavilion
//    - Declaring the match over and waking all threads to exit
//
//  WHY POLLING (not event-driven)?
//  Multiple events can fire in rapid succession like wicket + over completion
//  A polling loop is simpler and avoids complex multi-condition signaling.
//  The 3ms sleep keeps CPU usage low while staying responsive.
void* umpire_routine(void* arg) {
    (void)arg;

    while (true) {
        usleep(3000);  // poll every 3ms — short enough to react quickly, avoids busy-waiting

        // ── Check match termination ──────────────────
        LockChecked(&score_mutex, "score_mutex lock (umpire)");  // lock score_mutex to check match state for termination conditions
        bool over = (g_match_context.current_over >= MAX_OVERS ||
                     g_match_context.total_wickets >= MAX_WICKETS ||
                     match_completed);

        // Edge case: no active batsmen AND pavilion is empty → team is all out
        // This can happen if a run-out leaves only 1 batsman and no one is waiting
        if (active_batsmen_count <= 0 && pavilion_size <= 0 && !match_completed) {
            over = true;
            if (g_match_context.total_wickets < MAX_WICKETS)
                g_match_context.total_wickets = MAX_WICKETS;
        }

        if (over) { // if any termination condition met, set match_completed=true and log final score, then break loop to end umpire thread
            match_completed = true;
            int score = g_match_context.global_score;
            int wkts  = g_match_context.total_wickets;
            int ov    = g_match_context.current_over;
            int bl    = g_match_context.balls_in_current_over;
            UnlockChecked(&score_mutex, "score_mutex unlock (umpire end)"); // unlock score_mutex before logging and broadcasting to allow other threads to read final state

            Logf3("\n  [Umpire] MATCH OVER! Score: %d/%d in %d", score, wkts, ov);
            Logf(".%d overs\n", bl);

            // Wake everyone up so they can exit
            // Broadcast on ALL condition variables to prevent any thread from being stuck waiting
            BroadcastChecked(&delivery_cond, "delivery_cond (end)");
            BroadcastChecked(&resolved_cond, "resolved_cond (end)");
            BroadcastChecked(&fielders_cond, "fielders_cond (end)");
            BroadcastChecked(&keeper_cond, "keeper_cond (end)");
            BroadcastChecked(&batsman_cond, "batsman_cond (end)");
            BroadcastChecked(&run_exchange_cond, "run_exchange_cond (end)");
            BroadcastChecked(&bowler_manager_cond, "bowler_manager_cond (end)");
            break;
        }

        // ── Run exchange resolution ─────
        // When the striker hits 1 or 3 runs, both batsmen race to the other crease.
        // Each batsman thread reports its result (crease_done + let_go flags).
        // The umpire waits until BOTH have reported, then decides the outcome.
        //
        // Four possible outcomes:
        //   both safe      → full runs, strike swaps normally
        //   both fail      → benefit of doubt, runs-1 credited, no wicket
        //   striker safe   → non-striker RUN OUT (deadlock: striker's end is blocked)
        //   non-striker safe → striker RUN OUT
        if (run_exchange_needed) {
            bool s_done = striker_crease_done;
            bool n_done = non_striker_crease_done;

            // Auto-resolve if a batsman is no longer on the pitch
            // This can happen if the batsman was dismissed before the exchange completed
            if (s_done && !n_done && //if striker reported but non-striker didn't and non-striker is not active on pitch, we assume non-striker failed to make the crease
                (!active_non_striker_pcb || !active_non_striker_pcb->is_active_on_pitch)) {
                non_striker_crease_done = true;
                non_striker_let_go = true;
                n_done = true;
            }
            if (!s_done && n_done && // if non-striker reported but striker didn't and striker is not active on pitch, we assume striker failed to make the crease
                (!active_striker_pcb || !active_striker_pcb->is_active_on_pitch)) {
                striker_crease_done = true;
                striker_let_go = true;
                s_done = true;
            }

            if (s_done && n_done) { 
                // Both batsmen have reported; umpire makes the final call
                bool s_let = striker_let_go;
                bool n_let = non_striker_let_go;
                int  runs  = run_exchange_runs;

                if (!s_let && !n_let) {
                    // Both made it → full runs, swap strike
                    exchange_credited_runs = runs;
                    exchange_wicket = false;
                    SwapStrikeUnsafe();
                    Log("  UMPIRE: Both batsmen make their crease. Full runs credited. Strike swapped.\n");
                    LogS("  -> Striker is now %s\n", active_striker_pcb ? active_striker_pcb->name : "?");
                } else if (s_let && n_let) {
                    // Both failed → runs-1 credited, no wicket, no swap
                    // Benefit of doubt: neither batsman is given out when both failed
                    exchange_credited_runs = (runs > 1) ? runs - 1 : 0;
                    exchange_wicket = false;
                    Log("  UMPIRE: BOTH batsmen failed to make crease! runs-1 credited, no one out.\n");
                } else if (!s_let && n_let) {
                    // Striker safe, non-striker failed → DEADLOCK → run out non-striker
                    // Striker is occupying the crease the non-striker needs → non-striker can't make it
                    exchange_credited_runs = (runs > 1) ? runs - 1 : 0;
                    exchange_wicket = true;
                    exchange_runout_id = non_striker_id;
                    ++g_match_context.total_wickets;
                    --active_batsmen_count;
                    wicket_fell = true;
                    LogS("  DEADLOCK: [Umpire] Non-striker (%s) is RUN OUT!\n",
                         active_non_striker_pcb ? active_non_striker_pcb->name : "?");
                    Logf2("  UMPIRE: Only %d run(s) credited (was %d).\n",
                           exchange_credited_runs, runs);
                } else {
                    // Striker failed, non-striker safe → DEADLOCK → run out striker
                    exchange_credited_runs = (runs > 1) ? runs - 1 : 0;
                    exchange_wicket = true;
                    exchange_runout_id = active_striker_id;
                    ++g_match_context.total_wickets;
                    --active_batsmen_count;
                    wicket_fell = true;
                    LogS("  DEADLOCK: [Umpire] Striker (%s) is RUN OUT!\n",
                         active_striker_pcb ? active_striker_pcb->name : "?");
                    Logf2("  UMPIRE: Only %d run(s) credited (was %d).\n",
                           exchange_credited_runs, runs);
                }

                // Mark exchange as resolved and wake both batsmen so they can react to the outcome
                run_exchange_needed = false;
                exchange_resolved = true;
                UnlockChecked(&score_mutex, "score_mutex unlock (exchange)");
                BroadcastChecked(&run_exchange_cond, "run_exchange_cond (resolved)");
                continue;
            }
            // Not both done yet, wait
            // Loop back immediately (no unlock needed — already unlocked above in each branch)
            UnlockChecked(&score_mutex, "score_mutex unlock (exchange wait)");
            continue;
        }

        // ── Over completion ──────────────────────────
        // Checks if 6 balls have been bowled this over; if so, closes the over and
        // signals the bowler_manager to swap in the next bowler
        int balls = g_match_context.balls_in_current_over;
        if (balls >= BALLS_PER_OVER) { // if 6 or more balls have been bowled in the current over, we need to end the over and rotate strike
            g_match_context.balls_in_current_over = 0;
            ++g_match_context.current_over;// Increment the over count and reset the ball count for the new over
            int ov = g_match_context.current_over;

            // Swap strike at end of over (non-striker becomes striker for next over)
            SwapStrikeUnsafe();// This simulates the real cricket rule where the strike rotates at the end of each over

            // Check death overs 16th over
            // Ball 90 = over 15 completed → overs 16-20 are "death overs" (high intensity)
            int total_del = ov * BALLS_PER_OVER;
            if (total_del >= 90 && !match_intensity_high) {
                match_intensity_high = true;
                Log("  *** [Umpire] DEATH OVERS! Intensity HIGH! ***\n");
            }

            over_change_requested = true;  // tells bowler_manager to retire current bowler and spawn next
            UnlockChecked(&score_mutex, "score_mutex unlock (over)");

            Logf("  [Umpire] END OF OVER %d. Strike rotated.\n", ov); 
            LogS("  -> Striker is now %s\n", active_striker_pcb ? active_striker_pcb->name : "?");
            SignalChecked(&bowler_manager_cond, "bowler_manager_cond (over)");

            // Print mini scorecard after each over for live match feel
            LockChecked(&score_mutex, "score_mutex lock (mini score)");
            int sc = g_match_context.global_score;
            int wk = g_match_context.total_wickets;
            UnlockChecked(&score_mutex, "score_mutex unlock (mini score)");
            Logf3("  === Score: %d/%d after %d overs ===\n", sc, wk, ov);
            continue;
        }

        // ── Wicket: bring in next batsman ────────────
        // Triggered when a wicket fell and only 1 batsman remains on pitch.
        // Uses SJF (Shortest Job First) scheduling to pick the next batsman,
        // simulating how a captain sends in batsmen based on match situation.
        bool need_sub = (wicket_fell && active_batsmen_count < 2 && pavilion_size > 0 &&
                         g_match_context.total_wickets < MAX_WICKETS); // if a wicket has fallen and we have less than 2 active batsmen and there are players waiting in the pavilion and we haven't reached max wickets, then we need to bring in a new batsman
        if (need_sub) {  // if we need to bring in a new batsman from the pavilion, we will select one based on scheduling criteria (SJF or priority), then create a thread for that batsman and assign them to the striker or non-striker position as needed
            wicket_fell = false;

            // SJF / Priority scheduling for next batsman
            int sel = 0; // sel is the index of the selected batsman in the pavilion queue
            bool specialist = false;

            if (use_priority_scheduling && match_intensity_high) {
                // In death overs, prefer a death-over specialist if one is waiting
                for (int i = 0; i < pavilion_size; ++i) {
                    if (pavilion_queue[i] && pavilion_queue[i]->is_death_over_specialist) {
                        sel = i;
                        specialist = true;
                        break;
                    }
                }
            }

            if (!specialist && use_sjf_scheduling) { // If no specialist selected (or not using priority scheduling), use SJF to pick the batsman with the lowest expected_stay_duration (burst time analog)
                // SJF: pick the batsman with the lowest expected_stay_duration (burst time analog)
                // This minimises average waiting time in the pavilion queue
                int best = pavilion_queue[0] ? pavilion_queue[0]->expected_stay_duration : 999;
                for (int i = 1; i < pavilion_size; ++i) {
                    if (pavilion_queue[i] && pavilion_queue[i]->expected_stay_duration < best) {
                        best = pavilion_queue[i]->expected_stay_duration;
                        sel = i;
                    }
                }
            }

            // Remove selected batsman from the pavilion queue and compact the array
            PlayerControlBlock* next = pavilion_queue[sel]; // next is the PCB of the selected batsman who will come in to replace the dismissed batsman
            for (int i = sel + 1; i < pavilion_size; ++i) // shift all batsmen after the selected one forward in the queue to fill the gap
                pavilion_queue[i - 1] = pavilion_queue[i];
            pavilion_queue[--pavilion_size] = nullptr;
            UnlockChecked(&score_mutex, "score_mutex unlock (sub)");

            if (next) {
                psem_wait(&crease_semaphore);  // acquire crease slot before putting batsman on pitch (max 2 at a time)
                next->is_active_on_pitch    = true; // mark the selected batsman as active on the pitch so that other threads know they are now playing
                next->is_waiting_in_pavilion = false; // mark the selected batsman as no longer waiting in the pavilion
                next->dispatch_time = get_timestamp();// record the time when the batsman is dispatched to the pitch for wait time calculation
                double wait_ms = next->dispatch_time - next->arrival_time;  // total pavilion wait time in ms

                // Log wait time to CSV for scheduling analysis
                // Columns: Algorithm, ThreadID, Role, ExpectedDuration, WaitTimeMs, RosterIndex, IsMiddleOrder
                LockChecked(&file_mutex, "file_mutex lock (wait csv)"); // lock file_mutex to safely write to the wait time log CSV file
                if (wait_time_log_file.is_open()) {
                    const char* algo = use_sjf_scheduling ? "SJF" : "FCFS";
                    int ri = next->player_id + 1;           // 1-based roster index for readability
                    int mo = (ri >= 3 && ri <= 7) ? 1 : 0; // 1 if middle order (positions 3–7), else 0
                    wait_time_log_file << algo << ',' << next->player_id << ",Batsman,"
                                       << next->expected_stay_duration << ',' << wait_ms << ','
                                       << ri << ',' << mo << '\n';
                    wait_time_log_file.flush();
                }
                UnlockChecked(&file_mutex, "file_mutex unlock (wait csv)"); // unlock file_mutex after writing to allow other threads to log

                int rc = pthread_create(&next->thread_id, nullptr, batsman_routine, next);
                if (rc != 0) {
                    errno = rc; perror("pthread_create(batsman)");
                    psem_post(&crease_semaphore);  // release slot if thread creation failed — avoid leak
                } else {
                    // Assign to whichever role (striker or non-striker) is currently vacant
                    LockChecked(&score_mutex, "score_mutex lock (assign)");
                    if (!active_striker_pcb || !active_striker_pcb->is_active_on_pitch) {
                        active_striker_pcb = next;
                        active_striker_id  = next->player_id;
                    } else {
                        active_non_striker_pcb = next;
                        non_striker_id = next->player_id;
                    }
                    ++active_batsmen_count;
                    UnlockChecked(&score_mutex, "score_mutex unlock (assign)");

                    LogSI("  NEW IN: [Umpire] %s walks in (SJF, expected_stay=%d).\n",
                           next->name, next->expected_stay_duration);
                    // Track batting order for the final scorecard printout in main()
                    if (batting_order_count < TEAM_SIZE) {
                        batting_order[batting_order_count++] = next->player_id;
                    }
                }
            }
            continue;
        }

        UnlockChecked(&score_mutex, "score_mutex unlock (umpire loop)");
    }

    return nullptr;
}
