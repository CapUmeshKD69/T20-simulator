#ifndef GLOBALS_H
#define GLOBALS_H

#include <chrono>
#include <fstream>
#include <pthread.h>
#include <semaphore.h>

// ─────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────
constexpr int TEAM_SIZE         = 11;
constexpr int BOWLER_POOL_SIZE  = 5;
constexpr int INVALID_HIT_RESULT = -999;
constexpr int MAX_OVERS         = 20;
constexpr int MAX_WICKETS       = 10;
constexpr int BALLS_PER_OVER    = 6;

// ─────────────────────────────────────────────────
//  Types
// ─────────────────────────────────────────────────
struct MatchContext {
    int global_score;
    int total_wickets;
    int current_over;
    int balls_in_current_over;
};

enum class PlayerRole {
    BOWLER,
    BATSMAN,
    FIELDER,
    WICKETKEEPER
};

struct PlayerControlBlock {
    int player_id;
    pthread_t thread_id;
    PlayerRole role;
    bool is_active_on_pitch;
    bool is_waiting_in_pavilion;
    int expected_stay_duration;
    int deliveries_bowled;
    int last_ball_faced;
    double arrival_time;
    double dispatch_time;
    int priority_score;
    bool is_death_over_specialist;

    // Per-player stats for scorecard
    char name[32];
    int runs_scored;
    int balls_faced;
    int fours;
    int sixes;
    char how_out[64];

    // Bowler stats
    int total_balls_bowled;
    int runs_conceded;
    int wickets_taken;
};

// ─────────────────────────────────────────────────
//  Match state (protected by score_mutex)
// ─────────────────────────────────────────────────
extern MatchContext g_match_context;
extern bool match_completed;
extern int  active_striker_id;
extern int  non_striker_id;
extern int  active_batsmen_count;
extern PlayerControlBlock* active_striker_pcb;
extern PlayerControlBlock* active_non_striker_pcb;
extern bool match_intensity_high;

// ─────────────────────────────────────────────────
//  Delivery handshake  (protected by ball_mutex)
//
//  Flow each ball:
//    Bowler:  sets delivery_bowled=true, signals delivery_cond
//    Striker: wakes, plays shot
//             - hit  → sets ball_in_air, broadcasts fielders_cond, waits batsman_cond
//             - miss → sets keeper_event_pending, signals keeper_cond, waits batsman_cond
//    Fielder/Keeper: resolves ball, sets shared_hit_result, signals batsman_cond
//    Striker: reads result, sets delivery_resolved=true, signals resolved_cond
//    Bowler:  wakes, loops for next ball
// ─────────────────────────────────────────────────
extern bool delivery_bowled;
extern bool delivery_resolved;
extern bool ball_in_air;
extern bool is_ball_in_air;
extern int  shared_hit_result;
extern int  current_ball_sequence;
extern int  handled_ball_sequence;
extern bool keeper_event_pending;

// ─────────────────────────────────────────────────
//  Run exchange / crease deadlock
//
//  On odd runs (1, 3) both batsmen must swap creases.
//  Each tries to acquire their current end, then the other.
//  With some probability a batsman "lets go" of his crease.
//  Possible outcomes:
//    - Both let go      → runs-1 credited, no wicket
//    - Striker keeps, non-striker lets go → deadlock → umpire run-out
//    - Neither lets go  → runs-1 credited, no wicket (safe)
//    - Both keep        → full runs credited, swap complete
//  Even runs (0,2,4,6): no swap needed.
// ─────────────────────────────────────────────────
extern bool run_exchange_needed;      // striker sets after odd-run hit
extern int  run_exchange_runs;        // runs scored on this ball
extern bool striker_crease_done;      // striker finished crease attempt
extern bool non_striker_crease_done;  // non-striker finished crease attempt
extern bool striker_let_go;           // striker released crease voluntarily
extern bool non_striker_let_go;       // non-striker released crease voluntarily
extern bool exchange_resolved;        // umpire has given ruling
extern int  exchange_credited_runs;   // runs actually credited
extern bool exchange_wicket;          // someone got run out
extern int  exchange_runout_id;       // player_id of the run-out victim

// ─────────────────────────────────────────────────
//  Over changes / bowler management
// ─────────────────────────────────────────────────
extern bool over_change_requested;
extern bool wicket_fell;

// ─────────────────────────────────────────────────
//  Pavilion (SJF queue)
// ─────────────────────────────────────────────────
extern PlayerControlBlock* pavilion_queue[TEAM_SIZE];
extern int pavilion_size;

// ─────────────────────────────────────────────────
//  Bowler pool (Round-Robin)
// ─────────────────────────────────────────────────
extern PlayerControlBlock* bowler_pool[BOWLER_POOL_SIZE];
extern int bowler_pool_size;
extern int current_bowler_index;
extern PlayerControlBlock* current_bowler_pcb;

// Scorecard arrays (track all batsmen/bowlers for final output)
extern PlayerControlBlock* all_batsmen[TEAM_SIZE];
extern PlayerControlBlock* all_bowlers[BOWLER_POOL_SIZE];
extern int batting_order[TEAM_SIZE];
extern int batting_order_count;

// ─────────────────────────────────────────────────
//  Scheduling config
// ─────────────────────────────────────────────────
extern bool use_sjf_scheduling;
extern bool use_priority_scheduling;

// ─────────────────────────────────────────────────
//  Synchronization primitives
// ─────────────────────────────────────────────────
extern pthread_mutex_t score_mutex;   // MatchContext + batsman identity + exchange state
extern pthread_mutex_t end1_mutex;    // striker-end crease
extern pthread_mutex_t end2_mutex;    // non-striker-end crease
extern pthread_mutex_t ball_mutex;    // delivery handshake + fielding
extern pthread_mutex_t log_mutex;     // stdout
extern pthread_mutex_t file_mutex;    // CSV writes

extern pthread_cond_t delivery_cond;       // bowler → striker
extern pthread_cond_t resolved_cond;       // striker → bowler (ball done)
extern pthread_cond_t fielders_cond;       // striker → fielders
extern pthread_cond_t keeper_cond;         // striker → keeper
extern pthread_cond_t batsman_cond;        // fielder/keeper → striker
extern pthread_cond_t umpire_cond;         // batsman → umpire (wicket/over)
extern pthread_cond_t run_exchange_cond;   // exchange coordination
extern pthread_cond_t bowler_manager_cond; // umpire → bowler manager

extern sem_t crease_semaphore;  // max 2 batsmen active

// ─────────────────────────────────────────────────
//  Timing / CSV logging
// ─────────────────────────────────────────────────
extern std::chrono::high_resolution_clock::time_point simulation_start_time;
extern std::ofstream gantt_log_file;
extern std::ofstream wait_time_log_file;

double get_timestamp();

#endif  // GLOBALS_H
