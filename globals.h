#ifndef GLOBALS_H
#define GLOBALS_H

#include <chrono>
#include <fstream>
#include <pthread.h>

// semaphore implementation using mutex and condition variable
struct PthreadSemaphore {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int count;
};
inline int psem_init(PthreadSemaphore* s, int value) {
    s->count = value;
    int rc = pthread_mutex_init(&s->mutex, nullptr);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&s->cond, nullptr);
    if (rc != 0) { pthread_mutex_destroy(&s->mutex); return rc; }
    return 0;
}
// functions of semaphore (wait, post, destroy)
inline int psem_wait(PthreadSemaphore* s) {
    pthread_mutex_lock(&s->mutex);
    while (s->count <= 0)
        pthread_cond_wait(&s->cond, &s->mutex); // if count is less than and equal to 0 then go to sleep and wait for signal
    --s->count;
    pthread_mutex_unlock(&s->mutex);
    return 0;
}
// post function of semaphore (it is same as signal function of conditional variable)
inline int psem_post(PthreadSemaphore* s) {
    pthread_mutex_lock(&s->mutex);
    ++s->count;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}
// destroy function of semaphore
inline int psem_destroy(PthreadSemaphore* s) {
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->mutex);
    return 0;
}


//  Constants
constexpr int TEAM_SIZE         = 11;
constexpr int BOWLER_POOL_SIZE  = 5;
constexpr int INVALID_HIT_RESULT = -999; // used to indicate that no hit from batsman
constexpr int MAX_OVERS         = 20;
constexpr int MAX_WICKETS       = 10;
constexpr int BALLS_PER_OVER    = 6;


//  Types
struct MatchContext { // define the match context
    int global_score;
    int total_wickets;
    int current_over;
    int balls_in_current_over;
    int total_wides;
};

enum class PlayerRole { // define the player roles
    BOWLER,
    BATSMAN,
    FIELDER,
    WICKETKEEPER
};

struct PlayerControlBlock { // define the player control block
    //(which variables we need to save so that we can start from the same state after preemption)
    int player_id; // unique id for each player
    pthread_t thread_id; // thread id for the player thread
    PlayerRole role; // role of the player (batsman, bowler, fielder, wicketkeeper)
    bool is_active_on_pitch; // using the pitch or not (for batsman and bowler)
    bool is_waiting_in_pavilion; // waiting in the pavilion or not
    int expected_stay_duration; // exptected time to stay on the pitch
    int deliveries_bowled; // for bowlers: number of deliveries bowled
    int last_ball_faced; // for batsmen: sequence number of the last ball faced
    double arrival_time; // time when the player thread was created
    double dispatch_time; // time when the player was last dispatched
    int priority_score; // scheduling priority score (priority scheduling)
    bool is_death_over_specialist; // player is death over specialist or not

    // Per-player stats for final record (for batsmen)
    char name[32];
    int runs_scored;
    int balls_faced;
    int fours;
    int sixes;
    char how_out[64];

    // Bowler stats for final record
    int total_balls_bowled;
    int runs_conceded;
    int wickets_taken;
    int wides;
};


// extern means these variables are defined in globals.cpp and can be used in other files by including globals.h
//  Match state (protected by score_mutex)
extern MatchContext g_match_context;
extern bool match_completed;
extern int  active_striker_id;
extern int  non_striker_id;
extern int  active_batsmen_count;
extern PlayerControlBlock* active_striker_pcb;
extern PlayerControlBlock* active_non_striker_pcb;
extern bool match_intensity_high;

// ----------------------****------------------------
//  delivery the bowls and process till next bowl (protected by ball_mutex)
//
//  Flow each ball (how the threads are working sychronously):
//
//    Bowler:  sets delivery_bowled=true, signals delivery_cond (this wakes up batsman on striker end)
//    Striker: wakes, plays shot
//             - hit  → sets ball_in_air, broadcasts fielders_cond, waits batsman_cond
//             - miss → sets keeper_event_pending, signals keeper_cond, waits batsman_cond
//    Fielder/Keeper: resolves ball, sets shared_hit_result(run scored), signals batsman_cond (wake up batsman for next ball)
//    Striker: reads result, sets delivery_resolved=true, signals resolved_cond (wake up bowler for next ball)
//    Bowler:  wakes, loops for next ball
//    Umpire:   waits on umpire_cond for score updates, checks for over completion and signals bowler_manager_cond if over is complete
// ----------------------****------------------------
extern bool delivery_bowled; // true when bowler has bowled the delivery
extern bool delivery_resolved; // true if fielder or keeper has resolved the delivery
extern bool ball_in_air; // true when batsman hit the shot
extern int  shared_hit_result; // runs scored on the shot
extern int  current_ball_sequence; // sequence number of the current ball being bowled
extern int  handled_ball_sequence; // sequence number of the ball for which fielder or keeper has resolved the delivery
extern bool keeper_event_pending; // true when the keeper needs to process a missed delivery
extern bool delivery_is_wide;    // true when the bowler bowls a wide

