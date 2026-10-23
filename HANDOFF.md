# Handoff: Polytopia AI (2026-10-22, evening: S3 self-play on the PC, public replay site)

Paste this file as the prompt for the next session. The design docs live in `docs/design/`:
`README.md` (decision log), `iteration-plan.md` (the night-1 plan and its status log),
`research-log.md` (every run: pre-registration, config, result).

## Goal and standing decisions (from Ben)

- **Goal:** a superhuman Polytopia AI for the World Championship format only: 1v1 Might, Lakes,
  Small 14×14, fog on, either seat any of the 12 regular tribes, game version 2.17.3 (123 / logic
  data 28). Plus a research write-up, so every run is pre-registered and logged.
- **Exact rules only:** the shipped game's own compiled code (`GameLogicAssembly`) runs the
  games; nothing is re-implemented. The real app is the reference.
- **Fog:** our agent sees only what its client would show (observation allowlist, fog-leak test).
  The game's built-in AI keeps its full view.
- **No hand-written evaluator.** Local hardware and local play only (no paid cloud, no official
  servers).
- **Working style:**
  - Quick, direct work; no subagent swarms.
  - Ask Ben when a decision is his, and wait for the answer.
  - Docs over long chat reports.
  - Commit to `main`.
- **Hardware:**
  - **Mac** (M2 Pro, 10 cores): runs all games, using the native `build/polyhost-mac` on Ben's
    Steam `GameAssembly.dylib`.
  - **PC `ssh backhouse`** (Windows 11 + WSL2 Ubuntu-22.04, RTX 4070 Ti SUPER 16 GB, 28 threads):
    training, and since 2026-10-22 **games too**. The Android build's own `libil2cpp.so` runs under
    QEMU user-mode emulation (`POLYHOST=qemu`, kit in `~/polyarm`, built by
    `tools/setup_pc_games.sh`). No Steam is needed. It's bit-identical to the Mac (`py/parity_hosts.py`)
    and ~22× slower per thread.
  - How to reach the PC (school Wi-Fi vs hotspot), launch jobs and share it: memory `backhouse-guide`.

## Now (2026-10-22, 19:00): S3 self-play running on the PC; public replay site live

- **S3** (`/home/greff/runs/sp/s3/`, research-log S3): self-play generations from R7, which is
  gen 0 (trained on all 7,732 human games).
  - Each generation: 1,500 games, half against itself and half against earlier generations.
  - Learning: PPO with the value baseline, **no human-data anchor** (Ben: don't memorize the human
    set), lr 3e-5, and a **KL limit of 0.05 per generation** (`--target_kl`).
  - Evaluations of each generation, beside the next one's games: vs the previous generation, vs
    Crazy (standard seeds) and vs gen 0.
  - Gen 0 vs Crazy, corrected: 188/200.
  - Gen 1's step stopped at the KL limit after 2,200 of 7,636 steps; its evaluations were running
    at 19:00.
  - Settings change between steps via `~/runs/sp/s3/override.json` (e.g. `{"lr": 1e-5}`). If gen 1
    helps, a lower lr spreads the KL budget over more of the games.
  - Stop: `touch ~/runs/sp/s3/STOP`. Resumable.
- **Staying up:**
  - The Windows task `PolytopiaWSLKeepalive` (at logon; hidden `wsl -d Ubuntu-22.04 -- sleep
    infinity`) keeps the distro alive: WSL kills a distro's jobs when its last wsl.exe session
    ends.
  - The Mac's `tools/site_update.sh s3 loop` restarts `sp_loop.py` if it isn't running and there is
    no `STOP` file.
  - On 2026-10-23 a Windows restart (17:08) had stopped the loop at gen 18.
  - Results to gen 17: vs Crazy 198/200; vs gen 0 up to 65.5% (gen 16).
  - How the training works: `docs/design/training-explained.md`.
- **S2 failed and was stopped:** no anchor at lr 1e-4 moved the policy too far (KL 1.6), and gen 1
  lost to gen 0. S1 (from R4, with the anchor) was stopped after 1 generation.
- **Code is public:** https://github.com/bengreff/polytopia (pushed 2026-10-23). Push after
  committing.
  - The history was rewritten to drop the August outreach drafts (`docs/outreach.md`) from every
    commit; the original is in `~/polytopia-history-backup-2026-10-23.bundle`.
