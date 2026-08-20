#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include "PGNGame.h"
#include "StockfishEvaluator.h"
#include "TrainingDataDedup.h"
#include "TrainingDataReader.h"
#include "TrainingDataWriter.h"

size_t max_files_per_directory = 10000;
int64_t max_games_to_convert = 10000000;
size_t chunks_per_file = 4096;
size_t dedup_uniq_buffersize = 50000;
float dedup_q_ratio = 1.0f;
int num_threads = 0;
std::string dataset_name = "supervised";
std::string output_dir = "";
std::string output_prefix = "";
std::string stockfish_path;
int sf_depth = 10;
int sf_hash_mb = 128;

inline bool file_exists(const std::string &name) {
  auto s = std::filesystem::status(name);
  return std::filesystem::is_regular_file(s);
}

inline bool directory_exists(const std::string &name) {
  auto s = std::filesystem::status(name);
  return std::filesystem::is_directory(s);
}

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t max_size = 256)
      : max_size_(max_size), done_(false) {}

  void Push(T item) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_push_.wait(lock, [this] { return queue_.size() < max_size_ || done_; });
    if (done_) return;
    queue_.push(std::move(item));
    cv_pop_.notify_one();
  }

  bool Pop(T &item) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_pop_.wait(lock, [this] { return !queue_.empty() || done_; });
    if (queue_.empty()) return false;
    item = std::move(queue_.front());
    queue_.pop();
    cv_push_.notify_one();
    return true;
  }

  void SetDone() {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    cv_push_.notify_all();
    cv_pop_.notify_all();
  }

 private:
  const size_t max_size_;
  bool done_;
  std::queue<T> queue_;
  std::mutex mutex_;
  std::condition_variable cv_push_;
  std::condition_variable cv_pop_;
};

