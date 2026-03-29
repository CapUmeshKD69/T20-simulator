#include "entities/entities.h"
#include "globals.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
using namespace std;

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
    PlayerControlBlock* fielding_team[TEAM_SIZE] = {nullptr}; // array to hold pointers to all fielders + wicketkeeper
    PlayerControlBlock* batting_team[TEAM_SIZE]  = {nullptr}; // array to hold pointers to all batsmen 
    pthread_t umpire_thread          = static_cast<pthread_t>(0); // this is the thread  variable that will run the umpire routine , return 0  when  created succeesfily 
    pthread_t bowler_manager_thread  = static_cast<pthread_t>(0); // this is the thread  variable that will run the bowler manager routine

    const int burst_times[] = {
    99,  // Rohit Sharma       - opener, won't be in pavilion
    99,  // Abhishek Sharma    - opener, won't be in pavilion
    30,   // Virat Kohli        - top order, SJF picks first
    5,   // Suryakumar Yadav
    7,   // KL Rahul
    12,  // Hardik Pandya
    10,  // MS Dhoni           - lower than Hardik, SJF picks Dhoni before Hardik
    15,  // Axar Patel
    20,  // Varun Chakravarthy
    25,  // Arshdeep Singh
    30   // Jasprit Bumrah     - tail, SJF picks last
};

    //Track init status for cleanup , it is not strictly needed for this simulation since we will exit after main, but it's good practice
    bool score_mutex_created = false, end1_mutex_created = false, end2_mutex_created = false;
    bool ball_mutex_created = false, log_mutex_created = false, file_mutex_created = false;
    bool delivery_cond_created = false, resolved_cond_created = false;
    bool fielders_cond_created = false, keeper_cond_created = false;
    bool umpire_cond_created = false, batsman_cond_created = false;
    bool run_exchange_cond_created = false, bowler_manager_cond_created = false;
    bool crease_sem_created = false;

    // mutexes ,  we are initializing all mutexes and condition variables at the start of main, and if any initialization fails, we print an error and exit. We also keep track of which ones were successfully created so that we can clean them up properly before exiting.
    if (!InitMutex(&score_mutex, "score_mutex"))   return EXIT_FAILURE; // no mutex, no point continuing , no need to cleanup 
    score_mutex_created = true;
    if (!InitMutex(&end1_mutex,  "end1_mutex"))    goto cleanup;
    end1_mutex_created = true;
    if (!InitMutex(&end2_mutex,  "end2_mutex"))    goto cleanup;
    end2_mutex_created = true;
    if (!InitMutex(&ball_mutex,  "ball_mutex"))    goto cleanup;
    ball_mutex_created = true;
    if (!InitMutex(&log_mutex,   "log_mutex"))     goto cleanup;
    log_mutex_created = true;
    if (!InitMutex(&file_mutex,  "file_mutex"))    goto cleanup;
    file_mutex_created = true;

    // condition variables
    if (!InitCondVar(&delivery_cond,       "delivery_cond"))       goto cleanup;
    delivery_cond_created = true;
    if (!InitCondVar(&resolved_cond,       "resolved_cond"))       goto cleanup;
    resolved_cond_created = true;
    if (!InitCondVar(&fielders_cond,       "fielders_cond"))       goto cleanup;
    fielders_cond_created = true;
    if (!InitCondVar(&keeper_cond,         "keeper_cond"))         goto cleanup;
    keeper_cond_created = true;
    if (!InitCondVar(&umpire_cond,         "umpire_cond"))         goto cleanup;
    umpire_cond_created = true;
    if (!InitCondVar(&batsman_cond,        "batsman_cond"))        goto cleanup;
    batsman_cond_created = true;
    if (!InitCondVar(&run_exchange_cond,   "run_exchange_cond"))   goto cleanup;
    run_exchange_cond_created = true;
    if (!InitCondVar(&bowler_manager_cond, "bowler_manager_cond")) goto cleanup;
    bowler_manager_cond_created = true;

    if (psem_init(&crease_semaphore, 2) != 0) {
        perror("psem_init(crease_semaphore)");
        goto cleanup;
    }
    crease_sem_created = true;

  // hardcoded config for simplicity
    use_sjf_scheduling      = true;
    use_priority_scheduling = false;


    // Record simulation start time for timestamping logs , just to start 
    simulation_start_time = chrono::high_resolution_clock::now();

    //Open CSV files 
    gantt_log_file.open("gantt_chart.csv", ios::out | ios::trunc);
    if (!gantt_log_file.is_open()) { perror("open(gantt_chart.csv)"); goto cleanup; }
    gantt_log_file << "Timestamp,ThreadID,Role,Action,Resource\n";
    gantt_log_file.flush();

    { // opening wait time log file in a separate block just to reuse the wf variable and keep it close to its usage
        const char* wf = use_sjf_scheduling ? "wait_times_sjf.csv" : "wait_times_fcfs.csv"; // selecting filename based on scheduling algorithm for easier analysis
        wait_time_log_file.open(wf, ios::out | ios::trunc);
        if (!wait_time_log_file.is_open()) { perror("open(wait_times)"); goto cleanup; }
        wait_time_log_file << "Algorithm,ThreadID,Role,ExpectedDuration,WaitTimeMs,RosterIndex,IsMiddleOrder\n";
        wait_time_log_file.flush();
    }

    //  Reset all shared state
    g_match_context = {0, 0, 0, 0}; // global_score, total_wickets, current_over, balls_in_current_over
    match_completed       = false; // set to true by umpire when match is over
    match_intensity_high  = false;// it will become true after death over activation, affects scheduling decisions
    active_striker_id     = -1; // player_id of striker, -1 if no striker currently  like in between overs and wicket there will be no batsman just to prevent deadlock
    non_striker_id        = -1; // player_id of non-striker, -1 if no non-striker currently
    active_batsmen_count  = 0; // 0 batsmen at start
    delivery_bowled       = false; // set by bowler when ball is bowled, striker waits on this
    delivery_resolved     = true;   // so bowler can start first ball
    ball_in_air           = false; // striker sets this on hit, fielder/keeper set to false after resolving
    shared_hit_result     = INVALID_HIT_RESULT; //  how many runs are there for a particular ball , set by fielder or keeper to resolve the ball, striker waits on this after hit  or mis ,  
    current_ball_sequence = 0; //  incremented by bowler for each new delivvery , so that we can make a track of which delivery is being processed
    handled_ball_sequence = -1; // set by fielder/keeper when they resolve a ball, so that we can track if the striker is waiting for a resolution for the correct delivery
    keeper_event_pending  = false; // set by striker when he misses, so that keeper can step in to resolve, also used for byes when striker misses but keeper has to decide if he can take a run or not
    run_exchange_needed   = false; // set by striker when he takes an odd run, so that both batsmen know they need to swap creases, umpire waits on this to coordinate the exchange
    exchange_resolved     = false; // set by umpire when the run exchange is resolved, so that batsmen can proceed after an odd run
    exchange_wicket       = false; // set by umpire when there is a run-out during the exchange, so that batsmen can know the outcome of the exchange and react accordingly
    over_change_requested = false; // set by umpire when an over is completed so that bowler manager can bring in the next bowler and reset the strike if needed
    wicket_fell           = false; // set by umpire when a wicket falls, so that he can bring in the next batsman if needed and also for stats tracking
    pavilion_size         = 0; // size of the pavilion queu that is  how many batsmen are waiting
    bowler_pool_size      = 0; // how many bowlers are in the pool
    current_bowler_index  = 0; // index of the current bowler in the pool  this will be used for round-robin selection
    current_bowler_pcb    = nullptr; // pointer to the current bowler CB 
    active_striker_pcb    = nullptr;// pointer to the current striker CB
    active_non_striker_pcb = nullptr;// pointer to the current non-striker CB

    //  Create player control blocks

    for (int i = 0; i < TEAM_SIZE; ++i) {
        fielding_team[i] = new PlayerControlBlock{}; // allocate a new PCB for each fielder and wicketkeeper
        batting_team[i]  = new PlayerControlBlock{};// allocate a new PCB for each batsman , this is a struct define in file
        // from here we are setting the initial state for each player control block, we set the player_id, role, and other fields to their initial values. For fielders, player_id 0 is the wicketkeeper, 1 to BOWLER_POOL_SIZE are bowlers, and the rest are regular fielders. For batsmen, all are assigned the BATSMAN role and are initially waiting in the pavilion except for the openers who will be set as active on pitch later by the umpire. We also set some default values for stats and names, which will be updated during the match.
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
        fielding_team[i]->wickets_taken          = 0; // these fields are not really relevant for fielders but we will keep them initialized for consistency, we will set actual stats later for the fielders as well based on their performance like catches and run-outs
        memset(fielding_team[i]->name, 0, sizeof(fielding_team[i]->name)); // making it empty string just to be safe, we will set actual names later
        memset(fielding_team[i]->how_out, 0, sizeof(fielding_team[i]->how_out)); // not really needed for fielders but just to be consistent, we will set actual status later

        if (i == 0) {
            fielding_team[i]->role = PlayerRole::WICKETKEEPER; // first player is the wicketkeeper
            fielding_team[i]->is_active_on_pitch = true; // wicketkeeper is always active on the pitch
        } else if (i <= BOWLER_POOL_SIZE) {
            fielding_team[i]->role = PlayerRole::BOWLER; // next few players are bowlers and they go into the bowler pool
            bowler_pool[bowler_pool_size++] = fielding_team[i]; // add to bowler pool
            all_bowlers[bowler_pool_size - 1] = fielding_team[i]; // also add to all_bowlers just for  scorecard tracking
        } else {
            fielding_team[i]->role = PlayerRole::FIELDER;// rest are regular fielders
            fielding_team[i]->is_active_on_pitch = true; // all fielders are active on the pitch
        }
        // batting team setup 
        batting_team[i]->player_id             = i;
        batting_team[i]->thread_id             = static_cast<pthread_t>(0);
        batting_team[i]->role                  = PlayerRole::BATSMAN;
        batting_team[i]->is_active_on_pitch    = false;
        batting_team[i]->is_waiting_in_pavilion = true;
        batting_team[i]->expected_stay_duration = burst_times[i]; // we are hardcoding the expected stay duration for each batsman based on their typical performance and role in the team, this will be used by the SJF scheduler to make decisions about who to send in next from the pavilion when a wicket falls
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
         // below two are for state tracking and stats
        memset(batting_team[i]->name, 0, sizeof(batting_team[i]->name)); // making it empty string just to be safe, we will set actual names later
        strncpy(batting_team[i]->how_out, "not out", sizeof(batting_team[i]->how_out) - 1); // initially all are not out, we will update this when they get out

        all_batsmen[i] = batting_team[i]; // pool of all batsmen used at last for scorecard
    }

    // Death-over specialists
    batting_team[5]->is_death_over_specialist = true;  // Hardik Pandya
    batting_team[5]->priority_score = 100; // giving him a high priority score so that if we enable priority scheduling in high intensity situations, he will be preferred over others for the next wicket after the openers and top order are gone
    batting_team[6]->is_death_over_specialist = true;  // MS Dhoni
    batting_team[6]->priority_score = 100; 

    // naming players
    {
        const char* bat_names[] = {
            "Rohit Sharma", "Abhishek Sharma", "Virat Kohli",
            "Suryakumar Yadav", "KL Rahul", "Hardik Pandya",
            "MS Dhoni", "Axar Patel", "Varun chakravarthy",
            "Arshdeep Singh", "Jasprit Bumrah"
        };
        const char* field_names[] = {
            "Jos Buttler",      // keeper
            "Jofra Archer",     // bowler 1
            "Ben Stokes",        // bowler 2
            "Adil Rashid",      // bowler 3
            "Chris Woakes",     // bowler 4
            "Moeen Ali",        // bowler 5
            "Phil Salt",       // fielder 6
            "Joe Root",         // fielder 7
            "Jecob Bethel",   // fielder 8
            "Harry Brook",      // fielder 9
            "Liam Livingstone"  // fielder 10
        };
         //  assigning named players to the pcb we created   for both teams
        for (int i = 0; i < TEAM_SIZE; ++i) {
            strncpy(batting_team[i]->name, bat_names[i], sizeof(batting_team[i]->name) - 1);
            strncpy(fielding_team[i]->name, field_names[i], sizeof(fielding_team[i]->name) - 1);
        }
    }

    // giving first bowler the ball to start 
    if (bowler_pool_size > 0) {
        current_bowler_index = 0;
        current_bowler_pcb = bowler_pool[0]; // first bowler
        current_bowler_pcb->is_active_on_pitch = true;
    }

    // Print match banner 
    puts("\n+----------------------------------------------+");
    puts("|         T20 MATCH SIMULATOR                  |");
    puts("+----------------------------------------------+\n");
    fflush(stdout);

    // Start umpire 
    {
        int rc = pthread_create(&umpire_thread, nullptr, umpire_routine, nullptr); //  creating a umpire thread with its routine   and storing  the tread in umpire thread variable 
        if (rc != 0) { errno = rc; perror("pthread_create(umpire)"); goto cleanup; }
    }

    // Start bowler manager
    {
        int rc = pthread_create(&bowler_manager_thread, nullptr, bowler_manager_routine, nullptr); //  if created succesfuly will give 0 and store the thread id in bowler_manager_thread variable
        if (rc != 0) { errno = rc; perror("pthread_create(bowler_mgr)"); goto cleanup; }
    }

    //Start fielding team threads 
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

    //Start opening batsmen 
    for (int i = 0; i < 2; ++i) {
        psem_wait(&crease_semaphore);

        batting_team[i]->is_active_on_pitch     = true;
        batting_team[i]->is_waiting_in_pavilion  = false;

        int rc = pthread_create(&batting_team[i]->thread_id, nullptr, batsman_routine, batting_team[i]);
        if (rc != 0) { // error handlong for thread craetion
            errno = rc; perror("pthread_create(opener)");
            psem_post(&crease_semaphore);
            match_completed = true;
            goto join_and_cleanup;
        }
        ++active_batsmen_count;
    }

    active_striker_pcb     = batting_team[0];
    active_non_striker_pcb = batting_team[1];
    active_striker_id      = batting_team[0]->player_id;
    non_striker_id         = batting_team[1]->player_id;

    printf("  -> Openers: %s (striker) & %s (non-striker)\n\n",
                batting_team[0]->name, batting_team[1]->name);
    fflush(stdout);

    // Track batting order
    batting_order[0] = 0;
    batting_order[1] = 1;
    batting_order_count = 2;

    // Remaining batsmen go to pavilion 
    for (int i = 2; i < TEAM_SIZE; ++i) {
        batting_team[i]->arrival_time = get_timestamp();
        pavilion_queue[pavilion_size++] = batting_team[i];
    }
 // below is the join and cleanup section where we wait for all threads to finish and then print the final scorecard and free resources, we wait for the umpire thread first since it controls the match flow and will signal when the match is completed, then we wake up all waiting threads to let them exit, and finally we join all threads to ensure they have finished before we proceed to print the scorecard and free resources.
