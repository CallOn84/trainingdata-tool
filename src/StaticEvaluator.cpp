#include "StaticEvaluator.h"
#include <cmath>
#include <algorithm>

// Piece-Square Tables (from white's perspective, index 0 = a1, index 63 = h8)
// Values in centipawns, positive = good for white

// Pawns: encourage central control and advancement
const int StaticEvaluator::PST_PAWN[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
  50, 50, 50, 50, 50, 50, 50, 50,
  10, 10, 20, 30, 30, 20, 10, 10,
   5,  5, 10, 25, 25, 10,  5,  5,
   0,  0,  0, 20, 20,  0,  0,  0,
   5, -5,-10,  0,  0,-10, -5,  5,
   5, 10, 10,-20,-20, 10, 10,  5,
   0,  0,  0,  0,  0,  0,  0,  0
};

// Knights: strong in center, weak on edges
const int StaticEvaluator::PST_KNIGHT[64] = {
 -50,-40,-30,-30,-30,-30,-40,-50,
 -40,-20,  0,  0,  0,  0,-20,-40,
 -30,  0, 10, 15, 15, 10,  0,-30,
 -30,  5, 15, 20, 20, 15,  5,-30,
 -30,  0, 15, 20, 20, 15,  0,-30,
 -30,  5, 10, 15, 15, 10,  5,-30,
 -40,-20,  0,  5,  5,  0,-20,-40,
 -50,-40,-30,-30,-30,-30,-40,-50
};

// Bishops: encourage diagonals and activity
const int StaticEvaluator::PST_BISHOP[64] = {
 -20,-10,-10,-10,-10,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5, 10, 10,  5,  0,-10,
 -10,  5,  5, 10, 10,  5,  5,-10,
 -10,  0, 10, 10, 10, 10,  0,-10,
 -10, 10, 10, 10, 10, 10, 10,-10,
 -10,  5,  0,  0,  0,  0,  5,-10,
 -20,-10,-10,-10,-10,-10,-10,-20
};

// Rooks: encourage 7th rank and open files
const int StaticEvaluator::PST_ROOK[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
   5, 10, 10, 10, 10, 10, 10,  5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
   0,  0,  0,  5,  5,  0,  0,  0
};

// Queen: slight central preference
const int StaticEvaluator::PST_QUEEN[64] = {
 -20,-10,-10, -5, -5,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5,  5,  5,  5,  0,-10,
  -5,  0,  5,  5,  5,  5,  0, -5,
   0,  0,  5,  5,  5,  5,  0, -5,
 -10,  5,  5,  5,  5,  5,  0,-10,
 -10,  0,  5,  0,  0,  0,  0,-10,
 -20,-10,-10, -5, -5,-10,-10,-20
};

// King middlegame: encourage castling, stay safe
const int StaticEvaluator::PST_KING_MG[64] = {
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -20,-30,-30,-40,-40,-30,-30,-20,
 -10,-20,-20,-20,-20,-20,-20,-10,
  20, 20,  0,  0,  0,  0, 20, 20,
  20, 30, 10,  0,  0, 10, 30, 20
};

// King endgame: centralize
const int StaticEvaluator::PST_KING_EG[64] = {
 -50,-40,-30,-20,-20,-30,-40,-50,
 -30,-20,-10,  0,  0,-10,-20,-30,
 -30,-10, 20, 30, 30, 20,-10,-30,
 -30,-10, 30, 40, 40, 30,-10,-30,
 -30,-10, 30, 40, 40, 30,-10,-30,
 -30,-10, 20, 30, 30, 20,-10,-30,
 -30,-30,  0,  0,  0,  0,-30,-30,
 -50,-30,-30,-30,-30,-30,-30,-50
};

float StaticEvaluator::cpToWinProbability(int cp) {
  // Sigmoid function to convert cp to [-1, 1]
  // Using standard Stockfish normalization
  return 2.0f / (1.0f + std::exp(-0.004f * cp)) - 1.0f;
}

int StaticEvaluator::getPhase(board_t* board) {
  // Phase: 24 = opening, 0 = endgame
  // Each minor = 1, each rook = 2, each queen = 4
  int phase = 0;
  
  for (int sq = 0; sq < 64; sq++) {
    int piece = board->square[sq];
    if (piece == WhiteKnight12 || piece == BlackKnight12) phase += 1;
    if (piece == WhiteBishop12 || piece == BlackBishop12) phase += 1;
    if (piece == WhiteRook12 || piece == BlackRook12) phase += 2;
    if (piece == WhiteQueen12 || piece == BlackQueen12) phase += 4;
  }
  
  return std::min(phase, 24);
}

