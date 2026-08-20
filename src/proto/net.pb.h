#pragma once

namespace pblczero {
class NetworkFormat {
 public:
  enum InputFormat {
    INPUT_UNKNOWN = 0,
    INPUT_CLASSICAL_112_PLANE = 1,
    INPUT_112_WITH_CASTLING_PLANE = 2,
    INPUT_112_WITH_CANONICALIZATION = 3,
    INPUT_112_WITH_CANONICALIZATION_HECTOPLIES = 4,
    INPUT_112_WITH_CANONICALIZATION_HECTOPLIES_ARMAGEDDON = 132,
    INPUT_112_WITH_CANONICALIZATION_V2 = 5,
    INPUT_112_WITH_CANONICALIZATION_V2_ARMAGEDDON = 133,
  };
  enum OutputFormat {
    OUTPUT_UNKNOWN = 0,
    OUTPUT_CLASSICAL = 1,
    OUTPUT_WDL = 2,
  };
  enum MovesLeftFormat {
    MOVES_LEFT_NONE = 0,
    MOVES_LEFT_V1 = 1,
  };
};

// Minimal Net definition
class Net {
public:
    // Add members if compilation fails due to missing members
};

}
