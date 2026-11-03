# CrazyPlus: the game's own AI, improved

**What it is.** A hand-coded evaluation opponent for the championship format (1v1 Might, Lakes,
Small 14×14, any of the 12 regular tribes in either seat, game 2.17.3) that plays from a person's
seat (handicap 1, so Crazy has +3★ a turn over it) and beats the game's hardest AI. It is not the
learned agent. Built 2026-11-02 after Ben rejected RuleBot (`docs/design/rulebot.md`: 15% vs Crazy):
"Base it off of the built in crazy bot, just improved ... hard code things like giant popping,
bomber/catapult etc targeting (high value targets), and other clear decisions, but use a more
adaptive and generalized algorithm (still hand coded) for combat rather than assigning units to
roles". Ben also allowed it a **full view** of the game, like the built-in AI: it reads the true
state and simulates on copies of it.

**Result** (fresh validation seeds 24,000,000–099, 200 games per opponent, seats swapped, §5): **94.0%
vs Crazy** (89.8–96.5%), 98.5% vs Hard, 100% vs Normal, from a person's seat. The same games played by
the built-in AI on our seat: 10.5%, 37%, 69.5%. Against RuleBot (15% vs Crazy) it won 91 of 100. Against the network
`r4_human_ft/step020000.pt` (94% vs Crazy) it won 2 of 20. ~180 ms per decision.

**How, in one paragraph.** The built-in AI is a greedy one-command-at-a-time scorer that never looks at
the reply (§1), and it is deterministic. CrazyPlus asks it for its command at every decision and plays it,
unless a short list of alternatives (every kill, multi-unit kills, captures, walking into an empty city,
the unit staying put or retreating, garrisons, and in the first 12 turns every research/train/build option)
does better in an exact simulation: each candidate is played on a copy of the full game, the AI finishes
our turn, the game's own Crazy AI plays its whole reply (so against Crazy the reply is predicted exactly),
and a hand-written evaluation scores the result. Tactical overrides must survive a second round of
lookahead; a simulated line that wins the game is followed to the end. City rewards follow two rules
where the AI is wasteful (border growth at level 4, walls without a threat). No unit roles, no fronts.

## 1. What the built-in AI does

Sources: the class layout in `game/dump/diffable-cs/DiffableCs/GameLogicAssembly/AI.cs` and `AIState.cs`,
the method list in `game/dump/symbols.tsv` (`AI$$...`, 45 methods), five methods decompiled with Ghidra
(`AI$$GetMove/3`, `GetTileCommands/3`, `PickBestPossibleCommand/3`, `AddAttackOptions/6`, `RateBattle/3`,
in `game/dump/decompiled/AI__GetMove_3.c`), and measurements in polyhost. The decompilation is low-level
(most il2cpp calls are unresolved thunks), so the constants of its scoring formulas were not recovered.
The structure below is read from the method names, their parameters and the type layouts (the decompiled
code resolved no calls between AI methods), so the order of steps is an inference; the choices and the
determinism are measured.

