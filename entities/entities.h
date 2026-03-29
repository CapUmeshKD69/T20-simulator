#ifndef ENTITIES_H
#define ENTITIES_H

#include <pthread.h>
#include "../globals.h"

/* *--------------------------------------------------------
 * Thread Entry Points
 * ---------------------------------------------------------
 * These functions serve as the execution starting points for 
 * the POSIX threads (pthreads) in the simulation.
 *
 * Each function maps to one role in the match:
 *   bowler_routine        → bowls one delivery at a time; waits for the ball
 *                           to be fully resolved before bowling the next one
 *   batsman_routine       → striker plays shots and handles results;
 *                           non-striker is dormant except during run exchanges
 *   fielder_routine       → all fielders + keeper wait for ball events;
 *                           fielders compete for a hit ball (one wins per delivery),
 *                           keeper activates only on a miss
 *   umpire_routine        → manages over completion,
 *                           run-exchange adjudication, wicket replacements,
 *                           death-over activation, and match termination
 *   bowler_manager_routine→ handles bowler thread lifecycle (retire + spawn)
 *                           on each over change using round-robin selection
 */
void* bowler_routine(void* arg);
void* batsman_routine(void* arg);
void* fielder_routine(void* arg);
void* umpire_routine(void* arg);
void* bowler_manager_routine(void* arg);

#endif  // ENTITIES_H
