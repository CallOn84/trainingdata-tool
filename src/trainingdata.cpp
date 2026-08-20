#include "trainingdata.h"
#include "utils/bititer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace lczero {
// Remove ambiguous forward declaration
}

// Minimal implementation if not linked (it should be linked from lc0 utils, but
// to be safe) Actually lc0 has it in utils/bitmanip.h -> utils/bititer.h

lczero::V6TrainingData get_v6_training_data(
    lczero::GameResult game_result, const lczero::PositionHistory& history,
    lczero::Move played_move, lczero::MoveList legal_moves, float Q,
    lczero::Move best_move, uint32_t visits, int plies_left, float D,
    float played_policy_share) {
  lczero::V6TrainingData result;
  std::memset(&result, 0, sizeof(result));

  result.version = 6;
  // User requested INPUT_CLASSICAL_112_PLANE
  auto input_format = pblczero::NetworkFormat::INPUT_CLASSICAL_112_PLANE;
  result.input_format = input_format;

  // Initialize probabilities to -1 (illegal)
  for (auto& probability : result.probabilities) {
    probability = -1.0f;
  }

  // A PGN gives us one move and no alternatives, so the policy target is
  // built from the played move alone. played_policy_share == 1.0 is the plain
  // one-hot target; a smaller share spreads the remainder evenly over the
  // other legal moves, which is the closest thing to a visit distribution we
  // can reconstruct without a search.
  size_t legal_count = 0;
  for (lczero::Move move : legal_moves) {
    if (lczero::MoveToNNIndex(move, 0) < 1858) ++legal_count;
  }
  const float share = (legal_count > 1) ? played_policy_share : 1.0f;
  const float other_share =
      (legal_count > 1) ? (1.0f - share) / (legal_count - 1) : 0.0f;

  for (lczero::Move move : legal_moves) {
    uint16_t idx = lczero::MoveToNNIndex(move, 0);
    if (idx < 1858) {
      result.probabilities[idx] = other_share;
    }
  }

  const auto& position = history.Last();

  // Played move takes its share (with bounds check to prevent crash from
  // invalid moves)
  uint16_t played_idx = lczero::MoveToNNIndex(played_move, 0);
  if (played_idx < 1858) {
    result.probabilities[played_idx] = share;
  } else {
// Invalid move - this shouldn't happen but prevents crash
// Log warning in debug builds
#ifndef NDEBUG
    std::cerr << "Warning: Invalid played_move index " << played_idx
              << " (max 1857)" << std::endl;
#endif
  }

  // Populate planes
  int transform = 0;
  lczero::InputPlanes planes = lczero::EncodePositionForNN(
      input_format, history, 8, lczero::FillEmptyHistory::FEN_ONLY, &transform);

  // V6 stores first 104 planes (8 history * 13 planes)
  for (size_t i = 0; i < 104 && i < planes.size(); ++i) {
    result.planes[i] = lczero::ReverseBitsInBytes(planes[i].mask);
  }

  const auto& castlings = position.GetBoard().castlings();

  // Populate castlings
  result.castling_us_ooo = castlings.we_can_000() ? 1 : 0;
  result.castling_us_oo = castlings.we_can_00() ? 1 : 0;
  result.castling_them_ooo = castlings.they_can_000() ? 1 : 0;
  result.castling_them_oo = castlings.they_can_00() ? 1 : 0;

  // Side to move and enpassant (For Classical, it is 0 or 1 for side to move)
  result.side_to_move_or_enpassant = 0;
  if (position.IsBlackToMove()) {
    result.side_to_move_or_enpassant = 1;
  }

  // Invariance info (0 for Classical)
  result.invariance_info = 0;

  result.rule50_count = position.GetRule50Ply();

  // Result Q and D
  float res_q = 0.0f;
  float res_d = 0.0f;
  if (game_result == lczero::GameResult::WHITE_WON) {
    res_q = position.IsBlackToMove() ? -1.0f : 1.0f;
    res_d = 0.0f;
  } else if (game_result == lczero::GameResult::BLACK_WON) {
    res_q = position.IsBlackToMove() ? 1.0f : -1.0f;
    res_d = 0.0f;
  } else {
    // Draw
    res_q = 0.0f;
    res_d = 1.0f;
  }
  result.result_q = res_q;
  result.result_d = res_d;

  // Q values (relative to side-to-move)
  result.root_q = result.best_q = Q;

  // D values (draw probability) - from Stockfish WDL when available
  result.root_d = result.best_d = D;

  // M values (moves left estimate from engine) - placeholder
  result.root_m = result.best_m = static_cast<float>(plies_left);

  // plies_left is the MLH training target
  result.plies_left = static_cast<float>(plies_left);

  // Played move values (set same as root for supervised)
  result.played_q = Q;
  result.played_d = D;
  result.played_m = static_cast<float>(plies_left);

  // Orig values (for value repair) - set to NaN as we don't have cache
  result.orig_q = std::nanf("");
  result.orig_d = std::nanf("");
  result.orig_m = std::nanf("");

  // Set visits
  result.visits = visits;

  // Use the already-validated played_idx (or 0 if invalid)
  result.played_idx = (played_idx < 1858) ? played_idx : 0;

  // best_idx with bounds check
  uint16_t best_idx = lczero::MoveToNNIndex(best_move, 0);
  result.best_idx = (best_idx < 1858) ? best_idx : result.played_idx;

  // Policy KLD - not applicable for supervised data
  result.policy_kld = 0.0f;

  // Reserved
  result.reserved = 0;

  return result;
}