**One command at a time, greedy.** `AI.GetMove(gameState, player)` returns a single command:
1. `AnalyzeSituationIfNeeded`: once per turn (`AIState.missionsForTurn`) it runs `AnalyzeSituation`, which
   builds, from the full game state (its methods take the `GameState`, not a player's view):
   - `PlayerMapData` (each player's units, territory and cities);
   - `playerMilitaryStats` (`CollectMilitaryStatsFromTile`: summed defence, attack, movement, range per player);
   - `missions`: `MakeMissionsForCity` gives every city it could take (villages, enemy cities) a Capture
     mission with a score;
   - `hitList`: `UpdateHitlistForUnit` scores enemy units as targets;
   - `militaryNeeds` (`AdjustMilitaryNeeds`), `unitNeed`, `enemyCities`, `enemyPopulation`, `roughTerrain`,
     `upgradeableCities`, `blockedTiles`.
2. `GetTileCommands` scores every possible command on every tile (`ScoredCommand`):
   - units: `AddUnitMoveOptions` scores each destination with `RatePosition` (mission distance via
     `GetPositionScoreForMissions`, the best attack from there via `GetBestAttackForPosition`, improvement
     sites, city threat); `AddAttackOptions` scores each attack with `RateBattle`; `AddUnitActionOptions`
     (capture, recover, upgrade, ...);
   - cities and economy: `AddPossibleTrainUnitCommands` (`GetDesiredUnitStats`, `GetBuildUnitScore`, a price
     factor), `AddPossibleImprovementCommands` (`GetImprovementScore`), `AddPossibleResearchCommands`
     (`CheckForTechNeeds`/`AddTechNeed`: techs wanted for the improvements and units it wants),
     `AddPossibleRoadBuildingCommands` (`GetConnectionScore`), `AddPossibleDiplomacyCommands`.
3. `PickBestPossibleCommand` sorts them and plays the best; `StupidFactor` adds deliberate mistakes on easier
   levels (`GetStupidity` = max(0, 2 − handicap) with AutoPlay; 0 for Crazy and for a person seat).
4. A unit it does not want to use gets the AI-only `StayCommand`; `EndCommand` ends the turn.
5. Separate entry points: `ChooseCityReward`, `ChooseInfiltrationReward`, `ShouldAcceptPeace`.

Difficulty sets the handicap (stars at the capital: Easy 0, Normal 1, Hard 2, Crazy 4; a person 1) and
through it the stupidity (Easy 2, Normal 1, Hard and Crazy 0); `PlayerExtensions.GetAggression` may also read
it (not traced). For a person seat the AI also runs with stupidity 0. So **Crazy is essentially the AI of a
person seat with +3★ a turn**, which is why that AI loses 88.5% to it (§4, the baseline).

**Its choices, measured** (the AI on a person seat, 40 games against Crazy, seeds 23,000,000–039, up to
turn 60):
- city rewards: level 2 workshop 75 / explorer 68; level 3 wall 33 / +5★ 34; **level 4 border growth 27 of
  27** (never population growth); level 5+ super unit (Giant) 17 of 17;
- research: Fishing, Sailing, Ramming and Riding in nearly every game, then Navigation and Roads;
- training: warriors 1,143, riders 270, swordsmen 190, defenders 107, archers 47, cloaks 2; never a catapult
  or a knight;
- building: **194 ports** (about 5 a game, 7★ each), then harvests 59, farms 51, fishing 42, hunting 39.

**What follows from the structure (and what the measurements showed):**
- Every command is scored on its own; nothing plans a sequence. Two attacks that kill together, a softening
  hit before a finishing hit, or "attack with the unit that takes no retaliation first" happen only when the
  individual scores happen to line up.
- It never looks at the opponent's reply. Position scores contain a threat term, but nothing checks whether a
  move walks into a kill, whether a garrison leaving a city opens it, or whether an attack leaves the
  attacker to be killed.
- It is deterministic given the state (stupidity 0), so its next turn can be predicted exactly by playing it on
  a copy (§3: 12 of 12 turns identical, command for command).

## 2. Design

`py/crazyplus/`:

| File | What |
|---|---|
| `bot.py` | `CrazyPlus`: each decision asks the AI, builds candidates, scores them by rollouts, plays the best |
| `tactics.py` | The board from both players' observations; candidate command sequences for a decision |
| `econ.py` | Opening economy candidates (research, training, building) |
| `evalfn.py` | The hand-written evaluation of a position (stars) |
| `rules.py` | Hard-coded decisions (city rewards) |
| `sim.py` | Clones and rollouts; the two polyhost ops it adds |
| `match.py` | Match runner, same interface as `rulebot.match` (`run(...)`, CLI), Wilson CIs, recording |
| `review.py` | Text rendering of recorded games: the board each turn and every override with its scores |

polyhost gets two operations in a new header, `polyhost/crazyplus.hpp` (one include and one dispatch
line in `polyhost/wire.hpp`), built to `build/polyhost-mac-cp`:
- **op 40 BOTTURN:** the built-in AI plays the current player's commands to the end of its turn (then bot
  seats move), returning the commands it played. Same as repeated `STEP(STEP_BOT_OWN)` but without
  listing the legal commands after each command, which is most of a step's cost: a whole-turn rollout
  with the opponent's reply costs 5–50 ms.
- **op 41 STAY:** the game's own `StayCommand` for one of the current player's units (the AI's way to be
  done with a unit; a person simply leaves the unit alone). It lets the bot veto the AI's command for a
  unit and let the AI go on with the others.
Neither changes a rule. Without them (an older polyhost, e.g. on the PC) the bot falls back to
pure-Python rollouts and no vetoes.

### 2.1 Each decision

The bot returns one command per decision (a legal-list row, `STEP_BOT_OWN` for the AI's own command, or
a stay). Each decision (`CrazyPlus.act`):

1. **A committed winning line** (below) is replayed command by command while it still wins.
2. **Ask the AI** for its command `c*` on the real game (`env.bot`).
3. **City rewards** go to `rules.choose_reward` (§2.4); a rule's pick that differs from the AI's is
   kept only if its 4-round rollout is within 3★ of the AI's pick.
4. **Win check, once a turn:** the AI's own play is rolled out for `win_depth` = 3 rounds; if the game is won
   inside them (the opponent model replying exactly), the bot commits to that line.
5. **Candidates** (`tactics.candidates`, §2.2; in the opening also `econ.candidates`). With none, `c*` is
   played without a search (about a third of decisions).
6. **Rollouts.** The full state is saved (`env.save`, 0.3 ms) and, for `c*` and each candidate, loaded into
   a scratch slot (1 ms) where: the candidate's commands are played; the AI finishes our turn (op 40);
   the opponent model (the game's own Crazy AI, whatever the real opponent is) plays its whole reply; and,
   for `depth` > 1, more rounds. The position is then scored by `evalfn` (§2.3). A rollout costs 5–100 ms.
   Depths: 1 round for tactical decisions, `econ_horizon` = 4 rounds for economic decisions up to turn
   `econ_turns` = 12.
7. **Choice.** The best candidate is played if it beats `c*` by `margin` = 1★ (2★ for economy), else `c*`.
   A tactical override is first **verified at depth 2** (`verify_depth`): both it and `c*` are rolled out
   one more round, and the override stands only if it still wins by the margin (this vetoes about three in
   four depth-1 proposals: many gains vanish one turn later). A multi-command candidate (move then attack)
   keeps its remaining commands as the first candidate of the next decision.
8. **Commitment.** Any rollout that ends in a won game (the enemy capital captured within its rounds) is
   played at once and its whole command sequence is kept as a line: from then on the bot replays it, and
   at each new turn re-checks on a copy that the rest of the line still wins (against an opponent that
   doesn't play like the model, the check fails and the bot goes back to searching).

**Costs and shortcuts.**
- The value of `c*` is reused when the AI's command is the one the previous chosen rollout predicted
  (98.6% exact on a checked game), which saves a rollout on most searched decisions.
- A candidate that lost by more than `skip_below` = 5★ earlier in the turn is not tried again that turn.
- `budget_ms` = 1,500 caps the rollout time of one decision (it binds in fewer than 1 decision in 1,000).
- Mean 176 ms per decision in the validation against Crazy (4 game processes on a Mac shared with other
  jobs; 68% of decisions searched, 5.3 rollouts each, 59 ms per rollout); a game takes ~2 minutes on
  average, up to ~15 for long won games with big armies.

### 2.2 Candidates

Candidates are command sequences proposed by cheap estimates (the strategy book's verified damage formula
from `rulebot.combat`, a reach-based danger map); only the rollouts decide. In the order they are tried
(the time budget may cut the tail):

| Group | Candidates |
|---|---|
| pending | the rest of a multi-command candidate chosen at the previous decision |
| city | every Capture; walking into an empty enemy city or village this turn |
| kills | every single attack that kills (any unit, not just the one the AI is moving); two attacks from where units stand that kill together; move then attack for a kill (dash units); **kill plans**: for the most urgent enemy units (on or next to my cities first, then by value), a greedy sequence of up to 4 attackers, no-retaliation hits first and the finisher last, from where they stand or after one move |
| unit | for the unit the AI wants to act with, when an enemy is within 3: **stay** (the StayCommand op), and the move with the least danger (**retreat**) |
| garrison | an empty city of mine with an enemy within 2: the unit that can walk in with the least danger, and training the sturdiest unit there; a wounded garrison: recover; a weak garrison in danger: step out and train a fresh unit in its place (**rotation**) |
| attacks | the five best other attacks by the estimate (soften a target) |
| econ (turns ≤ 12) | when `c*` is research, training or building (or ending the turn with 5★ or more): every research and training option and the useful builds (population, port), up to 10 |

There are no unit roles and no fronts: every unit is the AI's, except when a candidate about it wins a
rollout.

### 2.3 The evaluation

`evalfn.evaluate`, in stars, mine minus the opponent's, read from both players' observations of the
cloned state (so cloaks are seen):
- **material:** each unit's value (its cost, Warrior 2.5, Knight 7.5, Catapult 7, Giant 15; a boat is its
  passenger plus its hull: Scout/Rammer +4, Bomber +12) × (0.3 + 0.7 × health share), +1.5 for a veteran;
- **cities:** 8 + 3 × level, +12 for the capital, +2 for a wall;
- **economy:** income × 3, stars × 0.6, techs × 1.5;
- **occupation** (positions are read at the start of a turn): an enemy unit standing on my city (it captures
  next) costs 0.6 × the city's value, on my capital 150; my unit standing on an enemy city is worth 0.8 × its
  value, on the enemy capital 200 (both × 0.6 or × 1 depending on whose turn starts);
- a finished game is ±10,000.
Constant terms cancel: only differences between candidates matter.

### 2.4 Hard-coded decisions

`rules.choose_reward` (city level-up rewards), from the measured AI choices (§1):
- level 5+: the super unit (Giant). The AI already always takes it, so this rule never fired; the bot's
  "giant popping" comes from the economy rollouts, which value a Giant at 15★ and see what it does.
- level 4: **population growth** instead of the AI's border growth (fired in most games).
- level 3: a **wall only if an enemy unit is within 3** of the city, else +5★ (the AI took walls half the time).
- Both are checked: if the rule's pick scores more than 3★ below the AI's own pick in a 4-round rollout, the
  AI's pick stands (`reward_check`; added after a rule's wall broke a line the rollouts had found with
  the AI's +5★).
High-value targeting (bombers, catapults, kill shots on units in or next to cities) is not a rule: every
attack is a candidate and the rollout scores what it kills, and kill plans put the units threatening my
cities first.

## 3. Measurements behind the design

All on the Mac, `build/polyhost-mac-cp` (the deterministic build plus the two ops).

| What | Measured |
|---|---|
| Engine costs | `env.bot` 1.4 ms; a step 3.0 ms (mostly relisting legal commands); `save` 0.26 ms, `load` 1.0 ms; the opponent's whole turn 5–60 ms; a rollout (our turn by the AI via op 40 + the reply + two observations) 5–100 ms, mean 59 ms |
| **Is the reply predictable?** | Our turn played by the AI on a clone and on the real game, then Crazy's reply, for 12 turns of a game: **12 of 12 turns identical**: the same command sequence, the same observations for both players (the saved bytes differ in a field that doesn't affect play). The built-in AI is deterministic, and its per-turn memory (`AIState`, not saved) is rebuilt identically on a clone loaded at a turn start (mid-turn, see the next row) |
| The reused value of `c*` (a clone loaded mid-turn) | 142 reuses checked against a fresh rollout in one game: 140 identical |
| Depth-2 verification | vetoes 73 of 96 proposed tactical overrides per game (v3): most depth-1 gains are gone one turn later (a unit that "stays safe" is attacked the turn after, a softening hit gets no follow-up); yet removing it changed no result measurably (§4 ablations) |
| The enemy capital | empty (no Crazy unit on it) at the start of **~50% of our turns** at every stage of the game (100 v1 games, 2,800 turns); our nearest unit was within 2 tiles of it only 115 of those times |
| Who wins vs Crazy, by the AI alone | the AI on our seat: 8% in seat 1, 15% in seat 2 (200 games) |

## 4. Iterations

Tuning seeds 23,000,000–023,000,049 (100 games: each seed in both seats, tribes from the seed as in
`py/eval_vs_bot.py`), all against Crazy, all variants on the same games. Batches of 100 rather than 200: the
first two gaps were 25+ points, far outside the ±9% noise; the later ones are within it. "Paired" counts games whose result changed.

| Version | What changed, and why (from the reviewed games) | vs Crazy (95% CI) | Paired | ms/decision |
|---|---|---|---|---|
| v0 | **Baseline:** the built-in AI on our person seat (`mode: ai`, `STEP_BOT_OWN`) | 14% (8.5–22.1) on these 100; 11.5% (7.8–16.7) on 200 (23,000,000–099) | — | 1 |
| v1 | The search: at each tactical decision, the AI's command against kills, two-unit kills, move + kill, the best attacks, captures, walking into an empty city, stay, retreat; rollouts of 1 round (our turn + Crazy's whole reply), margin 1★ | **68%** (58.3–76.3) | vs v0: 55 to wins, 1 to a loss | 196 |
| v2 | Review of v1's 32 losses: the economy fell behind early (by turn 20 Crazy had 6–10 cities to our 3–5, 2–3× our income), and two capitals fell to a unit walking into an empty or weakly held capital. Changes: **opening economy search** (turns ≤ 12: every research, training and useful build tried by 4-round rollouts); **depth-2 verification** of tactical overrides; **garrison candidates** (walk into or train in an empty threatened city); **reward rules** (level 4 population growth, level 3 wall only when threatened); speed (value reuse, skip, budget) | **94%** (87.5–97.2) | vs v1: 29 to wins, 3 to losses | 207 |
| v3 | v2's economy rollouts found forced wins (e.g. turn 6: research Climbing → the swordsman reaches the empty enemy capital, won at turn 8), but one such line was abandoned by a later depth-1 override (won only at turn 44). Changes: **commit to a winning line** and a **win check** every turn (the AI's own play for 3 rounds); **kill plans** (greedy up to 4 attackers on the units threatening my cities); **recover and rotation** for weak garrisons; an enemy unit on my capital costs 150 (from 80) | **96%** (90.2–98.4) | vs v2: 2 to wins, 0 to losses | 185 |
| final | v3, plus: training a garrison only when an enemy is adjacent (or at the capital): 946 of ~1,000 such proposals were vetoed; and a **rule's reward must not lose** more than 3★ to the AI's pick in a 4-round rollout (a wall chosen by the rule broke a line the rollouts had found with the AI's +5★, a loss at turn 11) | §5 | — | — |
| v4 (not adopted) | Review of the validation losses of the final version (§5): 5 of its 12 losses to Crazy were capital snipes (our capital taken while we still held 4–7 cities), e.g. the garrison left two turns before an enemy defender walked in, which one round of lookahead doesn't see. Change: an empty capital with an enemy within 2 costs 30★ in the evaluation (+8★ for the enemy's), and the capital's garrison always gets a stay candidate when an enemy is within 4 | 51 of 56 (stopped there), where v3 won 53: 2 to losses, 0 to wins | — | — |

