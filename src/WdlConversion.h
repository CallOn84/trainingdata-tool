#if !defined(WDL_CONVERSION_H_INCLUDED)
#define WDL_CONVERSION_H_INCLUDED

#include <algorithm>
#include <cmath>

// Single source of truth for turning an engine's scalar centipawn score
// into lc0's (Q, D) pair. Both evaluation paths use this so they can never
// drift apart: -pgn-eval-mode (reading evals out of PGN comments) and
// -stockfish (running the engine live) must map the same score to the same
// Q, or the two modes would produce inconsistent training data.

namespace wdl {

// Score -> (W, D, L), using lc0's own WDL model.
//
// This is the "WDL_mu" score type from search/classic/search.cc, run
// backwards. That code reports
//
//     uci_info.score = 100 * mu_uci
//
// i.e. mu IS the eval in pawns (the UCI score is just mu in centipawns).
// So going from a score back to a distribution needs no inversion of any
// display formula -- feed the eval in as mu and evaluate the same logistic
// pair WDLRescale() reconstructs with:
//
//     W = logistic((mu - 1) / s)      L = logistic((-mu - 1) / s)
//     Q = W - L                       D = 1 - W - L
//
// Do NOT use the "centipawn" score type (cp = 90*tan(1.5637541897*Q)) for
// this. That is a *display* convention for rendering Q as a
// centipawn-looking number, not a calibrated win-probability model.
// Measured against real game outcomes it is badly off in both directions:
// at +0.37 pawns it claims Q=+0.25 where the true figure is +0.02, and at
// +2.45 pawns it claims +0.78 where the truth is +1.00.
// (scripts/measure_pgn_wdl.py reproduces that comparison.)
//
// `scale` and `spread` are fitted against the real outcomes of the games
// being converted -- see Options::wdl_scale in PGNGame.h. `scale` lands
// near 1.0 as the model implies; `spread` is lc0's scale_reference and
// follows from the draw rate.
inline void ScoreToWDL(float score_pawns, float scale, float spread,
                       float& q, float& d) {
  const float mu = scale * score_pawns;
  const float w = 1.0f / (1.0f + std::exp(-(mu - 1.0f) / spread));
  const float l = 1.0f / (1.0f + std::exp(-(-mu - 1.0f) / spread));
  q = w - l;
  // W and L already sum to <= 1 by construction, so D needs no clamping
  // against |Q| the way it would if Q came from an unrelated formula.
  d = (std::max)(0.0f, 1.0f - w - l);
}

// Convenience wrapper for callers holding integer centipawns.
inline float CentipawnToQ(int centipawns, float scale, float spread) {
  float q, d;
  ScoreToWDL(static_cast<float>(centipawns) / 100.0f, scale, spread, q, d);
  return q;
}

// How certain a 50-move draw is, given a halfmove clock.
//
// Callers pass the clock the *played move leaves behind*, so that the two
// move types which reset it -- pawn moves and captures -- are scored at full
// value, and only moves that extend it are decremented.
//
// 0 while the clock is below `damp_start`, rising linearly to 1 at 100 plies
// -- the point at which the game IS drawn, whatever is on the board. The
// dead zone exists because a moderate clock carries no information: 20-odd
// plies of maneuvering is ordinary endgame play, not shuffling, and damping
// from ply 1 would bias every long endgame toward a draw.
inline float Rule50DrawCertainty(int halfmove_clock, int damp_start) {
  constexpr int kFiftyMovePlies = 100;  // game.cpp: ply_nb >= 100 -> DRAW_FIFTY
  if (damp_start >= kFiftyMovePlies) return 0.0f;
  if (halfmove_clock <= damp_start) return 0.0f;
  if (halfmove_clock >= kFiftyMovePlies) return 1.0f;
  return static_cast<float>(halfmove_clock - damp_start) /
         static_cast<float>(kFiftyMovePlies - damp_start);
}

// Blend a (Q, D) toward a certain draw as the halfmove clock runs out.
//
// This is a straight interpolation in W/D/L space,
//
//     (W, D, L)_out = (1-c)*(W, D, L)_in + c*(0, 1, 0)
//
// which reduces to the two lines below and keeps the triple on the simplex.
// At c=1 the result is exactly Q=0, D=1, matching the actual rule.
//
// Damping Q alone would be worse than doing nothing: it would produce
// Q->0 with D unchanged, i.e. "certain, and equally likely to be won or
// lost" -- the opposite of the drawn position it is meant to describe.
//
// Only for evaluations that do not already model the rule. A real engine's
// score does (Stockfish damps its own eval by the halfmove clock, and its
// search sees the terminal draw outright), so applying this on top of
// -stockfish or -pgn-eval-mode would double-count it.
inline void ApplyRule50Draw(int halfmove_clock, int damp_start, float& q,
                            float& d) {
  const float c = Rule50DrawCertainty(halfmove_clock, damp_start);
  if (c <= 0.0f) return;
  q *= (1.0f - c);
  d += (1.0f - d) * c;
}

}  // namespace wdl

#endif
