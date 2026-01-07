#if !defined(PGN_GAME_H_INCLUDED)
#define PGN_GAME_H_INCLUDED

#include "neural/encoder.h"
#include "neural/network.h"
#include "trainingdata/trainingdata_v6.h"
#include "pgn.h"
#include "polyglot_lib.h"
#include "PGNMoveInfo.h"

class PGNMoveInfo;

struct Options {
  bool verbose = false;
  bool lichess_mode = false;
};

struct PGNGame {
  char result[PGN_STRING_SIZE];
  char fen[PGN_STRING_SIZE];
  std::vector<PGNMoveInfo> moves;

  explicit PGNGame(pgn_t* pgn);
  std::vector<lczero::V6TrainingData> getChunks(Options options) const;
};

#endif
