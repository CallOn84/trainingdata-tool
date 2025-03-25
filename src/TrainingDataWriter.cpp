#include "TrainingDataWriter.h"

#include <utility>

TrainingDataWriter::TrainingDataWriter(size_t max_files_per_directory,
                                       size_t chunks_per_file,
                                       std::string dir_prefix)
    : files_written(0),
      max_files_per_directory(max_files_per_directory),
      chunks_per_file(chunks_per_file),
      dir_prefix(std::move(dir_prefix)) {}

void TrainingDataWriter::EnqueueChunks(
    const std::vector<lczero::V6TrainingData>& chunks) {
  for (const auto& chunk : chunks) {
    chunks_queue.push(chunk);
  }
  WriteQueuedChunks(chunks_per_file);
}

void TrainingDataWriter::EnqueueChunks(
    const std::vector<lczero::V6TrainingData>& chunks, bool finalize) {
  for (const auto& chunk : chunks) {
    chunks_queue.push(chunk);
  }
  WriteQueuedChunks(chunks_per_file, finalize);
}

void TrainingDataWriter::EnqueueChunks(
    const std::unordered_map<lczero::V6TrainingData, size_t>& chunks) {
  for (const auto& chunk : chunks) {
    chunks_queue.push(chunk.first);
  }
  WriteQueuedChunks(chunks_per_file);
}

void TrainingDataWriter::EnqueueChunks(
    const std::unordered_map<lczero::V6TrainingData, size_t>& chunks,
    bool finalize) {
  for (const auto& chunk : chunks) {
    chunks_queue.push(chunk.first);
  }
  WriteQueuedChunks(chunks_per_file, finalize);
}

void TrainingDataWriter::WriteQueuedChunks(size_t min_chunks) {
  WriteQueuedChunks(min_chunks, false);
}

void TrainingDataWriter::WriteQueuedChunks(size_t min_chunks, bool finalize) {
  while ((chunks_queue.size() > min_chunks || finalize) &&
         !chunks_queue.empty()) {
    lczero::TrainingDataWriter writer(
        files_written,
        dir_prefix + std::to_string(files_written / max_files_per_directory));
    for (size_t i = 0; i < chunks_per_file && !chunks_queue.empty(); ++i) {
      writer.WriteChunk(chunks_queue.front());
      chunks_queue.pop();
    }
    writer.Finalize();
    files_written++;
    if (!finalize) break;
  }
}

void TrainingDataWriter::Finalize() { WriteQueuedChunks(0, true); }
