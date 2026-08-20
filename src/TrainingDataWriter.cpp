#include "TrainingDataWriter.h"
#include "trainingdata/writer.h"

#include <utility>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>

TrainingDataWriter::TrainingDataWriter(size_t max_files_per_directory,
                                       size_t chunks_per_file,
                                       std::string dir_prefix)
    : files_written(0),
      max_files_per_directory(max_files_per_directory),
      chunks_per_file(chunks_per_file),
      dir_prefix(std::move(dir_prefix)){};

void TrainingDataWriter::EnqueueChunks(
    const std::vector<lczero::V6TrainingData> &chunks) {
  if (chunks.empty()) return;

  // Only the file index and the directory bookkeeping need the lock. Gzip
  // compression and the write itself are the expensive part and are thread
  // local once the index is reserved, so they must happen outside it --
  // holding the mutex across them serialised every worker onto one core and
  // was why more threads did not make this faster.
  size_t index;
  std::string directory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    index = files_written++;
    directory = dir_prefix + std::to_string(index / max_files_per_directory);
    // The mkdir stays inside the lock. Doing it outside means a second worker
    // can see the directory already marked as created and start writing into
    // it before the first worker's create_directories has returned. It only
    // runs once per max_files_per_directory files, so it costs nothing.
    if (created_dirs_.insert(directory).second) {
      std::error_code ec;
      std::filesystem::create_directories(directory, ec);
    }
  }

  std::ostringstream oss;
  oss << directory << "/game_" << std::setfill('0') << std::setw(6) << index
      << ".gz";

  // Write all chunks from this game to a single file (one game per file)
  lczero::TrainingDataWriter writer(oss.str());
  for (const auto& chunk : chunks) {
    writer.WriteChunk(chunk);
  }
  writer.Finalize();
}

void TrainingDataWriter::EnqueueChunks(
    const std::unordered_map<lczero::V6TrainingData, size_t> &chunks) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto chunk : chunks) {
    chunks_queue.push(chunk.first);
    WriteQueuedChunks(chunks_per_file);
  }
}

void TrainingDataWriter::WriteQueuedChunks(size_t min_chunks) {
  while (chunks_queue.size() > min_chunks) {
    std::string directory = dir_prefix + std::to_string(files_written / max_files_per_directory);
    std::filesystem::create_directories(directory);

    std::ostringstream oss;
    oss << directory << "/game_" << std::setfill('0') << std::setw(6) << files_written << ".gz";
    std::string filename = oss.str();

    lczero::TrainingDataWriter writer(filename);
    for (size_t i = 0; i < chunks_per_file && !chunks_queue.empty(); ++i) {
      writer.WriteChunk(chunks_queue.front());
      chunks_queue.pop();
    }
    writer.Finalize();
    files_written++;
  }
}

void TrainingDataWriter::Finalize() {
  std::lock_guard<std::mutex> lock(mutex_);
  WriteQueuedChunks(0);
}

size_t TrainingDataWriter::FilesWritten() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return files_written;
}