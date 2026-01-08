#ifndef TRAININGDATA_TOOL_V6TRAININGDATAHASHUTIL_H
#define TRAININGDATA_TOOL_V6TRAININGDATAHASHUTIL_H

#include "utils/hashcat.h"
#include "trainingdata/trainingdata_v6.h"

#define ARR_LENGTH(a) (sizeof(a) / sizeof(a[0]))

namespace std {
template <>
struct hash<lczero::V6TrainingData> {
  size_t operator()(const lczero::V6TrainingData& k) const {
    // Hash the planes array using lc0's HashCat
    uint64_t hash = 0;
    for (size_t i = 0; i < ARR_LENGTH(k.planes); ++i) {
      hash = lczero::HashCat(hash, k.planes[i]);
    }
    // Combine with other fields
    hash = lczero::HashCat(hash, k.castling_us_ooo);
    hash = lczero::HashCat(hash, k.castling_us_oo);
    hash = lczero::HashCat(hash, k.castling_them_ooo);
    hash = lczero::HashCat(hash, k.castling_them_oo);
    hash = lczero::HashCat(hash, k.side_to_move_or_enpassant);
    hash = lczero::HashCat(hash, k.rule50_count);
    return static_cast<size_t>(hash);
  }
};

template <>
struct equal_to<lczero::V6TrainingData> {
  bool operator()(const lczero::V6TrainingData& lhs,
                  const lczero::V6TrainingData& rhs) const {
    return std::equal(lhs.planes, lhs.planes + ARR_LENGTH(lhs.planes),
                      rhs.planes) &&
           lhs.castling_us_ooo == rhs.castling_us_ooo &&
           lhs.castling_us_oo == rhs.castling_us_oo &&
           lhs.castling_them_ooo == rhs.castling_them_ooo &&
           lhs.castling_them_oo == rhs.castling_them_oo &&
           lhs.side_to_move_or_enpassant == rhs.side_to_move_or_enpassant &&
           lhs.rule50_count == rhs.rule50_count;
  }
};
}  // namespace std

#endif  // TRAININGDATA_TOOL_V6TRAININGDATAHASHUTIL_H