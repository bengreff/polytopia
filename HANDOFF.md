# Handoff: Polytopia AI (2026-11-10 evening: S10 read; S11 running, planner v2 in the loop)

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

## Now (2026-11-14, 13:20): S11 running again at gen 98 after two power cuts (`s11x`, planner v2 in the loop on half the turns)

- **The PC crashed twice** (research log "S11" run notes, 2026-11-14):
  - Wed 21:41, under S11's load, with a fatal hardware error reported at boot;
  - Fri 19:55, with nothing of ours running.
  - Each time it stayed off until someone started it (19.5 h, 12 h). The CPU is an i7-14700K on
    microcode 0x129 (BIOS 1.C0, 2024-08-13).
  - **Probably power cuts, not hardware:** Ben found the PC's power strip switched off on
    Sat 2026-11-14. Ben's calls: keep the strip on (or the PC off it); BIOS "Restore on AC Power
    Loss: Power On"; an automatic relaunch after boot (crontab `@reboot` + a logon task for the
    throttle). A BIOS update for the newer microcode is optional.
  - Relaunched Sat 12:58: the Windows throttle (pid 13352) and `sp_resume.sh`.
- **After any restart:** start the Windows throttle (command in `tools/throttle.ps1`), then run
  `DELAY=0 bash ~/poly/py_sp/sp_resume.sh` in Ubuntu-22.04.


- **S10 read** (research log "S10 read"): vs `s10c` over gens 92–95, `s10x` 53.2%, `s10p` 50.0%,
  `s10t` 44.5% (falsified). None passes 55%. The control vs gen 0: 80.5% at gen 83, 81.5% at
  gen 95, so self-play without search has flattened. Every track scores 97.5–100% vs Crazy and
  CrazyPlus. Planner v2 vs CrazyPlus: 100/100.
- **S11** (research log "S11", pre-registered; Ben, 2026-11-10: "Yes, go"):
  - **The track:** `s11x` from `s10x` gen 95 (`tools/s11_setup.sh`). Planner v2 plans half the
    learner's turns (`py/search_v2.py`, `selfplay --search_v2 {}`, binary `polyhost-cp`). Otherwise
    as `s10x`.
  - **Evaluations:** each generation vs its frozen start `s10x95`, vs previous, vs gen 0 and from
    human positions.
  - **Read at gen 107:** includes planner vs planner, which needs `--b_net`/`--b_scorer` added to
    `tactics.match` first.
  - **Cost:** about 2–3 h per generation at full speed (guessed); check gen 96's games time in
    `~/runs/sp/s11x/loop.log`.
  - **Watchdog:** `LIMIT=120`.
  - **Stop:** `touch ~/runs/sp/s11x/STOP` (after the current step) or `~/runs/sp/TRACKS_STOP`.
- **2026-11-11 (research log "S11" run notes):**
  - A Windows sign-out at about 15:30 stopped the distro mid-gen 98. Relaunched 19:38 with
    `bash ~/poly/py_sp/sp_resume.sh` (watchdog + rotation if not running; self-play resumes).
    After any sign-out or restart, run it again. An `@reboot` crontab entry that runs it
    automatically was blocked by the permission check: Ben's call.
  - The evening throttle is now a Windows process (`tools/throttle.ps1`; pid 1932 on 2026-11-11;
    log `C:\Users\ben\polytopia_throttle.log`). The WSL one (`~/throttle/throttled.sh`) can't
    change the VM's affinity once the ssh session that started it ends; don't use it. A Windows
    restart stops the new one too: start it again from ssh (command in the script's header).
  - Gens 96–98 ran on the E-cores alone (the old throttle never unpinned at 22:00): 8.7 h and
    5.6 h per generation (measured), so those are not the full-speed cost.
  - gen 96 / 97 vs their start: 52.5% / 52.0% (200 games each; read at 104–107).
- **On the PC:**
  - crucible's job restarted itself after the sign-out (its own `CrucibleResume` task; one core,
    nice); left running;
  - Roblox, LM Studio, Chrome and Discord came back with the sign-in; Minecraft didn't.
- **Play server is not running** (port 8123 free). For Ben's games, use
  `TACTICAL=1 SCORER=<critic> tools/play.sh <gen>` with the newest `s11x` generation and its
  critic.

## Now (2026-11-04, 13:50): S10 recovered from a 17 h GPU hang; HP1 failed; WSP-1 launched

- **S10** (`s10c s10x s10p s10t`, at gens 91–92 on 2026-11-05 06:00, read at gen 95, about a day out, guessed):
  - **The hang:** s10x's gen 89 froze from 20:32 on 2026-11-03 to 13:08 on 2026-11-04. Its 4
    inference shards had grown to ~3.7 GB each and hung the GPU.
  - **Fixes:** s10x now runs 3 shards. `~/poly/py_sp/sp_watchdog.sh` (repo `tools/sp_watchdog.sh`)
    kills a track's self-play after 45 min without a new game. Its log is `~/runs/sp/watchdog.log`;
    stop it with `touch ~/runs/sp/WATCHDOG_STOP`.
  - **Gen 88 vs `s10c`:** s10x 52%, s10p 51%, s10t 48% (200 games each, no separation yet).
  - **Gen 87 vs CrazyPlus:** 96–100/100; vs Crazy: 194–198/200. CrazyPlus doesn't separate the
    tracks.
