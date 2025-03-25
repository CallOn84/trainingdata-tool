#include "trainingdata.h"

#include <cstring>
#include <tuple>
#include <algorithm>
#include <limits>
#include <iostream>

uint64_t ReverseBitsInBytes(uint64_t v) {
  v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
  v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
  v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
  return v;
}

// DriftCorrect ensures that q remains in [-1,1] and d remains in [0,1]
std::tuple<float, float> DriftCorrect(float q, float d) {
  const float allowed_eps = 0.000001f;
  if (q > 1.0f) {
    if (q > 1.0f + allowed_eps) {
      std::cerr << "Unexpectedly large drift in q " << q;
    }
    q = 1.0f;
  }
  if (q < -1.0f) {
    if (q < -1.0f - allowed_eps) {
      std::cerr << "Unexpectedly large drift in q " << q;
    }
    q = -1.0f;
  }
  if (d > 1.0f) {
    if (d > 1.0f + allowed_eps) {
      std::cerr << "Unexpectedly large drift in d " << d;
    }
    d = 1.0f;
  }
  if (d < 0.0f) {
    if (d < -allowed_eps) {
      std::cerr << "Unexpectedly large drift in d " << d;
    }
    d = 0.0f;
  }
  float w = (1.0f - d + q) / 2.0f;
  float l = w - q;
  if (w < 0.0f || l < 0.0f) {
    float drift = 2.0f * std::min(w, l);
    if (drift < -allowed_eps) {
      std::cerr << "Unexpectedly large drift correction for d based on q. " << drift;
    }
    d += drift;
    if (d < 0.0f) {
      d = 0.0f;
    }
  }
  return {q, d};
}

lczero::V6TrainingData get_v6_training_data(
        const lczero::PositionHistory& history,
        lczero::GameResult game_result,
        lczero::Move best_move,
        lczero::Move played_move,
        lczero::MoveList legal_moves,
        float Q,
        bool lichess_mode) { // new parameter to indicate lichess mode
  lczero::V6TrainingData result;
  const auto& position = history.Last();

  // Set version.
  result.version = 6;
  result.input_format = input_format_;

  // Populate planes.
  int transform;
  lczero::InputPlanes planes = EncodePositionForNN(
    input_format_, history, 8, lczero::FillEmptyHistory::FEN_ONLY,
    &transform);
  int plane_idx = 0;
  for (auto& plane : result.planes) {
    plane = ReverseBitsInBytes(planes[plane_idx++].mask);
  }

  // Populate probabilities.
  for (auto& probability : result.probabilities) {
    probability = -1;
  }
  for (lczero::Move move : legal_moves) {
    result.probabilities[move.as_nn_index()] = 0;
  }
  result.probabilities[played_move.as_nn_index()] = 1.0f;

  // Populate castlings.
  if (Is960CastlingFormat(input_format_)) {
    const auto& castlings = position.GetBoard().castlings();
    // For FRC, calculate bit masks representing the rook positions.
    uint8_t our_queen_side = 1;
    uint8_t our_king_side = 1;
    uint8_t their_queen_side = 1;
    uint8_t their_king_side = 1;

    our_queen_side <<= castlings.our_queenside_rook();
    our_king_side  <<= castlings.our_kingside_rook();
    their_queen_side <<= castlings.their_queenside_rook();
    their_king_side  <<= castlings.their_kingside_rook();

    result.castling_us_ooo = castlings.we_can_000() ? our_queen_side : 0;
    result.castling_us_oo  = castlings.we_can_00() ? our_king_side : 0;
    result.castling_them_ooo = castlings.they_can_000() ? their_queen_side : 0;
    result.castling_them_oo  = castlings.they_can_00() ? their_king_side : 0;
  } else {
    // Standard chess: simply use binary flags.
    result.castling_us_ooo = position.CanCastle(lczero::Position::WE_CAN_OOO) ? 1 : 0;
    result.castling_us_oo = position.CanCastle(lczero::Position::WE_CAN_OO) ? 1 : 0;
    result.castling_them_ooo = position.CanCastle(lczero::Position::THEY_CAN_OOO) ? 1 : 0;
    result.castling_them_oo = position.CanCastle(lczero::Position::THEY_CAN_OO) ? 1 : 0;
  }

  // Other params.
  if (IsCanonicalFormat(input_format_)) {
    result.side_to_move_or_enpassant =
      position.GetBoard().en_passant().as_int() >> 56;
    if ((transform & FlipTransform) != 0) {
      result.side_to_move_or_enpassant =
        ReverseBitsInBytes(result.side_to_move_or_enpassant);
    }
    result.invariance_info = transform | (position.IsBlackToMove() ? (1u << 7) : 0u);
  } else {
    result.side_to_move_or_enpassant = position.IsBlackToMove() ? 1 : 0;
    result.invariance_info = 0;
  }

  result.dummy = 0;
  result.rule50_count = position.GetRule50Ply();

  // Game result.
  if (game_result == lczero::GameResult::WHITE_WON) { // use game_result parameter here.
    result.result_q = position.IsBlackToMove() ? -1.0f : 1.0f;
    result.result_d = 0.0f;
  } else if (game_result == lczero::GameResult::BLACK_WON) {
    result.result_q = position.IsBlackToMove() ? 1.0f : -1.0f;
    result.result_d = 0.0f;
  } else {
    result.result_q = 0.0f;
    result.result_d = 1.0f;
  }

  // Q for Q+Z training.
  result.root_q = result.best_q = position.IsBlackToMove() ? -Q : Q;
  result.root_d = result.best_d = 0.0f;
  
  // For played and original evaluations, default to the same values.
  result.played_q = result.orig_q = result.root_q;
  
  // Move measures: set to default.
  result.root_m = result.best_m = result.played_m = result.orig_m = 0.0f;
  
  // Search statistics: set to default values.
  result.visits = 0;
  result.played_idx = 0;
  result.best_idx = 0;
  result.policy_kld = 0.0f;
  
  // Reserved field and plies left.
  result.reserved = 0;
  result.plies_left = 0;
  
  // Apply drift correction only when -lichess-mode is active.
  if (lichess_mode) {
    std::tie(result.root_q, result.root_d) = DriftCorrect(result.root_q, result.root_d);
    std::tie(result.best_q, result.best_d) = DriftCorrect(result.best_q, result.best_d);
    std::tie(result.played_q, result.played_d) = DriftCorrect(result.played_q, result.played_d);
    std::tie(result.orig_q, result.orig_d) = DriftCorrect(result.orig_q, result.orig_d);
  }
  
  return result;
}