- **Public site:** https://bengreff.github.io/polytopia-ai/ (repo bengreff/polytopia-ai, public).
  - It holds only the static site, exported board states and a names/stats extract; no game code
    or data files.
  - The results panel and replays (the whole board, moves/s speeds, games ranked by
    back-and-forth).
  - **Auto-update:** `tools/site_update.sh s3 loop` on the Mac (log `runs/sp/site_update_s3.log`)
    republishes after every evaluated generation: `py/rank_games.py`, then `py/export_site.py`,
    then git push. It needs the Mac awake and the link to the PC.
- **Local app:** `tools/play.sh …`, then "Watch recorded games":
  - sort and filter by back-and-forth, comeback, result or set;
  - moves/s speeds and a fitted board;
  - `#watch=<path>&speed=2.5` links.
- **Seat-2 mirror-match bug fixed** (research-log): every evaluation against the built-in bots had
  the network in seat 2 play the bot's tribe. The M4 gate needs re-running before it is quoted.
- **Speed:**
  - ~530 decisions/s on 28 game processes, CPU-bound; ~70 min per generation in the daytime.
  - polyhost checks moves once per unit: identical legal lists, 3.4× faster.
- **The PC is shared:** crucible (inquiry) runs only 10pm–6am. The loop uses 28 game processes in
  the daytime and 14 at night.
- **Connection:** from school Wi-Fi the PC is reachable only while it isn't paused in the eero app
  (memory `backhouse-guide`).

## Where things stand (morning of 2026-10-22)

**Fine-tuning on human championship replays made the raw policy (no search) beat Crazy about 92% of
the time.** Every cell below is 200 games, sides swapped, on fixed seeds; see research-log.md.

| Policy on our seat | Easy | Normal | Hard | Crazy |
|---|---|---|---|---|
| The game's own AI (the teacher) | 96.5% | 65.0% | 31.0% | 10.0% |
| R3 step 100k: behaviour cloning of the AI, 20k games | — | 59.0% | 29.0% | 9.0% |
| R4 step 6k: R3 at 100k + human fine-tune | — | 100% | 97.5% | 90.0% |
| **R4 step 20k: the reference policy** | — | 99.5% | 99.5% | **94.0%** |

- **R4 step 20k** (`runs/r4_human_ft/step020000.pt` on the Mac; `/home/greff/runs/r4_human_ft/` on
  the PC) is the best checkpoint.
  - Wins by early capital capture (~22 turns), as people do.
  - Picks the human's move 43% of the time (top-5 73%) on held-out human games.
- **Milestones:**
  - **M1 gate (≥ 60% vs Normal):** passed (99.5%).
  - **M2 thresholds** (Normal ≥ 90%, Hard ≥ 70%, Crazy ≥ 50%, all as lower bounds): passed by the
    raw policy, with no search.
  - **M4 gate:** full run on R4 at 20k, 20 games in each of 288 tribe-pair × seat cells vs Crazy.
    - Pooled: **92.4% (lower bound 91.7%): pass.**
    - **3 cells below 70%: fail, narrowly.** Oumaji vs Vengir (seat 1), Oumaji vs Xin-xi (seat
      1) and Xin-xi vs Vengir (seat 2): 65–70% over 40 games each, counting a re-test on fresh
      seeds.
    - The last M4 step is Ben's best-of-5.
- **Tried tonight, no gain** (each pre-registered, with its falsifier, in research-log.md):
  - **R5 / R5b, policy-only PPO** from R4: the greedy policy stayed flat after 1–2 iterations.
    The sampled policy (temperature 0.5) did improve, from 84% to 91% vs Crazy in its own games.
  - **R5c, PPO on the 19 weakest matchup cells:** flat after 2 iterations (316/380 against R4's
    328/380 on those cells).
  - **R6, imitating only the winning side of human games:** identical to R4 (586/600 each).
  - **R7, 20k more steps on the complete human set (7,732 games):** better imitation (top-1
    43.7%), the same play (Crazy 191 vs 188; weak cells 323 vs 328; pooled p = 0.91).
