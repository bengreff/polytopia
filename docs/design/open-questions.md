# Open questions (v5)

## Answered (2026-10-20)
- Human data → championship replay links (see training-and-data.md).
- Official servers → local play only. Compute → local hardware only.
- Ben's level → intermediate; beats Crazy and his brother; no ranked games.
- Goals → beat top humans + a research write-up. First model beats the default bots in ~2 days.
  No deadline after that. No need to prove it against humans. Checkpoints per stage.

- PC → `backhouse`: i7-14700K 20C/28T, 64 GB, RTX 4070 Ti SUPER 16 GB, Windows 11 + WSL2,
  Tailscale over Wi-Fi (~10 ms).
- Steam → Ben bought Polytopia; **not installed on the PC yet** (checked 2026-10-20).
- Default bots → **Crazy** (curriculum from lower difficulties is fine; Crazy bots are "quite
  stupid").

## For Ben
1. **Install Polytopia on the PC** through the Steam client (it needs your Steam login, so I can't
   do it), and tell me when it's done. I'll check the build and the `GameAssembly.dll`.
2. **Challengermode:** does a qualifier match page show a replay button or game ID?

## For the polyhost agent (M0)
- The Layer 0 contract in architecture.md: batched API, legal-command list, history-stripped clone,
  canonical hash, allowlist observation builder, determinized-state builder with a taint check,
  `tryCall`.
- Microbenchmarks and the end-to-end dummy-network throughput on 14×14 Lakes.
- Bot checks: Crazy-brain/Normal-income, omniscience hooks, human handicap 1.
- Spikes: native build (2 days), replay ingestion (1 day).
- Does the rules engine ever read the command history (needed for history stripping)?
- Rule invariance under rotations and reflections (needed for symmetry augmentation).

## To measure
- Decisions per turn on Lakes Small, early and mid-game.
- What the client shows about the opponent: score, techs, stars.
- Seat order and first-move advantage.
- The embassy tech-price multiplier (0.5?), and which ruin rewards are eligible in this build.

## Risks we're watching
- Silent pipeline bugs (Polyfish's killer). Mitigation: the CI suite in architecture.md.
- Value over-optimization by search. Watch the selection-bias metric.
- Local-only throughput. Measured in M0; the M3 A/B decides how search is spent.
- Game patches: pin build 16375; fine-tune after patches; version-tag all data.

## Found 2026-10-21 (night)
- **Human replays with an empty legal list mid-game:** 14 clean games (e.g. 1fa4f4f5, e65f6f89,
  292d2daf; v121/v122) have recorded commands at turns 7–26 while our replay's legal list is
  empty, as if `env::Match` thought the game had ended or stalled. Their labelled decisions are
  correct and stay in training; find out why the replay disagrees (`data/replays/raw/<id>.json`).
  4 PeaceTreaty misses in v103–v112 games are real legal-list gaps on old versions.
- **Seat 1 wins 75% of human championship games** (it always moves first; bot games 50.8%). Real
  first-move edge between humans, or tournament seating (stronger player hosts)?