int StaticEvaluator::evaluateMaterial(board_t* board) {
  int score = 0;
  int whiteBishops = 0, blackBishops = 0;
  
  for (int sq = 0; sq < 64; sq++) {
    int piece = board->square[sq];
    switch (piece) {
      case WhitePawn12:   score += PAWN_VALUE; break;
      case BlackPawn12:   score -= PAWN_VALUE; break;
      case WhiteKnight12: score += KNIGHT_VALUE; break;
      case BlackKnight12: score -= KNIGHT_VALUE; break;
      case WhiteBishop12: score += BISHOP_VALUE; whiteBishops++; break;
      case BlackBishop12: score -= BISHOP_VALUE; blackBishops++; break;
      case WhiteRook12:   score += ROOK_VALUE; break;
      case BlackRook12:   score -= ROOK_VALUE; break;
      case WhiteQueen12:  score += QUEEN_VALUE; break;
      case BlackQueen12:  score -= QUEEN_VALUE; break;
    }
  }
  
  // Bishop pair bonus
  if (whiteBishops >= 2) score += BISHOP_PAIR_BONUS;
  if (blackBishops >= 2) score -= BISHOP_PAIR_BONUS;
  
  return score;
}

int StaticEvaluator::evaluatePST(board_t* board, int phase) {
  int scoreMG = 0, scoreEG = 0;
  
  for (int sq = 0; sq < 64; sq++) {
    int piece = board->square[sq];
    int whiteSq = sq;           // For white pieces
    int blackSq = sq ^ 56;      // Flip for black (mirror vertically)
    
    switch (piece) {
      case WhitePawn12:
        scoreMG += PST_PAWN[whiteSq];
        scoreEG += PST_PAWN[whiteSq];
        break;
      case BlackPawn12:
        scoreMG -= PST_PAWN[blackSq];
        scoreEG -= PST_PAWN[blackSq];
        break;
      case WhiteKnight12:
        scoreMG += PST_KNIGHT[whiteSq];
        scoreEG += PST_KNIGHT[whiteSq];
        break;
      case BlackKnight12:
        scoreMG -= PST_KNIGHT[blackSq];
        scoreEG -= PST_KNIGHT[blackSq];
        break;
      case WhiteBishop12:
        scoreMG += PST_BISHOP[whiteSq];
        scoreEG += PST_BISHOP[whiteSq];
        break;
      case BlackBishop12:
        scoreMG -= PST_BISHOP[blackSq];
        scoreEG -= PST_BISHOP[blackSq];
        break;
      case WhiteRook12:
        scoreMG += PST_ROOK[whiteSq];
        scoreEG += PST_ROOK[whiteSq];
        break;
      case BlackRook12:
        scoreMG -= PST_ROOK[blackSq];
        scoreEG -= PST_ROOK[blackSq];
        break;
      case WhiteQueen12:
        scoreMG += PST_QUEEN[whiteSq];
        scoreEG += PST_QUEEN[whiteSq];
        break;
      case BlackQueen12:
        scoreMG -= PST_QUEEN[blackSq];
        scoreEG -= PST_QUEEN[blackSq];
        break;
      case WhiteKing12:
        scoreMG += PST_KING_MG[whiteSq];
        scoreEG += PST_KING_EG[whiteSq];
        break;
      case BlackKing12:
        scoreMG -= PST_KING_MG[blackSq];
        scoreEG -= PST_KING_EG[blackSq];
        break;
    }
  }
  
  // Tapered evaluation
  int mgWeight = phase;
  int egWeight = 24 - phase;
  return (scoreMG * mgWeight + scoreEG * egWeight) / 24;
}

