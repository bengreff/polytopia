# Training and data (v5)

**Goals:**
- Beat the default bots within ~2 days of the infrastructure being ready.
- Then background self-play with no deadline, aiming at top-human play and a research write-up.
- Ben checks in at every stage.

**Constraints:** local hardware only, local play only, no hand-written evaluator.
- **Mac:** M2 Pro, 10 cores, 16 GB; runs the Android emulator.
- **PC `backhouse`** (ssh): i7-14700K (20 cores / 28 threads), 64 GB, **RTX 4070 Ti SUPER
  16 GB**, Windows 11 + WSL2 (Ubuntu 22.04), 1.8 TB free. Reached over Tailscale on Wi-Fi 6E
  (~10 ms round trip, ~400 Mb/s).

## Milestones

Each milestone has a definition of done (DoD), a check-in with Ben, and kill criteria. The order of
work was set by the strategy review: **prove learning works with the smallest loop first, and add
layers only when a metric asks for them.**

### M0 — infrastructure and measurement (polyhost agent, with spikes)
- The Layer 0 contract (architecture.md).
- **Microbenchmarks on 14×14 Lakes:** clone, legal-command generation, encoding. Then
  **end-to-end decisions per second with a dummy network.**
- **Bot checks:**
  - Can we create a Crazy-brain, Normal-income bot? Does aggression read the handicap?
  - Are bots omniscient? Hook `GetExplored` / `IsInvisible` during `AI.GetMove`.
  - Do two human seats get handicap 1, i.e. does a level-1 capital yield 2★?
- **Spike: native Windows build — top priority, timeboxed to 2 days.**
  - Ben owns Polytopia on Steam; it isn't installed on the PC yet. Host the Steam
    `GameAssembly.dll` natively on the PC.
  - It must match the Android build's game version, and pass a byte-identical replay test against
    the Android build.
  - **If it works, the PC does everything** (20 cores of actors + GPU inference + learner, with
    no network hop), and the Mac becomes the dev box plus spare actors.
  - This is the single biggest throughput gain available: about 4× the Mac's effective actor
    cores, and no emulator.
- **Spike: replay ingestion, timeboxed to 1 day.** Load one *old* share-link game end to end. Never
  block a milestone on it.
- **DoD:** the cost-model table in architecture.md is filled with measured numbers; the vocabulary,
  fog-leak and determinism tests pass.

### M1 — smallest learning loop (target: ≤ 2 days after M0)
- **Data:** 20k bot games, Lakes Small, all tribes, both seats. The acting bot is Crazy-brain with
  Normal income if possible. If bots are omniscient, drop imitation of moves that depend on
  unexplored information.
- **Model:** ~5M-parameter trunk, pointer policy, value head, one ownership aux head. Behaviour
  cloning only. No search.
- **DoD:** the raw policy wins **≥ 60% vs Normal** over 400 games (200 fixed seeds, sides swapped).
  Report the Wilson lower bound.
- **Ben:** plays 2 games against it.
- **Kill:** not met within 2 weeks of agent time → re-scope (rules access, encoding, or data
  problem), don't add layers.

### M2 — turn sampler: "beats the default bots" (Ben: default = **Crazy**)
- N whole-turn samples from the policy, scored by end-of-turn V (architecture.md Layer 2).
- **Curriculum** Easy → Normal → Hard → Crazy. Ben: even Crazy bots are quite stupid; their edge
  is economic (+3★ per turn), not skill.
- **DoD:**
  - **vs Crazy:** pooled Wilson lower bound **≥ 50%** (it wins more than it loses, with confidence);
  - on the way: Normal ≥ 90% and Hard ≥ 70% (lower bounds);
  - search gain measured and positive.
- **Ben:** plays it.

### M3 — decision experiment: PPO vs expert iteration
- Two arms from the same M2 checkpoint, **equal simulator time** (48 h each, run back to back):
  - **(a) Regularized PPO:** KL magnet, top-25% advantage filtering, EMA weights, sparse reward +
    aux heads; search only at play time.
  - **(b) Gumbel expert iteration:** per-decision search on 25% of turns.
- Both arms train against the same opponent mix and are scored by Elo against the same frozen
  anchor set.