// The writer is owned by the caller and shared across every input file. It
// used to be constructed here, per file, so its file counter restarted at
// game_000000 for each PGN and every input silently overwrote the previous
// one's output -- a run over 29 files kept only the last file's games.
void convert_games(const std::string &pgn_file_name, Options options,
                   StockfishEvaluator *evaluator, TrainingDataWriter &writer,
                   int threads_count) {
  int workers_count = threads_count;
  if (workers_count <= 0) {
    workers_count = static_cast<int>(std::thread::hardware_concurrency());
    if (workers_count <= 0) workers_count = 4;
  }
  std::cout << "Processing " << pgn_file_name << " using " << workers_count
            << " worker thread(s)..." << std::endl;

  pgn_t pgn[1];
  pgn_open(pgn, pgn_file_name.c_str());
  const size_t files_before = writer.FilesWritten();

  BoundedQueue<PGNGame> queue(256);
  std::atomic<int64_t> games_processed{0};

  std::vector<std::thread> workers;
  workers.reserve(workers_count);
  for (int t = 0; t < workers_count; ++t) {
    workers.emplace_back([&]() {
      PGNGame game;
      while (queue.Pop(game)) {
        auto chunks = game.getChunks(options, evaluator, sf_depth);
        if (!chunks.empty()) {
          writer.EnqueueChunks(chunks);
        }
        int64_t count = ++games_processed;
        if (count % 1000 == 0) {
          std::cout << count << " games written." << std::endl;
        }
      }
    });
  }

  int64_t games_read = 0;
  while (pgn_next_game(pgn) && games_read < max_games_to_convert) {
    queue.Push(PGNGame(pgn));
    games_read++;
  }

  queue.SetDone();

  for (auto &worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  // Not Finalize() -- the writer outlives this file and is finalized once all
  // inputs are done, so its counter keeps climbing instead of restarting.
  std::cout << "Finished writing " << games_processed.load() << " games ("
            << (writer.FilesWritten() - files_before)
            << " chunk files created, " << writer.FilesWritten()
            << " total so far)." << std::endl;
  pgn_close(pgn);
}

int main(int argc, char *argv[]) {
  std::cout << "TrainingData Tool v1.1 (Stockfish Arg Fix)" << std::endl;
  lczero::InitializeMagicBitboards();
  polyglot_init();
  Options options;
  bool deduplication_mode = false;

  for (size_t idx = 0; idx < argc; ++idx) {
    if (0 == static_cast<std::string>("-v").compare(argv[idx])) {
      std::cout << "Verbose mode ON" << std::endl;
      options.verbose = true;
    } else if (0 ==
               static_cast<std::string>("-pgn-eval-mode").compare(argv[idx])) {
      std::cout << "PGN eval mode ON (reading evals already in the PGN's "
                   "move comments -- no engine spawned)"
                << std::endl;
      options.pgn_eval_mode = true;
    } else if (0 ==
               static_cast<std::string>("-wdl-scale").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -wdl-scale requires a positive float argument."
                  << std::endl;
        return 1;
      }
      const char* scale_arg = argv[++idx];
      options.wdl_scale = std::atof(scale_arg);
      if (options.wdl_scale <= 0.0f) {
        std::cerr << "Error: -wdl-scale must be a positive number, got '"
                  << scale_arg << "'." << std::endl;
        return 1;
      }
      std::cout << "WDL scale (pgn-eval-mode) set to: " << options.wdl_scale
                << std::endl;
    } else if (0 ==
               static_cast<std::string>("-wdl-spread").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -wdl-spread requires a positive float argument."
                  << std::endl;
        return 1;
      }
      const char* spread_arg = argv[++idx];
      options.wdl_spread = std::atof(spread_arg);
      if (options.wdl_spread <= 0.0f) {
        std::cerr << "Error: -wdl-spread must be a positive number, got '"
                  << spread_arg << "'." << std::endl;
        return 1;
      }
      std::cout << "WDL spread (pgn-eval-mode) set to: " << options.wdl_spread
                << std::endl;
    } else if (0 ==
               static_cast<std::string>("-visit-budget").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -visit-budget requires a positive integer."
                  << std::endl;
        return 1;
      }
      const char* budget_arg = argv[++idx];
      options.visit_budget = std::atoi(budget_arg);
      if (options.visit_budget < 0) {
        std::cerr << "Error: -visit-budget must be >= 0, got '" << budget_arg
                  << "'." << std::endl;
        return 1;
      }
      std::cout << "Pseudo visit budget set to: " << options.visit_budget
                << " (policy share = 0.5 + |Q|/2, remainder spread over the "
                   "other legal moves)"
                << std::endl;
    } else if (0 ==
               static_cast<std::string>("-r50-damp-start").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -r50-damp-start requires an integer argument."
                  << std::endl;
        return 1;
      }
      const char* r50_arg = argv[++idx];
      options.r50_damp_start = std::atoi(r50_arg);
      if (options.r50_damp_start < 0 || options.r50_damp_start > 100) {
        std::cerr << "Error: -r50-damp-start must be between 0 and 100 plies, "
                     "got '"
                  << r50_arg << "'." << std::endl;
        return 1;
      }
      std::cout << "Rule-50 damping (static eval) starts at halfmove clock: "
                << options.r50_damp_start << std::endl;
    } else if (0 == static_cast<std::string>("-stockfish").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -stockfish requires a path argument." << std::endl;
        return 1;
      }
      stockfish_path = argv[++idx];
      std::cout << "Stockfish mode ON, binary: " << stockfish_path << std::endl;
      options.stockfish_mode = true;
    } else if (0 == static_cast<std::string>("-sf-depth").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -sf-depth requires a positive integer argument."
                  << std::endl;
        return 1;
      }
      const char* depth_arg = argv[++idx];
      sf_depth = std::atoi(depth_arg);
      if (sf_depth <= 0) {
        std::cerr << "Error: -sf-depth must be a positive integer, got '"
                  << depth_arg << "'." << std::endl;
        return 1;
      }
      std::cout << "Stockfish depth set to: " << sf_depth << std::endl;
    } else if (0 == static_cast<std::string>("-sf-hash").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -sf-hash requires a positive integer argument."
                  << std::endl;
        return 1;
      }
      const char* hash_arg = argv[++idx];
      sf_hash_mb = std::atoi(hash_arg);
      if (sf_hash_mb <= 0) {
        std::cerr << "Error: -sf-hash must be a positive integer (MB), got '"
                  << hash_arg << "'." << std::endl;
        return 1;
      }
      std::cout << "Stockfish hash set to: " << sf_hash_mb << " MB"
                << std::endl;
    } else if (0 == static_cast<std::string>("-files-per-dir").compare(argv[idx]) ||
               0 == static_cast<std::string>("--files-per-dir").compare(argv[idx]) ||
               0 == static_cast<std::string>("-chunks-per-dir").compare(argv[idx]) ||
               0 == static_cast<std::string>("--chunks-per-dir").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -files-per-dir requires an integer argument." << std::endl;
        return 1;
      }
      max_files_per_directory = std::atoi(argv[++idx]);
      std::cout << "Max files per directory set to: " << max_files_per_directory
                << std::endl;
    } else if (0 == static_cast<std::string>("-max-games-to-convert")
                        .compare(argv[idx])) {
      max_games_to_convert = std::atoi(argv[idx + 1]);
      std::cout << "Max games to convert set to: " << max_games_to_convert
                << std::endl;
    } else if (0 == static_cast<std::string>("-chunks-per-file")
                        .compare(argv[idx])) {
      chunks_per_file = std::atoi(argv[idx + 1]);
      std::cout << "Chunks per file set to: " << chunks_per_file << std::endl;
    } else if (0 == static_cast<std::string>("-deduplication-mode")
                        .compare(argv[idx])) {
      deduplication_mode = true;
      std::cout << "Position de-duplication mode ON" << std::endl;
    } else if (0 == static_cast<std::string>("-dedup-uniq-buffersize")
                        .compare(argv[idx])) {
      dedup_uniq_buffersize = std::atoi(argv[idx + 1]);
      std::cout << "Deduplication buffersize set to: " << dedup_uniq_buffersize
                << std::endl;
    } else if (0 ==
               static_cast<std::string>("-dedup-q-ratio").compare(argv[idx])) {
      dedup_q_ratio = std::stof(argv[idx + 1]);
      std::cout << "Deduplication Q ratio set to: " << dedup_q_ratio
                << std::endl;
    } else if (0 == static_cast<std::string>("-threads").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -threads requires an integer argument." << std::endl;
        return 1;
      }
      num_threads = std::atoi(argv[++idx]);
      std::cout << "Worker threads set to: " << num_threads << std::endl;
    } else if (0 == static_cast<std::string>("-name").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -name requires a dataset name argument." << std::endl;
        return 1;
      }
      dataset_name = argv[++idx];
      std::cout << "Dataset name set to: " << dataset_name << std::endl;
    } else if (0 == static_cast<std::string>("-output-dir").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -output-dir requires a directory path argument." << std::endl;
        return 1;
      }
      output_dir = argv[++idx];
      std::cout << "Output directory set to: " << output_dir << std::endl;
    } else if (0 == static_cast<std::string>("-output").compare(argv[idx])) {
      if (idx + 1 >= static_cast<size_t>(argc) || argv[idx + 1][0] == '-') {
        std::cerr << "Error: -output requires a prefix argument." << std::endl;
        return 1;
      }
      output_prefix = argv[++idx];
      std::cout << "Output prefix set to: " << output_prefix << std::endl;
    }
  }

  // Resolve output_prefix from -name and -output-dir if not explicitly set by -output
  if (output_prefix.empty()) {
    std::string clean_name = dataset_name;
    for (char &c : clean_name) {
      if (c == ' ') c = '-';
    }
    if (!clean_name.empty() && clean_name.back() != '-') {
      clean_name += "-";
    }
    if (!output_dir.empty()) {
      std::filesystem::path p(output_dir);
      output_prefix = (p / clean_name).generic_string();
    } else {
      output_prefix = clean_name;
    }
  }
  std::cout << "Target output prefix: " << output_prefix
            << " (e.g. " << output_prefix << "0/, " << output_prefix << "1/)"
            << std::endl;

  // Initialize Stockfish if requested
  std::unique_ptr<StockfishEvaluator> evaluator;
  if (options.stockfish_mode) {
    evaluator =
        std::make_unique<StockfishEvaluator>(stockfish_path, sf_hash_mb);
    if (!evaluator->init()) {
      std::cerr << "Failed to initialize Stockfish. Exiting." << std::endl;
      return 1;
    }
    std::cout << "Stockfish initialized successfully." << std::endl;
  }

  // One writer for the whole run: its file counter has to span every input
  // file, or each PGN restarts at game_000000 and overwrites the last one.
  TrainingDataWriter writer(max_files_per_directory, chunks_per_file,
                            output_prefix);
  for (size_t idx = 1; idx < argc; ++idx) {
    std::string arg = argv[idx];
    // Skip option flags and their values
    if (arg[0] == '-') {
      // Skip the value for options that take a parameter
      if (arg == "-stockfish" || arg == "-sf-depth" || arg == "-sf-hash" ||
          arg == "-wdl-scale" || arg == "--wdl-scale" ||
          arg == "-wdl-spread" || arg == "--wdl-spread" ||
          arg == "-r50-damp-start" || arg == "--r50-damp-start" ||
          arg == "-visit-budget" || arg == "--visit-budget" ||
          arg == "-files-per-dir" || arg == "--files-per-dir" ||
          arg == "-chunks-per-dir" || arg == "--chunks-per-dir" ||
          arg == "-max-games-to-convert" || arg == "--max-games-to-convert" ||
          arg == "-chunks-per-file" || arg == "--chunks-per-file" ||
          arg == "-dedup-uniq-buffersize" || arg == "-dedup-q-ratio" ||
          arg == "-threads" || arg == "--threads" ||
          arg == "-name" || arg == "--name" ||
          arg == "-output-dir" || arg == "--output-dir" ||
          arg == "-output" || arg == "--output") {
        ++idx;  // Skip the next argument (the value)
      }
      continue;
    }

    if (deduplication_mode) {
      if (!directory_exists(argv[idx])) continue;
      TrainingDataReader reader(argv[idx]);
      training_data_dedup(reader, writer, dedup_uniq_buffersize, dedup_q_ratio);
    } else {
      if (!file_exists(argv[idx])) continue;

      // Check for .pgn or .pgn.gz extension (simple case-insensitive check)
      std::string path = argv[idx];
      bool is_pgn = false;
      if (path.length() >= 4 && strcasecmp(path.substr(path.length() - 4).c_str(), ".pgn") == 0) is_pgn = true;
      if (path.length() >= 7 && strcasecmp(path.substr(path.length() - 7).c_str(), ".pgn.gz") == 0) is_pgn = true;
      if (path.length() >= 3 && strcasecmp(path.substr(path.length() - 3).c_str(), ".gz") == 0) is_pgn = true;

      if (!is_pgn) {
        if (options.verbose) {
          std::cout << "Skipping non-PGN file: " << path << std::endl;
        }
        continue;
      }

      if (options.verbose) {
        std::cout << "Opening '" << argv[idx] << "'" << std::endl;
      }
      convert_games(argv[idx], options, evaluator.get(), writer, num_threads);
    }
  }

  writer.Finalize();
  std::cout << "All inputs done: " << writer.FilesWritten()
            << " chunk files written in total." << std::endl;
}
