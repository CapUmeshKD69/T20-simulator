#include "globals.h"
using namespace std;
MatchContext g_match_context = {0, 0, 0, 0};

bool match_completed       = false;
int  active_striker_id     = -1;
int  non_striker_id        = -1;
int  active_batsmen_count  = 0;
PlayerControlBlock* active_striker_pcb     = nullptr;
PlayerControlBlock* active_non_striker_pcb = nullptr;
bool match_intensity_high  = false;

// Delivery handshake
bool delivery_bowled    = false;
bool delivery_resolved  = false;
bool ball_in_air        = false;
int  shared_hit_result  = INVALID_HIT_RESULT;
int  current_ball_sequence  = 0;
int  handled_ball_sequence  = -1;
bool keeper_event_pending   = false;

// Run exchange
bool run_exchange_needed      = false;
int  run_exchange_runs        = 0;
bool striker_crease_done      = false;
bool non_striker_crease_done  = false;
bool striker_let_go           = false;
bool non_striker_let_go       = false;
bool exchange_resolved        = false;
int  exchange_credited_runs   = 0;
bool exchange_wicket          = false;
int  exchange_runout_id       = -1;

// Over/wicket
bool over_change_requested = false;
bool wicket_fell           = false;

// Pavilion
PlayerControlBlock* pavilion_queue[TEAM_SIZE] = {nullptr};
int pavilion_size = 0;

// Bowler pool
PlayerControlBlock* bowler_pool[BOWLER_POOL_SIZE] = {nullptr};
int bowler_pool_size     = 0;
int current_bowler_index = 0;
PlayerControlBlock* current_bowler_pcb = nullptr;

// Scorecard arrays
PlayerControlBlock* all_batsmen[TEAM_SIZE] = {nullptr};
PlayerControlBlock* all_bowlers[BOWLER_POOL_SIZE] = {nullptr};
int batting_order[TEAM_SIZE] = {0};
int batting_order_count = 0;

// Config
bool use_sjf_scheduling      = true;
bool use_priority_scheduling  = true;

// Synchronization primitives
pthread_mutex_t score_mutex;
pthread_mutex_t end1_mutex;
pthread_mutex_t end2_mutex;
pthread_mutex_t ball_mutex;
pthread_mutex_t log_mutex;
pthread_mutex_t file_mutex;

pthread_cond_t delivery_cond;
pthread_cond_t resolved_cond;
pthread_cond_t fielders_cond;
pthread_cond_t keeper_cond;
pthread_cond_t batsman_cond;
pthread_cond_t umpire_cond;
pthread_cond_t run_exchange_cond;
pthread_cond_t bowler_manager_cond;

PthreadSemaphore crease_semaphore;

chrono::high_resolution_clock::time_point simulation_start_time;
ofstream gantt_log_file;
ofstream wait_time_log_file;

double get_timestamp() {
    auto now = chrono::high_resolution_clock::now();
    return chrono::duration<double, milli>(now - simulation_start_time).count();
}
