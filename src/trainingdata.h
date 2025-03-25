#if !defined(TRAININGDATA_H_INCLUDED)
#define TRAININGDATA_H_INCLUDED

#include "neural/encoder.h"
#include "neural/network.h"
#include "trainingdata/writer.h"

lczero::V6TrainingData get_v6_training_data(
        const lczero::PositionHistory& history,
        lczero::GameResult result,
        lczero::Move best_move,
        lczero::Move played_move,
        lczero::MoveList legal_moves,
        float Q,
        bool lichess_mode);

#endif
