#include "PGNGame.h"
#include "StaticEvaluator.h"
#include "StockfishEvaluator.h"
#include "trainingdata.h"
#include "WdlConversion.h"
#include "utils/fastmath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <regex>

#include <sstream>
#include <vector>

// Everything below ports lc0's own WDL reconstruction machinery from
// lc0/src/search/classic/{search.cc,params.{h,cc}}, run in the direction
// those files never need: from a bare eval number back to (Q, D), rather
// than from an already-known (Q, D) to a UCI display number.

struct WDLRescaleParams {
  float ratio;
  float diff;
};

// Ports AccurateWDLRescaleParams() (search/classic/params.cc) verbatim.
// Converts contempt/draw-rate/book-bias settings into the (ratio, diff)
// WDLRescale() applies. This is the variant lc0 itself selects by default
// (kWDLCalibrationElo == 0 in params.cc's constructor) -- the alternative,
// SimplifiedWDLRescaleParams(), instead expects real Elo estimates for both
// sides, which we have no use for here.
//
// At the args passed below -- lc0's own defaults for a neutral, no-contempt
// setup (kContempt=0, kWDLDrawRateTarget=0 i.e. "use reference",
// kWDLDrawRateReference=0.5, kWDLBookExitBias=0.65, kContemptMaxValue=420,
// kWDLContemptAttenuation=1.0) -- this comes out to a pure identity
// (ratio=1, diff=0): contempt=0 zeroes out diff entirely (it's a factor at
// the end of the expression), and draw_rate_target=0 collapses
// scale_target to scale_reference, giving ratio=1. Ported as a real
// function rather than hardcoding ratio=1/diff=0 so wiring up an actual
// contempt/draw-rate CLI option later is a one-line change to the call
// site below, not a rewrite.
WDLRescaleParams ComputeWDLRescaleParams(float contempt,
                                         float draw_rate_target,
                                         float draw_rate_reference,
                                         float book_exit_bias,
                                         float contempt_max,
                                         float contempt_attenuation) {
  if (draw_rate_target > 0.0f && draw_rate_target < 0.001f) {
    draw_rate_target = 0.001f;
  }
  float scale_reference = 1.0f / std::log((1.0f + draw_rate_reference) /
                                          (1.0f - draw_rate_reference));
  float scale_target =
      (draw_rate_target == 0
           ? scale_reference
           : 1.0f / std::log((1.0f + draw_rate_target) /
                             (1.0f - draw_rate_target)));
  float ratio = scale_target / scale_reference;
  // Parenthesized (std::min)/(std::max) to dodge windows.h's min()/max()
  // macros (StockfishEvaluator.h pulls windows.h in transitively without
  // NOMINMAX).
  float clamped_contempt =
      (std::min)(contempt_max, (std::max)(-contempt_max, contempt));
  float diff =
      scale_target / (scale_reference * scale_reference) /
      (1.0f / std::pow(std::cosh(0.5f * (1 - book_exit_bias) / scale_target),
                       2) +
       1.0f / std::pow(std::cosh(0.5f * (1 + book_exit_bias) / scale_target),
                       2)) *
      std::log(10.0f) / 200.0f * clamped_contempt * contempt_attenuation;
  return {ratio, diff};
}

// Ports WDLRescale() (search/classic/search.cc) verbatim, minus the
// invert=true branch: that direction undoes a rescale for UCI display,
// which isn't a step we ever perform here.
void WDLRescale(float& v, float& d, float ratio, float diff, float sign,
                float max_reasonable_s) {
  float w = (1 + v - d) / 2;
  float l = (1 - v - d) / 2;
  const float eps = 0.0001f;
  if (w > eps && d > eps && l > eps && w < (1.0f - eps) && d < (1.0f - eps) &&
      l < (1.0f - eps)) {
    float a = lczero::FastLog(1 / l - 1);
    float b = lczero::FastLog(1 / w - 1);
    float s = (std::min)(max_reasonable_s, 2 / (a + b));
    float mu = (a - b) / (a + b);
    float s_new = s * ratio;
    float mu_new = mu + sign * s * s * diff;
    float w_new = lczero::FastLogistic((-1.0f + mu_new) / s_new);
    float l_new = lczero::FastLogistic((-1.0f - mu_new) / s_new);
    v = w_new - l_new;
    d = (std::max)(0.0f, 1.0f - w_new - l_new);
  }
}

