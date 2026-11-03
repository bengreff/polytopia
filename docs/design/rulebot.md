# RuleBot: a fixed-logic opponent for the championship format

**What it is.** A hand-coded ("scripted") player for 1v1 Might, Lakes, Small 14×14, fog on, any of
the 12 regular tribes in either seat, game 2.17.3. It is a fixed, interpretable opponent for the
evaluation panel and a sparring partner, not the project's learned agent. Ben asked for "a bot
built similar to the built in crazy bots, fixed logic, just more advanced, with how it plays
reviewed and tuned by an LLM". This document is the design, the review-and-tune log, the results
and how to run it. Written by the session that built it (2026-11-02).

**Rules it keeps.**
- **Exact rules only.** Every game runs in polyhost (the game's own compiled rules). Each decision
  the bot returns one row of the game's own legal-command list. It computes facts to choose (the
  book's verified damage formula, unit stats from `GameLogicData28.txt`), but never executes rules
  itself.
- **Fog.** It decides only from `env.observe([(slot, my_id)])` and the legal list, plus what it
  remembers from earlier observations (enemy unit types and improvements seen, known enemy cities).
  It never reads the true state, never asks the game's AI (`env.bot`), and does not use
  `determinize` (the damage arithmetic was exact enough without it, §2.3).
  The only "outside" knowledge is a map-layout prior (§2.6) built from fresh maps, the kind of
  knowledge an experienced player has.
- **Cost.** ~0.7 ms per decision on the Mac; a 200-game batch with 4 polyhost processes takes
  about 2.5 minutes.

## 1. How to run it

```
# from the repo root
PYTHONPATH=py POLYHOST=mac POLYHOST_BIN=build/polyhost-mac-det .venv/bin/python -m rulebot.match \
    --vs bot:3 --seeds 100 --procs 4 --port 8800            # 200 games vs Crazy, seats swapped
    [--first_seed 20000000] [--params '{"attack_min": -0.5}'] [--record DIR] [--out results.jsonl]
# against a network checkpoint (greedy policy, torch on MPS: keep --procs at 1-2)
... -m rulebot.match --vs runs/r4_human_ft/step020000.pt --seeds 20 --procs 2 --port 8800

# review recorded games (the bot's own view each turn, and every decision with its reason)
PYTHONPATH=py .venv/bin/python -m rulebot.review DIR --summary [--only loss]
PYTHONPATH=py .venv/bin/python -m rulebot.review DIR/20000003_s1.pkl --turns 10-14 --log
```

- Seeds and tribes follow `py/eval_vs_bot.py`: each seed is played twice on the same map and
  tribes with seats swapped; tribes = `np.random.default_rng(seed).choice(regular tribes, 2)`.
  The bot plays a person's seat (handicap 1), so Crazy has +3 stars a turn over it.
- Use as a library: `RuleBot(params).act(obs, status) -> legal row` with `obs` from
  `env.observe([(slot, my_player_id)])`. One `RuleBot` object per game (it keeps memory).
- `--record` keeps per game: the bot's observation at each of its turn starts, its decision log,
  attack-prediction checks, the finished game's state, and (for review only, never shown to the
  bot) the opponent's own city/income/unit/tech counts each turn.

## 2. Design

`py/rulebot/`:

| File | What |
|---|---|
| `data.py` | Constants from the game's data file: unit stats and abilities, tech tree (tiers, parents, unlocks), improvement costs, tribes' starting techs; the tech-price formula; the enemy-capital map prior |
| `state.py` | `View`: the observation parsed into my/enemy units, cities, villages, ruins, techs, stars, and the legal list indexed by kind and tile |
| `combat.py` | The verified damage and retaliation formula and the defence bonuses |
| `tactics.py` | Danger map (what the visible enemy could deal to a unit on each tile next turn) and the attack planner |
| `econ.py` | Research valuation (with the human opening book), improvement values, city-reward choices |
| `geo.py` | Cost-distance maps over the known map (roads, forests, mountains, unexplored tiles) |
| `bot.py` | `RuleBot`: the turn's phases, unit roles and movement, training, the army's front |
| `match.py` | Match runner (multiprocess, Wilson CIs, recording) |
| `review.py` | Text rendering of recorded games and per-game indicator lines |
| `capital_pairs.npy` | Capital positions on 4,000 fresh maps (seeds 21,000,000–21,003,999) for the prior |