- **HP1 arm A failed every pre-registered test** (research log "HP1 read"): top-1 40.8% vs R7's
  43.7%, z followed 15–36% of the gap, 25% vs R7, 75% vs Crazy. It is mostly a training-budget result
  (36k steps vs ~140k). Arm B isn't worth running as planned.
- **Weight-space programming, plan v2** (`docs/design/weight-space.md`):
  - the MIT post overstates its case (critique and replications);
  - what carries over is defect-fixing with forward passes.
- **WSP-1 is closed** (research log): no critic edit; ExPO loses; `research038` gave 55.0% at the
  gate but 51.75% on confirmation (pooled 53.4% over 800). Not adopted.
- **Playing Ben with the planner:** `SCORER=runs/sp/s9v/critic_g083.pt tools/play.sh runs/sp/s9v/gen083.pt`
  (`py/plan_agent.py`, `agent_server.py --planner`, `build/polyhost-mac-play`).
  - Running since 2026-11-04 16:55 for Ben's 3–5 games, on http://localhost:8123; log `runs/play_planner.log`.
  - Games are recorded in `runs/games/play_2026-11-04/`.
  - Ben always moves first in this app.
- **Ben beat gen 83 + planner in 42 turns** (research log "Ben vs s9v gen 83", 2026-11-04).
  - Catapults were 38% of its units; it trained no knights or defenders and researched 9 techs. It
    scouted well early, then stalled at 6 cities.
  - It meets knights (13%) and long games in self-play, so the gap is adapting, plus the planner's
    single greedy reply, which never models a knight chain.
  - **Planner v2 is built and committed** (`py/tactics/`; research log, planner v2 sections).
    - **Results:** vs v1 209/400 = 52.25% (CI 47.4–57.1); vs gen 0 93.5%.
    - **Tactical suite:** defence better (+1.58 stars, CI +0.24 to +2.92); offence +0.27 (n.s.,
      since v1 already plays most chains).
    - **Ben's chain positions:** the best reply against it fell from 78 to 17 stars (turn 35) and from
      51 to 16 (turn 27).
    - **Time:** median 4.35 s per turn on the Mac, max 9.4 s.
    - **Bar:** it misses the pre-registered "clearly better offence" part only, so **adopting it is
      Ben's call.**
    - Run it with `TACTICAL=1 SCORER=runs/sp/s9v/critic_g083.pt tools/play.sh runs/sp/s9v/gen083.pt`
      (stop the v1 server on 8123/8131 first).
    - The CrazyPlus check is still running on the PC (`~/runs/tac/driver6.sh`, results in
      `~/runs/tac/crazyplus.jsonl`).
  - The play server is still up for his next games.
- **CrazyPlus:** validated (Crazy 94%, Hard 98.5%, Normal 100% from a person seat). It's in S10's
  evaluations; about 780 MB of recorded games in `runs/crazyplus/` can be deleted.
- **Waiting for Ben:** a game login (only needed if arm B or other downloads are wanted).

## Now (2026-11-02, 21:15): S10 running; search works; an improved Crazy-based bot is being built

