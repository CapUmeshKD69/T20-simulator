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
//
//  All 11 fielding players (10 fielders + 1 keeper) run this
//  same function. The keeper is distinguished by its role field.
//
//  FIELDER RACE:
//  When striker hits the ball, ALL fielder threads are woken via
//  broadcast on fielders_cond. They all compete to claim the ball,
//  but only one wins — enforced by the (handled_ball_sequence == seq)
//  guard. The winner sets shared_hit_result and signals the striker.
//
//  KEEPER PATH:
//  The keeper never competes for a hit ball. It only activates on
//  keeper_cond, which the striker signals when it misses (shot 78-95)
//  or when both miss (shot 98-100).
void* fielder_routine(void* arg) {
    auto* pcb = static_cast<PlayerControlBlock*>(arg);
    if (!pcb) return nullptr;

    thread_local mt19937 rng(random_device{}());
    uniform_int_distribution<int> field_roll(1, 100);    // determines fielding outcome (runs scored or catch)
    uniform_int_distribution<int> keeper_fumble(1, 100); // determines if the keeper fumbles (lets through byes)

    while (!MatchOver()) {
        LockChecked(&ball_mutex, "ball_mutex lock (fielder)");

        if (pcb->role == PlayerRole::WICKETKEEPER) {
            // ── Keeper path ──────────────────────────────
            // Keeper sleeps here until striker signals a miss event via keeper_cond
            while (!keeper_event_pending && !match_completed) {
                pthread_cond_wait(&keeper_cond, &ball_mutex);
            }
            if (match_completed) {
                UnlockChecked(&ball_mutex, "ball_mutex unlock (keeper)");
                break;
            }
            keeper_event_pending = false;  // consume the event flag before processing

            // Keeper has 5% chance of fumbling → byes
            // Only sets result if no one else has set it yet (shared_hit_result still INVALID)
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
            SignalChecked(&batsman_cond, "batsman_cond signal (keeper)");  // wake striker to process keeper's result
            continue;
        }

        // ── Fielder path ─────────────────────────────
        // Fielder sleeps here until the striker broadcasts ball_in_air=true on fielders_cond
        while (!ball_in_air && !match_completed) {
            pthread_cond_wait(&fielders_cond, &ball_mutex);
        }
        if (match_completed) {
            UnlockChecked(&ball_mutex, "ball_mutex unlock (fielder)");
            break;
        }

        int seq = current_ball_sequence;
        if (handled_ball_sequence == seq || !ball_in_air) {
            // Another fielder already claimed this delivery — skip it
            // This is the race-condition guard: only one fielder handles each ball
            UnlockChecked(&ball_mutex, "ball_mutex unlock (fielder)");
            continue;
        }

        // This fielder claims the ball for this delivery
        handled_ball_sequence = seq;  // mark so no other fielder picks this up
        ball_in_air = false;          // ball is no longer in the air

        // Determine fielding outcome via random roll
        int roll = field_roll(rng);
        int result;
        if (roll <= 45)       result = 1;   // single  
        else if (roll <= 65)  result = 2;   // double  
        else if (roll <= 68)  result = 3;   // triple 
        else if (roll <= 92)  result = 4;   // boundary (4 runs)
        else if (roll <= 98)  result = 6;   // six (over the rope)
        else                  result = -1;  // CAUGHT! batsman out (-1 is the catch sentinel)

        shared_hit_result = result;  // striker is waiting on batsman_cond for this value
        UnlockChecked(&ball_mutex, "ball_mutex unlock (fielder)");

        if (result == -1) {
            LogS("  [%s] takes the CATCH! OUT!\n", pcb->name);
        } else {
            LogSI("  [%s] fields the ball -> %d run(s)\n", pcb->name, result);
        }
        LogGanttEvent(pthread_self(), "Fielder", result == -1 ? "Catch" : "Fielded", "Ball");

        SignalChecked(&batsman_cond, "batsman_cond signal (fielder)");  // wake striker to process result
    }
    return nullptr;
}