// Reconstructs (Q, D) from a bare eval, using lc0's "WDL_mu" model run
// backwards -- see wdl::ScoreToWDL in WdlConversion.h for the derivation.
// In short: search.cc reports `score = 100 * mu`, so mu is just the eval in
// pawns, and feeding it into the same logistic pair WDLRescale()
// reconstructs with yields W and L, hence Q and D together. One model, one
// consistent distribution -- Q and D are not computed from separate
// sources and so cannot disagree.
//
// Both parameters are fitted against the real outcomes of the games being
// converted (scripts/measure_pgn_wdl.py); see Options::wdl_scale in
// PGNGame.h.
void PawnScoreToWDL(float score_pawns, float scale, float spread, float& q,
                    float& d) {
  // Shared with StockfishEvaluator's fallback path (WdlConversion.h) so a
  // given score maps to the same (Q, D) in both modes.
  wdl::ScoreToWDL(score_pawns, scale, spread, q, d);

  // Apply lc0's real contempt/draw-rate rescale on top -- but only when it
  // would actually do something. At lc0's neutral defaults it computes to
  // ratio=1, diff=0, which is mathematically an identity, and running it
  // anyway is not free: WDLRescale() re-derives s from the (w, l) pair and
  // clamps it to max_reasonable_s, so a round trip through it *changes*
  // the values whenever the natural s exceeds that clamp. Skipping the
  // identity case keeps the reconstruction above intact.
  //
  // Note max_reasonable_s is lc0's WDLMaxS (default 1.4) -- a clamp on the
  // decomposed sharpness, NOT the same quantity as our fitted `spread`.
  // Passing `spread` here was a bug; they are unrelated parameters that
  // happen to both describe "spread".
  static const WDLRescaleParams kRescaleParams = ComputeWDLRescaleParams(
      /*contempt=*/0.0f, /*draw_rate_target=*/0.0f,
      /*draw_rate_reference=*/0.5f, /*book_exit_bias=*/0.65f,
      /*contempt_max=*/420.0f, /*contempt_attenuation=*/1.0f);
  constexpr float kWDLMaxS = 1.4f;  // lc0's WDLMaxS default.
  const bool rescale_is_identity =
      std::fabs(kRescaleParams.ratio - 1.0f) < 1e-6f &&
      std::fabs(kRescaleParams.diff) < 1e-6f;
  if (!rescale_is_identity) {
    WDLRescale(q, d, kRescaleParams.ratio, kRescaleParams.diff, /*sign=*/1.0f,
               kWDLMaxS);
  }
}

bool extract_pgn_eval_comment_score(const char* comment, float& score_pawns) {
  std::string s(comment);
  // Fishtest/cutechess-cli's own comment format, e.g. "-0.76/18 1.813s" or,
  // for a mate score, "+M27/18 0.141s" (sometimes with a trailing
  // adjudication note this tool doesn't care about: "-M20/42 0.153s,
  // Black wins by adjudication"). Unlike Lichess's [%eval] -- always
  // White-relative, and the code this replaced never corrected for that --
  // this score is already self-relative: cutechess has each engine
  // annotate its own move with its own evaluation, positive meaning good
  // for whoever just moved, which is exactly the perspective Q needs here.
  // No side-to-move sign flip required.
  //
  // Leading whitespace is tolerated: pgn.cpp copies whatever sits between
  // '{' and '}' verbatim, and not every writer hugs the braces tight the
  // way Fishtest's own PGNs do -- e.g. python-chess's PGN exporter (used by
  // scripts/finish_games.py) writes "{ -0.76/18 1.813s }" with inner spaces.
  static std::regex mate_rgx("^\\s*([+-])M\\d+/");
  static std::regex score_rgx("^\\s*([+-]?\\d+(\\.\\d+)?)/");
  std::smatch matches;
  try {
    if (std::regex_search(s, matches, mate_rgx)) {
      // A saturating "huge" score, sign-preserved -- same convention the
      // old lichess mate handling used, since Q only needs to be finite
      // and drive the win-probability sigmoid to ~+-1 either way.
      score_pawns = matches[1].str() == "-" ? -128.0f : 128.0f;
      return true;
    }
    if (std::regex_search(s, matches, score_rgx)) {
      score_pawns = std::stof(matches[1].str());
      return true;
    }
  } catch (const std::exception& e) {
    // Failed to parse eval score
    return false;
  }
  return false;
}