**Ablations of the final version** (the same 100 tuning games, after the validation; paired with v3, whose
code differs from the final only in the two small "final" changes):

| Without | vs Crazy | Paired with v3 | ms/decision | Reading |
|---|---|---|---|---|
| the opening economy search (`econ_turns: 0`) | 80/100 = 80% (71.1–86.7%) | 18 to losses, 2 to wins | 189 | **the largest single gain after the tactical search itself** (16 points) |
| depth-2 verification (`verify_depth: null`) | 86/91 = 94.5% (one game process died in game 23,000,032 s1 and took its remaining 8 games with it; see §6) | on the 91: 2 to losses, 1 to a win | 157 | no measurable effect against Crazy; it costs ~15% of the time |

So, in order of what mattered: (1) the exact one-round tactical search itself (v0 → v1: 14% → 68%), (2) the
opening economy search with 4-round rollouts (80% → 96% when it is added to everything else), (3) everything
else together (verification, garrisons, reward rules, win commitment, kill plans, rotation: v1's 68% → 80%
without the economy search; none measurable alone at this sample size).

## 5. Final validation

Fresh seeds 24,000,000–24,000,099, never used for tuning: each seed in both seats, tribes from the seed,
CrazyPlus on a person's seat (handicap 1). Settings: `bot.DEFAULTS` (the "final" row of §4). The same
games played by the built-in AI on our seat are the reference.