- **Rule:** expert iteration must win by ≥ 50 Elo to justify its ~15× simulator cost.
- **Ben signs off** on the result.

### M4 — "beats Ben", then the background run
- **Gate:**
  - pooled win rate vs **Crazy** (a +3★ per turn edge), Wilson lower bound ≥ 90%;
  - **no tribe-pair × seat cell below 70%** (cells with ≥ 20 games);
  - then Ben loses a best-of-5.
- **Ben:** about 3 logged games a week from then on. He picks the tribes, and his notes on each loss
  go into a regression set.
- **Background run:** the M3 winner, with heavy play-time search.

### M5+ — layers, each added only on evidence

| Layer | Trigger |
|---|---|
| Belief sampler | Losses traced to cloaks or unexplored tiles; capital-safety misses |
| Play-time reply check | Losses to 2-turn capital strikes |
| Symmetry augmentation | After polyhost verifies rule invariance |
| Human-replay regularizer and opening prior | Weak openings vs the championship move-agreement benchmark |
| Exploiter runs | Every 1–2 weeks once strong (exploitability proxy) |
| Larger network | Learning curve flattens while the GPU has headroom |

### Kill and stop criteria (always on)
- **Elo flat** against the frozen anchor over 3 equal-budget readings (400 games each): stop.
  Do a loss autopsy, and **do not stack changes**.
- **Any CI test failing:** that run doesn't count.
- **Draw rate at the 60-turn cap rising:** investigate passivity. Capped games are dropped from the
  value loss, or adjudicated with the aux heads, never scored as draws.

## Gate statistics
- Fixed evaluation seed sets with sides swapped. Every win rate is reported with its Wilson 95%
  interval.
- Per-cell gates use a floor (70%), not ≥ 90% per cell. At 20–50 games per cell, a true 93–95%
  agent would fail a per-cell 90% gate by chance in many of the 288 cells.
- **Promotion:** a sequential probability ratio test (SPRT) instead of "55% over 400" (only 2
  standard errors), or no gating with EMA weights, as KataGo does.

## Data

### Bots
- Unlimited and available now.
- **Crazy has +3★ per turn over a human** (handicap 4 vs 1), which makes it harder and distorts its
  spending as a teacher. Prefer Crazy-brain with Normal income for imitation data; keep real
  Crazy for gates.
- Bots make deliberate mistakes (`StupidFactor`) and are probably omniscient. **Use them for
  positions and outcomes more than for moves.**

### Championship replays (human)
- **Links:** `share.polytopia.io/g/<GUID>` is only a landing page; the game client downloads the
  replay from Midjiwan's servers. Ingestion goes through the client's own replay path, and
  probably needs an authenticated session.
- **Download path (found 2026-10-21):** `POST https://polytopia-prod.net/api/game/get_game_view_model`
  (fallback `spectate_game`), body `{"GameId": "<GUID>"}`, header `Authorization: Bearer <jwt>`.
  - The JWT comes from a logged-in client. The game logs in through Steam, Google Play, Game
    Center, Epic, Nintendo or Tesla, and a logged-in Mac client stores the token as a `jwtToken`
    file under `~/Library/Application Support`.
  - The response carries `initialGameStateData` and `currentGameStateData` (base64
    `GameState.Serialize`, including the command stack) plus `gameSettingsJson`.
  - Prior art: usemsedge/PolytopiaBoardRenderer `pyrender_UPDATED/replayextractor`.
  - The host is reachable from the Mac's filtered network (401 without a token).
  - For us: deserialize with the game's own reader (polyhost `StateIO`), then replay the command
    stack through ActionManager and record obs, legal list and label per command, as in `gen_data.py`.
  - **Built:**
    - polyhost ops 11 REPLAY and 12 RSTEP (`env::Match::loadReplay`/`replayStep`);
    - `py/fetch_replays.py` (download, ~4 s apart);
    - `py/convert_replays.py` (championship-format filter, npz in `gen_data.py`'s layout plus the
      game id and version, misses logged to `not_in_legal.tsv`).

    Self-test on a bot game recorded locally: 571/571 labels equal the AI's own legal-list
    choice, and the replayed final state is byte-identical to the live one after the same
    save/load. Real downloads are untested until we have a token.