### 2.1 Each decision

The bot re-plans from the current view at every decision and returns one legal row. Phases, in
priority order (the first that wants to act does):

1. **Pending choice:** city reward (below), peace request (always rejected: Might is won by
   capture).
2. **Promote** a veteran (full heal, +5 max HP).
3. **Garrison refills:** a threatened city whose garrison just stepped out ("rotation", §2.4)
   trains a fresh unit now; a threatened empty city that no unit can reach trains one.
4. **Research** (before captures, because every new city raises every tech's price), from the
   stars not reserved for units.
5. **Capture** villages and cities; **examine ruins**.
6. **Snipe:** walk into an empty enemy capital (capturing it next turn wins).
7. **Attacks** (§2.3), including move-then-attack; a pending second half of a move-attack first.
8. **High-value improvements** (harvests, farms, mines: value/cost ≥ 0.95; ≥ 1.25 under threat).
9. **Unit moves** (§2.4).
10. **Training** (§2.5).
11. Late research, other improvements, boat upgrades, then a **no-banking** rule: above
    6 + turn/3 stars, train anything useful, research anything, build anything.
12. Heal others (mind benders), **Recover** wounded units that didn't act, **End turn**.

### 2.2 Economy

- **Research value** (`econ.tech_values`): the star value over ~10 turns of the population the
  tech unlocks inside my borders now (5 stars per population point, minus build costs; e.g.
  Organization = fruit tiles × (5 − 2)), plus its value for the war (Riding 8 early; Archery,
  Smithery, Mathematics, Chivalry 10–16 from turn 8 or first contact; +8 for the first fighting
  tech; Shields +6 under threat), plus half the value of the tech it leads to, plus the human
  opening book. Research when value ≥ price (tier × cities + 4, a third off with Literacy).
- **Opening book:** for each tribe, the first techs of the human championship winners in
  `data/human1` (3,000 games mined; first-four sequences and turn-10 shares). E.g. Xin-xi: Mining,
  Smithery, Hunting, Fishing; Hoodrick: Hunting, Forestry, Mathematics; Zebasi: Construction,
  Riding, Fishing. Diplomacy, Meditation, Spiritualism and Aquatism are never researched for
  value (only by the no-banking rule).
- **Improvements:** population per star, with a bonus when the build levels the city up now.
  Ports: at most one per three cities and one per city (an early version bought seven in four
  turns).
- **City rewards:** level 2 workshop (explorer once, before turn 6, when most of the map is
  unexplored); level 3 +5 stars, but a **wall** for the capital once the enemy is met and for any
  threatened city; level 4 population growth; level 5+ super unit (Giant). Human winners chose
  resources 13,317, workshop 10,281, super 9,748, explorer 5,752, pop growth 5,578, border 2,305,
  wall 302, park 173 times.

### 2.3 Tactics (`tactics.py`)

- **Damage arithmetic:** the book's formula (Part II §1.1), defence bonuses for forest (Archery),
  mountain (Climbing), cities (fortify units only: ×1.5, ×4 with a wall), retaliation exclusions
  (kill, out of reach, `stiff`). Checked live: the bot compares every attack's prediction with its
  next observation; **1,120 of 1,179 attacks (95%) were exact**, and all misses were on the side of
  more damage than predicted (the bot assumes the enemy has Archery and Climbing from turn 6 when
  it can't infer its techs).
- **Melee advance:** measured from the checks: a land melee unit that kills moves onto the
  victim's tile, unless the victim stood on water (a boat). The planner uses the advance tile for
  the attacker's exposure and for leaving a garrison.
- **Planner:** for every visible enemy unit, every attack this turn (from where units stand, or
  after one legal move for units with `dash`), the best attack tile per unit (least retaliation,
  then least danger), and ordered combinations of up to 3 attackers (5 when the enemy stands on
  one of our cities: only a kill stops the capture). Order: attackers that draw no retaliation
  first, cheap units soften, strong units finish. Score = value of the kill (unit stars, +100 for a
  unit on our capital, +12–22 on another of our cities, +8 next to our capital, +12 for a unit
  standing in the enemy capital, +2 for catapults/knights/bombers) or 0.55 × the damage share for
  a hit, minus retaliation, minus exposure (expected loss of the attacker at its end tile through
  the enemy's turn, from the danger map), minus the cost of opening a garrisoned city. The best
  plan is played if its score is at least −1.5 (attacking readily paid +10 points vs Hard, §4).
  Execute the first step, re-plan.
- **Danger map:** each visible enemy unit threatens tiles within (movement if it has `dash`) +
  range; for a unit on a tile, the threats' attacks in sequence, strongest first.

### 2.4 Units and positions

Each decision the fresh units (not yet moved this turn) get roles, highest priority first; caps
count the roles already used this turn:

1. **Capital safety swap:** a capital garrison the visible enemies could kill steps out when a unit
   that would survive there can step in.
2. **Garrisons** for threatened cities (enemies within 3, 4 for the capital): the occupant holds,
   or, if it is wounded or weak and the city can train, it **rotates** out next to the city and a
   fresh unit is trained in the city; an empty threatened city gets the best garrison that can
   reach it (defence × city bonus × health: fortify units get ×1.5, ×4 behind a wall).
3. **Capital defence:** when enemy strength within 3 of the capital ≥ 0.6 × ours there, army
   units within 8 land cost come back.
4. **Hunt invaders:** enemy units on our land or within 2 of our cities get 1–2 hunters.
5. **Villages and ruins:** greedy matching by travel turns (goals kept across turns).
6. **Wounded units** (< 50% HP) go home to recover.
7. **Scouts:** 2 (3 while no enemy city is known) until turn 8, then one until the enemy capital
   is found. Scout goals: a far tile with unexplored tiles around it, weighted by the
   enemy-capital posterior (§2.6) and enemy-owned land; kept across turns.
8. **Army:** everyone else goes to the army's **front** (below).

**The front** (one tile per turn, `_update_mode`): if any of our cities is outweighed (enemy
strength within 3 − 0.8 × ours within 3 ≥ 3, capital weighted ×1.5), the front is that city
("defend"). Otherwise it walks the land path from our capital to the target enemy city (the
nearest known by land, capital preferred): two steps when nothing is ahead, one when our strength
within 2 of the front ≥ 1.3 × the enemy's within 3 of the next point (plus the city's own
defence), back one step when we are outweighed where we stand. Army units move to within ~1.5 of
the front, avoid tiles where the visible enemy could kill them (weighted less while defending or
assaulting), walk into empty enemy cities, and keep ranged units out of melee reach.

**Naval transport:** an army unit whose land route to the front costs more than 4 embarks at one
of our ports when sailing (water distance / 2 + a turn to embark and land) is shorter; rafts in our
territory are upgraded (Rammer, then Scout ship, Bomber if affordable); boats steer by water
distance to the front and land next to it.

### 2.5 Training

- A unit value target for the army (4 + 2.2 × turn stars, more under threat, at least 1.2 × the
  visible enemy army); the stars for it are reserved at the turn start (25% before contact, 55%
  after, 70–100% under threat).
- Expansion units (rider, else warrior) while known villages have no unit heading for them.
- Garrisons: Defender in the capital and walled cities, otherwise Swordsman, Archer, Defender,
  Warrior.
- Army weights: Swordsman 2.8, Catapult 2.6 (while ranged < 30%), Archer 2.4 (< 35%), Knight 2.2
  (< 30% of units), Rider 1.3, Defender 0.9, Warrior 1.0 before turn 12; it saves a turn for a
  preferred unit it can almost afford.
- City capacity is level + 1 units (measured), so trained units step off city tiles.

### 2.6 The enemy-capital prior

In Might 1v1 the game ends with a capital, so knowing where it is matters. On 4,000 fresh maps the
capitals were always 5–9 tiles apart (5: 168, 6: 576, 7: 1,486, 8: 1,337, 9: 433). `data.py`
turns these pairs into P(enemy capital on a tile | our capital's tile); the scouts weight their
goals by that prior restricted to tiles not yet explored.

## 3. Sources and research

- The project's strategy book (`docs/strategy/universal-book.md`): mechanics, damage formula,
  one-hit tables, retaliation, defence bonuses, fortify, knight chains, naval play.
- The human championship games (`data/human1`, 3,000 of 7,732 mined): per-tribe opening tech
  orders, turn-10/20 tech shares, units trained by turn 20, city-reward choices.
- Web (2026-11-02, brief):
  - [gameplay.tips comprehensive guide](https://gameplay.tips/guides/8250-the-battle-of-polytopia.html)
    (search summary): first ten turns decide most games; find a village by turn 3–4; star income
    first; Bardur: move, Organization, harvest, second warrior only after income rises.
  - [Steam: "Are Crazy Bots Too Easy"](https://steamcommunity.com/app/874390/discussions/0/2800629075207222851/):
    the AI chases weak units instead of empty cities and threatening ranged units; it attacks a
    sturdy unit placed next to it; it wastes stars on parks and temples.
  - [Ai-Mo/Strategy](https://polytopia.fandom.com/wiki/Ai-Mo/Strategy) and
    [Swordsman](https://polytopia.fandom.com/wiki/Swordsman) (search snippets): Swordsmen finish
    walled defenders and survive the counter; defenders anchor sieges.
  - [PolyChampions Lakes Small 1v1 tournaments](https://www.challengermode.com/s/PolyChampions/tournaments/21cf6b7f-8e6a-4e67-95ad-08db40e8eb44?lang=en):
    the format is played competitively; no written meta found.
- Measured here: city capacity = level + 1; level n → n+1 needs n+1 population; the melee-advance
  rule; prediction accuracy of the damage formula.

## 4. Review-and-tune log

Each version was measured against the game's own AI with the same seeds (paired), seats swapped,
tribes from the seed. Until v10 a measurement was 40–100 games (±9–14%); from the v10 sweep on,
200 games (±6.5%). Differences under ~8 points at 200 games are noise.

**Reference (not the bot):** the game's own AI playing our person seat (`STEP_BOT_OWN`, same
seeds, 100 games each) won **69% vs Normal, 44% vs Hard, 16% vs Crazy**.

| Version | Change (and why: what the reviewed games showed) | vs Hard: won (95% CI), games |
|---|---|---|
| v0 | First build: phases, tactics planner, economy, unit roles. Easy 2/4. Review: **1,000+ stars banked**, army deadlocked short of the enemy city (the "can advance" test counted only units already near it), scouts dithering in a corner | — |
| v1 | Spend rules (train anything when rich), army gather/assault with the deadlock fixed, goals kept across turns, invader hunting, re-garrison by training, targets by land distance | 25% (14–40%), 40. Easy 95% (84–99%), Normal 62.5% (47–76%), Crazy 12.5% (6–26%) |
| v2 | Review: the capital fell to a unit we only damaged; a garrison left the capital through the "retrain" rule. Change: units on our cities get kill-only plans with up to 5 attackers (+100 on the capital); the capital garrison never leaves. Damage formula checked live: **1,120 of 1,179 attacks exact**; melee advance rule measured | 22.5% (12–38%), 40 |
| v3 | Review: 2–4 cities, mines bought while the capital was attacked, wounded units fighting on. Change: research by unlocked population value, threat-first spending, wounded units retreat | 25% (18–34%), 100 |
| v4 | Review: capital lost with a 4-HP warrior inside while a full giant stood 2 tiles away; 20 riders vs swordsmen, no fighting tech by turn 20. Change: capital-safety swap, first-fighting-tech rule, unit weights | 27% (19–36%), 100 |
| v5 | Review (capital in the last 3 turns of every loss): **no capital ever had a wall**; AI massed on the capital while our army was away; giants (no city bonus) as garrisons. Change: walls for the capital and threatened cities, garrison quality with fortify/wall, capital-defence recall | 30% (22–40%), 100. A/B same seeds: no wall 26%, no recall 29%, wall from turn 0 31% |
| v6 | Review: 192 stars banked with one unit: the only city's tile held its garrison, so nothing could be trained. Change: **garrison rotation** (garrison steps out, a fresh unit is trained in the city) | 30% (22–40%), 100; but 41–46 defenders trained per game |
| v7 | Change: the army "front" (defend the outweighed city, else walk the path to the target while locally stronger) | **13%** (8–21%), 100, 14 stalemates: the front flip-flopped (a city's own strength counted units within 1 tile only) |
| v8 | Fix: count own strength within 3 like the enemy's | 27% (19–36%), 100 |
| v9 | Review: 7 ports bought in 4 turns; research stalled at 5 techs by turn 20 because tier-2/3 fighting techs scored below zero after their price. Change: research when value ≥ price, with prerequisite paths; ports capped | 16% (10–24%), 100 (more research, fewer units early) |
| v10 | Review: ~20 defenders per game (atk 1). Change: defenders only for the capital and walled cities; offensive unit weights | 24% (17–33%), 100; research ratio 1.6: 18% |
| v10 sweep | 200 games each, same seeds: baseline **20.5%** (16–27%); attack threshold −0.5: 25.0%; looser front advance 16.5%; higher defend trigger 20.5% | — |
| v11 | Review: we **first saw the enemy capital at turn ~32** in 14 of 19 games, though it is 5–9 tiles away, and it was often empty. Change: scout toward the enemy capital, snipe an empty capital, bonus for killing its garrison | 28.5% (23–35%), 200 |
| v12 | Found a scout bug (goals overwritten each decision); map-layout prior for the enemy capital; ordered combinations in the kill search (an emergency search had made some games take 1 s per decision) | 27.5% (22–34%), 200 |
| v13 | Found: roles were re-assigned every decision, so caps on scouts and hunters never held (a conga line of 7 scouts). Change: per-turn role counts | 25.0% (20–31%), 200. Capital still first seen ~turn 33: across the lakes it needs boats |
| v13 sweep | Attack threshold −1.5: **35.5%** (29–42%); −3: 32.5%; −5: 33.5%; −1.5 with partial hits worth more: 35.0%; −0.5 with movement danger ×0.5: 30.5%; −0.5 with exposure ×0.5: 28.0%. **Attacking more is the one lever that clearly paid** | — |
| v14 | Attack threshold −1.5 by default | 35.5% (29–42%), 200 |
| v15 | Review: 23 swordsmen per game and no archers/catapults (the swordsman weight always won); 3 of 11 losses had 10+ daggers around our capital (cloaks infiltrating empty cities); 1,861 stars banked with free capacity because every city tile held a healthy garrison. Change: ranged units preferred below a 25% share; every city garrisoned once cloaks/daggers are seen; rotation also when rich | 33.0% (27–40%), 200; cloak guard off 33.0%; with rotation-when-rich 31.5% |
| v16 | Review (a stalemate, turns 80–100): the army stood behind a lake while the target was across the water; the bot never sailed. Change: **naval transport**: army units embark at our ports when sailing is shorter than walking, rafts are upgraded (Rammer, Scout ship), boats steer by water distance and land next to the front | **38.0%** (31.6–44.9%), 200; same seeds without transport 31.5% |
| v16 sweep | Capital target bias 8 (from 3): 38.0%; embark from land cost 5 (from 7): 40.5%; 4: **41.5%** (34.9–48.4%); 3: 41.0% | — |
| final | v16 with embarking from land cost 4 (all settings are `bot.DEFAULTS`) | the ladder in §5 |

**What the loop taught.** Most single-game diagnoses were real bugs, and fixing them was
necessary but rarely moved the win rate by itself (v2–v13 all sit at 20–30% vs Hard). Three
changes did measurably move it: spending everything and unblocking the army (v1: Easy 50% → 95%),
attacking much more readily (v13 sweep: +10 points), and sailing the army across the lakes (v16:
+6.5 points on the same seeds). The structural "front" doctrine, more scouting, and more research
did not help or hurt. Bigger batches (200 paired games) were what made the later steps readable.

## 5. Results

Final version (all settings in `bot.DEFAULTS`), 200 games per level: seeds 20,000,000–20,000,099,
each played in both seats, tribes from the seed, the bot in a person's seat (handicap 1).

| Opponent | RuleBot final: won (95% Wilson CI) | Seat 1 / seat 2 | Stalemates (turn 100) | Mean winning turn | RuleBot v1 (40 games) |
|---|---|---|---|---|---|
| Easy | **96.5%** (93.0–98.3%) | 97 / 96 | 4 | 30.6 | 95.0% (83.5–98.6%) |
| Normal | **81.5%** (75.5–86.3%) | 82 / 81 | 5 | 34.6 | 62.5% (47.0–75.8%) |
| Hard | **41.5%** (34.9–48.4%) | 42 / 41 | 10 | 42.0 | 25.0% (14.2–40.2%) |
| Crazy (+3 stars a turn) | **15.0%** (10.7–20.6%) | 15 / 15 | 14 | 43.8 | 12.5% (5.5–26.1%) |

**Reference points** (the game's own AI playing a person's seat):

| Reference | Easy | Normal | Hard | Crazy |
|---|---|---|---|---|
| R3 teacher (`docs/design/research-log.md`, R3: 200 games each, seeds from 10,000,000) | 96.5% | 65.0% | 31.0% | 10.0% |
| Same, on this document's seeds (`STEP_BOT_OWN`, 100 games each) | — | 69% (59–77%) | 44% (35–54%) | 16% (10–24%) |
| Network `r4_human_ft/step020000.pt` (task brief) | | | | ~94% |

Reading:
- RuleBot is about as strong as the game's own AI playing from a person's seat: clearly better vs
  Normal (81.5% vs 65–69%), level with it vs Hard (41.5% vs 31–44%) and Crazy (15% vs 10–16%).
- It is far below the learned network (~94% vs Crazy). As a panel opponent it sits between
  Normal and Hard: a fixed, cheap (~0.7 ms a decision), fog-honest, interpretable rung.
- It plays the same in both seats.
- Cost: a 200-game level takes 1.5–3 minutes with 4 polyhost processes on the Mac.

**Against the network** `runs/r4_human_ft/step020000.pt` (greedy, its own observation; seeds
20,000,000–20,000,019, seats swapped): **0 of 40** (95% CI 0–8.8%). The network ends these games
quickly (mean turn 20.5, range 8–40); RuleBot is not a rung for checkpoints of that
strength, only below them.

## 6. Known weaknesses

From the reviewed losses (the last turns before each capital fell, the opponent's own counts
logged for review only):

1. **Expansion lags the AI.** By turn 20 the bot holds ~4.4 cities to the AI's ~7 (Hard), so the
   AI has more unit capacity (level + 1 per city) and income later. The game's AI probably knows
   where villages are in the fog; the bot finds them by walking.
2. **The enemy capital is found late** (median first sighting ~turn 33 although it is 5–9 tiles
   away): scouts are land units, lakes block them, and the map prior only helps where land leads.
   Boats don't scout.
3. **Invisible cloaks.** The AI's cloaks infiltrate cities and spawn daggers; 3 of 11 reviewed Hard
   losses had 10+ daggers around our capital. The bot can't see cloaks; keeping every city
   garrisoned once one is seen made no measurable difference.
4. **Positional play is shallow.** The danger map ignores terrain, zone of control and blocking,
   and is pessimistic (every enemy in reach attacks), so units hold back; there is no lookahead of
   the AI's reply (`determinize` is unused). The exchange is ~1.5 kills per loss, which isn't
   enough against the AI's extra production.
5. **Stalemates and late banking.** 5% of games vs Hard reach turn 100. Once city capacity is full
   the bot can bank hundreds of stars, and assaults stall against walled defenders and giants;
   catapults stay rare (Mathematics comes late), knights and mind benders are hardly used.
6. **Naval play is transport only:** no bomber sniping, no naval defence of the shore.
7. **Tuned against the game's AI only.** The aggressive attack threshold that pays against the AI
   may be baitable by a human; it has never played a person.
8. **Garrison and reward heuristics are coarse:** walls only for the capital and threatened
   cities, explorer/workshop by a fixed rule, peace always rejected.

Next steps if it is developed further: boat scouting and bomber play; a one-turn lookahead with
`determinize` for attack sequencing near cities; an expansion push with riders in turns 3–12;
using catapults to crack walled cities.