| Opponent | CrazyPlus: won (95% Wilson CI) | Seat 1 / 2 | Mean winning turn | ms/decision | The built-in AI on our seat, same games | Paired: AI lost, CrazyPlus won / the reverse |
|---|---|---|---|---|---|---|
| **Crazy** (+3★ a turn) | **188/200 = 94.0% (89.8–96.5%)** | 97 / 91 | 27.8 | 176 | 21/200 = 10.5% (7.0–15.5%) | 167 / 0 |
| Hard | **197/200 = 98.5% (95.7–99.5%)** | 98 / 99 | 23.3 | 128 | 74/200 = 37.0% (30.6–43.9%) | 123 / 0 |
| Normal | **200/200 = 100% (98.1–100%)** | 100 / 100 | 19.2 | 80 | 139/200 = 69.5% (62.8–75.5%) | 61 / 0 |
| RuleBot (`py/rulebot`, its final settings; 15% vs Crazy) | 91/100 = 91% (83.8–95.2%), seeds 24,000,000–049 | 46 / 45 | 36.1 | 221 | — | — |
| Network `r4_human_ft/step020000.pt` (greedy; 94% vs Crazy) | 2/20 = 10% (2.8–30.1%), seeds 24,000,000–009 (also 2/20 with the version before the reward check) | 1 / 1 | 23.5 | 144 | — | — |

