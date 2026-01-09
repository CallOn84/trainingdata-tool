#include "PGNGame.h"
#include "trainingdata.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <regex>
#include <sstream>
#include <vector>

float convert_sf_score_to_win_probability(float score) {
  return 2 / (1 + exp(-0.4 * score)) - 1;
}

bool extract_lichess_comment_score(const char* comment, float& Q) {
  std::string s(comment);
  // Note: brackets must be escaped with \\ in C++ regex strings
  static std::regex rgx("\\[%eval (-?\\d+(\\.\\d+)?)\\]");
  static std::regex rgx2("\\[%eval #(-?\\d+)\\]");
  std::smatch matches;
  try {
    if (std::regex_search(s, matches, rgx)) {
      Q = std::stof(matches[1].str());
      return true;
    } else if (std::regex_search(s, matches, rgx2)) {
      Q = matches[1].str().at(0) == '-' ? -128.0f : 128.0f;
      return true;
    }
  } catch (const std::exception& e) {
    // Failed to parse eval score
    return false;
  }
  return false;
}

lczero::Move poly_move_to_lc0_move(move_t move, board_t* board) {
  lczero::Square from = lczero::Square::FromIdx(move_from(move));
  lczero::Square to = lczero::Square::FromIdx(move_to(move));
  lczero::Move m;

  if (move_is_promote(move)) {
    lczero::PieceType prom_type = lczero::kKnight;
    // Polyglot: 0=None, 1=Kn, 2=Bi, 3=Ro, 4=Qu
    int promo = (move >> 12) & 7;
    switch(promo) {
      case 1: prom_type = lczero::kKnight; break;
      case 2: prom_type = lczero::kBishop; break;
      case 3: prom_type = lczero::kRook; break;
      case 4: prom_type = lczero::kQueen; break;
    }
    m = lczero::Move::WhitePromotion(from, to, prom_type);
  } else if (move_is_castle(move, board)) {
    // Determine rook file based on target square
    // Kingside: to > from (e.g. g1 > e1) -> Rook on H (file 7)
    // Queenside: to < from (e.g. c1 < e1) -> Rook on A (file 0)
    lczero::File rook_file = (to.file().idx > from.file().idx) ? lczero::kFileH : lczero::kFileA;
    m = lczero::Move::WhiteCastling(from.file(), rook_file);
  } else {
    m = lczero::Move::White(from, to);
  }

  if (colour_is_black(board->turn)) {
    m.Flip();
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

std::vector<lczero::V6TrainingData> PGNGame::getChunks(Options options) const {
  std::vector<lczero::V6TrainingData> chunks;
  lczero::ChessBoard starting_board;
  std::string starting_fen =
      std::strlen(this->fen) > 0 ? this->fen : lczero::ChessBoard::kStartposFen;

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
    game_result = lczero::GameResult::DRAW; // fallback for unrecognized result
  }

  char str[256];
  // Iterate over moves with robust SAN cleaning and safe handling
  for (size_t i = 0; i < this->moves.size(); ++i) {
    const auto &pgn_move = this->moves[i];

    // ----- SAN cleaning -------------------------------------------------
    std::string san = pgn_move.move;
    // Trim leading/trailing whitespace
    san.erase(0, san.find_first_not_of(" \t\r\n"));
    if (!san.empty())
      san.erase(san.find_last_not_of(" \t\r\n") + 1);
    // Remove move numbers like "1.", "23..."
    size_t dotPos = san.find('.');
    if (dotPos != std::string::npos) {
      bool precedingDigits = true;
      for (size_t j = 0; j < dotPos; ++j) {
        if (!isdigit(san[j])) { precedingDigits = false; break; }
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
    while (!san.empty() && (san.back() == '!' || san.back() == '?' ||
                            san.back() == '+' || san.back() == '#' || san.back() == '=')) {
      san.pop_back();
    }
    // Remove trailing period
    if (!san.empty() && san.back() == '.') san.pop_back();
    // -------------------------------------------------------------------

    int move = move_from_san(san.c_str(), board);
    if (move == MoveNone || !move_is_legal(move, board)) {
      if (options.verbose) {
        std::cout << "Skipping illegal move \"" << pgn_move.move << "\" (parsed as \"" << san << "\")" << std::endl;
      }
      continue;
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

    lczero::Move lc0_move = poly_move_to_lc0_move(move, board);

    auto legal_moves = position_history.Last().GetBoard().GenerateLegalMoves();

    // Extract SF scores and convert to win probability
    float Q = 0.0f;
    if (options.lichess_mode) {
      if (pgn_move.comment[0]) {
        float lichess_score;
        bool success =
            extract_lichess_comment_score(pgn_move.comment, lichess_score);
        if (success) {
          Q = convert_sf_score_to_win_probability(lichess_score);
        } else if (options.verbose) {
          std::cout << "Skipping Lichess eval for move \"" << pgn_move.move << "\" – no %eval found" << std::endl;
        }
      } else if (options.verbose) {
        std::cout << "No Lichess comment for move \"" << pgn_move.move << "\" – skipping eval" << std::endl;
      }
    }

    if (!(bad_move && options.lichess_mode)) {
      // Generate training data
      // For non-Stockfish mode, best_move = played_move, visits = 1
      lczero::V6TrainingData chunk = get_v6_training_data(
          game_result, position_history, lc0_move, legal_moves, Q, lc0_move, 1);
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
        std::cout << "Write chunk: [" << lc0_move.ToString(false) << ", " << result
                  << ", " << Q << "]\n";
      }
    }

    // Execute move
    position_history.Append(lc0_move);
    move_do(board, move);
  }

  if (options.verbose) {
    std::cout << "Game end." << std::endl;
  }

  return chunks;
}