# Evaluation: brainstorm (v1, 2026-11-02)

Ben: "we need a more comprehensive and well rounded evaluation; it should obviously consist of
playing against a few fixed models, but I am not sure what past that." What follows are ideas,
grouped, then a proposed suite in three tiers and the questions for Ben.

## 1. What the current measures miss

| Measure now | What goes wrong |
|---|---|
| vs Crazy | Saturated at 96–98%. It doesn't rank checkpoints, and Ben beat a 98% network easily |
| vs gen 0 | One opponent of the same family. It sat at 64–78% for 60 generations whatever changed |
| vs the previous generation | 47–49% on every track while vs gen 0 stayed flat: generations cycle, so a single pairing misleads |
| vs a rival track's same generation | One opponent, 200 games (±7%), and only within an experiment |
| Human positions vs gen 0 | Good idea, one opponent, 100 positions, not split by situation |
| Indicators | Describe style, not strength; never tied to outcomes |
| Ben's games | One game so far; the only human signal |

What's missing: an absolute scale, robustness (worst case, exploitability), coverage (tribes, seats,
phases, styles), specific skills (tactics, defence, economy), and checks that these numbers predict
what Ben sees.

## 2. Principles

- **Frozen, versioned opponents and positions.** A result means the same thing next month. The panel
  lives in `panel_v1.json` with checksums and is only ever replaced by a v2.
- **Paired designs.** Every candidate plays the same seeds and positions against the same
  opponents, seats swapped. Differences between candidates are then paired, which roughly halves
  the games needed.
- **Several axes, not one number:** strength, robustness, coverage, skills, human-likeness. A single
  rating hides a candidate that is strong on average and broken in one matchup.
- **A cheap tier every generation, deep tiers per read and per milestone** (section 5).
- **Pre-register decisions against the suite**, as now, but on a rating over the panel rather than
  one rival.
- **Check the evaluation itself:** does the rating predict Ben's results and the exploiters' gains?

## 3. Ideas

### A. A frozen gauntlet with a rating (the core)
- **What:** a panel of about 12 fixed opponents spanning the strength range. Each candidate plays
  each of them 100 games (50 seeds × 2 seats). A Bradley–Terry (Elo) fit, with the panel's ratings
  fixed from one all-play-all calibration, gives the candidate a rating ± CI. Also reported: its worst
  panel matchup and the per-opponent table.
- **Panel v1 (proposal):**
  - **Bots:** Hard, Crazy, and Crazy +2★ and +4★ per turn (idea B).
  - **Imitation anchors:** R3 (the game's AI cloned) and R7 = gen 0 (human imitation).
  - **One champion per line, for style diversity:**
    - S3 gen 24 (the one Ben beat);
    - S4a gen 40;
    - S5 gen 47 (credit per move);
    - D gen 71.
  - **The S9 finals:** `s9c`, `s9v` gen 83.
  - **Specialists:** the read-time exploiters `e9*` (trained to beat one checkpoint).
- **Why:** many opponents average out the cycling, and opponents near the candidate's strength carry
  the most information. Weak anchors pin the bottom of the scale.
- **Cost:** about 1,200 games per candidate (~20 min on the whole PC), about ±10 Elo. Calibration
  once: 66 pairs × 100 games, ~2 h.

### B. Star odds: an absolute scale in ★ per turn
- **The mechanism:** the game's handicap is the extra stars a capital yields each turn (Easy 0,
  Normal 1, Hard 2, Crazy 4; a person is 1). `AddAIOpponent` takes it as its own argument. This is
  the game's own setting, not a rule change.
- **Two uses:**
  - **A bot ladder that doesn't saturate:** Crazy +1★, +2★, +4★, ... per turn. A candidate's
    "odds level" is the handicap at which it wins 50%.
  - **Network odds:** network vs network with one side given −1★/+1★ per turn turns a win rate
    into a margin in stars, e.g. "gen 83 still beats gen 0 while giving 1★ per turn".
- **Why:** it is interpretable, independent of our own family, and the pinned game version keeps it
  fixed forever.
- **The human link:** if Ben plays a few games against Crazy, +1★ and +2★, he is on the same scale.
  That is the only absolute human reference we can get locally.
- **Check first:** that the handicap only adds stars (scope-and-rules.md: the AI's stupidity
  depends on it only below 2).

### C. Exploitability (robustness)
- **Standard probe:** a fresh exploiter (started from the target, playing only it, no anchor) for 4
  generations of 1,500 games, then 200 games. The number is the exploiter's win rate; this is the S9
  read's part 2.
