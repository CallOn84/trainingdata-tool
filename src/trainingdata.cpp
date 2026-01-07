#include "trainingdata.h"
#include "utils/bititer.h"

#include <cstring>
#include <algorithm>

namespace lczero {
// Remove ambiguous forward declaration
}

// Minimal implementation if not linked (it should be linked from lc0 utils, but to be safe)
// Actually lc0 has it in utils/bitmanip.h -> utils/bititer.h

lczero::V6TrainingData get_v6_training_data(
        lczero::GameResult game_result, const lczero::PositionHistory& history,
        lczero::Move played_move, lczero::MoveList legal_moves, float Q) {
  lczero::V6TrainingData result;
  std::memset(&result, 0, sizeof(result));

  result.version = 6;
  // Use Hectoplies format as it is common
  auto input_format = pblczero::NetworkFormat::INPUT_112_WITH_CANONICALIZATION_HECTOPLIES;
  result.input_format = input_format;

  // Initialize probabilities to -1 (illegal)
  for (auto& probability : result.probabilities) {
    probability = -1.0f;
  }

  // Legal moves to 0
  for (lczero::Move move : legal_moves) {
    result.probabilities[lczero::MoveToNNIndex(move, 0)] = 0.0f;
  }

  // Played move to 1
  result.probabilities[lczero::MoveToNNIndex(played_move, 0)] = 1.0f;

  // Populate planes
  int transform = 0;
  lczero::InputPlanes planes = lczero::EncodePositionForNN(
      input_format, history, 8, lczero::FillEmptyHistory::FEN_ONLY, &transform);

  // V6 stores first 104 planes (8 history * 13 planes)
  for (size_t i = 0; i < 104 && i < planes.size(); ++i) {
    result.planes[i] = planes[i].mask;
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
  if (!position.GetBoard().en_passant().empty()) {
      // GetLowestBit returns unsigned long, cast to int for modulus
      int idx = static_cast<int>(lczero::GetLowestBit(position.GetBoard().en_passant().as_int()));
      int file = idx % 8;
      result.side_to_move_or_enpassant = (1 << file);
  }

  result.invariance_info = 0;
  if (position.IsBlackToMove()) {
     result.invariance_info |= (1 << 7);
  }
  result.invariance_info |= (transform & 0x7);

  result.rule50_count = position.GetRule50Ply();

  // Result
  float res_q = 0.0f;
  if (game_result == lczero::GameResult::WHITE_WON) {
    res_q = position.IsBlackToMove() ? -1.0f : 1.0f;
  } else if (game_result == lczero::GameResult::BLACK_WON) {
    res_q = position.IsBlackToMove() ? 1.0f : -1.0f;
  }
  result.result_q = res_q;
  
  // Q values
  result.root_q = result.best_q = position.IsBlackToMove() ? -Q : Q;
  
  // Set visits to 1 to avoid division by zero or empty checks in training
  result.visits = 1;
  
  result.played_idx = lczero::MoveToNNIndex(played_move, 0);
  result.best_idx = lczero::MoveToNNIndex(played_move, 0);

  return result;
}