- **Watching games:**
  - `tools/play.sh runs/r4_human_ft/step020000.pt` serves the web app at http://localhost:8123.
    Started at 05:58 and left running.
  - "Watch recorded games" in the menu lists every recorded game. All evaluations record their
    games (`runs/games/<tag>/`), and Ben's own browser games are recorded under
    `runs/games/play_<date>/`.
  - `python3 py/send_games.py` puts chosen games at the top ("Sent by Claude"). 22 are sent: R4's
    12 losses vs Crazy, 6 losses in the weak matchups, 4 wins.
- **Running at handoff (06:00):**
  - **R3 was stopped** on Ben's instruction at step ~259k. The PC's GPU is free.
  - Nothing of mine is running. The Mac's queue runner was told to stop (`runs/queue/stop`); delete
    that file before restarting `tools/queue.sh`.
  - `polyhost-mac env 8600` (pid 8009) is a stale process from before tonight, not mine. Left
    alone.

## What exists (new tonight; the engine is described in README.md)

- **Training:**
  - **`py/train_bc.py`** (behaviour cloning):
    - pointer policy over the game's legal list, value and ownership heads;
    - several data sources with shares, `dirA:0.5,dirB:0.5`;
    - `--init`, `--winners_only`;
    - `--value_val_games N`: value checked on 16 positions from each of N held-out games.
  - **`py/train_rl.py`:** PPO on game outcomes with a per-opponent baseline, a BC anchor,
    `--temperature`.
  - **`py/dagger.py`:** the policy plays and the AI labels every position it reaches.
    - Also records the played row and its log-probability, so the same games serve RL.
    - Options: `--opponents` mix, `--tribe_pairs`, `--temperature`.
    - Network and game processes are pipelined.
  - **`tools/rl_loop.sh`:** RL iterations end to end: play on the Mac, sync, learn on the PC,
    fetch, evaluate. Env knobs: OPPONENTS, TEMP, LR, BCW, PAIRS. Every transfer is retried.
- **Evaluation:**
  - **`py/eval_vs_bot.py`:** fixed seeds, sides swapped, pipelined. Options:
    - `--cells K`: the M4 layout, K games per tribe pair × seat;
    - `--cell_list`;
    - `--teacher`;
    - `--remote`.
  - **`tools/eval.sh`** (writes `runs/evals/summary.tsv`), **`tools/compare_evals.py`** (paired
    sign test on the same seeds), **`tools/cells.py`** (per-tribe and per-cell report),
    **`py/human_bench.py`** (agreement with held-out human moves, value accuracy by game phase).
- **Playing it:** `tools/play.sh runs/r4_human_ft/step020000.pt`, then open http://localhost:8123
  and pick "Our network" as the bot difficulty.
  - polyhost's `serve` asks `py/agent_server.py` for the network seat's moves, with the same
    observation and legal list as evaluation.
  - Checked with a headless screenshot.
- **Infrastructure:**
  - **`tools/queue.sh`:** a sequential job queue on the Mac (`runs/queue/todo.txt`).
  - **`tools/fetch_ckpt.sh`, `tools/fetch_last.sh`:** copy checkpoints from the PC.
  - **`py/infer_server.py` + `py/remote.py`:** policy inference on the PC's GPU over an ssh tunnel.
    It matches the local network but is only fast when the GPU isn't training: the network is
    ~2 GFLOP per position.
- **Data:**
  - `data/human1`: 7,732 championship-format human games, 3.17M decisions; final, done by the other
    session.
    - The PC has R4's 6,741-file snapshot as `~/polydata/human1` and the full set as
      `~/polydata/human_full`.
  - `~/polydata/m1v2` (PC): 19,956 AI-teacher games.
  - `data/rl/*`: RL games.

## Engine and reference tooling (unchanged; details in README.md and memory `oracle-findings`)

- **`polyhost/`:** C++ that loads the game's il2cpp library with no Unity and calls its rules.
  - `env.hpp` (`env::Match`, built as the real app builds a pass-and-play game, byte-identical),
    `wire.hpp` (`polyhost env <port>`, client `py/polyenv.py`), `serve.hpp` + `web/`.
  - Commands include `envtest`, `envbench`, `hotseatcheck`, `replay`, `fogtest`.
  - `./polyhost/run.sh mac` builds `build/polyhost-mac`; `POLYHOST=mac` makes `polyenv.launch` use
    it. Build to a new file and `mv` it into place while games run: overwriting a running binary
    can kill them.