- **Cheap proxy each read:** the best panel member's win rate against the candidate (the worst case
  of A). The exploiters join the next panel version, so the panel grows sharper over time.
- **Later:** exploiters with a style objective (navy-first, early rush), as evaluation opponents only.
  The no-hand-written-evaluator rule is about the agent's judgement; an opponent with a style bias
  only probes it. Ben to confirm.

### D. Human-referenced
1. **Human-position suite, larger and split:**
   - Up to 300 held-out human positions (all ~490 held-out games), split by game phase
     (early/middle/late) and by situation from the human game (even, ahead, behind: score ratio and
     who won).
   - Reported:
     - **conversion:** wins from the side that was ahead;
     - **defence:** wins or survival turns from the side that was behind;
     - win rate from even positions.
   - Each is played against fixed panel members, not only gen 0.
2. **Value calibration on held-out human games**, by phase: log loss, Brier, a reliability curve.
   The network's understanding of positions humans reach (the S9 critic check is one instance).
3. **Human move agreement** (top-1, top-5, by phase): a measure of style, not strength; watched for
   drift.
4. **Opening comparison:** per tribe, the first 5–6 turns' build order against strong humans'
   (tech order, first units, first expansion).

### E. Skill probes (measured with the game's own code)
These are derived facts the engine computes, used only to measure, never to steer the agent.
- **Win in one:** positions where the last enemy city can be taken this turn. How often is it
  taken? The positions are found offline by searching our own turn in the engine.