No game reached the turn-100 cap; no errors. RuleBot (fog-honest, a different policy from the opponent
model) does better against CrazyPlus than Crazy does (9% vs 6% of games), as the model mismatch predicts. A 200-game batch took 30 minutes (Normal) to 100 minutes
(Crazy) on 4 processes. For comparison (`docs/design/rulebot.md`, other seeds): RuleBot won 96.5% vs Easy,
81.5% vs Normal, 41.5% vs Hard, 15% vs Crazy.

**The losses to Crazy (12):** 5 were capital snipes (our capital taken while we still held 4–7 cities), 7
were lost on economy and material (Crazy at 8–11 cities and 2–4× our income; Giants in some). By tribe,
no matchup stands out (every tribe 9–19 wins out of 10–20 games, as either side).

## 6. Known weaknesses

1. **Its edge rests on an exact opponent model.** The rollouts reply with the game's own Crazy AI, which is
   exactly what Crazy (and Hard: the same brain with stupidity 0) will play, so against them the bot sees
   the future of every candidate. Against anything else the model is only an approximation: against the
   network `r4_human_ft/step020000.pt` (94% vs Crazy) it won 2 of 20 (§5), with exchanges of 1 kill per
   3–5 losses. It is a strong rung between Crazy and the network, not a strong player in general, and it
   would likely be exploitable by a human who plays unlike the AI.