- **Sources:**
  - YouTube descriptions: **1,376 unique IDs so far** (Justeeni Lingueeni 871, PolyChampions 370,
    the official channel 134; 2025–2026). Justeeni covers WC faceoffs, qualifiers and cups;
    PolyChampions is mostly team games. ~1.2k videos are still being scanned, slowly, because
    YouTube rate-limits fast crawls. IDs are in `data/replays/share_ids.tsv` (gitignored);
  - Challengermode (WC qualifiers, ~13.8k players): the public GraphQL API
    (`publicapi.challengermode.com/graphql`) exposes `Match.gameSession.lobbyInformation.gameSessionId`,
    but data queries need a key (refresh key → `/mk1/v1/auth/access_keys`). Untested: whether a
    non-host key can read Polytopia matches, and whether `gameSessionId` is the Polytopia game GUID.
    The game's own `GameContext` stores `ExternalMatchId`/`ExternalTournamentId`, so the server
    links the two;
  - Ben's own games.
- **Collect IDs now**, since replays may expire (PolyAnalytics' did). Record the build version per
  replay; older builds may not replay on 2.17.3. Detect auto-skipped and rushed turns caused by the
  live clock.
- **Uses, in priority order:**
  1. a test set (top-1 / top-5 agreement with champion moves; value AUC at predicting the winner
     from mid-game positions);
  2. an **opening prior** (the opening decides Lakes Small games);
  3. **varied start positions** for self-play (start some games from human-replay positions,
     Go-Exploit style);
  4. a decaying regularizer toward human play.

### Self-play records
- **Replay buffer:** windowed in **samples** (e.g. the last 2–5M positions), not games. Use a fixed
  replay ratio so the learner can't outrun the data. Sharded on disk on the PC.
- **Version tags on every record:** game build, game-data version, encoder version, network ID,
  planner config, seed.
- Stale positions are re-scored by the EMA network on the GPU (reanalysis).

## Topology and operations

- **Target (if the native Windows spike works):** the PC runs everything:
  - game actors on ~20 cores (native `GameAssembly.dll`);
  - batched FP16 inference on the 4070 Ti SUPER;
  - learner, replay buffer and evaluation (WSL2 with CUDA, or native Windows Python 3.11+; the
    installed Python 3.8 is too old).

  The Mac does development, plus emulator actors if useful.
- **Fallback (until then):** Mac = emulator actors only (~6 GB of 16 GB RAM already used, so cap
  processes by memory). PC = inference, learner, buffer, evaluation. The link is Tailscale over
  Wi-Fi (~10 ms, ~400 Mb/s): ~7.5 MB/s of traffic fits, and ≥ 2–4 games in flight per actor core
  hide the latency.
- **Operations:**
  - emulator/adb watchdog;
  - idempotent game IDs;
  - full checkpoints (EMA weights, optimizer, buffer cursor, random-number states);
  - per-game reseeding (`GetRandomPickableTribe` and `utilityRng` share one process-wide
    random-number generator);
  - keep the Mac awake (`caffeinate`).

## Metrics (tracked from M1)

- Elo against frozen anchors (bot difficulties, snapshots, Ben), and a 288-cell matchup matrix.
- **Search gain:** planner vs raw policy.
- **Selection bias:** the chosen candidate's V minus the realized outcome. This replaces aggregate
  calibration as the over-optimization alarm.
- Human-move agreement and value AUC on championship replays.
- Belief accuracy (once the belief sampler exists).
- Exploitability proxy: exploiter win rate (once exploiters run).

## Research log (for the write-up)

Every run gets a pre-registered entry: hypothesis, single change, expected result, falsifier,
config hash, seeds. Results are filled in by the evaluator, never by hand. Polyfish's habit was good;
its instrument wasn't.

## Honest outlook (local only)

- **Throughput:** measured in M0. Mac-only estimates: ~100–300k games/day policy-only; ~7–20k/day
  with per-decision search on 25% of turns. A native PC build would multiply these by about 4×.
- **Milestones:**
  - M1–M2 (beat the default bots): **days**;
  - M4 (beat Ben and Crazy): **1–3 weeks**;
  - strong-amateur level: **weeks to months**;
  - top-human level: **months, and not guaranteed on this hardware.**