- **Replays:**
  - polyhost ops REPLAY/RSTEP.
  - `py/fetch_replays.py` (polytopia-prod API, Ben's client JWT), `py/cm_replays.py`
    (Challengermode), `py/convert_replays.py` (final-state check per game).
- **`game/`** (gitignored, Midjiwan's files; never commit):
  - the Android 2.17.3 bundle;
  - `dump/` (symbols, gamedata, type dump);
  - `tools/` (NDK, Cpp2IL, Frida server);
  - `ghidra/`, `traces/`, `.venv`.
  - `game/mac` links the Steam build's data.
- **Reference app:** AVD `PolyRef` (Android 14 google_apis arm64, rootable; shut down tonight).
  - Start: `emulator -avd PolyRef -no-window -no-snapshot -cores 6 -memory 4096 -gpu
    swiftshader_indirect`, then `adb root`, `adb shell setenforce 0`, and start
    `/data/local/tmp/frida-server`.
  - `tools/frida/` (trace.py, boundary recorder, agent.js autoplay) and `tools/ghidra/decompile.sh`
    are the oracle tools.

## Lessons from the night (gotchas)

- **GPU memory (16 GB):**
  - R3's process alone holds ~7 GB. Three of my jobs beside it made the driver page, and every job
    stalled for 20+ minutes (twice).
  - **Rule: with R3 running, only one more training job on the GPU.**
- **Launching jobs in WSL over ssh:** `setsid nohup … &` must be followed by a `sleep`.
  Otherwise WSL can end the session before the job starts.
- **`pkill -f pattern` over ssh matches its own `bash -c` command line;** use `'[t]rain_bc'`.
  A plain pattern once paused R3 and R4 for 2.5 minutes.
- **The Tailscale link to the PC drops for 30–60 s** a few times an hour. Retry every transfer.
- **Ports used by Ben's own servers on this Mac:** 8600, 8631, 8640, 8650, 8655, 8660, 8765,
  8771. Ours: 8500/8520 (evaluations), 8740 (extra evaluations), 8900 (RL actors), 8123/8131
  (play).
- **Mac evaluation speed:** inference-bound on the M2 Pro's GPU (~80 ms per batch of 36). About
  1.3–1.5 games/s for the short games R4 plays.
- **Permission check:** stopping R3 (a run from an earlier session) was refused as interfering with
  a workload. Ask Ben.

## Next steps

1. **Ben:**
   - plays R4 at 20k (`tools/play.sh`), best-of-5 for M4;
   - decides on the asks below.
2. **The weak matchups** (Oumaji vs Vengir / Xin-xi, Xin-xi vs Vengir) are what fails the M4 gate.
   RL on those cells (R5c) didn't help at this scale. Next to try:
   - more weight on human games with those tribes;
   - a look at how the losses happen (`tools/mapascii.py` on lost games);
   - then re-run the failing cells (`--cell_list`).
3. **Human data has plateaued** for play strength at this model size (R7). Options:
   - a larger network (learning curve flat, GPU has headroom), or a bigger window per batch;
   - more data, e.g. other formats as pretraining: the 14k Might Continents "Instant Tournament"
     games are listed in training-and-data.md.
4. **RL:** policy-only PPO at this scale hasn't improved greedy play. Before more RL:
   - get more throughput (the native Windows host, or the PC's GPU for inference once R3 is off
     it);
   - consider acting at a lower temperature.
5. **Search (M2's turn sampler):** blocked on a fog-safe clone, i.e. the determinized-state
   builder in polyhost (architecture.md, Layer 0). Simulating our own turn on the true state would
   reveal hidden tiles, ruin rewards and invisible units.
6. **Value head:** held-out value is far better on human games (log loss ~0.5–0.6) than on AI
   games (1.1–1.7, memorized). Needed before search.

## For Ben

1. ~~Stop R3~~: done, on Ben's instruction.
2. **Install Polytopia on the PC through Steam** (still not installed). A native Windows host
   would let the PC play games next to its GPU, the biggest throughput gain available.
3. **OK to build the determinized-state builder** (fog-safe search)?
4. **Push the repo to GitHub** (`gh repo create bengreff/polytopia --public --source . --remote
   origin --push`). Still blocked by the permission check; run it yourself with `!` or allow it.
