/*
  Ethereal is a UCI chess playing engine authored by Andrew Grant.
  <https://github.com/AndyGrant/Ethereal>     <andrew@grantnet.us>

  Ethereal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Ethereal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "attacks.h"
#include "bitboards.h"
#include "board.h"
#include "castle.h"
#include "evaluate.h"
#include "fathom/tbprobe.h"
#include "history.h"
#include "move.h"
#include "movegen.h"
#include "movepicker.h"
#include "psqt.h"
#include "search.h"
#include "syzygy.h"
#include "thread.h"
#include "time.h"
#include "transposition.h"
#include "types.h"
#include "uci.h"
#include "windows.h"


int LMRTable[64][64]; // Late Move Reductions, LMRTable[depth][played]

volatile int ABORT_SIGNAL; // Global ABORT flag for threads

volatile int IS_PONDERING; // Global PONDER flag for threads


void initSearch(){

    // Init Late Move Reductions Table
    for (int d = 1; d < 64; d++)
        for (int p = 1; p < 64; p++)
            LMRTable[d][p] = 0.75 + log(d) * log(p) / 2.25;
}

void getBestMove(Thread* threads, Board* board, Limits* limits, uint16_t *best, uint16_t *ponder){

    ABORT_SIGNAL = 0; // Clear the ABORT signal for the new search

    updateTT(); // Table is on a new search, thus a new generation

    // Before searching, check to see if we are in the Syzygy Tablebases. If so
    // the probe will return 1, will initialize the best move, and will report
    // a depth MAX_PLY - 1 search to the interface. If found, we are done here.
    if (tablebasesProbeDTZ(board, best)) { *ponder = NONE_MOVE; return; }

    // Initialize SearchInfo, used for reporting and time managment logic
    SearchInfo info;
    memset(&info, 0, sizeof(SearchInfo));
    initTimeManagment(&info, limits);

    // Setup the thread pool for a new search
    newSearchThreadPool(threads, board, limits, &info);

    // Launch all of the threads
    pthread_t pthreads[threads[0].nthreads];
    for (int i = 1; i < threads[0].nthreads; i++)
        pthread_create(&pthreads[i], NULL, &iterativeDeepening, &threads[i]);
    iterativeDeepening((void*) &threads[0]);

    // Wait for all (helper) threads to finish
    for (int i = 1; i < threads[0].nthreads; i++)
        pthread_join(pthreads[i], NULL);

    // Save the best move and ponder move
    *best = info.bestMoves[info.depth];
    *ponder = info.ponderMoves[info.depth];
}

void* iterativeDeepening(void* vthread){

    Thread* const thread   = (Thread*) vthread;
    SearchInfo* const info = thread->info;
    Limits* const limits   = thread->limits;
    const int mainThread   = thread->index == 0;
    const int cycle        = thread->index % SMPCycles;

    // Bind when we expect to deal with Numa
    if (thread->nthreads > 8)
        bindThisThread(thread->index);

    // Perform iterative deepening until exit conditions
    for (thread->depth = 1; thread->depth < MAX_PLY; thread->depth++){

        // If we abort to here, we stop searching
        if (setjmp(thread->jbuffer)) break;

        // Perform the actual search for the current depth
        thread->value = aspirationWindow(thread, thread->depth, thread->value);

        // Occasionally skip depths using Laser's method
        if (!mainThread && (thread->depth + cycle) % SkipDepths[cycle] == 0)
            thread->depth += SkipSize[cycle];

        // Helper threads need not worry about time and search info updates
        if (!mainThread) continue;

        // Update the Search Info structure for the main thread
        info->depth                      = thread->depth;
        info->values[thread->depth]      = thread->value;
        info->bestMoves[thread->depth]   = thread->pv.line[0];
        info->ponderMoves[thread->depth] = thread->pv.length >= 2 ? thread->pv.line[1] : NONE_MOVE;

        // Send information about this search to the interface
        uciReport(thread->threads, -MATE, MATE, thread->value);

        // Update time allocation based on score and pv changes
        updateTimeManagment(info, limits, thread->depth, thread->value);

        // Don't want to exit while pondering
        if (IS_PONDERING) continue;

        // Check for termination by any of the possible limits
        if (   (limits->limitedBySelf  && terminateTimeManagment(info))
            || (limits->limitedBySelf  && elapsedTime(info) > info->maxUsage)
            || (limits->limitedByTime  && elapsedTime(info) > limits->timeLimit)
            || (limits->limitedByDepth && thread->depth >= limits->depthLimit))
            break;
    }

    // Main thread should kill others when finishing
    if (mainThread) ABORT_SIGNAL = 1;

    return NULL;
}

int aspirationWindow(Thread* thread, int depth, int lastValue){

    const int mainThread = thread->index == 0;
    int alpha, beta, value, delta = WindowSize;

    // Create an aspiration window, unless still below the starting depth
    alpha = depth >= WindowDepth ? MAX(-MATE, lastValue - delta) : -MATE;
    beta  = depth >= WindowDepth ? MIN( MATE, lastValue + delta) :  MATE;

    // Keep trying larger windows until one works
    while (1) {

        // Perform a search on the window, return if inside the window
        value = search(thread, &thread->pv, alpha, beta, depth, 0);
        if (value > alpha && value < beta)
            return value;

        // Report lower and upper bounds after at a certain time
        if (mainThread && elapsedTime(thread->info) >= WindowTimerMS)
            uciReport(thread->threads, alpha, beta, value);

        // Search failed low
        if (value <= alpha) {
            beta  = (alpha + beta) / 2;
            alpha = MAX(-MATE, alpha - delta);
        }

        // Search failed high
        if (value >= beta)
            beta = MIN(MATE, beta + delta);

        // Expand the search window
        delta = delta + delta / 2;
    }
}

int search(Thread* thread, PVariation* pv, int alpha, int beta, int depth, int height){

    const int PvNode   = (alpha != beta - 1);
    const int RootNode = (height == 0);
    Board* const board = &thread->board;

    unsigned tbresult;
    int quiets = 0, played = 0, hist = 0, cmhist = 0, fmhist = 0;
    int ttHit, ttValue = 0, ttEval = 0, ttDepth = 0, ttBound = 0;
    int R, newDepth, rAlpha, rBeta, oldAlpha = alpha;
    int inCheck, isQuiet, improving, extension, singular, skipQuiets = 0;
    int eval, value = -MATE, best = -MATE, futilityMargin, seeMargin[2];
    uint16_t move, ttMove = NONE_MOVE, bestMove = NONE_MOVE, quietsTried[MAX_MOVES];
    MovePicker movePicker;

    PVariation lpv;
    lpv.length = 0;
    pv->length = 0;

    // Step 1. Quiescence Search. Perform a search using mostly tactical
    // moves to reach a more stable position for use as a static evaluation
    if (depth <= 0 && !board->kingAttackers)
        return qsearch(thread, pv, alpha, beta, height);

    // Ensure positive depth
    depth = MAX(0, depth);

    // Updates for UCI reporting
    thread->seldepth = RootNode ? 0 : MAX(thread->seldepth, height);
    thread->nodes++;

    // Step 2. Abort Check. Exit the search if signaled by main thread or the
    // UCI thread, or if the search time has expired outside pondering mode
    if (ABORT_SIGNAL || (terminateSearchEarly(thread) && !IS_PONDERING))
        longjmp(thread->jbuffer, 1);

    // Step 3. Check for early exit conditions. Don't take early exits in
    // the RootNode, since this would prevent us from having a best move
    if (!RootNode){

        // Check for the fifty move rule, a draw by
        // repetition, or insufficient mating material
        if (boardIsDrawn(board, height))
            return 0;

        // Check to see if we have exceeded the maxiumum search draft
        if (height >= MAX_PLY)
            return evaluateBoard(board, &thread->pktable);

        // Mate Distance Pruning. Check to see if this line is so
        // good, or so bad, that being mated in the ply, or  mating in
        // the next one, would still not create a more extreme line
        rAlpha = alpha > -MATE + height     ? alpha : -MATE + height;
        rBeta  =  beta <  MATE - height - 1 ?  beta :  MATE - height - 1;
        if (rAlpha >= rBeta) return rAlpha;
    }

    // Step 4. Probe the Transposition Table, adjust the value, and consider cutoffs
    if ((ttHit = getTTEntry(board->hash, &ttMove, &ttValue, &ttEval, &ttDepth, &ttBound))){

        ttValue = valueFromTT(ttValue, height); // Adjust any MATE scores

        // Only cut with a greater depth search, and do not return
        // when in a PvNode, unless we would otherwise hit a qsearch
        if (ttDepth >= depth && (depth == 0 || !PvNode)){

            // Table is exact or produces a cutoff
            if (    ttBound == BOUND_EXACT
                || (ttBound == BOUND_LOWER && ttValue >= beta)
                || (ttBound == BOUND_UPPER && ttValue <= alpha))
                return ttValue;
        }
    }

    // Step 5. Probe the Syzygy Tablebases. tablebasesProbeWDL() handles all of
    // the conditions about the board, the existance of tables, the probe depth,
    // as well as to not probe at the Root. The return is defined by the Fathom API
    if ((tbresult = tablebasesProbeWDL(board, depth, height)) != TB_RESULT_FAILED){

        thread->tbhits++; // Increment tbhits counter for this thread

        // Convert the WDL value to a score. We consider blessed losses
        // and cursed wins to be a draw, and thus set value to zero.
        value = tbresult == TB_LOSS ? -MATE + MAX_PLY + height + 1
              : tbresult == TB_WIN  ?  MATE - MAX_PLY - height - 1 : 0;

        // Identify the bound based on WDL scores. For wins and losses the
        // bound is not exact because we are dependent on the height, but
        // for draws (and blessed / cursed) we know the tbresult to be exact
        ttBound = tbresult == TB_LOSS ? BOUND_UPPER
                : tbresult == TB_WIN  ? BOUND_LOWER : BOUND_EXACT;

        // Check to see if the WDL value would cause a cutoff
        if (    ttBound == BOUND_EXACT
            || (ttBound == BOUND_LOWER && value >= beta)
            || (ttBound == BOUND_UPPER && value <= alpha)){

            storeTTEntry(board->hash, NONE_MOVE, value, VALUE_NONE, MAX_PLY-1, ttBound);
            return value;
        }
    }

    // Step 6. Initialize flags and values used by pruning and search methods

    // We can grab in check based on the already computed king attackers bitboard
    inCheck = !!board->kingAttackers;

    // Save off static evaluation history. Reuse TT entry eval if possible
    eval = thread->evalStack[height] = ttHit && ttEval != VALUE_NONE ? ttEval
                                     : evaluateBoard(board, &thread->pktable);

    // Futility Pruning Margin
    futilityMargin = eval + FutilityMargin * depth;

    // Static Exchange Evaluation Pruning Margins
    seeMargin[0] = SEENoisyMargin * depth * depth;
    seeMargin[1] = SEEQuietMargin * depth;

    // Improving if our static eval increased in the last move
    improving = height >= 2 && eval > thread->evalStack[height-2];

    // Reset Killer moves for our children
    thread->killers[height+1][0] = NONE_MOVE;
    thread->killers[height+1][1] = NONE_MOVE;

    // Step 7. Razoring. If a Quiescence Search for the current position
    // still falls way below alpha, we will assume that the score from
    // the Quiescence search was sufficient.
    if (   !PvNode
        && !inCheck
        &&  depth <= RazorDepth
        &&  eval + RazorMargin < alpha)
        return qsearch(thread, pv, alpha, beta, height);

    // Step 8. Beta Pruning / Reverse Futility Pruning / Static Null
    // Move Pruning. If the eval is few pawns above beta then exit early
    if (   !PvNode
        && !inCheck
        &&  depth <= BetaPruningDepth
        &&  eval - BetaMargin * depth > beta)
        return eval;

    // Step 9. Null Move Pruning. If our position is so good that giving
    // our opponent back-to-back moves is still not enough for them to
    // gain control of the game, we can be somewhat safe in saying that
    // our position is too good to be true. We avoid NMP when we have
    // information from the Transposition Table which suggests it will fail
    if (   !PvNode
        && !inCheck
        &&  depth >= NullMovePruningDepth
        &&  eval >= beta
        &&  hasNonPawnMaterial(board, board->turn)
        &&  thread->moveStack[height-1] != NULL_MOVE
        &&  thread->moveStack[height-2] != NULL_MOVE
        && (!ttHit || !(ttBound & BOUND_UPPER) || ttValue >= beta)) {

        R = 4 + depth / 6 + MIN(3, (eval - beta) / 200);

        apply(thread, board, NULL_MOVE, height);
        value = -search(thread, &lpv, -beta, -beta+1, depth-R, height+1);
        revert(thread, board, NULL_MOVE, height);

        if (value >= beta) return beta;
    }

    // Step 10. Probcut Pruning. If we have a good capture that causes a cutoff
    // with an adjusted beta value at a reduced search depth, we expect that it
    // will cause a similar cutoff at this search depth, with a normal beta value
    if (   !PvNode
        &&  depth >= ProbCutDepth
        &&  abs(beta) < MATE_IN_MAX
        &&  eval + bestTacticalMoveValue(board) >= beta + ProbCutMargin){

        // Try tactical moves which maintain rBeta
        rBeta = MIN(beta + ProbCutMargin, MATE - MAX_PLY - 1);
        initNoisyMovePicker(&movePicker, thread, rBeta - eval);

        while ((move = selectNextMove(&movePicker, board, 1)) != NONE_MOVE){

            // Apply move, skip if move is illegal
            if (!apply(thread, board, move, height))
                continue;

            // Perform a reduced depth verification search
            value = -search(thread, &lpv, -rBeta, -rBeta+1, depth-4, height+1);

            // Revert the board state
            revert(thread, board, move, height);

            // Probcut failed high
            if (value >= rBeta) return value;
        }
    }

    // Step 11. Initialize the Move Picker and being searching through each
    // move one at a time, until we run out or a move generates a cutoff
    initMovePicker(&movePicker, thread, ttMove, height);
    while ((move = selectNextMove(&movePicker, board, skipQuiets)) != NONE_MOVE){

        // If this move is quiet we will save it to a list of attemped quiets.
        // Also lookup the history score, as we will in most cases need it.
        if ((isQuiet = !moveIsTactical(board, move))){
            quietsTried[quiets++] = move;
            getHistory(thread, move, height, &hist, &cmhist, &fmhist);
        }

        // Step 12. Quiet Move Pruning. Prune any quiet move that meets one
        // of the criteria below, only after proving a non mated line exists
        if (isQuiet && best > MATED_IN_MAX) {

            // Step 12A. Futility Pruning. If our score is far below alpha, and we
            // don't expect anything from this move, we can skip all other quiets
            if (   futilityMargin <= alpha
                && depth <= FutilityPruningDepth
                && hist + cmhist + fmhist < FutilityPruningHistoryLimit[improving])
                skipQuiets = 1;

            // Step 12B. Late Move Pruning / Move Count Pruning. If we have
            // tried many quiets in this position already, and we don't expect
            // anything from this move, we can skip all the remaining quiets
            if (   depth <= LateMovePruningDepth
                && quiets >= LateMovePruningCounts[improving][depth])
                skipQuiets = 1;

            // Step 12C. Counter Move Pruning. Moves with poor counter
            // move history are pruned at near leaf nodes of the search.
            if (   depth <= CounterMovePruningDepth[improving]
                && cmhist < CounterMoveHistoryLimit[improving])
                continue;

            // Step 12D. Follow Up Move Pruning. Moves with poor follow up
            // move history are pruned at near leaf nodes of the search.
            if (   depth <= FollowUpMovePruningDepth[improving]
                && fmhist < FollowUpMoveHistoryLimit[improving])
                continue;
        }

        // Step 13. Static Exchange Evaluation Pruning. Prune moves which fail
        // to beat a depth dependent SEE threshold. The use of movePicker.stage
        // is a speedup, which assumes that good noisy moves have a positive SEE
        if (    best > MATED_IN_MAX
            &&  depth <= SEEPruningDepth
            &&  movePicker.stage > STAGE_GOOD_NOISY
            && !staticExchangeEvaluation(board, move, seeMargin[isQuiet]))
            continue;

        // Apply move, skip if move is illegal
        if (!apply(thread, board, move, height))
            continue;

        // Update counter of moves actually played
        played += 1;

        // Step 14. Late Move Reductions. Compute the reduction,
        // allow the later steps to perform the reduced searches
        if (isQuiet && depth > 2 && played > 1){

            R  = LMRTable[MIN(depth, 63)][MIN(played, 63)];

            // Increase for non PV nodes
            R += !PvNode;

            // Increase for non improving nodes
            R += !improving;

            // Reduce for Killers and Counters
            R -= move == movePicker.killer1
              || move == movePicker.killer2
              || move == movePicker.counter;

            // Adjust based on history
            R -= MAX(-2, MIN(2, (hist + cmhist + fmhist) / 5000));

            // Don't extend or drop into QS
            R  = MIN(depth - 1, MAX(R, 1));

        } else R = 1;

        // Identify moves which are candidate singular moves
        singular =  !RootNode
                 &&  depth >= 8
                 &&  move == ttMove
                 &&  ttDepth >= depth - 2
                 && (ttBound & BOUND_LOWER);

        // Step 15. Extensions. Search an additional ply when we are in check, when
        // an early move has excellent continuation history, or when we have a move
        // from the transposition table which appears to beat all other moves by a
        // relativly large margin,
        extension =  (inCheck)
                  || (isQuiet && quiets <= 4 && cmhist >= 10000 && fmhist >= 10000)
                  || (singular && moveIsSingular(thread, ttMove, ttValue, depth, height));

        // Factor the extension into the new depth. Do not extend at the root
        newDepth = depth + (extension && !RootNode);

        // Step 16A. If we triggered the LMR conditions (which we know by the value of R),
        // then we will perform a reduced search on the null alpha window, as we have no
        // expectation that this move will be worth looking into deeper
        if (R != 1) value = -search(thread, &lpv, -alpha-1, -alpha, newDepth-R, height+1);

        // Step 16B. There are two situations in which we will search again on a null window,
        // but without a depth reduction R. First, if the LMR search happened, and failed
        // high, secondly, if we did not try an LMR search, and this is not the first move
        // we have tried in a PvNode, we will research with the normally reduced depth
        if ((R != 1 && value > alpha) || (R == 1 && !(PvNode && played == 1)))
            value = -search(thread, &lpv, -alpha-1, -alpha, newDepth-1, height+1);

        // Step 16C. Finally, if we are in a PvNode and a move beat alpha while being
        // search on a reduced depth, we will search again on the normal window. Also,
        // if we did not perform Step 18B, we will search for the first time on the
        // normal window. This happens only for the first move in a PvNode
        if (PvNode && (played == 1 || value > alpha))
            value = -search(thread, &lpv, -beta, -alpha, newDepth-1, height+1);

        // Revert the board state
        revert(thread, board, move, height);

        // Step 17. Update search stats for the best move and its value. Update
        // our lower bound (alpha) if exceeded, and also update the PV in that case
        if (value > best){

            best = value;
            bestMove = move;

            if (value > alpha){
                alpha = value;

                // Copy our child's PV and prepend this move to it
                pv->length = 1 + lpv.length;
                pv->line[0] = move;
                memcpy(pv->line + 1, lpv.line, sizeof(uint16_t) * lpv.length);

                // Search failed high
                if (alpha >= beta) break;
            }
        }
    }

    // Step 18. Stalemate and Checkmate detection. If no moves were found to
    // be legal (search makes sure to play at least one legal move, if any),
    // then we are either mated or stalemated, which we can tell by the inCheck
    // flag. For mates, return a score based on the distance from root, so we
    // can differentiate between close mates and far away mates from the root
    if (played == 0) return inCheck ? -MATE + height : 0;

    // Step 19. Update History counters on a fail high for a quiet move
    if (best >= beta && !moveIsTactical(board, bestMove))
        updateHistoryHeuristics(thread, quietsTried, quiets, height, depth*depth);

    // Step 20. Store results of search into the table
    ttBound = best >= beta    ? BOUND_LOWER
            : best > oldAlpha ? BOUND_EXACT : BOUND_UPPER;
    storeTTEntry(board->hash, bestMove, valueToTT(best, height), eval, depth, ttBound);

    return best;
}

int qsearch(Thread* thread, PVariation* pv, int alpha, int beta, int height){

    Board* const board = &thread->board;

    int eval, value, best, margin;
    int ttHit, ttValue = 0, ttEval = 0, ttDepth = 0, ttBound = 0;
    uint16_t move, ttMove = NONE_MOVE;

    MovePicker movePicker;

    PVariation lpv;
    lpv.length = 0;
    pv->length = 0;

    // Updates for UCI reporting
    thread->seldepth = MAX(thread->seldepth, height);
    thread->nodes++;

    // Step 1. Abort Check. Exit the search if signaled by main thread or the
    // UCI thread, or if the search time has expired outside pondering mode
    if (ABORT_SIGNAL || (terminateSearchEarly(thread) && !IS_PONDERING))
        longjmp(thread->jbuffer, 1);

    // Step 2. Draw Detection. Check for the fifty move rule,
    // a draw by repetition, or insufficient mating material
    if (boardIsDrawn(board, height))
        return 0;

    // Step 3. Max Draft Cutoff. If we are at the maximum search draft,
    // then end the search here with a static eval of the current board
    if (height >= MAX_PLY)
        return evaluateBoard(board, &thread->pktable);

    // Step 4. Probe the Transposition Table, adjust the value, and consider cutoffs
    if ((ttHit = getTTEntry(board->hash, &ttMove, &ttValue, &ttEval, &ttDepth, &ttBound))){

        ttValue = valueFromTT(ttValue, height); // Adjust any MATE scores

        // Table is exact or produces a cutoff
        if (    ttBound == BOUND_EXACT
            || (ttBound == BOUND_LOWER && ttValue >= beta)
            || (ttBound == BOUND_UPPER && ttValue <= alpha))
            return ttValue;
    }

    // Step 5. Eval Pruning. If a static evaluation of the board will
    // exceed beta, then we can stop the search here. Also, if the static
    // eval exceeds alpha, we can call our static eval the new alpha
    best = eval = ttHit && ttEval != VALUE_NONE ? ttEval
                : evaluateBoard(board, &thread->pktable);
    alpha = MAX(alpha, eval);
    if (alpha >= beta) return eval;

    // Step 6. Delta Pruning. Even the best possible capture and or promotion
    // combo with the additional boost of the futility margin would still fail
    margin = alpha - eval - QFutilityMargin;
    if (bestTacticalMoveValue(board) < margin)
        return eval;

    // Step 7. Move Generation and Looping. Generate all tactical moves
    // and return those which are winning via SEE, and also strong enough
    // the margin computed in the Delta Pruning step found above to beat
    initNoisyMovePicker(&movePicker, thread, MAX(QSEEMargin, margin));
    while ((move = selectNextMove(&movePicker, board, 1)) != NONE_MOVE) {

        // Apply move, skip if move is illegal
        if (!apply(thread, board, move, height))
            continue;

        // Search next depth
        value = -qsearch(thread, &lpv, -beta, -alpha, height+1);

        // Revert the board state
        revert(thread, board, move, height);

        // Improved current value
        if (value > best){
            best = value;

            // Improved current lower bound
            if (value > alpha){
                alpha = value;

                // Update the Principle Variation
                pv->length = 1 + lpv.length;
                pv->line[0] = move;
                memcpy(pv->line + 1, lpv.line, sizeof(uint16_t) * lpv.length);
            }
        }

        // Search has failed high
        if (alpha >= beta)
            return best;
    }

    return best;
}

int staticExchangeEvaluation(Board* board, uint16_t move, int threshold){

    int from, to, type, ptype, colour, balance, nextVictim;
    uint64_t bishops, rooks, occupied, attackers, myAttackers;

    // Unpack move information
    from  = MoveFrom(move);
    to    = MoveTo(move);
    type  = MoveType(move);
    ptype = MovePromoPiece(move);

    // Next victim is moved piece, or promotion type when promoting
    nextVictim = type != PROMOTION_MOVE
               ? pieceType(board->squares[from])
               : ptype;

    // Balance is the value of the move minus threshold. Function
    // call takes care for Enpass and Promotion moves. Castling is
    // handled as a result of a King's value being zero, by trichotomy
    // either the best case or the worst case condition will be hit
    balance = thisTacticalMoveValue(board, move) - threshold;

    // Best case is we lose nothing for the move
    if (balance < 0) return 0;

    // Worst case is losing the moved piece
    balance -= SEEPieceValues[nextVictim];
    if (balance >= 0) return 1;

    // Grab sliders for updating revealed attackers
    bishops = board->pieces[BISHOP] | board->pieces[QUEEN];
    rooks   = board->pieces[ROOK  ] | board->pieces[QUEEN];

    // Let occupied suppose that the move was actually made
    occupied = (board->colours[WHITE] | board->colours[BLACK]);
    occupied = (occupied ^ (1ull << from)) | (1ull << to);
    if (type == ENPASS_MOVE) occupied ^= (1ull << board->epSquare);

    // Get all pieces which attack the target square. And with occupied
    // so that we do not let the same piece attack twice
    attackers = allAttackersToSquare(board, occupied, to) & occupied;

    // Now our opponents turn to recapture
    colour = !board->turn;

    while (1){

        // If we have no more attackers left we lose
        myAttackers = attackers & board->colours[colour];
        if (myAttackers == 0ull) break;

        // Find our weakest piece to attack with
        for (nextVictim = PAWN; nextVictim <= QUEEN; nextVictim++)
            if (myAttackers & board->pieces[nextVictim])
                break;

        // Remove this attacker from the occupied
        occupied ^= (1ull << getlsb(myAttackers & board->pieces[nextVictim]));

        // A diagonal move may reveal bishop or queen attackers
        if (nextVictim == PAWN || nextVictim == BISHOP || nextVictim == QUEEN)
            attackers |= bishopAttacks(to, occupied) & bishops;

        // A vertical or horizontal move may reveal rook or queen attackers
        if (nextVictim == ROOK || nextVictim == QUEEN)
            attackers |=   rookAttacks(to, occupied) & rooks;

        // Make sure we did not add any already used attacks
        attackers &= occupied;

        // Swap the turn
        colour = !colour;

        // Negamax the balance and add the value of the next victim
        balance = -balance - 1 - SEEPieceValues[nextVictim];

        // If the balance is non negative after giving away our piece then we win
        if (balance >= 0){

            // As a slide speed up for move legality checking, if our last attacking
            // piece is a king, and our opponent still has attackers, then we've
            // lost as the move we followed would be illegal
            if (nextVictim == KING && (attackers & board->colours[colour]))
                colour = !colour;

            break;
        }
    }

    // Side to move after the loop loses
    return board->turn != colour;
}

int moveIsTactical(Board* board, uint16_t move){
    return board->squares[MoveTo(move)] != EMPTY
        || MoveType(move) == PROMOTION_MOVE
        || MoveType(move) == ENPASS_MOVE;
}

int hasNonPawnMaterial(Board* board, int turn){
    uint64_t friendly = board->colours[turn];
    uint64_t kings = board->pieces[KING];
    uint64_t pawns = board->pieces[PAWN];
    return (friendly & (kings | pawns)) != friendly;
}

int valueFromTT(int value, int height){
    return value >=  MATE_IN_MAX ? value - height
         : value <= MATED_IN_MAX ? value + height
         : value;
}

int valueToTT(int value, int height){
    return value >=  MATE_IN_MAX ? value + height
         : value <= MATED_IN_MAX ? value - height
         : value;
}

int thisTacticalMoveValue(Board* board, uint16_t move){

    int value = SEEPieceValues[pieceType(board->squares[MoveTo(move)])];

    if (MoveType(move) == PROMOTION_MOVE)
        value += SEEPieceValues[MovePromoPiece(move)] - SEEPieceValues[PAWN];

    if (MoveType(move) == ENPASS_MOVE)
        value += SEEPieceValues[PAWN];

    return value;
}

int bestTacticalMoveValue(Board* board){

    int value = SEEPieceValues[PAWN];

    // Look at enemy pieces we might try to capture
    uint64_t targets = board->colours[!board->turn];

    // Look for our strongest possible target on the board
    for (int piece = QUEEN; piece > PAWN; piece--) {
        if (targets & board->pieces[piece]) {
            value = SEEPieceValues[piece];
            break;
        }
    }

    // See if we have any pawns on promoting ranks. If so, assume that
    // we can promote one of our pawns to at least a queen
    if (   board->pieces[PAWN]
        &  board->colours[board->turn]
        & (board->turn == WHITE ? RANK_7 : RANK_2))
        value += SEEPieceValues[QUEEN] - SEEPieceValues[PAWN];

    return value;
}

int moveIsSingular(Thread* thread, uint16_t ttMove, int ttValue, int depth, int height){

    Board* const board = &thread->board;

    int value = -MATE;
    int rBeta = MAX(ttValue - depth, -MATE);

    uint16_t move;
    MovePicker movePicker;
    PVariation lpv; lpv.length = 0;

    // Table move was already applied
    revert(thread, board, ttMove, height);

    // Iterate and check all moves other than the table move
    initMovePicker(&movePicker, thread, NONE_MOVE, height);
    while ((move = selectNextMove(&movePicker, board, 0)) != NONE_MOVE){

        // Skip the table move
        if (move == ttMove) continue;

        // Apply move, skip if move is illegal
        if (!apply(thread, board, move, height))
            continue;

        // Perform a reduced depth search on a null rbeta window
        value = -search(thread, &lpv, -rBeta-1, -rBeta, depth / 2 - 1, height+1);

        // Revert board state
        revert(thread, board, move, height);

        // Move failed high, thus ttMove is not singular
        if (value > rBeta) break;
    }

    // Reapply the table move we took off
    apply(thread, board, ttMove, height);

    // Move is singular if all other moves failed low
    return value <= rBeta;
}
