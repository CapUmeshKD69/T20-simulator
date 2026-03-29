#include "entities.h"
#include "helpers.h"

#include <cerrno>
#include <cstdio>
#include <unistd.h>

// ═══════════════════════════════════════════════════
//  UMPIRE ROUTINE
//
//  Manages:
//    - Over completion + strike rotation end of over
//    - Run exchange resolution (deadlock detection)
//    - Wicket → bring in next batsman from pavilion (SJF)
//    - Match termination
// ═══════════════════════════════════════════════════
void* umpire_routine(void* arg) {
    (void)arg;

    while (true) {
        usleep(3000);

        // ── Check match termination ──────────────────
        LockChecked(&score_mutex, "score_mutex lock (umpire)");
        bool over = (g_match_context.current_over >= MAX_OVERS ||
                     g_match_context.total_wickets >= MAX_WICKETS ||
                     match_completed);

        if (active_batsmen_count <= 0 && pavilion_size <= 0 && !match_completed) {
            over = true;
            if (g_match_context.total_wickets < MAX_WICKETS)
                g_match_context.total_wickets = MAX_WICKETS;
        }

        if (over) {
            match_completed = true;
            int score = g_match_context.global_score;
            int wkts  = g_match_context.total_wickets;
            int ov    = g_match_context.current_over;
            int bl    = g_match_context.balls_in_current_over;
            UnlockChecked(&score_mutex, "score_mutex unlock (umpire end)");

            Logf3("\n  === [Umpire] MATCH OVER! Score: %d/%d in %d", score, wkts, ov);
            Logf(".%d overs\n", bl);

            // Wake everyone up so they can exit
            BroadcastChecked(&delivery_cond, "delivery_cond (end)");
            BroadcastChecked(&resolved_cond, "resolved_cond (end)");
            BroadcastChecked(&fielders_cond, "fielders_cond (end)");
            BroadcastChecked(&keeper_cond, "keeper_cond (end)");
            BroadcastChecked(&batsman_cond, "batsman_cond (end)");
            BroadcastChecked(&run_exchange_cond, "run_exchange_cond (end)");
            BroadcastChecked(&bowler_manager_cond, "bowler_manager_cond (end)");
            break;
        }

        // ── Run exchange resolution ──────────────────
        if (run_exchange_needed) {
            bool s_done = striker_crease_done;
            bool n_done = non_striker_crease_done;

            // Auto-resolve if a batsman is no longer on the pitch
            if (s_done && !n_done &&
                (!active_non_striker_pcb || !active_non_striker_pcb->is_active_on_pitch)) {
                non_striker_crease_done = true;
                non_striker_let_go = true;
                n_done = true;
            }
            if (!s_done && n_done &&
                (!active_striker_pcb || !active_striker_pcb->is_active_on_pitch)) {
                striker_crease_done = true;
                striker_let_go = true;
                s_done = true;
            }

            if (s_done && n_done) {
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
                    exchange_credited_runs = (runs > 1) ? runs - 1 : 0;
                    exchange_wicket = false;
                    Log("  UMPIRE: BOTH batsmen failed to make crease! runs-1 credited, no one out.\n");
                } else if (!s_let && n_let) {
                    // Striker safe, non-striker failed → DEADLOCK → run out non-striker
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

                run_exchange_needed = false;
                exchange_resolved = true;
                UnlockChecked(&score_mutex, "score_mutex unlock (exchange)");
                BroadcastChecked(&run_exchange_cond, "run_exchange_cond (resolved)");
                continue;
            }
            // Not both done yet, wait
            UnlockChecked(&score_mutex, "score_mutex unlock (exchange wait)");
            continue;
        }

        // ── Over completion ──────────────────────────
        int balls = g_match_context.balls_in_current_over;
        if (balls >= BALLS_PER_OVER) {
            g_match_context.balls_in_current_over = 0;
            ++g_match_context.current_over;
            int ov = g_match_context.current_over;

            // Swap strike at end of over
            SwapStrikeUnsafe();

            // Check death overs 16th over
            int total_del = ov * BALLS_PER_OVER;
            if (total_del >= 90 && !match_intensity_high) {
                match_intensity_high = true;
                Log("  *** [Umpire] DEATH OVERS! Intensity HIGH! ***\n");
            }

            over_change_requested = true;
            UnlockChecked(&score_mutex, "score_mutex unlock (over)");

            Logf("  [Umpire] END OF OVER %d. Strike rotated.\n", ov);
            LogS("  -> Striker is now %s\n", active_striker_pcb ? active_striker_pcb->name : "?");
            SignalChecked(&bowler_manager_cond, "bowler_manager_cond (over)");

            // Print mini scorecard
            LockChecked(&score_mutex, "score_mutex lock (mini score)");
            int sc = g_match_context.global_score;
            int wk = g_match_context.total_wickets;
            UnlockChecked(&score_mutex, "score_mutex unlock (mini score)");
            Logf3("  === Score: %d/%d after %d overs ===\n", sc, wk, ov);
            continue;
        }

        // ── Wicket: bring in next batsman ────────────
        bool need_sub = (wicket_fell && active_batsmen_count < 2 && pavilion_size > 0 &&
                         g_match_context.total_wickets < MAX_WICKETS);
        if (need_sub) {
            wicket_fell = false;

            // SJF / Priority scheduling for next batsman
            int sel = 0;
            bool specialist = false;

            if (use_priority_scheduling && match_intensity_high) {
                for (int i = 0; i < pavilion_size; ++i) {
                    if (pavilion_queue[i] && pavilion_queue[i]->is_death_over_specialist) {
                        sel = i;
                        specialist = true;
                        break;
                    }
                }
            }

            if (!specialist && use_sjf_scheduling) {
                int best = pavilion_queue[0] ? pavilion_queue[0]->expected_stay_duration : 999;
                for (int i = 1; i < pavilion_size; ++i) {
                    if (pavilion_queue[i] && pavilion_queue[i]->expected_stay_duration < best) {
                        best = pavilion_queue[i]->expected_stay_duration;
                        sel = i;
                    }
                }
            }

            PlayerControlBlock* next = pavilion_queue[sel];
            for (int i = sel + 1; i < pavilion_size; ++i)
                pavilion_queue[i - 1] = pavilion_queue[i];
            pavilion_queue[--pavilion_size] = nullptr;
            UnlockChecked(&score_mutex, "score_mutex unlock (sub)");

            if (next) {
                psem_wait(&crease_semaphore);
                next->is_active_on_pitch = true;
                next->is_waiting_in_pavilion = false;
                next->dispatch_time = get_timestamp();
                double wait_ms = next->dispatch_time - next->arrival_time;

                // Log wait time to CSV
                LockChecked(&file_mutex, "file_mutex lock (wait csv)");
                if (wait_time_log_file.is_open()) {
                    const char* algo = use_sjf_scheduling ? "SJF" : "FCFS";
                    int ri = next->player_id + 1;
                    int mo = (ri >= 3 && ri <= 7) ? 1 : 0;
                    wait_time_log_file << algo << ',' << next->player_id << ",Batsman,"
                                       << next->expected_stay_duration << ',' << wait_ms << ','
                                       << ri << ',' << mo << '\n';
                    wait_time_log_file.flush();
                }
                UnlockChecked(&file_mutex, "file_mutex unlock (wait csv)");

                int rc = pthread_create(&next->thread_id, nullptr, batsman_routine, next);
                if (rc != 0) {
                    errno = rc; perror("pthread_create(batsman)");
                    psem_post(&crease_semaphore);
                } else {
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
                    // Track batting order
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