join_and_cleanup:
    // Wait for umpire (drives match end)
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
        if (fielding_team[i] && fielding_team[i]->thread_id != static_cast<pthread_t>(0))
            pthread_join(fielding_team[i]->thread_id, nullptr);
    }

    for (int i = 0; i < TEAM_SIZE; ++i) {
        if (batting_team[i] && batting_team[i]->thread_id != static_cast<pthread_t>(0))
            pthread_join(batting_team[i]->thread_id, nullptr);
    }

    // Final scorecard
    {
        int sc = g_match_context.global_score;
        int wk = g_match_context.total_wickets;
        int ov = g_match_context.current_over;
        int bl = g_match_context.balls_in_current_over;

        puts("\n+============================================================+");
        puts("|                   FINAL SCORECARD                          |");
        printf("|  INDIA innings: %d/%d in %d.%d overs", sc, wk, ov, bl);
        puts("                            |");
        puts("+============================================================+");
        puts("|                                                            |");
        puts("|  BATTING                                                   |");
        printf("|  %-20s  %4s  %4s  %3s  %3s  %-20s  |\n",
                    "Batsman", "Runs", "Balls", "4s", "6s", "How Out");
        puts("|  --------------------------------------------------------  |");

        for (int i = 0; i < batting_order_count; ++i) {
            int idx = batting_order[i];
            PlayerControlBlock* b = all_batsmen[idx];
            if (!b) continue;
            printf("|  %-20s  %4d  %4d  %3d  %3d  %-20s  |\n",
                        b->name, b->runs_scored, b->balls_faced,
                        b->fours, b->sixes, b->how_out);
        }

        puts("|                                                            |");
        puts("|  BOWLING                                                   |");
        printf("|  %-20s  %4s  %4s  %3s  %7s  |\n",
                    "Bowler", "Overs", "Runs", "Wkts", "Econ");
        puts("|  --------------------------------------------------------  |");

        for (int i = 0; i < BOWLER_POOL_SIZE; ++i) {
            PlayerControlBlock* bw = all_bowlers[i];
            if (!bw) continue;
            int tb = bw->total_balls_bowled;
            int full_ov = tb / 6;
            int rem_b = tb % 6;
            double econ = (tb > 0) ? (bw->runs_conceded * 6.0 / tb) : 0.0;
            char ov_str[8];
            snprintf(ov_str, sizeof(ov_str), "%d.%d", full_ov, rem_b);
            printf("|  %-20s  %5s  %4d  %3d  %7.2f  |\n",
                        bw->name, ov_str, bw->runs_conceded,
                        bw->wickets_taken, econ);
        }

        puts("|                                                            |");
        printf("|  Total: %d/%d in %d.%d overs", sc, wk, ov, bl);
        puts("                                   |");
        puts("+============================================================+");
    }
    fflush(stdout);

    // Free PCBs 
    {
        unordered_set<PlayerControlBlock*> freed;
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
//  cleaning up the resources 
cleanup:
    if (gantt_log_file.is_open())      { gantt_log_file.flush();      gantt_log_file.close(); }
    if (wait_time_log_file.is_open())  { wait_time_log_file.flush();  wait_time_log_file.close(); }
    if (crease_sem_created) {
        psem_destroy(&crease_semaphore);
    }
    if (bowler_manager_cond_created)     pthread_cond_destroy(&bowler_manager_cond);
    if (run_exchange_cond_created)       pthread_cond_destroy(&run_exchange_cond);
    if (batsman_cond_created)            pthread_cond_destroy(&batsman_cond);
    if (umpire_cond_created)             pthread_cond_destroy(&umpire_cond);
    if (keeper_cond_created)             pthread_cond_destroy(&keeper_cond);
    if (fielders_cond_created)           pthread_cond_destroy(&fielders_cond);
    if (resolved_cond_created)           pthread_cond_destroy(&resolved_cond);
    if (delivery_cond_created)           pthread_cond_destroy(&delivery_cond);
    if (file_mutex_created)              pthread_mutex_destroy(&file_mutex);
    if (log_mutex_created)               pthread_mutex_destroy(&log_mutex);
    if (ball_mutex_created)              pthread_mutex_destroy(&ball_mutex);
    if (end2_mutex_created)              pthread_mutex_destroy(&end2_mutex);
    if (end1_mutex_created)              pthread_mutex_destroy(&end1_mutex);
    if (score_mutex_created)             pthread_mutex_destroy(&score_mutex);

    return EXIT_SUCCESS;
}
