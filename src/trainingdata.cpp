#include "trainingdata.h"
#include "utils/bititer.h"

#include <algorithm>
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
    lczero::Move best_move, uint32_t visits) {
  lczero::V6TrainingData result;
  std::memset(&result, 0, sizeof(result));

  result.version = 6;
  // Use Classical 112 plane format
  auto input_format = pblczero::NetworkFormat::INPUT_CLASSICAL_112_PLANE;
  result.input_format = input_format;

  // Initialize probabilities to -1 (illegal)
  for (auto& probability : result.probabilities) {
    probability = -1.0f;
  }

  // Legal moves to 0
  for (lczero::Move move : legal_moves) {
    uint16_t idx = lczero::MoveToNNIndex(move, 0);
    if (idx < 1858) {
      result.probabilities[idx] = 0.0f;
    }
  }

  // Played move to 1 (with bounds check to prevent crash from invalid moves)
  uint16_t played_idx = lczero::MoveToNNIndex(played_move, 0);
  if (played_idx < 1858) {
    result.probabilities[played_idx] = 1.0f;
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

  const auto& position = history.Last();
  const auto& castlings = position.GetBoard().castlings();

  // Populate castlings
  result.castling_us_ooo = castlings.we_can_000() ? 1 : 0;
  result.castling_us_oo = castlings.we_can_00() ? 1 : 0;
  result.castling_them_ooo = castlings.they_can_000() ? 1 : 0;
  result.castling_them_oo = castlings.they_can_00() ? 1 : 0;

  // Side to move and enpassant
  result.side_to_move_or_enpassant = 0;
  if (position.IsBlackToMove()) {
    result.side_to_move_or_enpassant = 1;
  }

  result.invariance_info = 0;

  result.rule50_count = position.GetRule50Ply();

  // Result
  float res_q = 0.0f;
  float res_d = 1.0f; // Default to draw
  if (game_result == lczero::GameResult::WHITE_WON) {
    res_q = position.IsBlackToMove() ? -1.0f : 1.0f;
    res_d = 0.0f;
  } else if (game_result == lczero::GameResult::BLACK_WON) {
    res_q = position.IsBlackToMove() ? 1.0f : -1.0f;
    res_d = 0.0f;
  }
  result.result_q = res_q;
  result.result_d = res_d;

  // Q values - store directly (relative to side-to-move)
  result.root_q = result.best_q = Q;
  result.root_d = result.best_d = 0.0f; // Static eval doesn't provide D

  // Set played move stats
  result.played_q = Q;
  result.played_d = 0.0f;
  result.played_m = 0.0f;
  result.root_m = 0.0f;
  result.best_m = 0.0f;

  // Set visits
  result.visits = visits;

  // Set policy KLD to neutral
  result.policy_kld = 0.0f;

  // Use the already-validated played_idx (or 0 if invalid)
  result.played_idx = (played_idx < 1858) ? played_idx : 0;

  // best_idx with bounds check
  uint16_t best_idx = lczero::MoveToNNIndex(best_move, 0);
  result.best_idx = (best_idx < 1858) ? best_idx : result.played_idx;

  return result;
}
