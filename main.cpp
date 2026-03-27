#include "entities.h"
#include "globals.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace {

bool InitMutex(pthread_mutex_t* mutex, const char* name) {
    int rc = pthread_mutex_init(mutex, nullptr);
    if (rc != 0) { errno = rc; perror(name); return false; }
    return true;
}

bool InitCondVar(pthread_cond_t* cv, const char* name) {
    int rc = pthread_cond_init(cv, nullptr);
    if (rc != 0) { errno = rc; perror(name); return false; }
    return true;
}

}  // namespace

int main() {
    PlayerControlBlock* fielding_team[TEAM_SIZE] = {nullptr};
    PlayerControlBlock* batting_team[TEAM_SIZE]  = {nullptr};
    pthread_t umpire_thread          = static_cast<pthread_t>(0);
    pthread_t bowler_manager_thread  = static_cast<pthread_t>(0);

    // ── Track init status for cleanup ────────────
    bool score_mutex_ready = false, end1_mutex_ready = false, end2_mutex_ready = false;
    bool ball_mutex_ready = false, log_mutex_ready = false, file_mutex_ready = false;
    bool delivery_cond_ready = false, resolved_cond_ready = false;
    bool fielders_cond_ready = false, keeper_cond_ready = false;
    bool umpire_cond_ready = false, batsman_cond_ready = false;
    bool run_exchange_cond_ready = false, bowler_manager_cond_ready = false;
    bool crease_sem_ready = false;

    // ── Init mutexes ─────────────────────────────
    if (!InitMutex(&score_mutex, "score_mutex"))   return EXIT_FAILURE;
    score_mutex_ready = true;
    if (!InitMutex(&end1_mutex,  "end1_mutex"))    goto cleanup;
    end1_mutex_ready = true;
    if (!InitMutex(&end2_mutex,  "end2_mutex"))    goto cleanup;
    end2_mutex_ready = true;
    if (!InitMutex(&ball_mutex,  "ball_mutex"))    goto cleanup;
    ball_mutex_ready = true;
    if (!InitMutex(&log_mutex,   "log_mutex"))     goto cleanup;
    log_mutex_ready = true;
    if (!InitMutex(&file_mutex,  "file_mutex"))    goto cleanup;
    file_mutex_ready = true;

    // ── Init condition variables ─────────────────
    if (!InitCondVar(&delivery_cond,       "delivery_cond"))       goto cleanup;
    delivery_cond_ready = true;
    if (!InitCondVar(&resolved_cond,       "resolved_cond"))       goto cleanup;
    resolved_cond_ready = true;
    if (!InitCondVar(&fielders_cond,       "fielders_cond"))       goto cleanup;
    fielders_cond_ready = true;
    if (!InitCondVar(&keeper_cond,         "keeper_cond"))         goto cleanup;
    keeper_cond_ready = true;
    if (!InitCondVar(&umpire_cond,         "umpire_cond"))         goto cleanup;
    umpire_cond_ready = true;
    if (!InitCondVar(&batsman_cond,        "batsman_cond"))        goto cleanup;
    batsman_cond_ready = true;
    if (!InitCondVar(&run_exchange_cond,   "run_exchange_cond"))   goto cleanup;
    run_exchange_cond_ready = true;
    if (!InitCondVar(&bowler_manager_cond, "bowler_manager_cond")) goto cleanup;
    bowler_manager_cond_ready = true;

    if (sem_init(&crease_semaphore, 0, 2) != 0) {
        perror("sem_init(crease_semaphore)");
        goto cleanup;
    }
    crease_sem_ready = true;

    // ── Configuration ────────────────────────────
    use_sjf_scheduling      = true;
    use_priority_scheduling = true;

    simulation_start_time = std::chrono::high_resolution_clock::now();

    // ── Open CSV files ───────────────────────────
    gantt_log_file.open("gantt_chart.csv", std::ios::out | std::ios::trunc);
    if (!gantt_log_file.is_open()) { perror("open(gantt_chart.csv)"); goto cleanup; }
    gantt_log_file << "Timestamp,ThreadID,Role,Action,Resource\n";
    gantt_log_file.flush();

    {
        const char* wf = use_sjf_scheduling ? "wait_times_sjf.csv" : "wait_times_fcfs.csv";
        wait_time_log_file.open(wf, std::ios::out | std::ios::trunc);
        if (!wait_time_log_file.is_open()) { perror("open(wait_times)"); goto cleanup; }
        wait_time_log_file << "Algorithm,ThreadID,Role,ExpectedDuration,WaitTimeMs,RosterIndex,IsMiddleOrder\n";
        wait_time_log_file.flush();
    }

    // ── Reset all shared state ───────────────────
    g_match_context = {0, 0, 0, 0};
    match_completed       = false;
    match_intensity_high  = false;
    active_striker_id     = -1;
    non_striker_id        = -1;
    active_batsmen_count  = 0;
    delivery_bowled       = false;
    delivery_resolved     = true;   // so bowler can start first ball
    ball_in_air           = false;
    is_ball_in_air        = false;
    shared_hit_result     = INVALID_HIT_RESULT;
    current_ball_sequence = 0;
    handled_ball_sequence = -1;
    keeper_event_pending  = false;
    run_exchange_needed   = false;
    exchange_resolved     = false;
    exchange_wicket       = false;
    over_change_requested = false;
    wicket_fell           = false;
    pavilion_size         = 0;
    bowler_pool_size      = 0;
    current_bowler_index  = 0;
    current_bowler_pcb    = nullptr;
    active_striker_pcb    = nullptr;
    active_non_striker_pcb = nullptr;

    // ── Create player control blocks ─────────────
    for (int i = 0; i < TEAM_SIZE; ++i) {
        fielding_team[i] = new PlayerControlBlock{};
        batting_team[i]  = new PlayerControlBlock{};

        fielding_team[i]->player_id             = i;
        fielding_team[i]->thread_id             = static_cast<pthread_t>(0);
        fielding_team[i]->is_active_on_pitch    = false;
        fielding_team[i]->is_waiting_in_pavilion = false;
        fielding_team[i]->expected_stay_duration = 0;
        fielding_team[i]->deliveries_bowled      = 0;
        fielding_team[i]->last_ball_faced        = -1;
        fielding_team[i]->arrival_time           = 0.0;
        fielding_team[i]->dispatch_time          = 0.0;
        fielding_team[i]->priority_score         = 0;
        fielding_team[i]->is_death_over_specialist = false;
        fielding_team[i]->runs_scored            = 0;
        fielding_team[i]->balls_faced            = 0;
        fielding_team[i]->fours                  = 0;
        fielding_team[i]->sixes                  = 0;
        fielding_team[i]->total_balls_bowled     = 0;
        fielding_team[i]->runs_conceded          = 0;
        fielding_team[i]->wickets_taken          = 0;
        std::memset(fielding_team[i]->name, 0, sizeof(fielding_team[i]->name));
        std::memset(fielding_team[i]->how_out, 0, sizeof(fielding_team[i]->how_out));

        if (i == 0) {
            fielding_team[i]->role = PlayerRole::WICKETKEEPER;
            fielding_team[i]->is_active_on_pitch = true;
        } else if (i <= BOWLER_POOL_SIZE) {
            fielding_team[i]->role = PlayerRole::BOWLER;
            bowler_pool[bowler_pool_size++] = fielding_team[i];
            all_bowlers[bowler_pool_size - 1] = fielding_team[i];
        } else {
            fielding_team[i]->role = PlayerRole::FIELDER;
            fielding_team[i]->is_active_on_pitch = true;
        }

        batting_team[i]->player_id             = i;
        batting_team[i]->thread_id             = static_cast<pthread_t>(0);
        batting_team[i]->role                  = PlayerRole::BATSMAN;
        batting_team[i]->is_active_on_pitch    = false;
        batting_team[i]->is_waiting_in_pavilion = true;
        batting_team[i]->expected_stay_duration = TEAM_SIZE - i;
        batting_team[i]->deliveries_bowled      = 0;
        batting_team[i]->last_ball_faced        = -1;
        batting_team[i]->arrival_time           = 0.0;
        batting_team[i]->dispatch_time          = 0.0;
        batting_team[i]->priority_score         = 1;
        batting_team[i]->is_death_over_specialist = false;
        batting_team[i]->runs_scored            = 0;
        batting_team[i]->balls_faced            = 0;
        batting_team[i]->fours                  = 0;
        batting_team[i]->sixes                  = 0;
        batting_team[i]->total_balls_bowled     = 0;
        batting_team[i]->runs_conceded          = 0;
        batting_team[i]->wickets_taken          = 0;
        std::memset(batting_team[i]->name, 0, sizeof(batting_team[i]->name));
        std::strncpy(batting_team[i]->how_out, "not out", sizeof(batting_team[i]->how_out) - 1);

        all_batsmen[i] = batting_team[i];
    }

    // Death-over specialist
    batting_team[7]->is_death_over_specialist = true;
    batting_team[7]->priority_score = 100;

    // ── Assign player names ──────────────────────
    {
        const char* bat_names[] = {
            "Rohit Sharma", "Shubman Gill", "Virat Kohli",
            "Suryakumar Yadav", "KL Rahul", "Hardik Pandya",
            "Ravindra Jadeja", "MS Dhoni", "R Ashwin",
            "Kuldeep Yadav", "Jasprit Bumrah"
        };
        const char* field_names[] = {
            "Jos Buttler",      // keeper
            "Jofra Archer",     // bowler 1
            "Mark Wood",        // bowler 2
            "Adil Rashid",      // bowler 3
            "Chris Woakes",     // bowler 4
            "Moeen Ali",        // bowler 5
            "Ben Stokes",       // fielder 6
            "Joe Root",         // fielder 7
            "Jonny Bairstow",   // fielder 8
            "Harry Brook",      // fielder 9
            "Liam Livingstone"  // fielder 10
        };
        for (int i = 0; i < TEAM_SIZE; ++i) {
            std::strncpy(batting_team[i]->name, bat_names[i], sizeof(batting_team[i]->name) - 1);
            std::strncpy(fielding_team[i]->name, field_names[i], sizeof(fielding_team[i]->name) - 1);
        }
    }

    // ── Setup first bowler ───────────────────────
    if (bowler_pool_size > 0) {
        current_bowler_index = 0;
        current_bowler_pcb = bowler_pool[0];
        current_bowler_pcb->is_active_on_pitch = true;
    }

    // ── Print match banner ───────────────────────
    std::puts("\n+----------------------------------------------+");
    std::puts("|         T20 MATCH SIMULATOR                  |");
    std::puts("|  Threads: Bowler, Batsmen, Fielders, Umpire  |");
    std::puts("|  Sync: pthreads mutexes + condvars + sem     |");
    std::puts("+----------------------------------------------+\n");
    std::fflush(stdout);

    // ── Start umpire ─────────────────────────────
    {
        int rc = pthread_create(&umpire_thread, nullptr, umpire_routine, nullptr);
        if (rc != 0) { errno = rc; perror("pthread_create(umpire)"); goto cleanup; }
    }

    // ── Start bowler manager ─────────────────────
    {
        int rc = pthread_create(&bowler_manager_thread, nullptr, bowler_manager_routine, nullptr);
        if (rc != 0) { errno = rc; perror("pthread_create(bowler_mgr)"); goto cleanup; }
    }

    // ── Start fielding team threads ──────────────
    for (int i = 0; i < TEAM_SIZE; ++i) {
        void* (*entry)(void*) = nullptr;

        if (fielding_team[i]->role == PlayerRole::BOWLER) {
            if (fielding_team[i] != current_bowler_pcb) continue;
            entry = bowler_routine;
        } else {
            entry = fielder_routine;
        }

        fielding_team[i]->is_active_on_pitch = true;
        int rc = pthread_create(&fielding_team[i]->thread_id, nullptr, entry, fielding_team[i]);
        if (rc != 0) {
            errno = rc; perror("pthread_create(fielder)");
            match_completed = true;
            goto join_and_cleanup;
        }
    }

    // ── Start opening batsmen ────────────────────
    for (int i = 0; i < 2; ++i) {
        sem_wait(&crease_semaphore);

        batting_team[i]->is_active_on_pitch     = true;
        batting_team[i]->is_waiting_in_pavilion  = false;

        int rc = pthread_create(&batting_team[i]->thread_id, nullptr, batsman_routine, batting_team[i]);
        if (rc != 0) {
            errno = rc; perror("pthread_create(opener)");
            sem_post(&crease_semaphore);
            match_completed = true;
            goto join_and_cleanup;
        }
        ++active_batsmen_count;
    }

    active_striker_pcb     = batting_team[0];
    active_non_striker_pcb = batting_team[1];
    active_striker_id      = batting_team[0]->player_id;
    non_striker_id         = batting_team[1]->player_id;

    std::printf("  -> Openers: %s (striker) & %s (non-striker)\n\n",
                batting_team[0]->name, batting_team[1]->name);
    std::fflush(stdout);

    // Track batting order
    batting_order[0] = 0;
    batting_order[1] = 1;
    batting_order_count = 2;

    // ── Remaining batsmen go to pavilion ─────────
    for (int i = 2; i < TEAM_SIZE; ++i) {
        batting_team[i]->arrival_time = get_timestamp();
        pavilion_queue[pavilion_size++] = batting_team[i];
    }

join_and_cleanup:
    // ── Wait for umpire (drives match end) ───────
    if (umpire_thread != static_cast<pthread_t>(0))
        pthread_join(umpire_thread, nullptr);

    // Ensure everyone wakes up
    match_completed = true;
    pthread_cond_broadcast(&delivery_cond);
    pthread_cond_broadcast(&resolved_cond);
    pthread_cond_broadcast(&fielders_cond);
    pthread_cond_broadcast(&keeper_cond);
    pthread_cond_broadcast(&batsman_cond);
    pthread_cond_broadcast(&run_exchange_cond);
    pthread_cond_broadcast(&bowler_manager_cond);

    if (bowler_manager_thread != static_cast<pthread_t>(0))
        pthread_join(bowler_manager_thread, nullptr);

    for (int i = 0; i < TEAM_SIZE; ++i) {
        if (fielding_team[i] && fielding_team[i]->thread_id != static_cast<pthread_t>(0)) {
            pthread_cancel(fielding_team[i]->thread_id);
            pthread_join(fielding_team[i]->thread_id, nullptr);
        }
    }

    for (int i = 0; i < TEAM_SIZE; ++i) {
        if (batting_team[i] && batting_team[i]->thread_id != static_cast<pthread_t>(0)) {
            pthread_cancel(batting_team[i]->thread_id);
            pthread_join(batting_team[i]->thread_id, nullptr);
        }
    }

    // ── Final scorecard ──────────────────────────
    {
        int sc = g_match_context.global_score;
        int wk = g_match_context.total_wickets;
        int ov = g_match_context.current_over;
        int bl = g_match_context.balls_in_current_over;

        std::puts("\n+============================================================+");
        std::puts("|                   FINAL SCORECARD                          |");
        std::printf("|  INDIA innings: %d/%d in %d.%d overs", sc, wk, ov, bl);
        std::puts("                            |");
        std::puts("+============================================================+");
        std::puts("|                                                            |");
        std::puts("|  BATTING                                                   |");
        std::printf("|  %-20s  %4s  %4s  %3s  %3s  %-20s  |\n",
                    "Batsman", "Runs", "Balls", "4s", "6s", "How Out");
        std::puts("|  --------------------------------------------------------  |");

        for (int i = 0; i < batting_order_count; ++i) {
            int idx = batting_order[i];
            PlayerControlBlock* b = all_batsmen[idx];
            if (!b) continue;
            std::printf("|  %-20s  %4d  %4d  %3d  %3d  %-20s  |\n",
                        b->name, b->runs_scored, b->balls_faced,
                        b->fours, b->sixes, b->how_out);
        }

        std::puts("|                                                            |");
        std::puts("|  BOWLING                                                   |");
        std::printf("|  %-20s  %4s  %4s  %3s  %7s  |\n",
                    "Bowler", "Overs", "Runs", "Wkts", "Econ");
        std::puts("|  --------------------------------------------------------  |");

        for (int i = 0; i < BOWLER_POOL_SIZE; ++i) {
            PlayerControlBlock* bw = all_bowlers[i];
            if (!bw) continue;
            int tb = bw->total_balls_bowled;
            int full_ov = tb / 6;
            int rem_b = tb % 6;
            double econ = (tb > 0) ? (bw->runs_conceded * 6.0 / tb) : 0.0;
            char ov_str[8];
            std::snprintf(ov_str, sizeof(ov_str), "%d.%d", full_ov, rem_b);
            std::printf("|  %-20s  %5s  %4d  %3d  %7.2f  |\n",
                        bw->name, ov_str, bw->runs_conceded,
                        bw->wickets_taken, econ);
        }

        std::puts("|                                                            |");
        std::printf("|  Total: %d/%d in %d.%d overs", sc, wk, ov, bl);
        std::puts("                                   |");
        std::puts("+============================================================+");
    }
    std::fflush(stdout);

    // ── Free PCBs ────────────────────────────────
    {
        std::unordered_set<PlayerControlBlock*> freed;
        auto del = [&](PlayerControlBlock*& p) {
            if (p && freed.insert(p).second) delete p;
            p = nullptr;
        };
        for (int i = 0; i < TEAM_SIZE; ++i) del(fielding_team[i]);
        for (int i = 0; i < TEAM_SIZE; ++i) del(batting_team[i]);
        del(active_striker_pcb);
        del(active_non_striker_pcb);
        for (int i = 0; i < TEAM_SIZE; ++i) del(pavilion_queue[i]);
        for (int i = 0; i < BOWLER_POOL_SIZE; ++i) del(bowler_pool[i]);
    }

cleanup:
    if (gantt_log_file.is_open())      { gantt_log_file.flush();      gantt_log_file.close(); }
    if (wait_time_log_file.is_open())  { wait_time_log_file.flush();  wait_time_log_file.close(); }
    if (crease_sem_ready)              sem_destroy(&crease_semaphore);
    if (bowler_manager_cond_ready)     pthread_cond_destroy(&bowler_manager_cond);
    if (run_exchange_cond_ready)       pthread_cond_destroy(&run_exchange_cond);
    if (batsman_cond_ready)            pthread_cond_destroy(&batsman_cond);
    if (umpire_cond_ready)             pthread_cond_destroy(&umpire_cond);
    if (keeper_cond_ready)             pthread_cond_destroy(&keeper_cond);
    if (fielders_cond_ready)           pthread_cond_destroy(&fielders_cond);
    if (resolved_cond_ready)           pthread_cond_destroy(&resolved_cond);
    if (delivery_cond_ready)           pthread_cond_destroy(&delivery_cond);
    if (file_mutex_ready)              pthread_mutex_destroy(&file_mutex);
    if (log_mutex_ready)               pthread_mutex_destroy(&log_mutex);
    if (ball_mutex_ready)              pthread_mutex_destroy(&ball_mutex);
    if (end2_mutex_ready)              pthread_mutex_destroy(&end2_mutex);
    if (end1_mutex_ready)              pthread_mutex_destroy(&end1_mutex);
    if (score_mutex_ready)             pthread_mutex_destroy(&score_mutex);

    return EXIT_SUCCESS;
}