int StaticEvaluator::evaluatePawnStructure(board_t* board) {
  int score = 0;
  
  // Count pawns per file
  int whitePawnsPerFile[8] = {0};
  int blackPawnsPerFile[8] = {0};
  int whitePawnRanks[8] = {0};  // Most advanced white pawn per file
  int blackPawnRanks[8] = {7};  // Most advanced black pawn per file
  
  for (int sq = 0; sq < 64; sq++) {
    int file = sq % 8;
    int rank = sq / 8;
    int piece = board->square[sq];
    
    if (piece == WhitePawn12) {
      whitePawnsPerFile[file]++;
      whitePawnRanks[file] = std::max(whitePawnRanks[file], rank);
    } else if (piece == BlackPawn12) {
      blackPawnsPerFile[file]++;
      blackPawnRanks[file] = std::min(blackPawnRanks[file], rank);
    }
  }
  
  for (int file = 0; file < 8; file++) {
    // Doubled pawns
    if (whitePawnsPerFile[file] > 1) {
      score += DOUBLED_PAWN_PENALTY * (whitePawnsPerFile[file] - 1);
    }
    if (blackPawnsPerFile[file] > 1) {
      score -= DOUBLED_PAWN_PENALTY * (blackPawnsPerFile[file] - 1);
    }
    
    // Isolated pawns
    bool whiteHasNeighbor = (file > 0 && whitePawnsPerFile[file-1] > 0) ||
                            (file < 7 && whitePawnsPerFile[file+1] > 0);
    bool blackHasNeighbor = (file > 0 && blackPawnsPerFile[file-1] > 0) ||
                            (file < 7 && blackPawnsPerFile[file+1] > 0);
    
    if (whitePawnsPerFile[file] > 0 && !whiteHasNeighbor) {
      score += ISOLATED_PAWN_PENALTY;
    }
    if (blackPawnsPerFile[file] > 0 && !blackHasNeighbor) {
      score -= ISOLATED_PAWN_PENALTY;
    }
    
    // Passed pawns (no enemy pawns on same or adjacent files ahead)
    if (whitePawnsPerFile[file] > 0) {
      bool passed = true;
      for (int f = std::max(0, file-1); f <= std::min(7, file+1); f++) {
        if (blackPawnRanks[f] > whitePawnRanks[file]) {
          passed = false;
          break;
        }
      }
      if (passed) {
        // Bonus based on how advanced
        score += PASSED_PAWN_BONUS_BASE + (whitePawnRanks[file] - 1) * 10;
      }
    }
    
    if (blackPawnsPerFile[file] > 0) {
      bool passed = true;
      for (int f = std::max(0, file-1); f <= std::min(7, file+1); f++) {
        if (whitePawnRanks[f] < blackPawnRanks[file]) {
          passed = false;
          break;
        }
      }
      if (passed) {
        // Bonus based on how advanced (from black's perspective)
        score -= PASSED_PAWN_BONUS_BASE + (6 - blackPawnRanks[file]) * 10;
      }
    }
  }
  
  return score;
}

int StaticEvaluator::evaluateMobility(board_t* board) {
  // Simplified mobility: count legal moves
  // This is a rough approximation - just count attack squares
  int score = 0;
  
  // Count piece mobility (simplified - just based on piece presence)
  for (int sq = 0; sq < 64; sq++) {
    int piece = board->square[sq];
    int file = sq % 8;
    int rank = sq / 8;
    
    // Knights: up to 8 moves, bonus for central position
    if (piece == WhiteKnight12) {
      int mobility = 8;
      if (file == 0 || file == 7) mobility -= 2;
      if (rank == 0 || rank == 7) mobility -= 2;
      score += mobility * MOBILITY_BONUS / 2;
    } else if (piece == BlackKnight12) {
      int mobility = 8;
      if (file == 0 || file == 7) mobility -= 2;
      if (rank == 0 || rank == 7) mobility -= 2;
      score -= mobility * MOBILITY_BONUS / 2;
    }
    
    // Bishops: bonus for open diagonals (simplified)
    if (piece == WhiteBishop12) {
      score += 5 * MOBILITY_BONUS / 2;
    } else if (piece == BlackBishop12) {
      score -= 5 * MOBILITY_BONUS / 2;
    }
    
    // Rooks: bonus for open files (simplified)
    if (piece == WhiteRook12) {
      score += 4 * MOBILITY_BONUS / 2;
    } else if (piece == BlackRook12) {
      score -= 4 * MOBILITY_BONUS / 2;
    }
    
    // Queens: large mobility bonus
    if (piece == WhiteQueen12) {
      score += 8 * MOBILITY_BONUS / 2;
    } else if (piece == BlackQueen12) {
      score -= 8 * MOBILITY_BONUS / 2;
    }
  }
  
  return score;
}

int StaticEvaluator::evaluate(board_t* board) {
  int phase = getPhase(board);
  
  int score = 0;
  score += evaluateMaterial(board);
  score += evaluatePST(board, phase);
  score += evaluatePawnStructure(board);
  score += evaluateMobility(board);
  
  // Return from side-to-move perspective
  return colour_is_white(board->turn) ? score : -score;
}
