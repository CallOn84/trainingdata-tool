#ifndef STOCKFISH_EVALUATOR_H
#define STOCKFISH_EVALUATOR_H

#include <string>
#include <cstdio>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// Manages communication with Stockfish via UCI protocol
class StockfishEvaluator {
 public:
  explicit StockfishEvaluator(const std::string& stockfish_path,
                              int hash_mb = 128);
  ~StockfishEvaluator();

  // Initialize the engine (send uci, wait for uciok)
  bool init();

  // Set position from FEN string
  void setPosition(const std::string& fen);

  // Set position using startpos/fen and move list
  void setPositionMoves(const std::string& start_fen, const std::vector<std::string>& moves);

  struct Result {
    int score_cp;
    std::string best_move; // Long algebraic notation (e.g. "e2e4")
    uint32_t nodes;
    // Real WDL straight from the engine's UCI_ShowWDL output, when it
    // reported one. These are the engine's own numbers -- prefer them over
    // anything derived from score_cp, which is a lossy projection of the
    // same information onto a single scalar.
    float draw_prob;       // Draw probability from UCI WDL [0, 1]
    float q_value;         // (win - loss) from UCI WDL, in [-1, 1]
    bool has_wdl;          // True if the engine reported a WDL line
    bool ok;               // False if the search failed/timed out

    Result()
        : score_cp(0),
          nodes(0),
          draw_prob(0.0f),
          q_value(0.0f),
          has_wdl(false),
          ok(false) {}
  };

  // Evaluate current position at given depth
  Result evaluate(int depth);

  // Convert centipawn score to win probability Q value [-1, 1].
  // Delegates to wdl::CentipawnToQ so this matches -pgn-eval-mode exactly;
  // only a fallback for engines/builds that do not report a WDL.
  static float cpToWinProbability(int centipawns);

  // Shutdown the engine
  void quit();

  // Check if engine is running
  bool isRunning() const {
#ifdef _WIN32
    return process_handle_ != nullptr;
#else
    return write_fd_ >= 0;
#endif
  }

 private:
  std::string stockfish_path_;
  int hash_mb_;
  std::string read_buffer_;

#ifdef _WIN32
  HANDLE process_handle_;
  HANDLE stdin_write_;
  HANDLE stdout_read_;
#else
  int child_pid_;
  int write_fd_;
  int read_fd_;
#endif
  
  // Send command to engine
  void sendCommand(const std::string& cmd);
  
  // Read line from engine output
  std::string readLine();
  
  // Wait for specific response
  bool waitFor(const std::string& expected, int timeout_ms = 5000);
};

#endif // STOCKFISH_EVALUATOR_H
