# trainingdata-tool

Tool to generate [lc0](https://github.com/LeelaChessZero/lc0) training data. Useful for [Supervised Learning](https://github.com/dkappe/leela-chess-weights/wiki/Supervised-Learning) from PGN games.

## How to build

### 1. Clone submodules

After cloning the repository locally, ensure all git submodules are initialized and updated:

```bash
git submodule sync --recursive
git submodule update --recursive --init
```

### 2. Build instructions

#### Linux (Ubuntu / Debian)

Install dependencies and build with CMake:

```bash
sudo apt-get update && sudo apt-get install -y cmake g++ zlib1g-dev

# Configure and build
cmake -S . -B build
cmake --build build -j$(nproc)
```

The binary will be located at `./build/trainingdata-tool`.

#### Windows (Visual Studio / MSVC)

Prerequisites: [Visual Studio 2019 or 2022](https://visualstudio.microsoft.com/) with the **"Desktop development with C++"** workload installed, or the standalone Visual Studio Build Tools with CMake.

*(Note: On Windows, `zlib` is bundled directly in the repository under `zlib/`, so no external zlib installation is required.)*

Open PowerShell or Developer PowerShell for VS and run:

```powershell
# 1. Configure CMake (generates Visual Studio solution in build/)
cmake -S . -B build

# 2. Build Release binary
cmake --build build --config Release
```

The compiled executable will be located at `.\build\Release\trainingdata-tool.exe`.

#### Windows (MinGW / GCC)

If you prefer compiling with MinGW-w64:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

The compiled executable will be located at `.\build\trainingdata-tool.exe`.

## Usage

Pass PGN input files and it will output training data in the same format lc0 selfplay produces:

```bash
# Linux
./build/trainingdata-tool games.pgn

# Windows
.\build\Release\trainingdata-tool.exe -pgn-eval-mode games.pgn
```

### Options

| Option | Description |
| --- | --- |
| `-v` | Verbose mode - shows detailed progress |
| `-pgn-eval-mode` | Read the eval already in each move's PGN comment (Fishtest/cutechess-cli's `SCORE/DEPTH TIMEs` format, e.g. `-0.76/18 1.813s`) instead of re-evaluating -- no engine spawned |
| `-wdl-scale <F>` | Scale for the fitted WDL model in `-pgn-eval-mode` (default: `1.13`, fitted -- see below). Affects both Q and D |
| `-wdl-spread <F>` | Spread for the fitted WDL model in `-pgn-eval-mode` (default: `0.21`, fitted -- see below). Affects both Q and D |
| `-visit-budget <N>` | Pseudo visit count written per position (default: `0` / one-hot). When set (e.g. `850`), sets total visits and distributes played move policy share as $0.5 + \|Q\|/2$, spreading remainder over other legal moves |
| `-r50-damp-start <N>` | Halfmove clock at which static evaluation starts decrementing toward a draw (default: `40`). Static mode only -- see below |
| `-stockfish <path>` | Use Stockfish binary to evaluate positions. Takes the engine's real WDL output directly, so the two flags above don't apply |
| `-sf-depth <N>` | Stockfish search depth (default: 10) |
| `-sf-hash <N>` | Stockfish hash table size in MB (default: 128) |
| `-files-per-dir <N>` | Max files per directory (default: 10000) |
| `-max-games-to-convert <N>` | Limit number of games to process |
| `-chunks-per-file <N>` | Chunks per output file in deduplication mode only; PGN conversion always writes one game per file |
| `-deduplication-mode` | Deduplicate existing training data |
| `-dedup-uniq-buffersize <N>` | Unique buffer size for deduplication mode (default: 50000) |
| `-dedup-q-ratio <F>` | Q-ratio threshold used during deduplication (default: 1.0) |
| `-threads <N>` | Number of worker threads for parallel game conversion (default: all CPU threads, e.g. `8`) |
| `-name <name>` | Custom dataset name prefix for output folders (default: `supervised` -> creates `supervised-0/`, `supervised-1/`, etc. E.g. `-name "Fishtest-New"` creates `Fishtest-New-0/`, `Fishtest-New-1/`) |
| `-output-dir <path>` | Target output directory where folders will be created (e.g. `-output-dir "C:/Users/.../training-data" -name "Fishtest"`) |
| `-output <prefix>` | Raw output prefix for generated training data files (default: `supervised-`) |

By default, the tool uses static evaluation unless `-stockfish` or `-pgn-eval-mode` is enabled.

### Examples

**Basic conversion:**

```bash
./build/trainingdata-tool games.pgn
```

**With Stockfish evaluation (generates Q-values):**

```bash
./build/trainingdata-tool -stockfish ./stockfish -sf-depth 15 games.pgn
```

**PGN eval mode (for games whose move comments already carry an eval, e.g. Fishtest PGNs):**

```bash
./build/trainingdata-tool -pgn-eval-mode fishtest_games.pgn
```

See [WDL reconstruction in PGN eval mode](#wdl-reconstruction-in-pgn-eval-mode) below for how `Q`/`D` are derived and how to tune target sharpness.

**Verbose with limits:**

```bash
./build/trainingdata-tool -v -max-games-to-convert 1000 -files-per-dir 500 games.pgn
```

## Output

Training data is written to `supervised-N/` directories containing `.gz` chunk files, one game per file.

## WDL reconstruction in PGN eval mode

### How the two eval modes differ

`-stockfish` mode asks the engine directly and gets a **real** WDL back (via `UCI_ShowWDL`), so `Q = (win - loss)/1000` and `D = draw/1000` are the engine's own numbers -- no model, no fitting. Nothing below applies to it.

`-pgn-eval-mode` only has the scalar in the PGN comment, so `(Q, D)` has to be reconstructed. That's what the rest of this section describes.

Both paths share `src/WdlConversion.h`, so a given centipawn value maps to the same `Q` either way. (If an engine doesn't report a WDL, `-stockfish` falls back to exactly the same reconstruction `-pgn-eval-mode` uses.)

### Why this exists

`-pgn-eval-mode` gives you a bare centipawn value per move, not a `(Q, D)` pair. `-stockfish` mode gets a real draw probability from Stockfish's live `UCI_ShowWDL` output, but a PGN comment has no such thing -- it's one number. So `Q` (win-loss) and `D` (draw probability) both have to be recovered from that single scalar:

**Both come from one model: lc0's `WDL_mu`, run backwards.** `search.cc` reports `score = 100 * mu` for that score type -- so `mu` is simply the eval in pawns, and no display formula needs inverting. Feed it into the same logistic pair `WDLRescale()` reconstructs with:

```
mu = scale · eval
W  = logistic((mu - 1) / spread)      L = logistic((-mu - 1) / spread)
Q  = W - L                            D = 1 - W - L
```

`Q` and `D` come out of the same `(W, L)`, so they're a consistent distribution by construction and can't disagree.

> **Do not use lc0's `centipawn` score type here.**
> `cp = 90·tan(1.5637541897·Q)` is a *display* convention for rendering Q as a centipawn-looking number -- it was never fitted to outcome frequencies. Measured against real results it's badly miscalibrated in both directions: at `+0.37` it claims `Q=+0.25` where the truth is `+0.02`, and at `+2.45` it claims `+0.78` where the truth is `+1.00`. `scripts/measure_pgn_wdl.py` reproduces that comparison.

**The two parameters are fitted against the real outcomes of the games being converted.** Bucket positions by the eval in their PGN comment, count the actual win/draw/loss frequencies, and fit `W` and `L` to them. Best fit: **`-wdl-scale 1.13`, `-wdl-spread 0.21`** (Root Mean Square Error: RMSE 0.015 on `(W, L)`), and it is stable across sources -- five separate Fishtest files fit to `scale` 1.11-1.27 and `spread` 0.20-0.21.

That `scale` lands near `1.0` is a consistency check, not luck: the model says `mu` *is* the eval, so a large correction would have meant the model was wrong.

Measured from one Fishtest file (6000 games), mover's perspective:

| eval | W | D | L | Q (real) | Q (model) |
| --- | --- | --- | --- | --- | --- |
| −2.46 | 0.000 | 0.002 | 0.998 | −0.998 | −1.000 |
| −1.19 | 0.001 | 0.159 | 0.840 | −0.839 | −0.836 |
| −0.65 | 0.004 | 0.782 | 0.214 | −0.210 | −0.217 |
| ±0.00 | 0.001 | **0.997** | 0.002 | −0.001 | −0.000 |
| +0.64 | 0.159 | 0.837 | 0.004 | +0.155 | +0.207 |
| +1.18 | 0.782 | 0.218 | 0.001 | +0.781 | +0.827 |
| +2.45 | 0.996 | 0.004 | 0.000 | +0.996 | +1.000 |

Note how sharply real engine chess behaves: draws dominate almost totally until about ±0.75, then it flips decisively. That shape is what the model has to reproduce, and it's why a gentler curve fits so poorly.

#### Why not lc0's `WDLDrawRateReference`?

Because that parameter describes the net you are *running* -- the lc0 blog tells you to look it up by running that net from startpos and reading its WDL output. Here we aren't running a net; we're generating training data, and these are Stockfish games with their own opening book, time control and adjudication rules, so a different draw-rate characteristic entirely. Targeting an lc0 net's draw rate would aim at the wrong distribution, and would be circular if that net is the one being trained. Concretely: fitting against lc0 self-play chunks gives `(2.3, 0.45)`, which scores a **Root Mean Square Error (RMSE) of 0.189** against this data versus **0.015** for the values above.

> **Caveat.** ~86% of Fishtest games end by adjudication, and cutechess adjudicates a draw precisely when the eval sits near zero (and a win when it stays high). So this curve is partly shaped by the adjudication rule rather than pure chess. It is still the real label distribution in the data, and it's self-consistent with `result_q`/`result_d`, which come from the same recorded results.

The result is then passed through the real, ported `WDLRescale()` (from `search.cc`) with the `(ratio, diff)` lc0 derives from its own neutral contempt/draw-rate defaults via `AccurateWDLRescaleParams()` (`params.cc`) -- a no-op at those defaults, but wired up faithfully so adding a contempt or draw-rate option later is a one-line change.

### The two knobs

| Flag | Default | Effect |
| --- | --- | --- |
| `-wdl-scale <F>` | `1.13` | Moves **where** the draw→decisive transition happens. Higher = transition at a smaller eval = more positions called decided. This is the main knob, and it is *not* phase-aware -- see below. |
| `-wdl-spread <F>` | `0.21` | Controls **how steep** the transition is, and sets the draw rate at an equal position. |

Both knobs move `Q` and `D` together, because both come out of the same pair of logistics -- there is no way to sharpen `D` while leaving `Q` alone, and that is the point: the triple stays consistent by construction.

Both exist because lc0's logistic anchors its transition at `mu = ±1`. `-wdl-spread` only stretches the curve vertically -- it cannot move where the transition sits. That's why spread alone can't match near-equal and decisive positions simultaneously. `-wdl-scale` supplies the missing degree of freedom.

`-wdl-spread` is not arbitrary: it's exactly lc0's `scale_reference`, so it can be *derived* from a draw rate rather than searched --

```
spread = 1 / log((1 + r) / (1 - r))        r = draw rate at an equal position
```

which inverts as `D(equal) = 1 - 2·logistic(-1/spread)`. `scripts/measure_pgn_draw_rate.py` reports both.

### Recommended values

**Use the defaults unless you have a specific reason not to:**

```bash
./build/trainingdata-tool -pgn-eval-mode games-finished.pgn
```

`1.13 / 0.21` is what actually matches the outcomes of the games being converted. Anything else is a deliberate distribution shift, not a correction -- you'd be training the value head toward different confidence than the source data supports. That may be exactly what you want, but do it knowingly.

### Making the targets sharper

**Raise `-wdl-scale`.** `D` collapses at a smaller eval, so advantages look decisively won instead of drawish:

Resulting draw probability `D` at a given eval (spread `0.21`):

| eval | `k=1.13` (default) | `k=1.4` | `k=1.8` | `k=2.8` |
| --- | --- | --- | --- | --- |
| `0.00` | 0.983 | 0.983 | 0.983 | 0.983 |
| `0.25` | 0.966 | 0.955 | 0.931 | 0.806 |
| `0.50` | 0.888 | 0.806 | 0.617 | 0.130 |
| `0.75` | 0.674 | 0.441 | 0.159 | 0.005 |
| `1.00` | 0.350 | 0.130 | 0.022 | 0.000 |
| `1.50` | 0.035 | 0.005 | 0.000 | 0.000 |
| `2.00` | 0.0025 | 0.0002 | ~0 | ~0 |

```bash
# sharper -- decisive by about 0.75
./build/trainingdata-tool -pgn-eval-mode -wdl-scale 1.4 games-finished.pgn
```

#### It is not a game-phase control

This is the easiest thing to get wrong about the knob, so it is worth being blunt: **`-wdl-scale` cannot make endgames sharper.** It maps an eval to a distribution and has no idea whether the board holds 32 pieces or 5. The scale sets the eval at which the model calls a position half-won, which is just `1 / scale`:

| scale | half-won at |
| --- | --- |
| `1.13` (default) | `0.88` |
| `1.4` | `0.71` |
| `1.8` | `0.56` |
| `2.8` | `0.36` |

Raising it does not reach *down* into decisive endgames -- those are already saturated. It reaches *up* into quieter positions, which in practice means **earlier** ones. Measured over 780,109 positions from 6,000 Fishtest games:

| band | share | what the knob does there |
| --- | --- | --- |
| \|eval\| >= 1.5 | 26.7% | nothing -- already `D<0.04` at every setting |
| 0.4 -- 1.5 | 49.4% | this is the only band that moves |
| \|eval\| < 0.4 | 23.9% | nothing until `k` gets large, then it collapses too |

At the default, endgames are already as sharp as the data allows: `D=0.035` by eval `1.50` and `0.002` by `2.00`. There is no headroom left to buy.

**Worked example of the trap.** Converting real Fishtest games at `-wdl-scale 1.8`, the very first position of a game -- an opening-book position, ply 0, with a PGN eval of `-0.78` -- came out as `Q=-0.873, D=0.127`. The measured outcome for that eval band (`0.75`--`1.00`, n=82,299) is `Q=0.445, D=0.550`. So `1.8` asserted an 87%-decided game before a single move had been played, in positions that really drew more than half the time. It never touched the endgame; it rewrote the opening.

| scale | Q at eval `0.78` | D |
| --- | --- | --- |
| `1.13` (default) | 0.362 | 0.637 |
| `1.3` | 0.517 | 0.483 |
| `1.4` | 0.608 | 0.392 |
| `1.8` | 0.873 | 0.127 |
| `2.8` | 0.996 | 0.004 |
| *measured* | *0.445* | *0.550* |

**Recommendation: stay at `1.13` unless you have a reason.** It is fitted to the real outcomes. If you want a deliberate nudge toward confidence, `1.3` straddles the measured figure rather than overshooting it; past `1.4` you are reshaping ordinary play, not endgames. If what you actually want is a value head that converts won endgames, this knob is not the lever -- the targets there are already maximal, and the problem lies in search or in the M head.

`-wdl-spread` sets the draw rate at equality and the transition steepness:

| `-wdl-spread` (at `wdl-scale=1.13`) | D at `0.00` | D at `0.75` | D at `2.00` |
| --- | --- | --- | --- |
| `0.14` | 0.998 | 0.748 | 0.0001 |
| `0.21` (default) | 0.983 | 0.674 | 0.0025 |
| `0.30` | 0.931 | 0.622 | 0.015 |
| `0.45` | 0.805 | 0.568 | 0.057 |

Lowering spread sharpens the decisive tail *and* makes equal positions more drawish at the same time. It is the only knob with any differential effect by eval magnitude, though the tail is already near zero so there is little to gain. Keep `-wdl-spread` at `0.21` unless you specifically want both effects.

### Making the targets less sharp

**Raise `-wdl-spread`** (the more effective knob here), or lower `-wdl-scale`:

```bash
# softer -- won positions retain noticeably more draw probability
./build/trainingdata-tool -pgn-eval-mode -wdl-spread 0.30 games-finished.pgn

# much softer, and less certain at equal too
./build/trainingdata-tool -pgn-eval-mode -wdl-spread 0.45 games-finished.pgn
```

**Caveat worth knowing:** raising spread softens the decisive end but *also* drags the draw rate at equality down (`D=0.98` → `0.81` going from `0.21` to `0.45`), because one parameter controls both. Lowering `-wdl-scale` softens while leaving equality untouched, but it flattens the whole curve, so a genuinely won position and a modest edge start looking alike. Neither knob acts on one part of the range in isolation -- pick which side effect you'd rather have, and re-measure afterwards.

### Policy Sharpness & Eval Distributions: What to Tweak and What NOT to Tweak

When converting PGN games with a pseudo visit budget (`-visit-budget 850`), the policy target confidence for the played move is derived from the position's evaluation ($Q$-score):
$$\text{played\_policy\_share} = \max(W, 1 - W) = 0.5 + \frac{|Q|}{2}$$
The remaining probability is distributed among the other legal moves. Because $Q$ comes directly from the logistic WDL formula with `-wdl-scale` and `-wdl-spread`, the sharpness of the policy head's targets is intimately tied to these parameters and the source PGN's evaluation profile.

#### 1. Real Eval Distribution in Fishtest PGNs (Measured across 96,000+ positions)

Analysis of real Fishtest test runs reveals how evaluations are distributed across a full test batch:

| Centipawn Evaluation Range | Percentage of Positions | Context in Game |
| --- | --- | --- |
| **$\le 0.50$ pawns** ($\le 50$ cp) | **21.8%** | Equal openings, quiet maneuvering, symmetrical endings |
| **$0.50$ – $1.50$ pawns** ($50$–$150$ cp) | **39.0%** | Significant initiative, pawn advantage, contested middlegames |
| **$1.50$ – $4.00$ pawns** ($150$–$400$ cp) | **16.4%** | Decisive tactical advantage, piece up |
| **$> 4.00$ pawns** ($> 400$ cp) | **22.8%** | Adjudication threshold band & endgame tails |

#### 2. The Sharpness Trap: Why `wdl-spread 0.21` Creates Near 100% 1-Hot Targets

At `-wdl-scale 1.13` and `-wdl-spread 0.21`, any evaluation past $+1.50$ pawns produces $Q \ge 0.965$. Since nearly **40% of positions in a typical Fishtest batch have evals $> 1.50$ pawns**, almost half the training dataset ends up with **$98.2\%$ to $100\%$ one-hot targets**:

| Eval (Pawns) | Current (`spread 0.21`) | Moderate (`spread 0.45`) | Smooth (`spread 0.60`) |
| --- | --- | --- | --- |
| **$0.00$** | $Q=0.000$ -> **50.0%** share | $Q=0.000$ -> **50.0%** share | $Q=0.000$ -> **50.0%** share |
| **$+0.25$** | $Q=0.030$ -> **51.5%** share | $Q=0.100$ -> **55.0%** share | $Q=0.112$ -> **55.6%** share |
| **$+0.50$** | $Q=0.111$ -> **55.6%** share | $Q=0.213$ -> **60.7%** share | $Q=0.227$ -> **61.4%** share |
| **$+0.75$** | $Q=0.326$ -> **66.3%** share | $Q=0.345$ -> **67.2%** share | $Q=0.346$ -> **67.3%** share |
| **$+1.00$** | $Q=0.650$ -> **82.5%** share | $Q=0.488$ -> **74.4%** share | $Q=0.466$ -> **73.3%** share |
| **$+1.50$** | $Q=0.965$ -> **98.2%** share *(saturated)* | $Q=0.748$ -> **87.4%** share | $Q=0.682$ -> **84.1%** share |
| **$+2.00$** | $Q=0.998$ -> **99.9%** share *(saturated)* | $Q=0.901$ -> **95.0%** share | $Q=0.834$ -> **91.7%** share |
| **$+3.00$** | $Q=1.000$ -> **100.0%** share | $Q=0.988$ -> **99.4%** share | $Q=0.964$ -> **98.2%** share |
| **$+4.00$** | $Q=1.000$ -> **100.0%** share | $Q=0.999$ -> **100.0%** share | $Q=0.995$ -> **99.8%** share |

#### 3. Practical Guidance: What to Tweak vs. What NOT to Tweak

##### What NOT to Tweak

1. **DO NOT raise `-wdl-scale` past `1.3` to seek "endgame sharpness"**:
  - `wdl-scale` does not know what game phase a position belongs to.
  - Raising `wdl-scale` to `1.8` forces early opening and middlegame moves evaluated at $\pm 0.78$ to be labeled as $87\%$ decided wins/losses when they really draw more than half the time. Endgames are already saturated at $\ge 1.50$ pawns and gain zero benefit.
2. **DO NOT use shallow depth-6 engine finishers to artificially extend games**:
  - Playing out adjudicated positions with low-depth search floods the dataset with $20\%+$ low-quality positions evaluated between $+5.00$ and $+80.00$, ruining `plies_left` targets and inflating policy loss.

##### What TO Tweak

1. **For an even, well-calibrated policy spread across all game phases**:
  - Use **`-wdl-spread 0.45` to `0.60`** with **`-wdl-scale 1.00` to `1.13`**.
  - This prevents middlegames with $+1.50$ pawns from collapsing into 1-hot targets and allows candidate moves to retain meaningful policy gradient until genuine conversion ($> 3.0$ pawns).
2. **For clean endgame training data**:
  - Rely on Cutechess/Fishtest adjudication rules (typically $+4.00$ to $+6.00$ pawns sustained over multiple plies).
  - Use Syzygy tablebase rescoring (`scripts/rescore_all.py` / `rescore_chunks.py`) to accurately correct terminal win/draw/loss labels and `plies_left` distance-to-mate.

### Re-fitting against your own data

If you switch to a different PGN source (different engines, time control, opening book, or adjudication settings), **re-measure rather than eyeballing** -- the draw-rate curve is a property of that data, not a universal constant.

**Start here.** This measures the target directly from the PGNs you're about to convert and fits *both* parameters at once:

```bash
py scripts/measure_pgn_wdl.py games.pgn.gz --limit 8000
```

It buckets positions by the signed eval in their comment (mover's perspective), counts the real win/draw/loss frequencies in each band, and grid-searches `(scale, spread)` against them. It prints the best pair and its RMSE on `(W, L)` -- feed those straight into `-wdl-scale` / `-wdl-spread`. It also prints what lc0's `centipawn` display formula would have claimed for the same bands, which is where the miscalibration quoted above comes from.

`scripts/measure_pgn_draw_rate.py` is the narrower version: draw rate per eval band plus the implied `-wdl-spread` for the near-equal band alone. Useful if you only care about the equality end, or as a cross-check on the spread the full fit picked.

The remaining scripts fit against **V6 chunks** rather than PGNs -- useful for inspecting an existing dataset or an lc0 self-play run, but remember those describe *that* distribution, not the PGNs you're converting:

| Script | What it does |
| --- | --- |
| `measure_draw_rate.py` | Measures a network's draw rate from its own V6 chunks, and converts it to the implied spread. The offline equivalent of reading `WDLDrawRateReference` off a running engine. |
| `calibrate_s.py` | Average/median real `D` for near-equal positions only. Quickest sanity check that a dataset looks like you expect. |
| `calibrate_s2.py` | Compares real `D` against the model across `|Q|` bands for candidate values. Shows *how* a setting is wrong, not just that it is. |
| `calibrate_s3.py` | Two-parameter `(scale, spread)` grid search over V6 chunks. |

If your chunks are packed in `.tar` archives (as lc0 training runs ship them), extract one first:

```bash
tar -xf training-run2-....tar -C /tmp/chunks
py scripts/calibrate_s3.py "/tmp/chunks/*/*.gz" 800
```

**One trap to avoid:** the two scales are *not* interchangeable. `Q=0.76` is a nearly-won position; an eval of `0.76` is only a modest edge. The model takes the **eval**, not `Q` -- `mu = scale * eval` -- so anything fitted against V6 chunks needs its `Q` mapped back to an eval first. Fit directly on `Q` and the numbers you get out will be nonsense: `D` saturates to ~0 by about `1.00` and nearly every position reads as decisively won. This is exactly the mistake the `centipawn` display formula invites, and it's why `measure_pgn_wdl.py` (which works from PGN evals) is the recommended fitting path.

## The 50-move rule in static eval

Static evaluation counts material and structure. It has no search behind it, so it cannot see a draw coming: a position a rook up reads as won even when the halfmove clock is at 99 and the game is drawn on the next ply. Left alone, that writes confidently winning targets for positions that are dead drawn.

Only two kinds of move reset the halfmove clock -- a **pawn move** (push or promotion) and a **capture** (including en passant). Everything else pushes it one ply closer to the 100-ply limit. So the penalty is attached to the clock the played move *leaves behind*:

```
c = (clock_after_move - damp_start) / (100 - damp_start)     clamped to [0, 1]

Q -> Q * (1 - c)
D -> D + (1 - D) * c
```

which is a straight interpolation toward a certain draw in W/D/L space, `(W,D,L) -> (1-c)·(W,D,L) + c·(0,1,0)`. At the limit it gives exactly `Q=0, D=1`.

Scoring the move's *resulting* clock rather than the one it inherited is the whole point: a capture or pawn push is scored at full value however long the shuffling before it ran, because those are the moves that make progress. A quiet move gets decremented by a little more each time. Shuffling a rook while a promotion is available is penalised; playing the promotion is not.

Below `-r50-damp-start` (default `40`, i.e. 20 full moves) nothing happens at all. A moderate clock carries no information -- twenty-odd plies of endgame maneuvering is ordinary play, not shuffling -- and damping from ply 1 would bias every long endgame toward a draw. Raise it to penalise only genuine shuffling; lower it to push the value head harder toward draws in slow endgames.

Worked example (rook up, clock starting at 60, `-r50-damp-start 40`):

| move | clock after | c | Q | D |
| --- | --- | --- | --- | --- |
| `Rh1` | 61 | 0.35 | 0.650 | 0.350 |
| `Rh2` | 63 | 0.38 | 0.617 | 0.383 |
| `Rh3` | 65 | 0.42 | 0.583 | 0.417 |
| `g8=Q` | **0** | 0.00 | **1.000** | **0.000** |
| `Qg4+` | 2 | 0.00 | 1.000 | 0.000 |

> **Static mode only.** `-stockfish` and `-pgn-eval-mode` take their numbers from a real engine, which already accounts for the rule -- Stockfish damps its own eval by the halfmove clock, and its search sees the terminal draw outright. Applying this on top would double-count it, so neither mode does. `-pgn-eval-mode` output is byte-identical with and without the flag.

Note that `D` is now populated in static mode. It was previously always `0.0`, which claims a certain decisive result for every position -- damping `Q` alone would have made that worse, producing `Q=0, D=0` ("certain, and equally likely won or lost") for exactly the drawn positions this is meant to describe. Both come from `wdl::ScoreToWDL`, the same model the other two modes use.

## Finishing prematurely-adjudicated games

Fishtest/cutechess-cli test games are usually stopped early by adjudication (a sustained eval imbalance) rather than played out to an actual checkmate, stalemate, or rule-based draw. That's fine for measuring engine strength, but it means the moves-left-head (M) training target -- which `trainingdata-tool` computes as real plies remaining until the *recorded* end of the game -- never gets a chance to count down to an actual conclusion; it just stops short wherever adjudication cut the game off.

`scripts/finish_games.py` fixes that up front, before conversion: for every game whose `[Termination]` is `"adjudication"`, it keeps playing both sides with Stockfish -- shallow and fast by default, since the goal is just a real conclusion, not a strong one -- until the position is actually checkmate, stalemate, or a claimable rule draw (50-move/repetition/insufficient material), or a safety ply cap is hit. Games already at a real conclusion are copied straight through unchanged.

Each added move gets an eval comment in the exact same self-relative `{SCORE/DEPTH TIMEs}` / `{+M<N>/DEPTH TIMEs}` format real Fishtest comments use, so the output needs no further changes to be read straight into `-pgn-eval-mode`.

```bash
py scripts/finish_games.py games.pgn.gz --stockfish "C:\path\to\stockfish.exe"
```

This writes `games-finished.pgn` next to the input by default. It accepts multiple input files, and both plain `.pgn` and gzip-compressed `.pgn.gz` (output is always plain `.pgn` -- `trainingdata-tool` doesn't read gzip PGNs directly). A progress bar shows games processed, how many were extended, and how many reached a real conclusion vs. hit the ply cap unresolved.

Only **decisive** adjudications get finished by default -- a game adjudicated as a draw already has the right result, and there's no mate distance being cut short to correct, so playing it out with a shallow engine would just burn time for no benefit (often grinding to the ply cap in an equal position instead of resolving). Pass `--include-draws` to finish those too anyway.

| Option | Description |
| --- | --- |
| `--stockfish <path>` | Path to the Stockfish binary (default: the copy under `Documents\Stockfish`) |
| `--depth <N>` | Search depth per continuation move (default: 10) |
| `--max-extra-plies <N>` | Safety cap on plies added per game (default: 300) |
| `--workers <N>` | Parallel worker processes, one Stockfish instance each (default: `cpu_count - 1`) |
| `--threads <N>` | UCI `Threads` per Stockfish instance (default: 1 -- parallelism comes from `--workers` instead) |
| `--hash <N>` | UCI `Hash` MB per Stockfish instance (default: 64) |
| `--only-termination <value>` | Only finish games with this `[Termination]` value (repeatable; default: just `adjudication`) |
| `--include-draws` | Also finish games adjudicated as a draw (default: skipped -- see above) |
| `--output <path>` | Output path (single input file only) |
| `--output-dir <dir>` | Directory for outputs (multiple inputs) |
| `--limit <N>` | Stop after this many games per input file (handy for a quick test run) |
| `--resume` | Skip inputs whose finished output already exists -- see below |
| `--no-progress` | Disable the progress bar (and the game-counting pre-pass it needs) |

Quick test on a handful of games before committing to a full run:

```bash
py scripts/finish_games.py games.pgn --limit 20 --depth 8 --workers 2
```

### Surviving an interrupted run

A full multi-file run takes hours, and anything that kills the shell kills it. Two things make that cheap to recover from:

- Each output is written to `<name>-finished.pgn.partial` and renamed only after the file completes. A killed run therefore never leaves a truncated file under the real name.
- `--resume` skips any input whose finished output already exists, so a restart picks up at the file that was in flight.

```bash
py -u scripts/finish_games.py *.pgn.gz --output-dir finished-all --resume
```

Use `py -u`. Without it, stdout is block-buffered when redirected to a log, and the per-file `Done:` summaries are lost if the run is killed -- leaving no record of which files finished. The progress bar still appears either way (it goes to stderr), which makes the loss easy to miss.

> **One caveat.** An output left behind by a run from *before* the `.partial` mechanism existed may be truncated, and `--resume` will treat it as complete. Delete the most recently written output before resuming over such a run. To check a file rather than guess, compare game counts: `grep -c '^\[Event ' out.pgn` against the same count in the input.

Then feed the result straight into conversion:

```bash
./build/trainingdata-tool -pgn-eval-mode games-finished.pgn
```

## Rescoring with Syzygy tablebases

Rescoring replaces guessed labels with the truth: any position that reaches a tablebase gets its real game-theoretic result and its real distance to the end. That corrects both `result_q`/`result_d` and the `plies_left` (M) target.

`scripts/rescore_chunks.py` drives lc0's `rescore_chunk` across a whole tree:

```bash
py scripts/rescore_chunks.py C:\path\to\chunks --syzygy C:\path\to\syzygy --replace
```

| Option | Description |
| --- | --- |
| `--syzygy <dir>` | Tablebase directory (default: the local `syzygy-4-5`) |
| `--rescorer <path>` | `rescore_chunk` binary (default: the local build) |
| `--workers <N>` | Parallel processes (default: `cpu_count - 1`) |
| `--replace` | Move each rescored chunk over its original once written -- keeps disk flat |
| `--resume` | Skip chunks already carrying a `_rescored.gz` twin |
| `--dist-temp`, `--dist-offset`, `--dtz-boost` | Passed through to the rescorer |

**Why this binary and not lc0's `rescorer`.** The standalone `rescorer` takes a whole directory in one process, but `--delete-files` defaults to *true* and its `remove()` sits outside the try/catch -- so it deletes its inputs on failure as well as on success. `rescore_chunk` has no delete logic at all: it reads one chunk and writes `<stem>_rescored.gz` beside it. This driver adds the parallelism that costs, and never removes an original except via `--replace`, and then only after a confirmed successful write.

Budget roughly 0.12s per chunk per worker, including tablebase init (which is mmap'd and cheap). A million chunks is a few hours across 7 workers.

### What it actually changes

Measured over 20 converted Fishtest games (3,307 frames, 3-4-5 tablebases):

| field | frames changed |
| --- | --- |
| `plies_left` | 27.3% |
| `result_q` | 18.1% |
| `result_d` | 18.1% |

That `result_q` figure is not noise, and it is worth understanding. Games finished by a shallow search routinely fail to convert won positions: one sampled game reached a rook-up position evaluated at `+4.7`, shuffled (`Re4 Re8 Re4 Rd4`) without making progress, and was recorded `1/2-1/2` by the 50-move rule. The tablebase relabels it as the win it was.

So finishing and rescoring are complementary, not alternatives -- finishing gets the game to a real conclusion, rescoring fixes the conclusions the finisher got wrong. Deeper `--depth` in `finish_games.py` reduces how many need fixing, and larger tablebases catch more of the rest; with 3-4-5 only, a game that shuffles into a 50-move draw with six pieces on the board stays mislabelled.

> **Expect MLH warnings afterwards.** `verify_chunks.py` checks `plies_left` against a simple ply-order count, which is right for freshly converted chunks and *wrong by design* after rescoring -- the whole point is that M now reflects real distance-to-conclusion. Mismatches there are evidence the rescorer worked, not that something broke.

## Packing chunks into archives

`scripts/pack_chunks.py` turns the converted directory tree into `.tar` archives laid out the way lc0 ships its training runs (members stored as `<dir>/<chunk>.gz`, so extracting recreates the layout):

```bash
py scripts/pack_chunks.py C:\path\to\chunks --output-dir C:\path\to\archives
```

| Option | Description |
| --- | --- |
| `--output-dir <dir>` | Where archives are written (required) |
| `--group <N>` | Chunk directories per archive (default: 1) |
| `--compress none\|gz` | Default `none` -- see below |
| `--resume` | Skip archives that already exist |
| `--no-verify` | Skip re-reading each archive to confirm its member count |

Each archive is written to a `.partial` and renamed only after its member count is verified, so an interrupted run never leaves a truncated `.tar` looking complete. Sources are never modified.

**Compression defaults to none on purpose.** The members are already gzipped chunks; re-compressing the tar buys almost nothing for a lot of CPU, which is why lc0 distributes plain `.tar`. `--compress gz` exists if you want to measure it yourself.

> **These archives are for storage and transfer, not for training directly.**
> Nothing in `tf/` reads tars -- `train.py:fast_get_chunks` walks one level of subdirectories collecting loose `.gz` files, and `chunkparser.py` opens each one with `gzip.open`. Extract before training:
> 
> ```bash
> tar -xf archives/sup01-0.tar -C /path/to/chunks
> ```

## Verifying converted chunks

`scripts/verify_chunks.py` reads the `.gz` chunk files `trainingdata-tool` writes and prints the decoded `V6TrainingData` fields per move -- useful for sanity-checking a conversion, especially the moves-left (M) target after using `finish_games.py`. It reads the real on-disk `plies_left`/`root_m`/`best_m`/`played_m` fields (not a guess) and cross-checks `plies_left` against the ply-order count it should have; it should count down to exactly 0 on the real final move of the game, and the script flags any chunk where the two disagree.

```bash
py scripts/verify_chunks.py supervised-0/game_000000.gz
```

Pass a directory instead of a single file to walk every `.gz` chunk under it:

```bash
py scripts/verify_chunks.py supervised-0/
```

For each move it prints `PliesLeft` (the M target), `RootQ`/`BestQ`/`ResultQ`, the played/best move indices, visit count, rule50 count, and castling rights, plus a running total of moves processed across all files.