2. **Capital snipes.** 5 of the 12 validation losses to Crazy came with 4–7 cities still ours: the capital
   was left empty or weakly held and an enemy unit walked in two turns later, beyond one round of lookahead.
   The obvious fix (v4: penalise an empty capital with an enemy near) did not help on the tuning games.
3. **Economy.** The other losses are economic: Crazy's +3★ a turn compounds into 8–11 cities and 2–4× our
   income, and Giants (Luxidoor) or masses of units break the defence. The economy search covers only the
   first 12 turns; after that research, training and building are the AI's (no catapults or knights, many
   ports).
4. **One round of lookahead for tactics.** Depth-2 verification removes most myopic overrides, but nothing
   looks two turns ahead except the economy search, the win check and the verification itself. The
   candidates are proposals from crude estimates (Chebyshev reach, no terrain or zone of control in the
   danger map), so moves the generator never proposes are never tried.
5. **Cost.** ~180 ms per decision, with long won games up to ~450 ms per decision (many units, more
   candidates); a 200-game batch takes ~1.5–2 hours on 4 processes. It needs `polyhost-mac-cp` (the two
   ops): on the PC's hosts it falls back to slower rollouts (through `STEP_BOT_OWN`) and no vetoes; tested on
   one game only (won, 106 ms per decision).