std::string poly_move_to_uci(move_t move, const board_t* board) {
  // Use Polyglot's board-aware canonical formatter so castling is emitted as
  // the king destination square (e.g. e1g1) rather than king-takes-rook
  // (e1h1), which standard UCI engines expect.
  char str[8];
  if (!move_to_can(move, board, str, sizeof(str))) {
    return "";
  }
  return str;
}

lczero::Move poly_move_to_lc0_move(move_t move, board_t* board,
                                   bool is_black_move) {
  // IMPORTANT: move_from() and move_to() return polyglot 0x88 format squares
  // lczero::Square::FromIdx() expects 0-63 indices
  // Use square_to_64() to convert from 0x88 to 0-63
  int from_0x88 = move_from(move);
  int to_0x88 = move_to(move);
  int from_64 = square_to_64(from_0x88);
  int to_64 = square_to_64(to_0x88);

  lczero::Square from = lczero::Square::FromIdx(from_64);
  lczero::Square to = lczero::Square::FromIdx(to_64);
  lczero::Move m;

  if (move_is_promote(move)) {
    lczero::PieceType prom_type = lczero::kKnight;
    // Polyglot: 0=None, 1=Kn, 2=Bi, 3=Ro, 4=Qu
    int promo = (move >> 12) & 7;
    switch (promo) {
      case 1:
        prom_type = lczero::kKnight;
        break;
      case 2:
        prom_type = lczero::kBishop;
        break;
      case 3:
        prom_type = lczero::kRook;
        break;
      case 4:
        prom_type = lczero::kQueen;
        break;
    }
    m = lczero::Move::WhitePromotion(from, to, prom_type);
    // Need to flip for black moves (except castling)
    if (is_black_move) {
      m.Flip();
    }
  } else if (move_is_castle(move, board)) {
    // For castling, files don't change with perspective, only ranks do
    // So castling is already in the correct orientation
    lczero::File rook_file =
        (to.file().idx > from.file().idx) ? lczero::kFileH : lczero::kFileA;
    m = lczero::Move::WhiteCastling(from.file(), rook_file);
    // Don't flip castling moves - they're perspective-independent
  } else {
    if (move_is_en_passant(move, board)) {
      m = lczero::Move::WhiteEnPassant(from, to);
    } else {
      m = lczero::Move::White(from, to);
    }
    // Lc0's board is always kept from white's perspective internally.
    // After ApplyMove(), Position::Mirror() is called to switch perspective.
    // When is_black_move is true, the polyglot board is from black's
    // perspective (after the previous mirror), so we need to flip the move
    // coordinates to white's perspective before applying it in lc0.
    if (is_black_move) {
      m.Flip();
    }
  }

  return m;
}

PGNGame::PGNGame(pgn_t* pgn) {
  strncpy(this->result, pgn->result, PGN_STRING_SIZE);
  strncpy(this->fen, pgn->fen, PGN_STRING_SIZE);

  char str[256];
  while (pgn_next_move(pgn, str, 256)) {
    this->moves.emplace_back(str, pgn->last_read_comment, pgn->last_read_nag);
  }
}

