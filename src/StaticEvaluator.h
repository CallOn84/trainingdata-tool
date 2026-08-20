#ifndef STATIC_EVALUATOR_H
#define STATIC_EVALUATOR_H

#include "polyglot_lib.h"
#include "WdlConversion.h"
#include <cstdint>

// Static position evaluator for normal mode (no engine)
// Returns evaluation in centipawns from side-to-move perspective

class StaticEvaluator {
public:
  // Defaults shared with Options::wdl_scale / wdl_spread (PGNGame.h) so a
  // caller that does not thread them through still gets the fitted model.
  static constexpr float kDefaultWdlScale = 1.13f;
  static constexpr float kDefaultWdlSpread = 0.21f;
  // Halfmove clock below which the 50-move rule is ignored entirely.
  static constexpr int kDefaultR50DampStart = 40;

  // Evaluate position, returns centipawns from side-to-move perspective.
  // Knows nothing about the 50-move rule -- use evaluateWDL for a training
  // target.
  static int evaluate(board_t* board);

  // Halfmove clock the position would have after `move` is played. Only two
  // kinds of move reset it -- a pawn move (push or promotion) and a capture
  // (including en passant) -- and everything else extends it by one. Mirrors
  // move_do.cpp exactly; see rule50PlyAfter's definition.
  static int rule50PlyAfter(board_t* board, int move);

  // Full training target: the static score mapped to (Q, D) through the same
  // model the other modes use, then penalised by however far `played_move`
  // leaves the halfmove clock from the 50-move limit. Pass the move actually
  // played so a move that resets the clock is not penalised for the shuffling
  // that preceded it.
  static void evaluateWDL(board_t* board, int played_move, float wdl_scale,
                          float wdl_spread, int r50_damp_start, float& q,
                          float& d);

  // Convert centipawns to win probability in [-1, 1] range
  static float cpToWinProbability(int cp);

private:
  // Material values (centipawns)
  static constexpr int PAWN_VALUE   = 100;
  static constexpr int KNIGHT_VALUE = 320;
  static constexpr int BISHOP_VALUE = 330;
  static constexpr int ROOK_VALUE   = 500;
  static constexpr int QUEEN_VALUE  = 900;
  
  // Bonuses/penalties
  static constexpr int BISHOP_PAIR_BONUS = 50;
  static constexpr int DOUBLED_PAWN_PENALTY = -20;
  static constexpr int ISOLATED_PAWN_PENALTY = -15;
  static constexpr int PASSED_PAWN_BONUS_BASE = 20;
  static constexpr int MOBILITY_BONUS = 4;
  
  // Piece-Square Tables (from white's perspective, index 0 = a1)
  static const int PST_PAWN[64];
  static const int PST_KNIGHT[64];
  static const int PST_BISHOP[64];
  static const int PST_ROOK[64];
  static const int PST_QUEEN[64];
  static const int PST_KING_MG[64];
  static const int PST_KING_EG[64];
  
  static int evaluateMaterial(board_t* board);
  static int evaluatePST(board_t* board, int phase);
  static int evaluatePawnStructure(board_t* board);
  static int evaluateMobility(board_t* board);
  static int getPhase(board_t* board);
};

#endif // STATIC_EVALUATOR_H