6. **Full view.** It reads the true state (allowed by Ben for an evaluation opponent). It is not a model of
   a fog-limited player.
7. **A rare game-process crash.** Once in ~1,300 CrazyPlus games (the no-verification ablation, the 17th
   game in a long-lived process) polyhost died on a managed exception inside a rollout; the same game
   replayed in a fresh process ran clean, so it looks like state accumulated in a long-lived process. The
   match runner now restarts the game process and scores that game as an error; before that fix the
   process's remaining games were lost too.
8. **Tuned only against Crazy**, on 100 games per variant (gaps of 25 points are clear at that size; the
   last few points between v2, v3 and v4 are not).

## 7. How to run it

```
# from the repo root; POLYHOST=mac runs build/polyhost-mac-cp (match.py switches POLYHOST_BIN to it
# when it points at another Mac build)
PYTHONPATH=py POLYHOST=mac .venv/bin/python -m crazyplus.match --vs bot:3 --seeds 100 --procs 4 --port 8800
    [--first_seed 23000000] [--params '{"econ_turns": 0}'] [--record DIR] [--out results.jsonl]
# other opponents: --vs bot:2 (Hard), bot:1, bot:0; --vs rulebot; --vs runs/r4_human_ft/step020000.pt
# (a network, greedy; torch on cuda/mps/cpu, keep --procs at 1-2)
# the baseline (the built-in AI on the person seat): --params '{"mode": "ai"}'

# review recorded games: the board each turn (--opp: also the opponent's own view) and every override
PYTHONPATH=py .venv/bin/python -m crazyplus.review DIR --summary [--only loss]
PYTHONPATH=py .venv/bin/python -m crazyplus.review DIR/24000003_s1.pkl --turns 10-14 --log [--opp]

# as an opponent for a network (sp_loop's scripted evaluation):
PYTHONPATH=py .venv/bin/python py/script_eval.py gen083.pt --bot crazyplus --seeds 50 --first_seed 19000000 --procs 4 --port 8800
```

- `crazyplus.match.run(vs, seeds, first_seed, procs, port, params=None, record_dir=None, opponent=None,
  quiet=False)` has `rulebot.match.run`'s semantics and return value: `(summary, results)` from
  CrazyPlus's side; `opponent` = a `.pt` path plays that network in the other seat.
- Library use: `CrazyPlus(env, slot, spec, me, params).act(status)` returns an action code for
  `crazyplus.sim.play(env, slot, code)` (a legal index, `STEP_BOT_OWN`, or a stay). It needs the env and
  the slot (it clones the game into slot + 1) and the match's `Spec`.
- Settings are `bot.DEFAULTS`; `{"mode": "ai"}` is the baseline. v1 is
  `{"verify_depth": null, "econ_turns": 0, "skip_below": null, "budget_ms": null, "win_depth": 0,
  "reward_rules": false, "garrison": false, "kill_plans": false, "weights": {"occupied_capital": 80}}`.
- **Rebuild the host:** `clang++ -std=c++20 -O2 -Wall -o build/polyhost-mac-cp polyhost/main.cpp`. On
  the PC (QEMU or native Linux builds of polyhost) the ops need the same header compiled in; without
  them the bot still runs, slower (rollouts through `STEP_BOT_OWN`, ~2.5× the cost) and without stays.

**Files and records.**
- Code: `py/crazyplus/` (8 modules); the polyhost header `polyhost/crazyplus.hpp` and its include and
  dispatch line in `polyhost/wire.hpp` (3 lines); the binary `build/polyhost-mac-cp`.
- Results (git-ignored, `runs/crazyplus/`, ~780 MB with the recorded games): `results.jsonl` (tuning
  batches: baseline, v1, v2, v3, the two ablations), `results_val.jsonl` (validation), one log per batch,
  and recorded games for review in `v1/`, `v2/`, `v3/`, `v4/` (partial), `val_crazy/`, `val_hard/`,
  `val_normal/`, `val_rulebot/`, `val_net/`, `val_net2/`. The recordings can be deleted; the `.jsonl`
  files hold every result.