std::vector<lczero::V6TrainingData> PGNGame::getChunks(
    Options options, StockfishEvaluator* evaluator, int sf_depth) const {
  std::vector<lczero::V6TrainingData> chunks;
  lczero::ChessBoard starting_board;
  std::string starting_fen =
      std::strlen(this->fen) > 0 ? this->fen : lczero::ChessBoard::kStartposFen;
  std::vector<std::string> uci_moves;

  {
    std::istringstream fen_str(starting_fen);
    std::string board;
    std::string who_to_move;
    std::string castlings;
    std::string en_passant;
    fen_str >> board >> who_to_move >> castlings >> en_passant;
    if (fen_str.eof()) {
      starting_fen.append(" 0 0");
    }
  }

  if (options.verbose) {
    std::cout << "Started new game, starting FEN: '" << starting_fen << "'"
              << std::endl;
  }

  starting_board.SetFromFen(starting_fen, nullptr, nullptr);

  lczero::PositionHistory position_history;
  position_history.Reset(starting_board, 0, 0);
  board_t board[1];
  board_from_fen(board, starting_fen.c_str());

  lczero::GameResult game_result;
  if (strcmp(this->result, "1-0") == 0) {
    game_result = lczero::GameResult::WHITE_WON;
  } else if (strcmp(this->result, "0-1") == 0) {
    game_result = lczero::GameResult::BLACK_WON;
  } else if (strcmp(this->result, "1/2-1/2") == 0) {
    game_result = lczero::GameResult::DRAW;
  } else {
    game_result = lczero::GameResult::DRAW;  // fallback for unrecognized result
  }

  char str[256];
  // Iterate over moves with robust SAN cleaning and safe handling
  for (size_t i = 0; i < this->moves.size(); ++i) {
    const auto& pgn_move = this->moves[i];

    // ----- SAN cleaning -------------------------------------------------
    std::string san = pgn_move.move;
    // Trim leading/trailing whitespace
    san.erase(0, san.find_first_not_of(" \t\r\n"));
    if (!san.empty()) san.erase(san.find_last_not_of(" \t\r\n") + 1);
    // Remove move numbers like "1.", "23..."
    size_t dotPos = san.find('.');
    if (dotPos != std::string::npos) {
      bool precedingDigits = true;
      for (size_t j = 0; j < dotPos; ++j) {
        if (!isdigit(san[j])) {
          precedingDigits = false;
          break;
        }
      }
      if (precedingDigits) {
        san = san.substr(dotPos + 1);
        san.erase(0, san.find_first_not_of(" \t"));
      }
    }
    // Discard any PGN comment start '{' and everything after it
    size_t bracePos = san.find('{');
    if (bracePos != std::string::npos) san = san.substr(0, bracePos);
    // Remove trailing annotation symbols (!, ?, +, #, =)
    while (!san.empty() &&
           (san.back() == '!' || san.back() == '?' || san.back() == '+' ||
            san.back() == '#' || san.back() == '=')) {
      san.pop_back();
    }
    // Remove trailing period
    if (!san.empty() && san.back() == '.') san.pop_back();
    // -------------------------------------------------------------------

    int move = move_from_san(san.c_str(), board);
    if (move == MoveNone || !move_is_legal(move, board)) {
      // Continuing after an illegal SAN would leave the board and position
      // history at the previous ply while the next PGN move belongs to a
      // later position, producing a corrupted game. Abort this game instead.
      std::cerr << "Aborting game: illegal move \"" << pgn_move.move
                << "\" (parsed as \"" << san << "\")" << std::endl;
      return {};
    }

    if (options.verbose) {
      move_to_san(move, board, str, 256);
      std::cout << "Read move: " << str << std::endl;
      if (pgn_move.comment[0]) {
        std::cout << str << " pgn comment: " << pgn_move.comment << std::endl;
      }
    }

    bool bad_move = false;
    if (pgn_move.nag[0]) {
      if (pgn_move.nag[0] == '2' || pgn_move.nag[0] == '4' ||
          pgn_move.nag[0] == '5' || pgn_move.nag[0] == '6') {
        bad_move = true;
      }
    }

    // Determine if it's black's move by checking if the position history
    // indicates so
    bool is_black_move = position_history.IsBlackToMove();
    lczero::Move lc0_move = poly_move_to_lc0_move(move, board, is_black_move);

    auto legal_moves = position_history.Last().GetBoard().GenerateLegalMoves();

    // Evaluation
    float Q = 0.0f;
    float D = 0.0f;
    uint32_t visits = 1;
    std::string sf_best_move_str;

    if (options.stockfish_mode && evaluator) {
      // Use move history instead of FEN to prevent engine hangs
      evaluator->setPositionMoves(starting_fen, uci_moves);
      auto sf_result = evaluator->evaluate(sf_depth);
      if (!sf_result.ok) {
        // The search failed or timed out; writing a partially populated
        // result would corrupt the training data. Reject this game.
        std::cerr << "Aborting game: Stockfish evaluation failed" << std::endl;
        return {};
      }
      if (sf_result.has_wdl) {
        // The engine reported a real win/draw/loss distribution, which is
        // exactly what the training data wants. Use it as-is rather than
        // reconstructing it from the scalar score -- no model, no fit.
        Q = sf_result.q_value;
        D = sf_result.draw_prob;
      } else {
        // No WDL available (older engine, or UCI_ShowWDL unsupported):
        // fall back to the same reconstruction -pgn-eval-mode uses, so
        // both paths agree on what a given score means.
        wdl::ScoreToWDL(sf_result.score_cp / 100.0f, options.wdl_scale,
                        options.wdl_spread, Q, D);
      }
      visits = sf_result.nodes;
      sf_best_move_str = sf_result.best_move;

      if (options.verbose) {
        std::cout << "SF eval: " << sf_result.score_cp << " cp, Q=" << Q
                  << ", D=" << D
                  << (sf_result.has_wdl ? " (engine WDL)" : " (reconstructed)")
                  << ", bestmove=" << sf_best_move_str << std::endl;
      }
    } else if (options.pgn_eval_mode) {
      float pgn_score;
      if (pgn_move.comment[0] &&
          extract_pgn_eval_comment_score(pgn_move.comment, pgn_score)) {
        PawnScoreToWDL(pgn_score, options.wdl_scale, options.wdl_spread, Q, D);
      } else {
        // Without a parsed eval, the position would be written with a fake
        // Q of 0.0 indistinguishable from an equal evaluation. Abort this
        // game instead to keep the move/evaluation sequence aligned.
        std::cerr << "Aborting game: no eval comment found for move \""
                  << pgn_move.move << "\"" << std::endl;
        return {};
      }
    } else {
      // Normal mode: use static evaluation
      StaticEvaluator::evaluateWDL(board, move, options.wdl_scale,
                                   options.wdl_spread, options.r50_damp_start,
                                   Q, D);
      if (options.verbose) {
        std::cout << "Static eval: " << StaticEvaluator::evaluate(board)
                  << " cp, Q=" << Q << ", D=" << D << ", rule50 ply "
                  << board->ply_nb << " -> "
                  << StaticEvaluator::rule50PlyAfter(board, move) << std::endl;
      }
    }

    // Restore filtering of moves explicitly marked as bad by NAG annotation
    if (options.pgn_eval_mode && bad_move) {
      if (options.verbose) {
        std::cout << "Skipping bad move (NAG) \"" << pgn_move.move << "\""
                  << std::endl;
      }
      // Apply the move to keep the move/evaluation sequence aligned while
      // omitting this position from the chunks.
      uci_moves.push_back(poly_move_to_uci(move, board));
      position_history.Append(lc0_move);
      move_do(board, move);
      continue;
    }

    // Resolve best_move. Fall back to the known-legal played move so a
    // failed lookup can never leave a null move to be policy-mapped.
    lczero::Move best_move = lc0_move;
    if (!sf_best_move_str.empty()) {
      for (const auto& m : legal_moves) {
        // On Black's turn legal_moves are in lc0's mirrored side-to-move
        // coordinates, while Stockfish returns absolute UCI coordinates.
        // Flip a copy only for the string comparison and retain the original
        // canonical move for the training data.
        // ToString(false) produces coordinate notation e.g. "e2e4"
        lczero::Move cmp = m;
        if (is_black_move) cmp.Flip();
        if (cmp.ToString(false) == sf_best_move_str) {
          best_move = m;
          break;
        }
      }
    }

    // Note: plies_left is calculated as placeholder here (0).
    // It will be updated in post-processing after we know total game length.
    int plies_left_placeholder = 0;

    // Pseudo visit counts from the evaluation. A PGN records no search, so
    // visits would otherwise be a meaningless 1 for every position. The share
    // is symmetric around an equal position:
    //
    //     W = (1 + Q) / 2          share = max(W, 1 - W) = 0.5 + |Q|/2
    //
    // Using W directly would hand the played move a *smaller* share the more
    // lost the position is -- and below 1/legal_moves it would drop under the
    // moves nobody played, inverting the policy target. Measured on this data
    // that hits 21% of frames, with 14% landing at exactly zero, because Q
    // flips sign every ply. Mirroring instead of inverting keeps the played
    // move dominant while still tracking how decided the position is; for
    // Q >= 0 the two are identical.
    float played_policy_share = 1.0f;
    uint32_t chunk_visits = visits;
    if (options.visit_budget > 0) {
      const float w = 0.5f * (1.0f + Q);
      played_policy_share = (std::max)(w, 1.0f - w);
      chunk_visits = static_cast<uint32_t>(options.visit_budget);
    }

    lczero::V6TrainingData chunk = get_v6_training_data(
        game_result, position_history, lc0_move, legal_moves, Q, best_move,
        chunk_visits, plies_left_placeholder, D, played_policy_share);
    chunks.push_back(chunk);
    if (options.verbose) {
      std::string result;
      switch (game_result) {
        case lczero::GameResult::WHITE_WON:
          result = "1-0";
          break;
        case lczero::GameResult::BLACK_WON:
          result = "0-1";
          break;
        case lczero::GameResult::DRAW:
          result = "1/2-1/2";
          break;
        default:
          result = "???";
          break;
      }
      std::cout << "Write chunk: [" << poly_move_to_uci(move, board) << ", "
                << result << ", " << Q << "]" << std::endl;
    }

    // Track move for Stockfish history (canonical form needs the pre-move
    // board, e.g. for castling king-destination notation)
    uci_moves.push_back(poly_move_to_uci(move, board));

    // Apply move
    position_history.Append(lc0_move);
    move_do(board, move);
  }

  // Post-process chunks to update played_q (eval of played move) and
  // plies_left (MLH) Logic: The position after playing the move is the next
  // chunk's position. The eval of next chunk (best_q) is from opponent's
  // perspective. So value of played move for us is -next_chunk.best_q.

  if (!chunks.empty()) {
    int total_plies = static_cast<int>(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
      // MLH: plies remaining until game end
      float plies_left = static_cast<float>(total_plies - i - 1);
      chunks[i].plies_left = plies_left;
      chunks[i].root_m = plies_left;
      chunks[i].best_m = plies_left;
      chunks[i].played_m = plies_left;

      // Update played_q (played move eval) from next position
      if (i < chunks.size() - 1) {
        chunks[i].played_q = -chunks[i + 1].best_q;
      }
    }
    // For the last chunk, the played move led directly to the game result
    chunks.back().played_q = chunks.back().result_q;
  }

  if (options.verbose) {
    std::cout << "Game end." << std::endl;
  }

  return chunks;
}