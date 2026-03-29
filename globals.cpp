#include "globals.h"
using namespace std;

// global variables initialize 
MatchContext g_match_context = {0, 0, 0, 0, 0}; // runs, wickets, overs, balls in current over
bool match_completed       = false;
int  active_striker_id     = -1;
int  non_striker_id        = -1;
int  active_batsmen_count  = 0; // in starting no batsman is active on pitch so count is 0 
PlayerControlBlock* active_striker_pcb     = nullptr; // in starting no batsman is on strike
PlayerControlBlock* active_non_striker_pcb = nullptr; // in starting no batsman is on non strike
bool match_intensity_high  = false;

// ball state variables initialize 
bool delivery_bowled    = false; // when bowler has bowled then we set this to true
bool delivery_resolved  = false; // when fielder or keeper has resolved then we set this to true
bool ball_in_air        = false; // when ball is in air then we set this to true 
int  shared_hit_result  = INVALID_HIT_RESULT; // output of the shot
int  current_ball_sequence  = 0; // sequence number of the current ball being bowled
int  handled_ball_sequence  = -1; // sequence number of the ball for which fielder or keeper has resolved 
bool keeper_event_pending   = false; // set by striker when he misses
bool delivery_is_wide       = false; // set by bowler when the delivery is a wide

// variables for strike exchange logic
bool run_exchange_needed      = false; //strike exchange needted or not
int  run_exchange_runs        = 0; // runs on the current ball
bool striker_crease_done      = false; // striker come back to crease or not
bool non_striker_crease_done  = false; // non striker come back to crease or not
bool striker_let_go           = false; // striker left the crease or not
bool non_striker_let_go       = false; // non striker left the crease or not
bool exchange_resolved        = false; // strike exchange completed or not
int  exchange_credited_runs   = 0; // runs actually credited
bool exchange_wicket          = false; // wicket fell on the exchange or not
int  exchange_runout_id       = -1; // player id of the player run out on strike exchange if it happened

// Over complete variable and wicket fall variable
bool over_change_requested = false; // over change requested or not
bool wicket_fell           = false; // wicket fell or not

// Pavilion queue for batsmen waiting to come on pitch
PlayerControlBlock* pavilion_queue[TEAM_SIZE] = {nullptr}; 
int pavilion_size = 0;

// Bowler pool and current bowler variables
PlayerControlBlock* bowler_pool[BOWLER_POOL_SIZE] = {nullptr};
int bowler_pool_size     = 0;
int current_bowler_index = 0;
PlayerControlBlock* current_bowler_pcb = nullptr;

// arrays to hold all player PCBs for both batsmen and bowlers
PlayerControlBlock* all_batsmen[TEAM_SIZE] = {nullptr};
PlayerControlBlock* all_bowlers[BOWLER_POOL_SIZE] = {nullptr};
int batting_order[TEAM_SIZE] = {0}; // batting order of the batsmen
int batting_order_count = 0;

// scheduling flags and parameters
bool use_sjf_scheduling      = true;
bool use_priority_scheduling  = true;

// Synchronization mutexes and condition variables
pthread_mutex_t score_mutex; // for synchronizing access to match score and stats
pthread_mutex_t end1_mutex;  // for synchronizing access to end1 (striker's end)
pthread_mutex_t end2_mutex; // for synchronizing access to end2 (non-striker's end)
pthread_mutex_t ball_mutex; // for synchronizing access to ball state variables
pthread_mutex_t log_mutex; // for synchronizing access to log statements
pthread_mutex_t file_mutex; // for synchronizing access to log files

pthread_cond_t delivery_cond; // signaled by bowler after bowling a delivery
pthread_cond_t resolved_cond; // signaled by striker after processing the delivery
pthread_cond_t fielders_cond; // signaled by batsman after the shot is played
pthread_cond_t keeper_cond; // signaled by batsman after the shot is missed
pthread_cond_t batsman_cond; // signaled by fielder or keeper after resolving the delivery
pthread_cond_t umpire_cond; // singaled by fielders to update the score
pthread_cond_t run_exchange_cond; // signaled by striker to do strike exchange
pthread_cond_t bowler_manager_cond; // signaled by umpire after over completion 

PthreadSemaphore crease_semaphore; // semaphore to make sure only 2 batsman are on the pitch

chrono::high_resolution_clock::time_point simulation_start_time;
ofstream gantt_log_file; // gantt chart log file
ofstream wait_time_log_file; // wait time log file

double get_timestamp() {
    auto now = chrono::high_resolution_clock::now();
    return chrono::duration<double, milli>(now - simulation_start_time).count();
}