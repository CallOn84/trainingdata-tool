#!/usr/bin/env python3
import struct
import gzip
import os
import sys

# V6TrainingData structure layout
# uint32_t version;
# uint32_t input_format;
# float probabilities[1858];
# uint64_t planes[104];
# uint8_t castling_us_ooo;
# uint8_t castling_us_oo;
# uint8_t castling_them_ooo;
# uint8_t castling_them_oo;
# uint8_t side_to_move_or_enpassant;
# uint8_t rule50_count;
# uint8_t invariance_info;
# uint8_t dummy;
# float root_q;
# float best_q;
# float root_d;
# float best_d;
# float root_m;
# float best_m;
# float plies_left;
# float result_q;
# float result_d;
# float played_q;
# float played_d;
# float played_m;
# float orig_q;
# float orig_d;
# float orig_m;
# uint32_t visits;
# uint16_t played_idx;
# uint16_t best_idx;
# float policy_kld;
# uint32_t reserved;

STRUCT_FMT = (
    "<"      # Little endian
    "I"      # version
    "I"      # input_format
    "1858f"  # probabilities
    "104Q"   # planes
    "8B"     # castling/rule50/invariance/dummy
    "15f"    # root_q ... orig_m
    "I"      # visits
    "H"      # played_idx
    "H"      # best_idx
    "f"      # policy_kld
    "I"      # reserved
)

STRUCT_SIZE = 8356

def read_chunks(filename):
    print(f"Reading {filename}...")
    try:
        with gzip.open(filename, "rb") as f:
            while True:
                data = f.read(STRUCT_SIZE)
                if not data:
                    break
                if len(data) != STRUCT_SIZE:
                    raise ValueError(
                        f"Incomplete chunk in {filename}: got {len(data)} bytes, "
                        f"expected {STRUCT_SIZE}"
                    )

                unpacked = struct.unpack(STRUCT_FMT, data)
                
                # Extract relevant fields for verification
                version = unpacked[0]
                input_format = unpacked[1]
                # probabilities start at index 2, end at 2+1858
                probs_end = 2 + 1858
                # planes start at probs_end, end at probs_end+104
                planes_end = probs_end + 104
                # uint8s start at planes_end
                uint8s_start = planes_end
                castling_us_ooo = unpacked[uint8s_start]
                castling_us_oo = unpacked[uint8s_start+1]
                castling_them_ooo = unpacked[uint8s_start+2]
                castling_them_oo = unpacked[uint8s_start+3]
                side_to_move = unpacked[uint8s_start+4]
                rule50 = unpacked[uint8s_start+5]
                invariance = unpacked[uint8s_start+6]
                dummy = unpacked[uint8s_start+7]

                # Check 15 floats starting after uint8s: root_q, best_q,
                # root_d, best_d, root_m, best_m, plies_left, result_q,
                # result_d, played_q, played_d, played_m, orig_q, orig_d,
                # orig_m -- in that exact struct order.
                floats_start = planes_end + 8
                root_q = unpacked[floats_start]
                best_q = unpacked[floats_start+1]
                root_d = unpacked[floats_start+2]
                best_d = unpacked[floats_start+3]
                root_m = unpacked[floats_start+4]
                best_m = unpacked[floats_start+5]
                plies_left = unpacked[floats_start+6]
                result_q = unpacked[floats_start+7]
                result_d = unpacked[floats_start+8]
                played_q = unpacked[floats_start+9]
                played_d = unpacked[floats_start+10]
                played_m = unpacked[floats_start+11]

                # Check indices
                visits = unpacked[floats_start+15]
                played_idx = unpacked[floats_start+16]
                best_idx = unpacked[floats_start+17]
                policy_kld = unpacked[floats_start+18]

                # The policy target itself: 1858 float32 at byte offset 8,
                # i.e. tuple indices 2..1860. Illegal moves are -1, legal ones
                # carry their share. This is what the trainer's policy loss
                # reads (chunkparser.py unpacks it as `probs`), so it is the
                # field -visit-budget actually changes.
                probs = unpacked[2:probs_end]
                legal = [p for p in probs if p >= 0.0]
                played_p = (probs[played_idx]
                            if 0 <= played_idx < 1858 else float("nan"))
                others = [p for p in legal if p != played_p]

                yield {
                    "n_legal": len(legal),
                    "played_p": played_p,
                    "policy_sum": sum(legal),
                    "other_p": (max(others) if others else 0.0),
                    "version": version,
                    "input_format": input_format,
                    "root_q": root_q,
                    "best_q": best_q,
                    "root_d": root_d,
                    "best_d": best_d,
                    "root_m": root_m,
                    "best_m": best_m,
                    "plies_left": plies_left,
                    "result_q": result_q,
                    "result_d": result_d,
                    "played_q": played_q,
                    "played_d": played_d,
                    "played_m": played_m,
                    "visits": visits,
                    "played_idx": played_idx,
                    "best_idx": best_idx,
                    "policy_kld": policy_kld,
                    "rule50": rule50,
                    "castling": (castling_us_ooo, castling_us_oo, castling_them_ooo, castling_them_oo),
                    "raw_size": len(data)
                }
    except Exception as e:
        raise RuntimeError(f"Error reading {filename}: {e}") from e

