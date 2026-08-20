#include "StockfishEvaluator.h"

#include "WdlConversion.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <chrono>
#include <thread>
#include <regex>
#include <deque>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/select.h>
#endif

StockfishEvaluator::StockfishEvaluator(const std::string& stockfish_path,
                                       int hash_mb)
    : stockfish_path_(stockfish_path), hash_mb_(hash_mb)
#ifdef _WIN32
    , process_handle_(nullptr), stdin_write_(nullptr), stdout_read_(nullptr)
#else
    , child_pid_(-1), write_fd_(-1), read_fd_(-1)
#endif
{}

StockfishEvaluator::~StockfishEvaluator() {
  quit();
}

#ifdef _WIN32
// Windows implementation using CreateProcess with separate stdin/stdout pipes
bool StockfishEvaluator::init() {
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE stdin_read = nullptr;
  HANDLE stdout_write = nullptr;

  if (!CreatePipe(&stdin_read, &stdin_write_, &sa, 0)) {
    std::cerr << "Failed to create stdin pipe" << std::endl;
    return false;
  }
  if (!SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0)) {
    std::cerr << "Failed to configure stdin pipe" << std::endl;
    CloseHandle(stdin_read);
    CloseHandle(stdin_write_);
    stdin_write_ = nullptr;
    return false;
  }

  if (!CreatePipe(&stdout_read_, &stdout_write, &sa, 0)) {
    std::cerr << "Failed to create stdout pipe" << std::endl;
    CloseHandle(stdin_read);
    CloseHandle(stdin_write_);
    stdin_write_ = nullptr;
    return false;
  }
  if (!SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0)) {
    std::cerr << "Failed to configure stdout pipe" << std::endl;
    CloseHandle(stdin_read);
    CloseHandle(stdin_write_);
    CloseHandle(stdout_read_);
    CloseHandle(stdout_write);
    stdin_write_ = nullptr;
    stdout_read_ = nullptr;
    return false;
  }

  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_read;
  si.hStdOutput = stdout_write;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  if (!CreateProcessA(nullptr, const_cast<char*>(stockfish_path_.c_str()),
                      nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    std::cerr << "Failed to start Stockfish: " << stockfish_path_ << std::endl;
    CloseHandle(stdin_read);
    CloseHandle(stdin_write_);
    CloseHandle(stdout_read_);
    CloseHandle(stdout_write);
    stdin_write_ = nullptr;
    stdout_read_ = nullptr;
    return false;
  }

  // Parent no longer needs the child's pipe ends
  CloseHandle(stdin_read);
  CloseHandle(stdout_write);

  process_handle_ = pi.hProcess;
  CloseHandle(pi.hThread);

  sendCommand("uci");
  if (!waitFor("uciok", 5000)) {
    std::cerr << "Stockfish did not respond to uci command" << std::endl;
    quit();
    return false;
  }

  sendCommand("setoption name Threads value 2");
  sendCommand("setoption name Hash value " + std::to_string(hash_mb_));
  sendCommand("setoption name UCI_ShowWDL value true");
  sendCommand("isready");

  if (!waitFor("readyok", 5000)) {
    std::cerr << "Stockfish did not respond to isready command" << std::endl;
    quit();
    return false;
  }

  return true;
}

#else
// POSIX implementation using fork/exec
bool StockfishEvaluator::init() {
  // Ignore SIGPIPE to avoid crashing if Stockfish terminates unexpectedly
  signal(SIGPIPE, SIG_IGN);

  int stdin_pipe[2];   // Parent writes, child reads
  int stdout_pipe[2];  // Child writes, parent reads

  if (pipe(stdin_pipe) < 0) {
    std::cerr << "Failed to create pipes" << std::endl;
    return false;
  }
  if (pipe(stdout_pipe) < 0) {
    std::cerr << "Failed to create pipes" << std::endl;
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    return false;
  }

  child_pid_ = fork();

  if (child_pid_ < 0) {
    std::cerr << "Failed to fork" << std::endl;
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return false;
  }
  
  if (child_pid_ == 0) {
    // Child process
    close(stdin_pipe[1]);   // Close write end of stdin pipe
    close(stdout_pipe[0]);  // Close read end of stdout pipe

    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    // Redirect stderr to /dev/null so engine diagnostics cannot corrupt UCI
    // protocol parsing on stdout.
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    execl(stockfish_path_.c_str(), stockfish_path_.c_str(), nullptr);

    // If exec fails
    _exit(1);
  }
  
  // Parent process
  close(stdin_pipe[0]);   // Close read end of stdin pipe
  close(stdout_pipe[1]);  // Close write end of stdout pipe
  
  write_fd_ = stdin_pipe[1];
  read_fd_ = stdout_pipe[0];

  // Make read non-blocking so readLine() never blocks past the data that
  // select() reported as available.
  fcntl(read_fd_, F_SETFL, fcntl(read_fd_, F_GETFL) | O_NONBLOCK);
  
  // Initialize UCI
  sendCommand("uci");
  if (!waitFor("uciok", 5000)) {
    std::cerr << "Stockfish did not respond to uci command" << std::endl;
    quit();
    return false;
  }
  
  sendCommand("setoption name Threads value 2");
  sendCommand("setoption name Hash value " + std::to_string(hash_mb_));
  sendCommand("setoption name UCI_ShowWDL value true");
  sendCommand("isready");
  
  if (!waitFor("readyok", 5000)) {
    std::cerr << "Stockfish did not respond to isready command" << std::endl;
    quit();
    return false;
  }
  
  return true;
}
#endif

