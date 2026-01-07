#ifndef TRAININGDATA_TOOL_TRAININGDATAREADER_H
#define TRAININGDATA_TOOL_TRAININGDATAREADER_H

#include <optional>
#include <vector>
#include <string>
#include <zlib.h>

#include "trainingdata/trainingdata_v6.h"

class TrainingDataReader {
public:
  TrainingDataReader(const std::string &in_directory);
  virtual ~TrainingDataReader();
  std::optional<lczero::V6TrainingData> ReadChunk();

private:
  gzFile getCurrentFile();
  std::vector<std::string> in_files;
  std::vector<std::string>::iterator in_files_it;
  gzFile file;
};

#endif