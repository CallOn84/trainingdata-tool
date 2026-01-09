#include "TrainingDataWriter.h"
#include "trainingdata/writer.h"

#include <utility>
#include <filesystem>
#include <iomanip>
#include <sstream>

TrainingDataWriter::TrainingDataWriter(size_t max_files_per_directory,
                                       size_t chunks_per_file,
                                       std::string dir_prefix)
    : files_written(0),
      max_files_per_directory(max_files_per_directory),
      chunks_per_file(chunks_per_file),
      dir_prefix(std::move(dir_prefix)){};

void TrainingDataWriter::EnqueueChunks(
    const std::vector<lczero::V6TrainingData> &chunks) {
  // Write all chunks from this game to a single file (one game per file)
  std::string directory = dir_prefix + std::to_string(files_written / max_files_per_directory);
  std::filesystem::create_directories(directory);

  std::ostringstream oss;
  oss << directory << "/game_" << std::setfill('0') << std::setw(6) << files_written << ".gz";
  std::string filename = oss.str();

  lczero::TrainingDataWriter writer(filename);
  for (const auto& chunk : chunks) {
    writer.WriteChunk(chunk);
  }
  writer.Finalize();
  files_written++;
}

void TrainingDataWriter::EnqueueChunks(
    const std::unordered_map<lczero::V6TrainingData, size_t> &chunks) {
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

void TrainingDataWriter::Finalize() { WriteQueuedChunks(0); }