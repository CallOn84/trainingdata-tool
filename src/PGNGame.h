#if !defined(PGN_GAME_H_INCLUDED)
#define PGN_GAME_H_INCLUDED

#include "neural/encoder.h"
#include "neural/network.h"
#include "trainingdata/trainingdata_v6.h"
#include "pgn.h"
#include "polyglot_lib.h"
#include "PGNMoveInfo.h"

class PGNMoveInfo;
class StockfishEvaluator;

struct Options {
  bool verbose = false;
  // Reads the eval already embedded in the PGN's move comments (Fishtest/
  // cutechess-cli's "SCORE/DEPTH TIMEs" format, e.g. "-0.76/18 1.813s") --
  // no engine spawned, no re-search. This used to be lichess_mode, which
  // parsed Lichess's [%eval] annotation instead; repurposed since Fishtest
  // PGNs carry real LTC-depth evals of their own, not Lichess-style ones,
  // and re-evaluating them with -stockfish would throw that away just to
  // recompute something weaker.
  bool pgn_eval_mode = false;
  bool stockfish_mode = false;
  // WDL model for pgn_eval_mode: mu = wdl_scale * eval, then
  // W = logistic((mu-1)/wdl_spread), L = logistic((-mu-1)/wdl_spread),
  // giving Q = W-L and D = 1-W-L together. This is lc0's "WDL_mu" model
  // run backwards -- search.cc reports score = 100*mu, so mu is simply the
  // eval in pawns. See wdl::ScoreToWDL in WdlConversion.h.
  //
  // Defaults are FITTED against the actual outcomes of the games being
  // converted: bucket positions by the eval in their PGN comment, count
  // the real win/draw/loss frequencies, and fit W and L to them.
  // RMSE 0.015 on (W, L) -- see scripts/measure_pgn_wdl.py, which runs
  // exactly this fit and prints the values to use. Stable across Fishtest
  // files: five separate ones fit to scale 1.11-1.27, spread 0.20-0.21.
  //
  // wdl_scale landing near 1.0 is a consistency check, not a coincidence:
  // the model says mu IS the eval, so a large correction would have meant
  // the model was wrong. Do not substitute lc0's "centipawn" score type
  // (cp = 90*tan(1.5637541897*Q)) here -- that is a display convention,
  // not a calibrated win-probability model, and measured against real
  // outcomes it is badly miscalibrated at both ends.
  //
  // Deliberately NOT lc0's WDLDrawRateReference. That parameter describes
  // the net you are *running* (looked up by running it from startpos and
  // reading its WDL), but here we are generating training data, and these
  // are Stockfish games with their own book, time control and adjudication
  // -- a different distribution entirely. Targeting an lc0 net's draw rate
  // would aim at the wrong thing, and would be circular if that net is the
  // one being trained.
  //
  // Caveat: ~86% of Fishtest games end by adjudication, and cutechess
  // adjudicates a draw exactly when the eval sits near zero, so this curve
  // is partly shaped by the adjudication rule rather than pure chess. It
  // is still the real label distribution in the data, and is consistent
  // with result_q/result_d, which come from the same recorded results.
  //
  // Raising wdl_scale sharpens: a given eval maps to a larger mu, so the
  // transition to a decided result happens at a smaller eval. wdl_spread
  // is lc0's scale_reference and follows from the draw rate at an equal
  // position: spread = 1/log((1+r)/(1-r)), equivalently
  // D(equal) = 1 - 2*logistic(-1/spread). Re-fit rather than eyeballing if
  // these change; scripts/measure_pgn_wdl.py fits both.
  float wdl_scale = 1.13f;
  float wdl_spread = 0.21f;
  // Halfmove clock at which static evaluation starts blending its (Q, D)
  // toward a certain draw, reaching a full draw at the 100-ply limit.
  // Static mode only: a real engine's score already accounts for the rule,
  // so -stockfish and -pgn-eval-mode must not apply it a second time.
  // See wdl::ApplyRule50Draw in WdlConversion.h.
  int r50_damp_start = 40;
  // Total pseudo visit count written per position, and the budget the played
  // move's policy share is drawn from. 0 disables it: visits stays 1 and the
  // policy target stays one-hot, which is the historical behaviour. A PGN
  // contains no search, so any value here is reconstructed from the
  // evaluation rather than measured -- see the call site in PGNGame.cpp.
  int visit_budget = 0;
};

struct PGNGame {
  char result[PGN_STRING_SIZE];
  char fen[PGN_STRING_SIZE];
  std::vector<PGNMoveInfo> moves;

  PGNGame() {
    result[0] = '\0';
    fen[0] = '\0';
  }
  explicit PGNGame(pgn_t* pgn);
  std::vector<lczero::V6TrainingData> getChunks(Options options,
                                                 StockfishEvaluator* evaluator = nullptr,
                                                 int sf_depth = 10) const;
};

#endif