def main():
    if len(sys.argv) < 2:
        print("Usage: verify_chunks.py <path_to_gzipped_chunk_file> [dir]")
        sys.exit(1)

    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    # --policy prints the policy target (probabilities[1858] at byte offset 8)
    # instead of the scalar fields. That array is what the trainer's policy
    # loss reads, and it is what -visit-budget rewrites.
    show_policy = "--policy" in sys.argv
    path = args[0]

    files = []
    if os.path.isdir(path):
        for root, _, filenames in os.walk(path):
            for f in filenames:
                if f.endswith(".gz"):
                    files.append(os.path.join(root, f))
    else:
        files.append(path)

    total_moves = 0
    total_mlh_mismatches = 0
    for f in sorted(files):
        print(f"--- File: {f} ---")
        moves = list(read_chunks(f))
        total_moves += len(moves)
        num_moves = len(moves)
        for i, move in enumerate(moves):
            # Expected MLH value if this file is one game in ply order (true
            # for trainingdata-tool's PGN conversion output, which always
            # writes one game per file). This is just an expectation to
            # check the stored field against -- it is NOT read from disk.
            expected_plies_left = num_moves - i - 1
            stored_plies_left = move["plies_left"]
            mismatch = ""
            if abs(stored_plies_left - expected_plies_left) > 1e-3:
                mismatch = (f" <<< MISMATCH: on-disk plies_left="
                            f"{stored_plies_left:.2f}, expected "
                            f"{expected_plies_left}")
                total_mlh_mismatches += 1
            if show_policy:
                visits = move["visits"]
                print(f"  Move {i} (MoveId={move['played_idx']}): "
                      f"RootQ={move['root_q']:+.4f}, "
                      f"PlayedPolicy={move['played_p']:.4f}, "
                      f"PlayedVisits={move['played_p']*visits:7.1f} of "
                      f"{visits}, LegalMoves={move['n_legal']}, "
                      f"EachOther={move['other_p']:.5f}, "
                      f"PolicySum={move['policy_sum']:.4f}{mismatch}")
                continue
            print(f"  Move {i} (MoveId={move['played_idx']}): "
                  f"PliesLeft={stored_plies_left:.2f}{mismatch}, "
                  f"RootM={move['root_m']:.2f}, BestM={move['best_m']:.2f}, "
                  f"PlayedM={move['played_m']:.2f}, "
                  f"Version={move['version']}, Format={move['input_format']}, "
                  f"ResultQ={move['result_q']:.4f}, RootQ={move['root_q']:.4f}, BestQ={move['best_q']:.4f}, "
                  f"PlayedQ={move['played_q']:.4f}, "
                  f"ResultD={move['result_d']:.4f}, RootD={move['root_d']:.4f}, BestD={move['best_d']:.4f}, "
                  f"PlayedD={move['played_d']:.4f}, "
                  f"PlayedIdx={move['played_idx']}, BestIdx={move['best_idx']}, Visits={move['visits']}, "
                  f"Rule50={move['rule50']}, Castling={move['castling']}")
        print(f"  Total moves in file: {num_moves}\n")

    print(f"Total moves processed: {total_moves}")
    if total_mlh_mismatches:
        print(f"WARNING: {total_mlh_mismatches} move(s) had an on-disk "
              f"plies_left that didn't match the expected ply-order count "
              f"-- the MLH target may not be what you expect for those "
              f"chunks.")
    else:
        print("MLH check: every chunk's on-disk plies_left matched its "
              "expected ply-order count.")

if __name__ == "__main__":
    main()