- **S9 read** (research log, parts 1–4):
  - No track beat the control by the 55% bar. The critic track was closest at 54.8%.
  - Fresh exploiters gained nothing (47–53.5%).
  - Engine-only throughput is 17k decisions/s against ~3.5k in self-play.
  - The soup of the four finals reached 55%.
  - **The planner works:** `turn_search.py --reply` (16 plans in fog-safe samples, scored after the
    opponent's reply by the S9 critic) beat unplanned networks 82–94%.
- **S10** (`tools/s10_setup.sh`, research log S10): `s10c` control, `s10p` privileged critic, `s10t`
  turn-clock credit, from `s9v` gen 83; read at gen 95. Not yet built:
  - `s10x` (search in the loop), to be pre-registered before it starts;
  - the fresh human pass (~20M parameters, strategy conditioning, the Might Continents games).
- **Evaluation bots:**
  - `py/rulebot` (Normal–Hard level) is in the S10 evaluations every third generation, through
    `py/script_eval.py`.
  - An agent is building `py/crazyplus` (the built-in AI plus overrides and a combat search) to beat
    Crazy. Switch the S10 `script_eval` to it once it's validated.
- **Ben's decisions:** docs/design/README.md (2026-11-02 rows) and memory `project-constraints`.
- **Docs:** `docs/design/evaluation.md` (evaluation brainstorm) and `docs/design/radical-ideas.md`
  (research report: kevinz917's bot, ideas, top five).

## Now (2026-10-30, 23:00): S9 running; the determinized-state builder v1 is done

- **The plan:** `docs/design/superhuman-plan.md` (v6 restrategy). Ben approved it on 2026-10-30
  ("Sounds good. Keep human starts in, they can't hurt. 3) yes. Go.").
  - Human-position starts stay in every training track.
  - Ben approved building the determinized-state builder.
- **S9** (research log S9; settings in `tools/s9_setup.sh` and each run's `override.json`):
  - Five tracks from track D (`s5k`) gen 71 rotate on the PC (`~/poly/py_sp/tracks.sh`,
    `~/runs/sp/tracks.list`):
    - `s9c` control;
    - `s9v` separate critic (`train_rl.py --critic`);
    - `s9m` magnet (`--anchor_every 6`);
    - `s9l` league main (`--league`);
    - `s9x` exploiter (`--reset_from`).
  - Each track stops at gen 83 (`LAST`), then comes the read: exploitability (4-generation fresh
    exploiters vs each track's gen 83), the critic's held-out quality, and the planner re-test.
  - One round of five takes ~3.3–4 h, so the read is about 2 days out.
- **Results so far** (S7/S8 in the research log):
  - D (KL anchor) beat A 55.5% at gens 65–71; B (value weight 1.0) collapsed and was stopped.
  - E crashed on a human start where the learner never moved (fixed).
- **The policy's value head memorizes outcomes within a pass** (its stream holds ~16 positions
  of every game per batch). The critic's own stream samples 25% of each game across 64 games.
- **Fog-safe search state:** `polyhost/determinize.hpp`, wire op 14, `polyenv.Env.determinize(src, dst, viewer, seed)`.
  - Test: `build/polyhost-mac-det detertest <games> [seed] [every]`.
  - It checks the viewer's observation is unchanged, idempotence, no leak under perturbation, AI
    playouts, and that the rules don't read the stripped history.
  - Known leak: enemy cities on unexplored tiles are kept in place.
  - On the PC it is `~/polylinux/polyhost-det` (sources in `~/polylinux/src_det`; its detertest passed).
    The S9 tracks still run `~/polylinux/polyhost`. Use `POLYHOST=linux POLYHOST_BIN=~/polylinux/polyhost-det`.
- **To do before the S9 read (gen 83, about 2 days out):**
  1. The exploitability runner: per track, a run dir with gen000 = that track's gen 83.
     - Override: `self_share 0`, `fixed ""`, `recent 0`, `league "<gen083.pt>:1.0"`,
       `eval_kinds gen0`, no anchor, no human starts, `LAST 4`.
     - Add them to tracks.list.
  2. A value check: the critic vs `s9c`'s value head, on held-out games and 150 held-out human games.
  3. The planner re-test (`py/turn_search.py`):
     - plans at temperature 0.5, scored after the opponent's reply;
     - played in determinized samples;
     - once with `s9v`'s critic, once with `s9c`'s head.
- **Network:**
  - Tailscale was stopped on the Mac on 2026-10-30. On the home LAN, use `ssh -o HostKeyAlias=100.81.254.12 ben@10.0.4.44`.
  - The Mac's watchdog (`tools/site_update.sh`, `ssh backhouse`) is offline until Tailscale runs.

## Update (2026-10-25, 10:15): S3 stopped; S4 branches running

- **S3 regressed after gen 30 and is stopped** (`~/runs/sp/s3/STOP`). See the research log.
- **Two branches from S3 gen 29 run side by side:**
  - `~/runs/sp/s4a`: the control, plain S3 settings;
  - `~/runs/sp/s4b`: plus 30% human starts, evaluated against s4a's same generation.
  - Settings are in each run's `override.json`.
- **Watchdog:** the Mac runs `WATCH_RUNS="s4a s4b" tools/site_update.sh s3 loop`. The site still
  shows S3.
- **Self-play is sharded with CUDA-graph inference** (`py/fast_infer.py`). Emulation (QEMU) is now
  the bottleneck.

## Update (2026-10-24, 12:45)

- **S3b from gen 30:** fixed opponents (gens 0–2), the last 5 generations, Crazy at 10%, and a turn
  cost on wins (0.005 per turn). Settings live in `~/runs/sp/s3/override.json`.
- **S3c armed for gen 37:** `~/runs/sp/s3c_switch.sh` waits for gen036.pt, then restarts the loop
  with 30% of non-bot games starting from human midgame states (`~/polydata/human_starts`, 1,715
  states). See the research log for the pre-registrations.
- **Strategy book:** `docs/strategy/universal-book.md` is the reference for training decisions;
  read Part 0 first. Indicators: `py/indicators.py`.
- **Play app** (`tools/play.sh runs/sp/s3/gen024.pt`):
  - a network panel (key N) and yellow one-hit-kill marks;
  - watch lists `human_showcase` and `human_starts_preview`, which open at their midgame turn.
- **Ben beat gen 24 easily** (2026-10-24). Its weaknesses: a catapult-heavy army, slow research,
  banked stars, no navy in that game.
- **92% of the human championship games are mirror matches.** Ben chose to keep drawing tribes
  independently.

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
