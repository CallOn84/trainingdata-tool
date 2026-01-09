#if !defined(TRAININGDATA_H_INCLUDED)
#define TRAININGDATA_H_INCLUDED

#include "neural/encoder.h"
#include "neural/network.h"
#include "trainingdata/trainingdata_v6.h"

lczero::V6TrainingData get_v6_training_data(
        lczero::GameResult game_result, const lczero::PositionHistory& history,
        lczero::Move played_move, lczero::MoveList legal_moves, float Q,
        lczero::Move best_move, uint32_t visits);

#endif