#include "entities.h"
#include "helpers.h"

#include <random>
using namespace std;

// ═══════════════════════════════════════════════════
//  FIELDER / WICKETKEEPER ROUTINE
//
//  Keeper: waits on keeper_cond, gathers missed ball
//  Fielder: waits on fielders_cond, catches/fields the ball
// ═══════════════════════════════════════════════════
void* fielder_routine(void* arg) {
    auto* pcb = static_cast<PlayerControlBlock*>(arg);
    if (!pcb) return nullptr;

    thread_local mt19937 rng(random_device{}());
    uniform_int_distribution<int> field_roll(1, 100);
    uniform_int_distribution<int> keeper_fumble(1, 100);

    while (!MatchOver()) {
        LockChecked(&ball_mutex, "ball_mutex lock (fielder)");

        if (pcb->role == PlayerRole::WICKETKEEPER) {
            // ── Keeper path ──────────────────────────────
            while (!keeper_event_pending && !match_completed) {
                pthread_cond_wait(&keeper_cond, &ball_mutex);
            }
            if (match_completed) {
                UnlockChecked(&ball_mutex, "ball_mutex unlock (keeper)");
                break;
            }
            keeper_event_pending = false;

            // Keeper has 5% chance of fumbling → byes
            int fumble = keeper_fumble(rng);
            if (fumble <= 5 && shared_hit_result == INVALID_HIT_RESULT) {
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

        // ── Fielder path ─────────────────────────────
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

        int roll = field_roll(rng);
        int result;
        if (roll <= 45)       result = 1;   // single  
        else if (roll <= 65)  result = 2;   // double  
        else if (roll <= 68)  result = 3;   // triple 
        else if (roll <= 92)  result = 4;   // boundar
        else if (roll <= 98)  result = 6;   // six     
        else                  result = -1;  // CAUGHT! 

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