void StockfishEvaluator::setPosition(const std::string& fen) {
  std::string cmd = "position fen " + fen;
  sendCommand(cmd);
}

void StockfishEvaluator::setPositionMoves(const std::string& start_fen, const std::vector<std::string>& moves) {
  std::ostringstream cmd;
  cmd << "position ";
  if (start_fen == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" || start_fen.empty()) {
    cmd << "startpos";
  } else {
    cmd << "fen " << start_fen;
  }
  
  if (!moves.empty()) {
    cmd << " moves";
    for (const auto& m : moves) {
      cmd << " " << m;
    }
  }
  sendCommand(cmd.str());
}

StockfishEvaluator::Result StockfishEvaluator::evaluate(int depth) {
  std::ostringstream cmd;
  cmd << "go depth " << depth;
  sendCommand(cmd.str());

  // Read output until we get "bestmove" (with 30s timeout)
  auto start = std::chrono::steady_clock::now();
  const int timeout_seconds = 30;

  Result result;
  bool found_score = false;
  bool got_bestmove = false;

  // Keep last 10 lines for diagnostics
  std::deque<std::string> recent_lines;
  const size_t max_recent_lines = 10;

  std::string line;
  while (true) {
    line = readLine();
    if (line.empty()) {
      // Check timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start).count();
      if (elapsed >= timeout_seconds) {
        std::cerr << "\n=== Stockfish Timeout after " << timeout_seconds << "s ===" << std::endl;
        std::cerr << "Last " << recent_lines.size() << " lines from Stockfish:" << std::endl;
        for (const auto& l : recent_lines) {
          std::cerr << "  " << l << std::endl;
        }
        std::cerr << "=== End Stockfish Output ===" << std::endl;
        // Stop the search and drain through bestmove so later output from
        // this search is not consumed as the next position's result.
        sendCommand("stop");
        auto drain_start = std::chrono::steady_clock::now();
        while (true) {
          std::string drain_line = readLine();
          if (drain_line.find("bestmove") != std::string::npos) break;
          auto drain_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now() - drain_start).count();
          if (drain_elapsed >= 5) break;
          if (drain_line.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
        return result;  // result.ok is false, signaling failure to the caller
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Store line for diagnostics
    recent_lines.push_back(line);
    if (recent_lines.size() > max_recent_lines) {
      recent_lines.pop_front();
    }

    // Check for errors from Stockfish
    if (line.find("Illegal move") != std::string::npos ||
        line.find("illegal move") != std::string::npos ||
        line.find("Error") != std::string::npos) {
      std::cerr << "Stockfish error: " << line << std::endl;
    }

    // Capture nodes if available
    if (line.find("nodes ") != std::string::npos) {
      std::regex nodes_regex("nodes (\\d+)");
      std::smatch matches;
      if (std::regex_search(line, matches, nodes_regex)) {
        try {
            result.nodes = std::stoul(matches[1].str());
        } catch (...) {}
      }
    }

    // Parse score from info lines

    if (line.find("score cp ") != std::string::npos) {
      std::regex cp_regex("score cp (-?\\d+)");
      std::smatch matches;
      if (std::regex_search(line, matches, cp_regex)) {
        result.score_cp = std::stoi(matches[1].str());
        found_score = true;
      }
    } else if (line.find("score mate ") != std::string::npos) {
      std::regex mate_regex("score mate (-?\\d+)");
      std::smatch matches;
      if (std::regex_search(line, matches, mate_regex)) {
        int mate_in = std::stoi(matches[1].str());
        // Convert mate score to high centipawn value
        result.score_cp = mate_in > 0 ? 10000 + (100 - mate_in) : -10000 - (100 + mate_in);
        found_score = true;
      }
    }

    // Parse WDL if enabled (e.g. "wdl 300 400 300").
    //
    // This is the engine's own win/draw/loss distribution, which is
    // strictly more information than the centipawn score: cp is a scalar
    // projection that cannot express D at all. So record W/D/L directly
    // and leave score_cp as the engine actually reported it.
    //
    // (An earlier version converted the WDL *into* a synthetic cp via a
    // sigmoid, overwriting the real score, and the caller then converted
    // that back into a Q. That round trip discarded the real draw
    // probability and the real cp to recover a worse estimate of both.)
    if (line.find(" wdl ") != std::string::npos) {
        std::regex wdl_regex(" wdl (\\d+) (\\d+) (\\d+)");
        std::smatch matches;
        if (std::regex_search(line, matches, wdl_regex)) {
            int w = std::stoi(matches[1].str());
            int d = std::stoi(matches[2].str());
            int l = std::stoi(matches[3].str());

            result.draw_prob = d / 1000.0f;
            result.q_value = (float)(w - l) / 1000.0f;
            result.has_wdl = true;
        }
    }

    if (line.find("bestmove") != std::string::npos) {
      std::regex bestmove_regex("bestmove ([a-h][1-8][a-h][1-8][qrbn]?)");
      std::smatch matches;
      if (std::regex_search(line, matches, bestmove_regex)) {
        result.best_move = matches[1].str();
      }
      got_bestmove = true;
      break;
    }
  }

  if (!found_score) result.score_cp = 0;
  result.ok = got_bestmove;
  return result;
}

float StockfishEvaluator::cpToWinProbability(int centipawns) {
  // Handle mate scores (evaluate() encodes these as +-10000-ish).
  if (centipawns >= 10000) return 1.0f;
  if (centipawns <= -10000) return -1.0f;

  // Shared with -pgn-eval-mode via wdl::ScoreToWDL, using the defaults
  // fitted against real game outcomes. This used to be an ad-hoc sigmoid,
  // 2/(1+exp(-0.4*cp/100))-1, which did not match what PGNGame.cpp does
  // and so gave a different Q for the same score depending on mode.
  //
  // Prefer the engine's own WDL (Result::has_wdl) when it reports one;
  // this is only for engines that do not.
  return wdl::CentipawnToQ(centipawns, /*scale=*/1.13f, /*spread=*/0.21f);
}

void StockfishEvaluator::quit() {
#ifdef _WIN32
  if (stdin_write_) {
    sendCommand("quit");
    CloseHandle(stdin_write_);
    stdin_write_ = nullptr;
  }
  if (stdout_read_) {
    CloseHandle(stdout_read_);
    stdout_read_ = nullptr;
  }
  if (process_handle_) {
    WaitForSingleObject(process_handle_, 3000);
    TerminateProcess(process_handle_, 0);
    CloseHandle(process_handle_);
    process_handle_ = nullptr;
  }
#else
  if (write_fd_ >= 0) {
    sendCommand("quit");
    close(write_fd_);
    close(read_fd_);
    write_fd_ = -1;
    read_fd_ = -1;
  }
  if (child_pid_ > 0) {
    waitpid(child_pid_, nullptr, 0);
    child_pid_ = -1;
  }
#endif
}

void StockfishEvaluator::sendCommand(const std::string& cmd) {
#ifdef _WIN32
  if (stdin_write_) {
    std::string line = cmd + "\n";
    DWORD written = 0;
    WriteFile(stdin_write_, line.c_str(),
              static_cast<DWORD>(line.size()), &written, nullptr);
  }
#else
  if (write_fd_ >= 0) {
    std::string line = cmd + "\n";
    (void)write(write_fd_, line.c_str(), line.size());
  }
#endif
}

std::string StockfishEvaluator::readLine() {
#ifdef _WIN32
  if (!stdout_read_) return "";

  std::string line;
  char c;
  DWORD read_bytes = 0;
  while (ReadFile(stdout_read_, &c, 1, &read_bytes, nullptr) &&
         read_bytes == 1) {
    if (c == '\n') break;
    if (c != '\r') line += c;
  }
  return line;
#else
  if (read_fd_ < 0) return "";

  // Return a buffered complete line first so leftovers are never missed.
  size_t buffered_newline = read_buffer_.find('\n');
  if (buffered_newline != std::string::npos) {
    std::string line = read_buffer_.substr(0, buffered_newline);
    read_buffer_.erase(0, buffered_newline + 1);
    while (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return line;
  }

  // Use select to check if data is available (with 100ms timeout)
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(read_fd_, &read_fds);

  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000;  // 100ms timeout

  int ret = select(read_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
  if (ret <= 0) {
    return "";  // Timeout or error
  }

  // read_fd_ is non-blocking: accumulate whatever is currently available into
  // the persistent buffer and return a line only once a newline arrives.
  char buf[4096];
  while (true) {
    ssize_t n = read(read_fd_, buf, sizeof(buf));
    if (n <= 0) break;  // EAGAIN (would block) or EOF/error
    read_buffer_.append(buf, static_cast<size_t>(n));
    if (n < static_cast<ssize_t>(sizeof(buf))) break;
  }

  size_t newline = read_buffer_.find('\n');
  if (newline == std::string::npos) {
    return "";  // No complete line yet
  }
  std::string line = read_buffer_.substr(0, newline);
  read_buffer_.erase(0, newline + 1);
  while (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
#endif
}


bool StockfishEvaluator::waitFor(const std::string& expected, int timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  
  while (true) {
    std::string line = readLine();
    if (line.find(expected) != std::string::npos) {
      return true;
    }
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (elapsed >= timeout_ms) {
      return false;
    }
    
    if (line.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}
