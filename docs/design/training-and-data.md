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
    save/load.
  - **Real downloads (64 games, native Mac polyhost):**
    - all 64 replay to the end, and **every one ends in the server's recorded final state**,
      apart from cosmetic or scratch fields (statecompare over a fresh save/load of both);
    - this holds although **none is version 123**: the 59 WC 2026 qualifier games are 122 (35)
      and 121 (24), plus 5 Glory games from 2022–23 (93–96);
    - v122: 26 identical in content, 9 differ only in `unit.birthClimateSkinType` (−1 vs 0, a
      skin); v121: only `VillageNameSeed` (village names); v93–96 also show tiny `aggressions`
      drifts (AI memory);
    - the 59 championship-format games give 26,328 labelled decisions with **0 human commands
      outside our legal list**. StartMatch (played by the client) and Resign (58 of 59 games end
      that way; our agent never resigns) are excluded.

    The converter writes the check per game (`final` column), so every game is verified, not
    just these.
- **Sources:**
  - YouTube descriptions: **1,456 unique IDs** (Justeeni Lingueeni, PolyChampions, the official
    channel; 2022–2026). Justeeni covers WC faceoffs, qualifiers and cups; PolyChampions is
    mostly team games. YouTube rate-limits fast crawls. IDs are in `data/replays/share_ids.tsv`
    (gitignored);
  - **Challengermode** (`py/cm_replays.py`, Ben's API refresh key in
    `~/.config/polytopia/challengermode_refresh_key`):
    - every tournament match reports statistics, among them the game's share link (`ReplayLink`,
      `General.ReplayLink` before ~2025), mode, map size, team count and both tribes;
      `lobbyInformation.gameSessionId` is only Challengermode's own match id;
    - tournament settings give the format: GameMode 1 Might; MapType 0 Drylands, 1 Lakes,
      2 Continents, 3 Archipelago, 5 Pangea; MapSize 0 Tiny, 1 Small;
    - **the API lists only the 50 most recent completed tournaments** (~17 days; filters don't
      widen it). The website's own search lists a space's whole history with the anonymous token
      any visitor gets. `tools/cm_list_tournaments.py` drives a headless Chrome with a fresh
      profile (no sign-in) and writes the ids to `data/replays/cm_tournaments.txt`. The search
      caps at 10,000 results per query, so it is split by tournament type:
      - Polytopia Official: 2,723 tournaments, plus 14,221 "Instant Tournaments" (Might
        **Continents** Small, ~3 games each, so out of format; a possible pretraining source);
      - PolyChampions: 3,779; Grid Haven (TLC): 589; Duelists: 388;
    - games older than ~2026 often no longer load (server error 1005 "Can't load game"): most
      from 2023-04/05 and 2025-09/11 failed; 2026 games load;
    - WC 2026 qualifiers: 12 tribes, 788–1,590 players each (~14k), split into numbered
      sub-tournaments ("Kickoo Qualifiers #3", 128 slots, ~40–80 games each), then invite-only
      finals;
    - first harvest (80 tournaments): **902 Might/Lakes/Small games** from 36 of them;
    - **full harvest (2026-10-22): 27,548 Might/Lakes/Small game ids** from the four spaces;
  - **The resulting set, `data/human1/` (gitignored; `games.tsv` indexes it):**
    - 8,420 games downloaded: 5,404 + 1,093 from Challengermode, the rest from YouTube links;
    - **7,732 championship-format games, 3,173,215 labelled human decisions**; 667 skipped
      (other format or special tribes), 13 failed (a recorded command rejected on our build,
      mostly old versions);
    - versions: 4,702 v122, 2,106 v121, 246 v123, ~680 older (v93–v120);
    - **7,623 replay to the server's final state** apart from `birthClimateSkinType` and
      `VillageNameSeed`; 274 games end by a timeout kick;
    - all 12 regular tribes, 746 (Vengir) to 1,847 (Imperius) seat-games each;
    - 70 human commands fall outside our legal list, in 27 games (1 in 45,000):
      - 11 of those games failed to convert anyway;
      - in the clean ones, most misses come with an **empty** legal list (our interface considered
        the game over or stalled while recorded commands continued; open question);
      - 4 PeaceTreaty misses in v103–v112 games.

      The trainer reads these from `not_in_legal.tsv`.
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
