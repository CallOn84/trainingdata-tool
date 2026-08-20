#ifndef TRAININGDATA_TOOL_TRAININGDATAWRITER_H
#define TRAININGDATA_TOOL_TRAININGDATAWRITER_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "neural/encoder.h"
#include "neural/network.h"
#include "trainingdata/trainingdata_v6.h"

#include "V6TrainingDataHashUtil.h"

class TrainingDataWriter {
 public:
  TrainingDataWriter(size_t max_files_per_directory, size_t chunks_per_file,
                     std::string dir_prefix = "supervised-");

  void EnqueueChunks(const std::vector<lczero::V6TrainingData>& chunks);
  void EnqueueChunks(
      const std::unordered_map<lczero::V6TrainingData, size_t>& chunks);

  void Finalize();

  size_t FilesWritten() const;

 private:
  void WriteQueuedChunks(size_t min_chunks);

  mutable std::mutex mutex_;
  std::queue<lczero::V6TrainingData> chunks_queue;
  size_t files_written;
  // Directories already created, so the common case costs a hash lookup
  // instead of a filesystem call per game.
  std::unordered_set<std::string> created_dirs_;
  size_t max_files_per_directory;
  size_t chunks_per_file;
  const std::string dir_prefix;
};

#endif