- **Free kills taken:** enemy units killable this turn without a loss. The share taken.
- **Hanging units:** our units left one-hit-killable at End Turn by a visible enemy unit (the
  strategy book's one-hit tables). Rate per game, compared with humans.
- **Capital defence:** positions where the enemy can take our capital next turn. How often is it
  prevented?
- **Throws:** games lost after the reference critic gave ≥ 0.8. **Comebacks:** games won from ≤ 0.2.
- **Economy:**
  - the indicators (tech tempo, banked stars, cities by turn, ports);
  - tech-to-first-use latency, mean city level beyond the capital;
  - all as distributions against the human ones.
- **Endings:** turns to win when ahead, and the share of capped games.

### F. Coverage
- **The 288-cell matrix** (12 × 12 tribe pairs × 2 seats) against one fixed reference (Crazy +2★,
  or gen 0): the worst cells, per-tribe tables as own and as opponent tribe. The M4 gate, kept
  current.
- **Seat split,** and mirror vs non-mirror (92% of human championship games are mirrors).
- **Game length and style of the games it wins,** so a rating gain from one trick (catapult walls)
  shows.

### G. Search and play-time budget (once planning works)
- The same suite with and without the planner: the search gain.
- The gain as a function of thinking time, against the championship's clock.

### H. Ben
- **Calibration games on the star-odds ladder** (B) to place Ben on the scale.
- **Blind A/B:**
  - best-of-3 against two checkpoints without knowing which is which;
  - his preference and notes go in the research log;
  - the games join the human set of the next evaluation version.
- **Per milestone:** the best-of-5 (M5).

### I. Statistics and hygiene
- **Paired seeds** across candidates. **Sequential tests** for champion promotion (SPRT, as chess
  engines' testing frameworks do): stop early once a result is clear, which saves most of the
  games.
- **A rating with CI.** Results go in one file, `runs/eval/ratings.tsv`, and one plot over time.
- **Hygiene:**
  - evaluation seeds (10M–17M) are never used for training;
  - human positions come from the held-out split;
  - the panel is checksummed;
  - one host (the native Linux build);
  - greedy play.

## 3b. More ideas (round 2, same day)

### J. Odds beyond stars: units and cities
- **What:** the determinized-state builder already edits states safely through the game's own
  serializer, so starts can be handicapped in other ways too:
  - the candidate's starting unit removed;
  - one city given to the opponent at turn 10;
  - the candidate's capital at half population.
- **Why:** the score is the share of these odds games won against panel members. That measures
  resilience the way piece odds do in chess, and it is the closest local stand-in for "a stronger
  player got ahead early".

### K. Grading the policy with an offline search oracle
- **What:** once planning works (S9 read item 6), run a heavy, slow search on a fixed set of
  positions:
  - many whole-turn plans, each scored after the opponent's reply, in determinized samples;
  - the score is the share of turns where the raw policy's own turn is within ε of the search's
    best, by a frozen reference critic.
- **Why:** it is chess engines' "agreement with a deep search" measure. It needs no opponent and
  grades every turn, not only results.
- **Caveat:** it is only as good as the reference critic, so it is checked against game results.

### L. Where games are decided
- **What:** with a frozen reference critic, find the turn each game's win estimate swung past 0.5
  for good, and the turns with the largest swings.
- **Output:** histograms for the network vs the panel, and the same on human games.
- **Why:** it tells us which phase to work on (opening, first contact, the naval middle game,
  conversion) instead of guessing. The swing turns are where to look at games by eye.

### M. A living list of known weaknesses, each with a probe
- **What:** every weakness Ben or a game review finds gets a named probe set, a regression test
  for play. Examples:
  - "faces an early rider rush";
  - "enemy boats near its catapults";
  - "a tech that pays off now is available";
  - "stars ≥ 10 at End Turn".
- **How:** positions are taken from human and self-play games where the situation arises (found
  with the game's own facts, section E), and the probe scores the candidate's outcome or choice
  there.
- **Where:** the list lives in this doc, and each read reports every probe.

### N. Adaptation and versatility
- **Adaptation index:** does the candidate's unit mix respond to the opponent's? Measured as the
  correlation between the opponent's early army mix and the candidate's later one, compared with
  the same index in human games. Humans answer catapults with riders and boats; a one-trick network
  doesn't.
- **Tribe distinctness:** how differently it plays each tribe (opening, tech order, army), against
  how differently strong humans do.
- **Style-locked opponents** (idea C, later): its win rate against each, and how fast it adjusts
  within a game.

### O. The value function's own consistency
These are cheap critic diagnostics on any games, with no labels needed.
- **Martingale check:** the estimate before a move against the mean estimate after it (it should
  not drift).
- **TD errors:** their size and bias by game phase.
- **Low-estimate calibration:** how often a position at < 5% is actually lost. The same number
  later sets resignation and search pruning.

### P. Critical moments in human games
- **What:** in held-out human games, take the turns where the eventual winner's move was followed by
  a large swing of the reference critic's estimate, and score agreement with that move there.
- **Why:** agreement on all moves (D.3) is mostly about style; agreement where it mattered is closer
  to strength.

### Q. Hygiene beyond seeds
- **Host drift:** each tier B replays a sample of evaluation games on the Mac's host and checks
  they come out identical.
- **Evaluation games never become training data.**
- **One results file per suite version,** so numbers from different panel versions are never
  compared.

## 4. What it would answer that we can't answer now

- Is a new checkpoint better on average (rating), and against the opponents that matter (worst case)?
- Is it harder to exploit?
- Does it handle every matchup and seat, and positions humans reach, including being behind?
- Did it fix the catapult, tech-tempo and banking habits, or only get better at beating itself?
- How far is it from Ben in ★ per turn?

## 5. Proposed suite

| Tier | When | Contents | Cost |
|---|---|---|---|
| A | Every generation, beside training | vs the current champion (200 paired games); vs 3 panel members (Crazy +2★, gen 0, the previous champion); indicators; value check on a fixed held-out set | ~10 min of 8 game processes |
| B | Every experiment read | Full gauntlet → rating ± CI and worst case; the human-position suite (300 × 2, split); skill probes; tribe/seat breakdown of the pooled games | ~1–2 h of the PC |
| C | Every milestone | Exploitability (fresh exploiters); the 288-cell matrix; the star-odds curve; Ben's sessions | ~1 day of the PC, plus Ben |

**Promotion:** a checkpoint becomes champion when its sequential test against the current champion
passes. Its gauntlet rating must not be lower, its worst panel matchup must not fall by more than 5
points, and its exploitability must not rise.

**Order of building:**
1. Star odds (verify the handicap; small change in polyhost and `selfplay.py`).
2. The panel file and the gauntlet runner with the rating fit; the panel calibration.
3. The human-position suite split.
4. Skill probes: hanging units first (cheapest), then win-in-one and capital defence.
5. The tier-A hook in `sp_loop.py`, replacing vs-previous and vs-Crazy.

## 6. For Ben

1. **Panel v1:** the membership in A, then frozen?
2. **Star odds:**
   - bots with more than Crazy's handicap as the absolute scale;
   - would you play a few calibration games against Crazy, +1★ and +2★?
3. **Opponents with a forced style** (navy-first, rush) for evaluation only: acceptable?
4. **Budget:** how much of the PC should evaluation get? This proposal is about 15–20%.