// ----------------------****------------------------
//  Run exchange / crease deadlock
//
//  On odd runs (1, 3) both batsmen must swap creases.(not if over ends)
//  Each tries to acquire their current end, then the other.
//  With some probability a batsman "lets go" of his crease.
//  Possible outcomes:
//    - Both let go      → runs-1 credited, no wicket
//    - one keeps, other one lets go → deadlock → umpire run-out
//    - Neither lets go  → no wicket (safe)
//    
//  Even runs (0,2,4,6): no swap needed.
// ----------------------****------------------------
extern bool run_exchange_needed;      // striker sets after odd-run hit
extern int  run_exchange_runs;        // runs scored on this ball
extern bool striker_crease_done;      // striker finished crease attempt
extern bool non_striker_crease_done;  // non-striker finished crease attempt
extern bool striker_let_go;           // striker released crease voluntarily
extern bool non_striker_let_go;       // non-striker released crease voluntarily
extern bool exchange_resolved;        // umpire has given decision
extern int  exchange_credited_runs;   // runs actually credited
extern bool exchange_wicket;          // someone got run out
extern int  exchange_runout_id;       // player_id of the run-out victim


//  Over changes by umpire
extern bool over_change_requested;
extern bool wicket_fell;


//  Pavilion (SJF queue)
extern PlayerControlBlock* pavilion_queue[TEAM_SIZE]; // queue to holding batsmen waiting to come on the pitch
extern int pavilion_size; // number of player currently waiting


//  Bowler pool (Round-Robin)
extern PlayerControlBlock* bowler_pool[BOWLER_POOL_SIZE]; // queue of bowlers
extern int bowler_pool_size; // number of bowlers in the pool
extern int current_bowler_index; // index of the current bowler
extern PlayerControlBlock* current_bowler_pcb; // pointer to the current bowler's PCB

// Scorecard arrays (track all batsmen/bowlers for final output)
extern PlayerControlBlock* all_batsmen[TEAM_SIZE];
extern PlayerControlBlock* all_bowlers[BOWLER_POOL_SIZE];
extern int batting_order[TEAM_SIZE];
extern int batting_order_count;

//  Scheduling config (either SJF or FCFS with priority)
extern bool use_sjf_scheduling;
extern bool use_priority_scheduling;

//  Synchronization mutexes and condition variables
extern pthread_mutex_t score_mutex;   // MatchContext + batsman identity + exchange state
extern pthread_mutex_t end1_mutex;    // striker-end crease
extern pthread_mutex_t end2_mutex;    // non-striker-end crease
extern pthread_mutex_t ball_mutex;    // delivery state (delivery_bowled, delivery_resolved, ball_in_air, shared_hit_result)
extern pthread_mutex_t log_mutex;     // for synchronizing access to log statements
extern pthread_mutex_t file_mutex;    // CSV writes

extern pthread_cond_t delivery_cond;       // bowler → striker
extern pthread_cond_t resolved_cond;       // striker → bowler (ball done)
extern pthread_cond_t fielders_cond;       // striker → fielders
extern pthread_cond_t keeper_cond;         // striker → keeper
extern pthread_cond_t batsman_cond;        // fielder/keeper → striker
extern pthread_cond_t umpire_cond;         // batsman → umpire (wicket/over)
extern pthread_cond_t run_exchange_cond;   // exchange coordination
extern pthread_cond_t bowler_manager_cond; // umpire → bowler manager (when over is complete)

extern PthreadSemaphore crease_semaphore;  // max 2 batsmen active


//  Timing and logging
extern std::chrono::high_resolution_clock::time_point simulation_start_time;
extern std::ofstream gantt_log_file;
extern std::ofstream wait_time_log_file;

double get_timestamp();

#endif