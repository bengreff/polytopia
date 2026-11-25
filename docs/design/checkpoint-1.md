# Checkpoint 1, and the plans for the next generation (2026-11-24)

Training stopped on 2026-11-21 at 20:31 (Ben, 2026-11-24: "no more training. What we have right now is
checkpoint 1"). This document says what checkpoint 1 is, how strong it is, how it plays, and gives two
plans: **Plan A**, run-time compute to beat Ben (up to 1 s per action on the Mac, no training), and
**Plan B**, the next network and training method. Nothing here is built yet. Numbers are labelled
measured, derived or guessed; the measurements are in [research-log.md](research-log.md) ("Checkpoint 1").

## 1. Checkpoint 1 in numbers

**Compute (measured, lower bound):** about **461 GPU-hours** on the PC's RTX 4070 Ti SUPER, counted as the
hours from 2026-10-21 to 11-21 in which some run wrote a file under `~/runs`. Jobs ran 1.9 at a time on
average (865 run-hours, derived). Roughly **0.5 billion decisions** of self-play (derived: ~937k files
under `~/runs`, most of them games, at ~500–630 decisions per game file). Mac time is not counted.

**Best checkpoint: `s11x` gen 107 with planner v2 and its own critic.**
- On the Mac: `runs/sp/s11x/gen107.pt` and `runs/sp/s11x/critic_g107.pt` (copied 2026-11-24). Play it:
  `TACTICAL=1 SCORER=runs/sp/s11x/critic_g107.pt tools/play.sh runs/sp/s11x/gen107.pt`.
- Why this one (measured):
  - gens 104–107 beat their start (`s10x` gen 95) 59.6% over 800 games; gen 107 alone 62.0% (+85 Elo, derived);
  - S12's gens 108, 109 and 110 scored 50.0%, 52.5% and 47.0% against it: 299/600 = 49.8% pooled, so nothing
    later is better;
  - gen 110 also lost 45.5% to gen 109, and its play shifted (wins at turn 38.7 instead of 26.6, boat
    upgrades 18.4 per game instead of 7.5): S12 was drifting, not climbing.
  - `s12` gen 109 is a statistical tie with gen 107 and is on the Mac too (`runs/sp/s12/`).

**The ladder so far (measured unless marked):**

| Step | Result |
|---|---|
| R7, imitation of 7,732 championship games (gen 0) | 94% vs Crazy, the only large jump (from 9%) |
| PPO self-play, S3 to S10 | `s10c` gen 95: 81.5% vs gen 0 (+258 Elo, derived) |
| Search in the training loop, S11 | gen 107: 62.0% vs its start; 98–99.5% vs Crazy, 99–100% vs CrazyPlus |
| S12 (planner on every turn, exploration lanes, exploiter, strategy starts) | no gain in 3 generations |
| The planner at play time (v1) | 82% vs its own raw network; about +250 Elo against similar networks |
| Planner v2 (tactical lanes) | 52.25% vs v1 (CI 47.4–57.1); defends knight chains better |
| A bigger human network (`hp1d`, 22M parameters) | 45.5% vs R7: size alone didn't help |

## 2. Can it beat humans?

**Verdict:** it should beat most casual players, but probably not Ben yet, and not a strong ladder player
(all guessed: no human has played anything newer than gen 83).

- It wins 99.5% against Crazy, the hardest built-in bot (measured). Players who lose to Crazy should lose to it.
- Ben beat gen 83 with planner v1 in 42 turns, 12 cities to 1. Since then the measured gain is about
  +140 Elo in self-play terms (derived, chained: gen 83 to `s10c` gen 95 about +12, `s10c` 95 to `s10x` 95
  +42, `s10x` 95 to gen 107 +85), and with the planner on both sides the S11 gain shrank from 62% to 55%.
  Self-play Elo usually overstates the gain against a human who probes for holes.
- The holes Ben used are still there (section 3): weak defence, a narrow repertoire, and frozen borders
  (gen 107 vs gen 106 had 12 of 200 games past 50 turns: neither side can break a stalemate).

## 3. How it plays: versatility (measured 2026-11-24)

**Method** (`py/versatility.py`, `py/versatility_records.py`, `py/versatility_report.py`): the 18 strategy
detectors of `py/strategy_positions.py` (built for the human games, unchanged) run on 2,500 human
championship games and on self-play games where both seats are the same network and play from turn 0:
gen 0 (R7, 1,500 games), gen 107 (450 of S12's gen 108 training games: planner v2 with S12's exploration
lanes on, so if anything more varied than in a game against Ben) and gen 110 (32 games, too few to read).
To match lengths (humans resign early: median 15 turns against 23–25), only games of at least 20 turns count,
and only strategy episodes started by turn 20.

**Diversity and situation (games ≥ 20 turns):**

| | Humans | Gen 0 | Gen 107 |
|---|---|---|---|
| Player-games | 1,186 | 2,150 | 694 |
| Effective number of strategy kinds (all games) | 14.5 | 12.1 | **10.7** |
| Effective number of kinds within a tribe | 13.0 | 10.5 | **8.9** |
| How much the tribe decides the plan (bits, minus the shuffled-tribe baseline) | 0.20 | 0.45 | **0.42** |
| Naval play, explored water at turn 8: low / middle / high third | 21% / 34% / 46% | 31% / 43% / 51% | 35% / 43% / 54% |

**What it uses (share of player-games with a successful episode started by turn 20):**

| Kind | Humans | Gen 0 | Gen 107 |
|---|---|---|---|
| City wall | 10.5% | 1.2% | **0.0%** |
| Defensive line | 10.5% | 4.3% | **3.3%** |
| Cloak and dagger | 4.5% | 2.0% | **0.3%** |
| Rider rush | 12.6% | 7.6% | **3.0%** |
| Knight chain | 10.1% | 3.0% | 7.5% |
| Mind bender | 1.5% | 1.9% | 0.4% |
| Economic boom | 3.0% | 1.9% | 1.3% |
| Giant push | 28.8% | 29.6% | 33.6% |
| Catapult siege | 9.6% | 22.7% | **32.3%** |
| Naval landing | 17.0% | 29.4% | **34.7%** |
| Naval bombard | 16.9% | 21.6% | 21.5% |
| Recapture | 6.2% | 10.8% | 11.2% |
| Early rush | 9.1% | 11.9% | 13.1% |

**Reading:**
- **Not versatile enough.** Its repertoire is about three quarters of the humans' (10.7 / 14.5 = 0.74, derived),
  and within a tribe about two thirds (8.9 / 13.0 = 0.68). Self-play narrowed it further from gen 0.
- **It plays tribe scripts.** The tribe decides about twice as much of its plan as a human's (0.42 vs 0.20 bits).
- **It adapts to the map, weakly.** Humans go from 21% to 46% naval as water rises (×2.2); gen 107 from 35% to
  54% (×1.5), and it goes naval on land maps far more than humans do.
- **Defence is the biggest hole.** It never took a city wall in 694 player-games; humans do in 10.5%. Defensive
  lines are a third as common. This is what Ben's knight chains exploit.
- **It leans on three plans:** catapult sieges (3.4× the human rate), naval landings (2×) and giant pushes.

## 4. Mechanisms that improve a fixed network (no training)

| Mechanism | Status | Evidence | Cost |
|---|---|---|---|
| Turn planner v1: 16 whole-turn plans × 2 fog-safe samples, the opponent's greedy reply, scored by the critic | built | 82% vs raw; ~+250 Elo vs similar networks | 2–4 s per turn (Mac) |
| Planner v2: plus tactical lanes (kill chains on offence and defence) | built | 52.25% vs v1; better against chains | 4.35 s median per turn (Mac) |
| **Fog-correct sampling** (the known leak: sampled worlds keep the enemy's hidden cities in their true places) | **not fixed** | simulated replies can use hidden cities | small |
| Per-action search with a policy update (Ataraxos pattern) | Plan A | Ataraxos: superhuman Stratego with 1.26 s per move | the 1 s budget |
| A human reply model (R7 predicts held-out human moves 43.7% top-1) instead of the bot's own greedy reply | Plan A | the planner gained most against the opponent it modelled (82% vs itself) | none extra |
| Critic ensemble (gens 104–109) | Plan A | the value is search's bottleneck | ×3–5 critic calls |
| Weight soup of the last generations | tested once | S9 finals' soup 55.3% (CI 50.4–60.0); one track's soup 49.3% | none |
| Candidate lanes from threat facts (walls, defenders, pull-backs, scored by the critic) | Plan A, Ben's call | targets the 0% walls | small |
| Adaptive time: more search where lanes disagree | Plan A | not measured | none |

## 5. Plan A: planner v3, up to 1 s per action on the Mac

**The budget (measured on the Mac, 2026-11-24, with other projects loading it):** the network evaluates
1,095 positions a second in batches of 64 on MPS (8.7 ms for one); the engine plays 1,468 decisions a second
per process (10 cores: 6 performance, 4 efficiency); saving a state takes 0.14 ms. A turn is about 16
commands, so v2's median 4.35 s per turn is about 0.27 s per action (derived). **1 s per action is 3.7× what
v2 spends (derived), about 1,000 network evaluations per action.**

**The design: re-plan at every action, not once per turn.**
1. **Fog first.** Fix the determinizer's leak: enemy cities on unexplored tiles are removed and re-sampled
   (positions from where the game's own map generator puts cities relative to the known map, run over
   many seeds; later a learned belief model, Plan B). Measure the strength change: some of the planner's
   measured gain may have come from the leak.
2. **Per-action search** (Gumbel sequential halving, Danihelka et al. 2022; the Ataraxos update):
   - candidates: 8 commands drawn by Gumbel-top-k from the policy, plus the next command of each tactical lane;
   - each candidate is scored by rollouts on fresh fog-safe samples: the candidate, the rest of the turn by the
     policy, End Turn, the opponent's reply, then the critic at the start of our next turn;
   - halving 8 → 4 → 2 → 1, about 40 rollouts per action (derived: a rollout is ~25 network calls, 16 commands
     of reply plus ~8 of our own; 40 × 25 = 1,000);
   - the move: argmax of Gumbel noise + logit + a transform of the rollout values (the Gumbel rule), or one
     KL-regularised policy step toward the values, sampled at a low temperature (the Ataraxos rule). Test both.
   - reuse: the chosen candidate's rollouts become the next action's warm start, so easy actions cost little.
3. **A human reply model.** Half the simulated replies come from R7 sampled at temperature 1 (it imitates
   championship players), half from gen 107. Against a person, this predicts the real opponent better than
   gen 107 replying to itself.
4. **A critic ensemble** (the critics of gens 104–109): the mean scores rollouts, and the spread decides where
   to spend more time.
5. **Defensive candidates from facts** (Ben's call, since it touches "facts, not judgements"): when an enemy unit
   can reach one of our cities next turn (a fact the engine computes), add candidates that take the City Wall
   reward, put a defender on the city, or pull units back. The critic still decides; this only makes sure the
   option is looked at.
6. **Time:** never more than 1 s on an action; actions with one legal choice or a near-certain policy take
   almost nothing.

**Tests, pre-registered before the run:**
- the gate: v3 vs v2, both on gen 107, 400 games with seats swapped, at the Mac's time budget scaled to the
  machine: expected 60–70% (guessed), falsified below 55%;
- no collapse vs gen 0 + v2 and vs CrazyPlus;
- the tactical suite from v2 plus positions from Ben's games where a wall or a defender is the answer;
- time: at most 1 s per action on the Mac, median per turn reported;
- then a best-of-5 against Ben.

**Effort and gain (guessed):** 3–5 days to build, 1–2 days of PC time to test. Expected +70 to +150 Elo over
v2: search was this project's largest measured gain (~+250 over the raw network), and v3 spends 3.7× v2's time
on better-targeted search. **Risk:** the critic is noisy, and more search on a noisy critic over-trusts lucky
rollouts (kevinz917's depth sweep). The ensemble and the human reply model are the hedges.

## 6. Plan B: the next network and training method

**What holds the current recipe back (measured):**
- Self-play has flattened: generations score ~50% against each other, and S12's bundle gained nothing.
- **Samples:** ~0.5 billion decisions in total (derived). Ataraxos trained on 208 billion environment steps,
  416× more (derived); kevinz917's Polytopia bot on 7.7 billion.
- **Throughput:** the network takes 87% of self-play time, one full pass per command; the engine could serve
  ~8× more decisions (16,991 a second on the PC vs ~2,200 in self-play).
- **The value is weak:** it explained ~14% of outcome variance in self-play when last measured (superhuman-plan.md),
  which limits search.
- **Versatility:** section 3. **Capacity is not the first limit:** the 22M human network didn't beat R7's 5.9M.

**What recent research says (sources at the end):**
- **Test-time compute inside the network:** recurrent-depth transformers loop one block a variable number of
  times; trained with random depths, they get better with more loops at inference (Geiping et al., NeurIPS
  2025). A 7M-parameter, 2-layer recursive network beat much larger models on hard puzzles (TRM, 2025).
- **Search distilled into the network:** a 270M transformer trained on 15 billion action values reached
  grandmaster blitz without search (Ruoss et al. 2024); DeepMind's MAV both guides and internalises search
  (Schultz et al. 2024–25). Values trained as classification (HL-Gauss) scale better (Farebrother et al. 2024).
- **Credit assignment:** learned critics are poor at ranking steps; Monte Carlo branches from intermediate
  states fix that (VinePPO, ICML 2025). GRPO-style multi-turn RL needs per-turn advantages (MARSHAL, ICLR 2026).
- **Imperfect information:** Ataraxos (Nature 2026) combines a self-play policy-value network, a belief network
  for the hidden pieces, and per-move search with a regularised policy update, on 16 H100s for a week.
- **Scaling RL networks:** in a 2025 study, dense SimBa networks got worse past ~17M parameters while sparse ones
  kept improving; soft mixtures of experts also scale better in RL (2024–2026).
- **Simple and fast wins:** superhuman Generals.io used plain policy gradient with a transformer, a parameter
  average and top-advantage filtering, on a simulator that runs over 40 million frames a second (ICML 2026;
  the paper's versions differ on the details). Samples mattered most.

**The design ("M2"):**
1. **Encode once per turn, decode the turn command by command** (radical-ideas.md C1). A large encoder reads the
   board at the turn start (and again after a fog reveal or a capture); a small decoder emits each command,
   reading the board encoding and the commands already played this turn, with a key-value cache as in LLM
   serving. Most commands then cost a small pass: expected 3–5× more decisions per second (guessed).
2. **A recurrent-depth trunk:** one block looped 2–8 times in training, up to 16 or more at play time. That gives
   Plan A a second knob: deeper thinking per position on top of more rollouts.
3. **Heads:** policy over the legal rows (as now); value as a distribution (HL-Gauss) over the win probability
   and the turns to the end; ownership; and a **belief head** for hidden enemy cities and units, which also
   replaces Plan A's map-generator sampling.
4. **Size:** about 20–30M parameters with soft mixture-of-experts MLPs, bf16, inside the PC's 16 GB.
5. **Training:**
   - a fresh human pass on the new architecture first (the only large jump so far came from human data);
   - then **expert iteration**: self-play with per-action Gumbel search (Plan A's search, cheaper settings),
     the policy trained toward the search's improved policy and the value toward search values and outcomes;
   - **branch rollouts for credit:** from sampled mid-game states, 4–8 continuations each (save/load is 0.14 ms),
     giving low-variance advantages without trusting the critic (VinePPO, GRPO);
   - a KL regulariser toward a slowly moving anchor (the Ataraxos and DeepNash family) for stability.
6. **Versatility by design:** a strategy-intent token per game, drawn from the human mix of the 18 detected kinds
   (AlphaStar's z, our HP1 z facts), so self-play keeps playing walls, rider rushes and cloaks; at play time the
   search can pick the intent. Plus a league with search-based exploiters, so defence gets tested.
7. **Throughput target:** ≥ 10,000 decisions a second on the PC, about 0.86 billion a day (derived), so 5 billion
   in about 6 days (derived).

**Gates, one at a time:**
- B0, speed: the encoder-decoder prototype is ≥ 3× faster per decision than the current network, before training.
- B1, human pass: top-1 on held-out human moves ≥ R7's 43.7%, and > 52% head to head vs R7 (HP1's kill line).
- B2, expert iteration (about 3 days): raw M2 ≥ 55% vs raw gen 107; with planner v3 on both sides ≥ 55%.
- B3, versatility: effective kinds ≥ 13 (humans 14.5), city walls and defensive lines ≥ 5% each.
- B4, Ben.

**Effort and gain (guessed):** 1–2 weeks to build, 2–3 weeks of the PC's GPU (shared with CrosswordBench).
The largest expected gains, in order: samples through throughput, search targets instead of PPO on sampled
moves, and branch-rollout credit. A bigger network alone is the least likely to pay (hp1d).

**Still advised against** (radical-ideas.md section 4, unchanged by the new research): an LLM as the player
(frontier models won 0.3–3% against an RL bot in this game), a learned world model (the exact engine is fast and
exact), and hand-written rewards.

## 7. Testing checkpoint 1 (next, Ben's call)

1. Ben vs gen 107 + planner v2 (the command in section 1). The fog leak is still in the planner, so it sees a
   little more than it should.
2. Ben vs gen 109 + planner v2, the tie candidate, to compare styles.
3. Optionally, one game against the raw gen 107 (no planner) to feel what the planner adds.
4. Watch self-play games in the watch app to see the scripts in section 3.

## Sources
- Ataraxos (Stratego): [arXiv 2511.07312](https://arxiv.org/abs/2511.07312), [MIT News](https://news.mit.edu/2026/game-playing-ai-stratego-new-champ-0930)
- Recurrent depth: [Geiping et al., arXiv 2502.05171](https://arxiv.org/abs/2502.05171); TRM: [arXiv 2510.04871](https://arxiv.org/abs/2510.04871)
- Searchless chess: [Ruoss et al., arXiv 2402.04494](https://arxiv.org/abs/2402.04494); MAV: [Schultz et al., arXiv 2412.12119](https://arxiv.org/abs/2412.12119); HL-Gauss: [arXiv 2403.03950](https://arxiv.org/abs/2403.03950)
- VinePPO: [arXiv 2410.01679](https://arxiv.org/abs/2410.01679); MARSHAL: [arXiv 2510.15414](https://arxiv.org/abs/2510.15414)
- RL network scaling: [Soft MoE for deep RL, arXiv 2402.08609](https://arxiv.org/abs/2402.08609); [network sparsity, arXiv 2506.17204](https://arxiv.org/abs/2506.17204)
- Generals.io: [ICML 2026](https://icml.cc/virtual/2026/78760), [arXiv 2606.23348](https://arxiv.org/abs/2606.23348)
- Gumbel search: [Danihelka et al., ICLR 2022](https://iclr.cc/virtual/2022/poster/6418)
