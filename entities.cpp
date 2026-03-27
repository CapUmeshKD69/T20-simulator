#include "entities.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <random>
#include <unistd.h>

// ═══════════════════════════════════════════════════
//  Helper wrappers
// ═══════════════════════════════════════════════════
namespace {

int LockChecked(pthread_mutex_t* m, const char* name) {
    int rc = pthread_mutex_lock(m);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int UnlockChecked(pthread_mutex_t* m, const char* name) {
    int rc = pthread_mutex_unlock(m);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int SignalChecked(pthread_cond_t* c, const char* name) {
    int rc = pthread_cond_signal(c);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}
int BroadcastChecked(pthread_cond_t* c, const char* name) {
    int rc = pthread_cond_broadcast(c);
    if (rc != 0) { errno = rc; perror(name); }
    return rc;
}

// Thread-safe formatted log to stdout.
// Uses variadic approach via a lambda to avoid the old Logf(fmt, a, b) hack.

void Log(const char* msg) {
    LockChecked(&log_mutex, "log_mutex lock");
    std::fputs(msg, stdout);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf(const char* fmt, int a) {
    LockChecked(&log_mutex, "log_mutex lock");
    std::printf(fmt, a);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf2(const char* fmt, int a, int b) {
    LockChecked(&log_mutex, "log_mutex lock");
    std::printf(fmt, a, b);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void Logf3(const char* fmt, int a, int b, int c) {
    LockChecked(&log_mutex, "log_mutex lock");
     std::printf("------------------------------------------------------\n");
    std::printf(fmt, a, b, c);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

// Format log with string + int args (for player names)
void LogS(const char* fmt, const char* s) {
    LockChecked(&log_mutex, "log_mutex lock");
    std::printf(fmt, s);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogSI(const char* fmt, const char* s, int a) {
    LockChecked(&log_mutex, "log_mutex lock");
    std::printf(fmt, s, a);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogSS(const char* fmt, const char* s1, const char* s2) {
    LockChecked(&log_mutex, "log_mutex lock");
    std::printf(fmt, s1, s2);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}
void LogS3(const char* fmt, const char* s, int a, int b) {
    LockChecked(&log_mutex, "log_mutex lock");
    std::printf("------------------------------------------------------\n");
    std::printf(fmt, s, a, b);
    std::fflush(stdout);
    UnlockChecked(&log_mutex, "log_mutex unlock");
}

void LogGanttEvent(pthread_t tid, const char* role, const char* action, const char* resource) {
    LockChecked(&file_mutex, "file_mutex lock");
    if (gantt_log_file.is_open()) {
        size_t tv = std::hash<pthread_t>{}(tid);
        gantt_log_file << get_timestamp() << ',' << tv << ','
                       << role << ',' << action << ',' << resource << '\n';
        gantt_log_file.flush();
    }
    UnlockChecked(&file_mutex, "file_mutex unlock");
}

bool MatchOver() {
    LockChecked(&score_mutex, "score_mutex lock");
    bool over = (g_match_context.current_over >= MAX_OVERS ||
                 g_match_context.total_wickets >= MAX_WICKETS ||
                 match_completed);
    UnlockChecked(&score_mutex, "score_mutex unlock");
    return over;
}

void SwapStrikeUnsafe() {
    int tmp_id = active_striker_id;
    active_striker_id = non_striker_id;
    non_striker_id = tmp_id;
    PlayerControlBlock* tmp = active_striker_pcb;
    active_striker_pcb = active_non_striker_pcb;
    active_non_striker_pcb = tmp;
}

}  // namespace

double get_timestamp() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(now - simulation_start_time).count();
}

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

        // ── Bowl a delivery ──────────────────────────
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

// ═══════════════════════════════════════════════════
//  FIELDER / WICKETKEEPER ROUTINE
//
//  Keeper: waits on keeper_cond, gathers missed ball
//  Fielder: waits on fielders_cond, catches/fields the ball
// ═══════════════════════════════════════════════════
void* fielder_routine(void* arg) {
    auto* pcb = static_cast<PlayerControlBlock*>(arg);
    if (!pcb) return nullptr;

    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> field_roll(1, 100);
    std::uniform_int_distribution<int> keeper_fumble(1, 100);

    while (!MatchOver()) {
        LockChecked(&ball_mutex, "ball_mutex lock (fielder)");

        if (pcb->role == PlayerRole::WICKETKEEPER) {
            // ── Keeper path ──────────────────────────
            while (!keeper_event_pending && !match_completed) {
                pthread_cond_wait(&keeper_cond, &ball_mutex);
            }
            if (match_completed) {
                UnlockChecked(&ball_mutex, "ball_mutex unlock (keeper)");
                break;
            }
            keeper_event_pending = false;

            // Keeper has 15% chance of fumbling → byes
            int fumble = keeper_fumble(rng);
            if (fumble <= 15 && shared_hit_result == INVALID_HIT_RESULT) {
                // Keeper missed too! 1 bye
                shared_hit_result = 1;
                LogS("  [%s] FUMBLES! Byes scored!\n", pcb->name);
            } else {
                if (shared_hit_result == INVALID_HIT_RESULT) {
                    shared_hit_result = 0;  // dot ball, keeper gathers cleanly
                }
                LogS("  [%s] gathers the ball cleanly.\n", pcb->name);
            }

            UnlockChecked(&ball_mutex, "ball_mutex unlock (keeper)");
            SignalChecked(&batsman_cond, "batsman_cond signal (keeper)");
            continue;
        }

        // ── Fielder path ────────────────────────
        while (!ball_in_air && !match_completed) {
            pthread_cond_wait(&fielders_cond, &ball_mutex);
        }
        if (match_completed) {
            UnlockChecked(&ball_mutex, "ball_mutex unlock (fielder)");
            break;
        }

        int seq = current_ball_sequence;
        if (handled_ball_sequence == seq || !ball_in_air) {
            UnlockChecked(&ball_mutex, "ball_mutex unlock (fielder)");
            continue;
        }

        // This fielder claims the ball
        handled_ball_sequence = seq;
        ball_in_air   = false;
        is_ball_in_air = false;

        int roll = field_roll(rng);
        int result;
        if (roll <= 30)       result = 1;   // single  (31%)
        else if (roll <= 58)  result = 2;   // double  (19%)
        else if (roll <= 62)  result = 3;   // triple  (10%)
        else if (roll <= 82)  result = 4;   // boundary(23%)
        else if (roll <= 93)  result = 6;   // six     (12%)
        else                  result = -1;  // CAUGHT! ( 7%)

        shared_hit_result = result;
        UnlockChecked(&ball_mutex, "ball_mutex unlock (fielder)");

        if (result == -1) {
            LogS("  [%s] takes the CATCH! OUT!\n", pcb->name);
        } else {
            LogSI("  [%s] fields the ball -> %d run(s)\n", pcb->name, result);
        }
        LogGanttEvent(pthread_self(), "Fielder", result == -1 ? "Catch" : "Fielded", "Ball");

        SignalChecked(&batsman_cond, "batsman_cond signal (fielder)");
    }
    return nullptr;
}

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

    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> shot_dist(1, 100);
    std::uniform_int_distribution<int> crease_dist(1, 100);

    while (!MatchOver() && pcb->is_active_on_pitch) {
        // ── Determine role ──────────────────────
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
            // 8% chance of letting go (not making it)
            int roll = crease_dist(rng);
            bool let_go = (roll <=0.3);

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
                std::strncpy(pcb->how_out, "run out", sizeof(pcb->how_out) - 1);
                pcb->is_active_on_pitch = false;
                sem_post(&crease_semaphore);
                return nullptr;
            }
            continue;
        }

        // ── NON-STRIKER idle ────────────────────
        if (!is_striker) {
            usleep(5000);
            continue;
        }

        // ══════════════════════════════════════════
        //  STRIKER: wait for bowler to deliver
        // ══════════════════════════════════════════
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
            // ── Play the shot ────────────────────
            int shot = shot_dist(rng);

            // Probabilities:
            // 1-63  : clean hit (fielders resolve)  (70%)
            // 64-88 : batsman misses, keeper collects (25%)
            // 89-95 : clean bowled (OUT)              ( 7%)
            // 96-100: both miss -> byes               ( 5%)
            if (shot <= 85) {
                // ── CLEAN HIT ────────────────────
                LogS("  >> [%s] smashes the ball into the field!\n", pcb->name);
                ++pcb->balls_faced;

                LockChecked(&ball_mutex, "ball_mutex lock (hit)");
                shared_hit_result = INVALID_HIT_RESULT;
                ++current_ball_sequence;
                ball_in_air   = true;
                is_ball_in_air = true;
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
                    // ── CAUGHT OUT ────────────────
                    LogS("  OUT: [%s] OUT! Caught by fielder!\n", pcb->name);
                    std::strncpy(pcb->how_out, "c fielder", sizeof(pcb->how_out) - 1);
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

                    sem_post(&crease_semaphore);
                    return nullptr;
                }

                // ── RUNS SCORED ──────────────────
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
                    bool let_go = (roll <= 0.3);

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
                        std::strncpy(pcb->how_out, "run out", sizeof(pcb->how_out) - 1);
                        pcb->is_active_on_pitch = false;
                        sem_post(&crease_semaphore);
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
            } else if (shot <= 92) {
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
                // ── CLEAN BOWLED (7%) ─────────────
                LogS("  WICKET: [%s] CLEAN BOWLED! OUT!\n", pcb->name);
                ++pcb->balls_faced;
                std::strncpy(pcb->how_out, "bowled", sizeof(pcb->how_out) - 1);

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

                sem_post(&crease_semaphore);
                return nullptr;
            } else {
                // ── BOTH MISS → BYES ─────────────
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

    sem_post(&crease_semaphore);
    return nullptr;
}

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

        // ── Check match termination ──────────────
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

        // ── Run exchange resolution ──────────────
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

        // ── Over completion ──────────────────────
        int balls = g_match_context.balls_in_current_over;
        if (balls >= BALLS_PER_OVER) {
            g_match_context.balls_in_current_over = 0;
            ++g_match_context.current_over;
            int ov = g_match_context.current_over;

            // Swap strike at end of over
            SwapStrikeUnsafe();

            // Check death overs
            int total_del = ov * BALLS_PER_OVER;
            if (total_del >= 114 && !match_intensity_high) {
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

        // ── Wicket: bring in next batsman ────────
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
                sem_wait(&crease_semaphore);
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
                    sem_post(&crease_semaphore);
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
