# Universal Book of Polytopia Strategy

**For:** the Claude session that manages this project's training. It is the reference for judging
whether the network plays well and for choosing training changes. Ben asked for it on 2026-10-24,
after beating generation 24 easily.

**Format:** the World Championship format, as in the rest of the project: 1v1, Might, Lakes, Small
14×14, the 12 regular tribes, the pinned game version (logic data 28).

**How it was made:**
- Two research agents (Sonnet) wrote Parts I and II from the Polytopia wiki, Steam guides,
  community tier lists and calculators, and the game's own data file
  (`game/dump/gamedata/GameLogicData28.txt`).
- I checked the numbers that matter against the game itself and corrected them (marked
  *Editor*).
- Fandom refused direct fetches (HTTP 402). One agent read wiki pages through a reader proxy
  (r.jina.ai), the other from search snippets, so wiki-only claims are less certain than
  data-file ones.

**Rule for using it:**
- Treat principles as hypotheses about what wins. Confirm them with measurements (§0.4 and the indicators in Parts I and II), not
  by hand-coding them. The project has no hand-written evaluator and keeps it that way: the book
  guides opponent mix, rewards, curricula and what to measure, never the network's judgement.
- Its numeric "targets" are mostly the agents' heuristics. Where possible, the human
  championship games (`data/human1`, 7,732 games) are the reference instead.

---

# Part 0 — Where the network stands, and what to do about it

## 0.1 Measured against the book (2026-10-24)

Indicators from `py/indicators.py`:
- **Human:** 400 human championship games.
- **Gens 0, 24, 29:** each generation's own self-play training games (temperature 0.5).
- **Ben's game:** gen 24 against Ben, who won at turn 29, ten cities to one.

| Indicator | Importance | Human | Gen 0 | Gen 24 | Gen 29 | Gen 24 vs Ben |
|---|---|---|---|---|---|---|
| Techs by turn 20 | critical (Part I §1.8, §2.7) | **9.2** | 7.1 | 7.4 | 7.2 | 6 by turn 29 (Ben: 14) |
| Stars left at End Turn, turns 3–20 | critical (I §1.1) | **3.1** | 4.8 | 5.5 | 5.9 | — |
| Cities at turns 5 / 10 / 20 | critical (I §2.2) | 2.2 / 3.9 / 5.8 | 2.3 / 3.9 / 5.7 | 2.3 / 3.9 / 5.6 | 2.3 / 4.0 / 5.7 | 5 at turn 12, then lost them |
| Port by turn 20 | critical on Lakes (II §5.2) | 88% | 83% | 81% | 82% | none |
| Ranged share of units trained | critical (II §4.2) | **13%** | 12% | 23% | 25% | 48% |
| Catapults | (same) | 5% | 8% | 16% | 17% | 20 of 42 units |
| Riders | — | 20% | 16% | ~5% | 6% | 0 |

Reading:
1. **The army mix is the clearest self-play drift.** Catapults went from 5% (humans) and 8% (gen 0)
   to 17%, and riders from 20% to 6%.
   - In self-play, catapult-heavy armies beat other catapult-heavy armies, and Crazy doesn't
     punish them. A human with knights and boats does (Part II §4, §5).
   - This is the book's top critical item and matches Ben's report exactly.
2. **Research is slow and stars are banked.**
   - Two techs behind humans by turn 20. That is inherited: gen 0 already was, because R3 imitated
     the game's AI first, and self-play hasn't fixed it.
   - Banking is getting worse every generation (4.8 → 5.9).
   - This matches "didn't exploit new techs" (Part I §2.7).
3. **Expansion count is fine:** the network matches humans at every checkpoint. Ben's
   "expanded suboptimally" is more likely about development (city levels) and tempo than about
   count. That needs the width-vs-depth indicator (Part I §2.2), not yet built.
4. **Ports are usually built in self-play** (82% by turn 20), but not in Ben's game. Whether its
   boats achieve water control (sniping catapults) is unmeasured.

## 0.2 Training levers for each weakness

Options, not decisions: anything beyond S3b's pre-registered settings needs Ben's go-ahead.

| Weakness | Levers | Cost | Notes |
|---|---|---|---|
| Catapult drift, exploitable army | (a) **Exploiters:** a league with agents trained only to beat the current network (AlphaStar-style), kept in the pool; (b) **Human-position starts:** self-play games that begin from positions in human games (turns 5–15), so the network faces human-style armies; (c) more weight on gen 0–2 in the pool (S3b has 6.25% each) | (a) high: a second training stream; (b) medium: needs loading human states into the engine; (c) free | (b) uses human games only as start states, never as labels, so it doesn't memorize the human set |
| Tech pace, banked stars | (d) **KL penalty toward gen 0's policy** on the network's own self-play states (keeps the human-like parts it learned, without replaying human games); (b) as above; (e) search (MCTS) at self-play time | (d) small: one extra forward pass of gen 0; (e) high | Gen 0 itself is slow at teching, so (d) alone won't fix pace; (b) gives positions where humans had more techs |
| Speed of finishing | turn cost, **already in S3b** (from gen 30) | — | Watch for it costing strength (pre-registered falsifier) |
| Knight chains | (a), (b); measure first (multi-kill enemy turns, Part II §4.4) | — | Not measured yet |
| Water control | measure first (ranged units lost to attacks from water, Part II §5.4) | — | Ports are built; the question is whether boats are used well |

Recommended next step: monitor these indicators every generation (cheap), and propose (b)
human-position starts as the next pre-registered experiment, since it targets several weaknesses
at once without labels.

## 0.3 What is verified, and against what

| Fact | Status |
|---|---|
| Unit, tech, improvement and tribe numbers | the game's data file (ground truth) |
| Tech cost = tier × cities + 4 | the game's `GetTechPrice` at 1 city (tiers 1–3: 5/6/7); the per-city slope is community-sourced |
| Damage and retaliation formula | matched the game's `BattleHelpers.GetBattleResults` on 27 open-field attacks (Part II §1.1) |
| Defence bonuses ×1.5 (forest, mountain, city) and ×4 (wall); fortify-only city bonus | community-sourced; fortify flags per unit from the data file |
| City level rewards, healing amounts, Market income, border growth | community-sourced (wiki), unverified |
| Tier lists | community, mostly not for this format; low confidence |

## 0.4 Indicators: built and to build

| Indicator | Where | Status |
|---|---|---|
| Techs by turns 10/20, cities by turn, stars banked, ports by turn 20, army mix, attacks | `py/indicators.py` | built (2026-10-24) |
| Tech-to-first-use latency (Part I §2.7) | — | to build |
| Mean city level excluding the capital (width vs depth, Part I §2.2) | — | to build: needs city levels from the observation tiles |
| Multi-kill enemy turns (knight chains, Part II §4.4) | — | to build: needs per-turn unit losses |
| Exposed ranged-unit turns, front line between ranged units and enemies (Part II §4.1) | — | to build |
| Ranged units lost to attacks from water (Part II §5.4) | — | to build |
| Turn of first contact, first village, first port | — | to build |


---

# Part I — Foundations, economy, expansion, tech and tribes


## 1. Core mechanics reference

### 1.1 Stars and income

Stars are the only currency: they pay for research, units, and improvements. Income is recalculated at the start of each turn from the sum of each city's level-based income plus improvement bonuses (market, port population, etc.).

- Each city level is worth **+1 star/turn** of base income (confirmed by wiki; not in the JSON data file). A level-1 city yields roughly 1 star/turn baseline, rising by 1 star per level thereafter — this is on top of whatever improvements the city works.
- Human capitals start producing **2 stars/turn**; bot capitals scale with difficulty (Easy 1, Normal 2, Hard 3, Crazy 5) — relevant only if your opponent pool includes bots, but useful for sanity-checking the "Crazy" baseline the human owner uses to judge the network.
- Tribe `startingStars` (from the JSON, i.e. guaranteed ground truth) varies a lot and is itself a balancing lever:

| Tribe | Starting stars | Starting tech | Starting unit |
|---|---|---|---|
| Luxidoor | 2 | none | Warrior |
| Bardur, Imperius, Kickoo, Vengir, Zebasi | 5 (default) | Hunting / Organization / Fishing / Smithery / Farming | Warrior / Warrior / Warrior / Swordsman / Warrior |
| Ai-Mo | 5 | Philosophy | Mind Bender |
| Oumaji | 6 | Riding | Rider |
| Hoodrick, Xin-xi, Quetzali, Yadakk | 7 | Archery / Climbing / Strategy(shields) / Roads | Archer / Warrior / Defender / Warrior |

(Imperius/Bardur/Kickoo/Vengir/Zebasi have no `startingStars` key in the data file, which I read as "uses the engine default of 5"; worth double-checking against the actual GameLogicAssembly default if you want zero ambiguity.)

**Signal:** `stars_banked_per_turn[t] = stars_available[t] - stars_spent[t]`. **Target:** should hover near 0 after the opening — a strong player ends almost every turn with either 0–2 stars left, or a deliberate, short-lived save-up toward a specific purchase (e.g. saving 2–3 turns for a Warrior + Monument together is fine; saving 10+ turns for no visible reason is not).
**Red flag:** stars sitting banked for 3+ consecutive turns while a legal, useful purchase (any improvement with cost ≤ banked stars, or a unit) was available and unbuilt. This is the single cleanest proxy for "wasting economy," and is cheap to compute from replay logs. **Importance: critical.**

Sources: [Technology — Polytopia Wiki](https://polytopia.fandom.com/wiki/Technology), [City — Polytopia Wiki](https://polytopia.fandom.com/wiki/City), GameLogicData28.txt `tribeData`.

### 1.2 Cities: levels, population, rewards

Population is a hidden counter per city; it fills from harvesting resources, training/killing units in combat near the city, and some improvements. To go from level *n* to level *n+1* the city needs *n* population past the previous threshold (i.e., level 2 needs 1 pop, level 3 needs 2 more, level 4 needs 3 more, etc. — thresholds grow linearly, so cumulative population to reach level L grows quadratically). Each level-up gives +1 star/turn income, +1 unit capacity, +50 score, and a choice of one of two rewards:

| Level reached | Choice A | Choice B |
|---|---|---|
| 2 | Workshop (lets you buy techs you don't have with stars, "literacy"-style) — note: some sources call this an explorer-adjacent reward; verify current wording in-client | Explorer (free auto-exploring Scout unit) |
| 3 | City Wall | +5 stars (resources) |
| 4 | Population Growth (+3 population, i.e. most of the way to the next level) | Border Growth (city radius expands from up to 8 tiles to up to 20 tiles) |
| 5+ | Park (+250 score, no economic effect) | Super Unit (one free powerful unique unit) |

Community guides consistently argue: take the 5-star option over City Wall at level 3 unless the city is about to be sieged (an early Wall is rarely worth the lost tempo); take Population Growth over Border Growth almost always, since Border Growth is only good when the newly-claimed tiles contain something valuable (a whale, a second resource cluster) and your city isn't already boxed in by a neighboring city — on a 14×14 1v1 map, cities are closer together than on bigger maps, so Border Growth has fewer good targets than usual. At level 5+, Super Unit is usually better than Park unless the city's super unit has nothing useful to do (hemmed in capital, no front line nearby).

**Signal:** for each level-up event, record which option was taken and whether a Wall/Border-Growth pick coincided with (a) an active siege/border threat for Wall, or (b) a valuable uncaptured tile in range for Border Growth. **Target:** Wall rate at level 3 should be low (rough prior: under ~20% of level-3-ups, spiking only when actually threatened); Border Growth rate at level 4 similarly low unless land is contested. **Importance: important** — individually each choice is a small star-equivalent swing, but they compound over a 20–30 turn game and are easy to get systematically wrong if the network hasn't learned the context-dependence.

Score: cities are worth 100 + 50×(level−1) points; this matters for score-based auxiliary rewards but not for Might victory, where only capturing all enemy capitals matters.

Sources: [City — Polytopia Wiki](https://polytopia.fandom.com/wiki/City) (via wikitext fetch), Steam guide "A not too simple guide", [Strategies — Polytopia Wiki](https://polytopia.fandom.com/wiki/Strategies).

### 1.3 Score

Score is irrelevant to who wins a Might game (only capital control matters), but it is a dense, well-defined auxiliary signal you may already be using for reward shaping, so it's worth having exact: army/territory points (5 points per star of a unit's cost; super units worth 50; 20 points per owned tile; 5 points per explored tile), city points (100 + 50 per level above 1; Parks +250), monument/temple points (monuments 400 each; temples 100 + 100 per level above 1, capped 500 at level 5), and technology points (100 per tech tier). Score is lost when a city is captured, a building destroyed, or a unit killed/disbanded. If you use score-delta as a shaping reward, note that it rewards territory and tech breadth somewhat independent of whether they help you win Might — a network could learn to farm score (explore, build temples) at the cost of military tempo, which looks exactly like the "expanded suboptimally" failure mode the human owner described. **Importance: important to flag, not necessarily to fix at the reward level.** *(Editor: our training uses no score shaping; the reward is win/loss, with a small turn cost on wins from S3b on. So score farming is not a cause here.)*

Sources: [Score — Polytopia Wiki](https://polytopia.fandom.com/wiki/Score).

### 1.4 Fog of war and vision

Each unit and city has a vision radius; unexplored tiles are hidden, revealed tiles outside current unit/city vision go to a "seen but stale" fog state showing last-known terrain but not current units. Villages and ruins are visible as icons once revealed even before approached; enemy units outside vision are invisible (no omniscience), which is why aggressive early scouting ("find a village by turn 3–4") is repeatedly cited as a top opening priority — it's both economic (more villages found = more expansion options) and informational (finding the enemy capital direction early shapes the whole game plan on a small map). Deeper vision-range numbers per unit and interactions with cloak/stealth belong to the companion document (units/tactics); the economic takeaway here is just: **scouting has compounding value and should not be delayed**.

**Signal:** turn number of first village sighted; turn number of first village captured; fraction of the map's land tiles revealed by turn 10. **Target:** first village found by turn 3–4 is called out explicitly by multiple sources as the single biggest early-game swing factor; on a 14×14 1v1 map with no allies to share vision, losing the scouting race is a bigger relative deficit than on larger team maps. **Importance: critical.**
**Red flag:** starting unit sits still or moves into unrevealed territory with no net new tiles revealed for 2+ consecutive turns in the first 6 turns.

Sources: [On Towards the Stars: Your First Ten Turns](https://theahura.substack.com/p/on-towards-the-stars-your-first-ten) (note: this blog mixes general 4X advice; the "find a village by turn 3-4" framing is Polytopia-specific and corroborated elsewhere), general community consensus.

### 1.5 Might victory condition

Might (the format's mode) is won by capturing **every other tribe's capital city** — not all cities, just capitals. In 1v1 this means: capture the opponent's one capital and the game ends immediately, full stop. This has a large strategic implication the economy sections return to repeatedly: in 1v1 Might there is no need to mop up the opponent's whole territory, which changes the calculus of "how much army is enough" and "when to go to war" relative to Domination-against-many-bots or Perfection (score race) framings that dominate a lot of generic strategy content online. Domination mode (the single-player analogue against bots) is scored on turns-to-win, battle efficiency, and difficulty, but that scoring is irrelevant to 1v1 Might — don't let turn-efficiency scoring intuitions from Domination bleed into the Might win condition, where only "capital captured, yes/no" matters.

**Signal:** this is the terminal signal itself (win/loss), but a useful intermediate one is *time-to-first-capital-threat*: the turn on which the winning side's army first reaches attack range of the loser's capital. **Importance: critical** (it's literally the objective), but note for training purposes: optimizing only for capital capture can underweight the economic buildup that makes the capital assault possible in the first place — hence all the economy indicators below.

Sources: [Game Modes — Polytopia Wiki](https://polytopia.fandom.com/wiki/Game_Modes) (via wikitext fetch).

### 1.6 Villages and ruins

**Villages** are neutral settlements; any unit that starts a turn standing on one captures it, instantly turning it into a level-1 city (ending that unit's turn, no star cost). Villages are placed with a minimum spacing rule (no two villages within the same 3×3 area), so on a Small 14×14 1v1 map there is a bounded, probably single-digit-per-player number of villages reachable before your opponent gets to them — this makes early scouting direction (§1.4) a scarce resource you're racing for, not something you can casually postpone.

**Ruins** also take a unit's whole turn to examine, but give a random reward instead of becoming a city: 10 stars; a free tech ("Scroll of Wisdom", only if the tech tree isn't fully researched); 3 free population to the capital; a free auto-exploring unit (only if unexplored tiles remain); a veteran Swordsman (land ruins) or veteran Rammer carrying a Warrior (water ruins); or (Aquarion-only, ocean ruins) a free level-3 walled city. There's no stated weighting toward any option and no negative outcome — ruins are pure upside, but opportunistic: guides explicitly warn against detouring deep to grab a ruin instead of grabbing a village, *unless* an opponent unit is close enough to grab it first, since a ruin in enemy hands can hand them a strong early unit or tech for free.

**Signal:** `villages_captured_by_turn[5, 10, 15]`; `turn_of_first_village_capture`; ratio of ruins-examined to villages-captured (should be low early — villages first). **Target (soft prior, cross-check against your human corpus):** on a 14×14 1v1 map, a competent player typically has their first non-capital city by turn 3–5 and 3–4 total cities by around turn 8–10, assuming no unusually hostile starting position — this is consistent with the general community guidance that "leveling capital to pop 3 for the resource bonus is almost always better than building an early unit" being balanced against grabbing nearby villages fast. **Importance: critical** — this is exactly the "expanded suboptimally" axis the human owner flagged, so treat city-count-by-turn-N as one of the highest-value diagnostic signals you can compute.
**Red flag:** a scout/warrior stands adjacent to or on top of a known, unguarded village for more than 1 extra turn without capturing it (pathing/priority bug, or a policy that doesn't value city count enough); or, symmetric mistake, repeatedly diverting early units to examine ruins while a visible village sits uncaptured.

Sources: [City — Polytopia Wiki](https://polytopia.fandom.com/wiki/City) (Village subsection via wikitext fetch), [Ruin — Polytopia Wiki, via r.jina.ai proxy fetch].

### 1.7 Borders

A city's border claims the tiles within its radius (up to 8 tiles for a fresh city; up to 20 if Border Growth is taken at level 4, see §1.2), and tiles inside a city's border are the ones that city can work (build improvements on, harvest from). Borders from two cities (yours or an opponent's) don't overlap claim — whichever city's border reaches a tile first owns it, which is why, on a small map, racing to found a second and third city in a promising direction can "claim" land before the opponent's border grows into it, even without any military contact. This is a second, quieter form of the "expansion" axis beyond raw city count: it's about *shape* — did the player's borders claim the resource-dense tiles nearby before either (a) the opponent's borders did, or (b) those tiles went permanently unclaimed because no city was ever founded close enough.

**Signal:** fraction of resource tiles (crop/fruit/game/fish/metal) within N tiles of a founded city that are inside *some* owned border by turn 15, vs. fraction still neutral/enemy. **Importance: important.**

Sources: Steam guides (border growth discussion), [City — Polytopia Wiki].

### 1.8 Tech cost scaling with city count

This is one of the most important and most precisely knowable numbers in the whole economy, and it is the direct lever behind the human owner's "didn't exploit newly researched techs" observation (research timing interacts tightly with city count, so a misjudged research order compounds).

**Formula** (cross-checked against the `cost` field in `techData`, which stores a tier 0–3 per tech, against the community-reported absolute formula):

> Research cost (stars) = tier × (number of cities owned) + 4

giving, at 1 city, Tier-1 = 5, Tier-2 = 6, Tier-3 = 7 — matching the tier numbers in the JSON (`basic`=0, most early techs=1, mid techs=2, late techs=3). Each additional city adds 1/2/3 stars to a Tier-1/2/3 tech respectively:

| Cities owned | Tier 1 cost | Tier 2 cost | Tier 3 cost |
|---|---|---|---|
| 1 | 5 | 6 | 7 |
| 2 | 6 | 8 | 10 |
| 3 | 7 | 10 | 13 |
| 4 | 8 | 12 | 16 |
| 5 | 9 | 14 | 19 |
| 6 | 10 | 16 | 22 |

Philosophy's Literacy ability cuts research cost by 33% (rounded up), which is a standing argument for the Organization→Philosophy line in economy-oriented games.

**The critical consequence for training diagnostics:** every additional city makes *every future tech* permanently more expensive. This means (a) there's a real, quantifiable cost to expanding "for free" — it's not just opportunity cost of stars spent on settling, it's a tax on the whole future tech curve — and (b) the order "found city → then research" vs "research → then found city" matters: research planned before a city count jump is cheaper than the same research after. A policy that researches reactively/late, after city count has already risen, is silently overpaying for every subsequent tech, which compounds into exactly the kind of mid-game economic softness that would show up as "beats weak opponents but loses to a disciplined one."

**Signal:** `tech_cost_paid[t] / tech_cost_cheapest_possible_at_that_tier_and_city_count[t]` — should be ≈1.0 if research timing is well-ordered; **Signal 2 (tech-exploitation latency):** for each tech researched, the number of turns until the first unit/improvement that the tech unlocks is actually built. **Target:** this latency should typically be 0–1 turns for improvements that were already affordable and had an available tile (e.g., researching Farming the same turn you have an unworked crop tile and spare stars should produce a Farm that same turn or the very next); longer latency is only justified when no valid tile/star budget exists yet. **Importance: critical** — this is a direct, cheap-to-compute proxy for the exact "didn't exploit newly researched techs" weakness reported. A simple, high-value diagnostic: histogram tech→first-use latency across the self-play corpus and flag techs whose median latency is suspiciously high (e.g., Smithery researched but Forge never built nearby, or Riding researched but no Rider ever trained for several turns after).
**Red flag:** a tech is researched and then the matching improvement/unit is never built for the rest of the game, or only built 5+ turns later despite a legal tile and sufficient stars existing earlier. Also flag: researching a tech whose only purpose is a unit/building the player has no terrain to use (e.g., Mining with zero mountain tiles in territory) — a "wasted tech" pattern, distinct from "unexploited tech."

Sources: [Technology — Polytopia Wiki] (cost formula, via r.jina.ai), GameLogicData28.txt `techData` (tier values cross-checked). *(Editor: verified against the game's own `GameLogicData.GetTechPrice` at one city: tiers 1/2/3 cost 5/6/7.)*

---

## 2. Economy and expansion

This is the section most directly aimed at the two reported weaknesses. Read §2.6–§2.8 first if you're triaging.

### 2.1 Opening build order (first 5 turns)

Sources agree on the shape, though exact turn-by-turn sequencing is tribe- and map-dependent (and much of the *very* turn-by-turn advice online is written for Perfection/bigger maps, not 1v1 Lakes Small — treat specifics as directional, not gospel):

1. **Turn 0:** If your tribe can reach a city level-up immediately from starting resources (Imperius, Bardur, Kickoo, and several others are specifically called "upgrade-class" tribes for this), take it — it's close to a free, instant +1 star/turn and a reward choice, and multiple sources call *always* taking an available T0 capital upgrade over training an extra Warrior a near-universal rule. Move your starting unit to reveal as much terrain as possible.
2. **Turns 1–2:** Research your tribe's first available cheap tech that either unlocks a resource improvement you're sitting on, or keep exploring. Harvest any resource tile adjacent to the capital that gives population (don't leave a harvestable crop/fruit/game/fish/metal tile unworked once you can afford the one-time harvest cost — these are typically cheap, 2–3 stars, one-time, and immediately convert to population/levels).
3. **Turns 2–4:** Decide, based on what's been revealed: is there a village within reach? If yes, beeline a unit to it — city count compounds (§1.8) and starting city count low is strictly better for every later tech purchase. If the position is hostile (enemy sighted early, since this is 1v1 with no allies to screen you), consider a second Warrior/Archer for defense before further economic investment.
4. **Turns 3–5 (Lakes specifically):** Lakes maps guarantee internal water features even on a small map, so Fishing is a very commonly strong early pick if adjacent to water/fish — Fishing is now Tier-1 cost (post "Path of the Ocean" rework; confirmed in the JSON, `fishing.cost == 1`) and leads directly to Port (7 stars) and Sailing, letting you scout/expand across the lake instead of only by land. Since the format's map is Lakes, check early whether your capital or an adjacent tile touches water before committing a 1v1 opening plan that assumes pure-land expansion.
5. By turn 5, a disciplined 1v1 opening has typically: upgraded the capital at least once, captured 0–2 villages (depending on luck/position), and either researched or be about to research the tech that lets the second city's terrain pay off (e.g., Farming if cropland was found, Fishing if coastal).

**Signal:** `cities_owned[5]`, `cities_owned[10]`, `stars_per_turn[5]`, `stars_per_turn[10]`, and the latency between "tile revealed" and "tile harvested/worked" for the first 10 resource tiles discovered. **Target (soft):** community guidance repeatedly treats "20 SPT by turn 10" as a benchmark for a strong Perfection-mode game on bigger maps; a 1v1 Lakes Small game will generally have a *lower* absolute ceiling (fewer total tiles, one opponent racing you for the same land) — until you have your own corpus numbers, use turn-10 SPT and city-count relative to your human-game dataset's percentiles rather than this absolute figure. **Importance: critical** for the early game as a whole; each individual tile-harvest-latency data point is `minor` on its own but `important` in aggregate (a systematically slow harvester is a real, fixable defect).

Sources: [On Towards the Stars](https://theahura.substack.com/p/on-towards-the-stars-your-first-ten) (general framing — written with some non-Polytopia-specific noise, use cautiously), Steam "General Strategy Roadmap" guide, [Strategies — Polytopia Wiki], tribe-specific wiki pages (§4).

### 2.2 Capital growth vs. expansion

There is a genuine trade-off, not a strict dominance either way:

- **Growing the capital** (harvesting its tiles, leveling it up) is capital-efficient star-for-star early, because the capital usually starts with better infrastructure and a head start on population, and because every level is +1 star/turn *forever*, which pays back fast.
- **Expanding** (capturing villages, eventually founding cities via Colonize-equivalent mechanics covered in the companion unit/tactics document) adds a second income stream and, crucially, **locks in land before the opponent's border or units get there** — but each new city also raises the cost of every future tech (§1.8), and a brand-new level-1 city is a low-income, low-defense liability until it's developed.

The resolved community heuristic (echoed across tribe-strategy pages) is: **grow the capital to around level 3 early (the resource/economy payoff), while expanding opportunistically whenever a village is reachable without seriously detouring from that capital development** — i.e., these aren't sequential phases, they're parallel priorities from turn 1, and the failure modes are at the extremes: (a) over-indexing on capital growth while visible, grabbable villages go uncaptured (lets the opponent's effective city-count/tech-cost advantage compound), or (b) over-indexing on expansion while the capital stalls at level 1–2 with poor income, producing lots of weak, underdeveloped cities that can't afford the techs or units to defend themselves (this is the "beat Crazy 96% but lost to Ben" shape reported — Crazy may not punish an over-expanded, under-defended economy the way a skilled human does).

**Signal:** `capital_level[t]` vs `second_city_level[t]` vs `total_cities[t]`, tracked turn by turn; also `mean_city_level_excluding_capital[t]` — a fleet of level-1 cities that never grow is the "expanded suboptimally" pattern made concrete. **Target:** by turn 10–12, expect capital at level 3+ and at least one other city past level 2, not a pile of level-1 outposts. **Importance: critical.**
**Red flag:** `total_cities[t]` increasing while `mean_city_level_excluding_capital[t]` stays pinned near 1 for many turns — i.e., width without depth. Also a red flag in the opposite direction: `total_cities[t]` flat at 1 (capital only) past turn ~8 on a map where villages were visible and reachable — depth without width.

Sources: [Strategies — Polytopia Wiki] (Bakalol guide, via search extraction), tribe strategy pages (§4), Steam "A not too simple guide".

### 2.3 Capturing villages (mechanics recap + strategic framing)

See §1.6 for the mechanic. Strategically: village value is front-loaded — the earlier a village is captured, the more turns it has to grow and the cheaper it was in opportunity cost (every tech is cheaper before you take it). Community guides explicitly note that **tech gets more expensive the moment you take a new village**, so there's a known, exploitable sequencing trick: *research the tech you're about to need, the turn before taking a new village, not after* — squeezing in the cheaper price while city count is still low. This is a crisp, checkable rule.

**Signal:** for each village capture event, compare the stars spent on research in the 1–2 turns immediately before vs. after the capture — a well-sequenced player clusters "cheap" research just before city-count increments. **Importance: important** (a clean, specific instance of the general tech-timing discipline in §1.8, concrete enough to grade directly).

Sources: community strategy threads (search-extracted), [Technology — Polytopia Wiki].

### 2.4 Exploring

Exploration is not just informational — on a small 1v1 map with no allied vision-sharing, every turn spent not expanding your revealed frontier is a turn your opponent might be the one to reach the next village, ruin, or defensible chokepoint first. The free "Explorer" unit (an option at city level 2, see §1.2) auto-paths to reveal fog without manual micromanagement, which is a cheap way to keep the scouting frontier moving without diverting a combat unit; sources note it can also be worth leveraging explorers to find and steal/race for techs via ruins' Scroll-of-Wisdom reward.

**Signal:** cumulative fraction of map tiles revealed by turn T for T ∈ {5, 10, 15}; number of active "exploring" units (those whose most common action class is movement into unrevealed fog) per turn in the opening. **Target:** exploration should not fully stop once 1–2 villages are found — a player who explores just enough to find the nearest village and then stops is leaving later-game intel (where is the opponent's second city, where is the capital, where are remaining neutral villages/ruins) on the table, which matters a lot in a 1v1 Might race to the enemy capital. **Importance: important.**

Sources: [On Towards the Stars](https://theahura.substack.com/p/on-towards-the-stars-your-first-ten), [City — Polytopia Wiki] (Explorer reward).

### 2.5 Star efficiency of improvements

All costs below are the build costs from `improvementData` in `GameLogicData28.txt` (ground truth); the *income* formulas (how much a building actually produces per turn) are **not** in that JSON and come from the wiki, with the explicit caveat below that the Market's exact current coefficient is contested across sources and should ideally be measured empirically from your own replay logs rather than trusted from any single web source.

| Improvement | Cost (stars) | Terrain/resource requirement | Effect | Notes |
|---|---|---|---|---|
| Road | 3 | field/forest | Movement connector | Needed for overland unit speed and (with Trade) city-to-city connections |
| Farm | 5 | crop | +2 population, one-time-ish | Needs a prior one-time Organization "harvest fruit"-style unlock per resource type; farm itself is a standing improvement (`maxLevel: 0` in JSON = not levelable) |
| Fishing (hut) | 2 | fish | +1 population, consumed on build | Cheap, one-time; take it immediately wherever a fish tile is in range |
| Hunting (hut) | 2 | game | +1 population, consumed | Same as above for game/animal tiles |
| Harvest Fruit | 2 | fruit | +1 population, consumed | Same pattern; organization-tech |
| Mine | 5 | metal | +2 population | Mirrors Farm for metal terrain |
| Port | 7 | water/wetland edge | +1 population; opens trade-route range up to ~4 water tiles to other ports/cities | Reduced from a higher legacy cost in the Path of the Ocean rework (web sources describe the cut; cost 7 is confirmed in the JSON) |
| Windmill | 5 | field, adjacent to Farm/Aquafarm | Scales with adjacent Farm levels (levelable up to 8) | A "multiplier" building — only buy once you have a Farm adjacent, not before |
| Sawmill | 5 | field, adjacent to Lumber Hut | Scales with adjacent Lumber Hut levels (levelable up to 8) | Same multiplier pattern, for forestry |
| Forge | 5 | field/forest, adjacent to Mine | Scales with adjacent Mine levels (levelable up to 8) | Same multiplier pattern, for mining |
| Market | 5 | field, adjacent to Windmill/Forge/Sawmill | Star income scaling with adjacent production-building levels (levelable up to 8) | **Contested exact coefficient** — see note below |
| Temple / Forest Temple / Water Temple / Mountain Temple | 20 / 15 / 20 / 20 | matching terrain | +1 population, +100 score, grows over time (levelable to 4, "patina" ability) | Primarily a score/long-game building; population payoff is real but slow — weigh against military spend in a fast 1v1 |
| Monument (×7 variants) | — (score/achievement-gated, not star-purchased like others) | field/water/ice | +3 population, +400 score, one-time, unique (max 1 per empire) | Community advice: spend monument effects immediately for economic/military acceleration rather than hoarding them defensively |
| Lumber Hut | 3 | forest | +1 population | Forestry-tech equivalent of Farm/Mine for forest tiles |
| Clear Forest / Burn Forest | 0 / 3 | forest | Converts forest→field (clear: +1 star one-time; burn: converts to field+crop) | Terrain-shaping tool, not a steady income source |

**On the Market/Customs House question specifically:** the pre-"Path of the Ocean" building was the **Customs House** (2 stars/turn per adjacent friendly Port, max one per city) — several older guides you'll find online still describe this and are **out of date** for the pinned version. The current data file confirms Customs House no longer exists as a key in `improvementData`; it has been replaced by **Market** (adjacency to Windmill/Forge/Sawmill instead of Port). Web sources disagree on the exact current numbers: one describes "1 star per adjacent level," another "2 stars per unique adjacent building, doubled if next to a Port," and the Market wiki page's own body text says the port-doubling is a *former* mechanic now relegated to a trivia note. **This is a case where the engine's own behavior should be measured directly from self-play logs** (regress a city's star income delta against the levels of adjacent Windmill/Forge/Sawmill and the presence of a Port before/after building a Market) rather than taken from any single source here — flagging this explicitly as an open question. **Importance of resolving it: important**, since Market is the backbone of the mid-game star economy and getting its ROI wrong (over- or under-building it) is a plausible real contributor to economic softness.

**Signal (general improvement efficiency):** stars spent on multiplier buildings (Windmill/Sawmill/Forge/Market) *before* the thing they multiply exists (i.e., buying a Windmill with no adjacent Farm yet) — these buildings do nothing until their adjacency condition is met, so early purchase without the prerequisite is pure waste. **Red flag:** any Windmill/Sawmill/Forge/Market built with zero qualifying adjacent improvement at build time. **Importance: important** (cheap, unambiguous, and catches a specific class of build-order error).

Sources: GameLogicData28.txt `improvementData` (costs, adjacency structure — ground truth), [Market — Polytopia Wiki] and [Port — Polytopia Wiki] (via r.jina.ai wikitext fetch, contested income coefficients flagged above), [Customs House — Polytopia Wiki], community search results on the Path of the Ocean rework.

### 2.6 When to stop expanding and switch to military — diagnosing "expanded suboptimally"

This is squarely the human owner's first concern, so let's be concrete about what "suboptimal expansion" can mean and how to tell which failure mode is occurring:

1. **Expanding too little** (under-expansion): low city count relative to the map's carrying capacity and the opponent's city count, income plateaus early, and the player tries to win on unit quality alone. Signal: `own_cities[t] < opponent_cities[t]` sustained for many turns with no military compensation (no attacking army being built either). This is "passive and behind," not actually safe.
2. **Expanding too much, too late to consolidate** (over-expansion / "wide but soft"): high city count but low mean city level (§2.2's red flag), weak defenses, and a tech curve that's gotten expensive (§1.8) without the corresponding income to match — classic "beat weak bots, lose to a disciplined human" shape, because a human opponent will identify and punish the softest city rather than attacking wherever, while a weaker bot (Crazy) may attack less surgically.
3. **Expanding in the wrong direction** (land grabbed that doesn't help): cities founded far from the capital's support range, on poor terrain, or in a direction that doesn't block the opponent's approach — city *count* looks fine but the shape doesn't serve either economy or defense. Signal: distance from each non-capital city to the capital and to the nearest enemy unit/city over time; terrain quality (resource tile count in radius) of each founded city.
4. **Correct stopping point:** multiple sources frame the expansion→military switch not as a fixed turn number but as a *relative* comparison: stop opportunistic expansion and start converting income to army once (a) no more villages/good land are reachable without military force, or (b) the opponent's visible army or city count starts to threaten your weakest point. In 1v1 Might specifically — where only the capital matters for the win condition — there's an additional, sharper trigger: once you can see or reasonably infer the opponent's capital location and your army (plus the time to get there) outpaces their capital's defenses, further pure economic growth has a shrinking marginal value relative to converting stars into the capital-ending army.

**Signal (the core diagnostic the human owner needs):** plot, per game, `(own_cities, opponent_cities, own_mean_city_level, own_army_value, opponent_army_value)` against turn number, and look specifically at games lost to strong opponents (Ben) vs. won against weak ones (Crazy): if losses systematically show high city count / low mean level / low army value at the turn the opponent's attack lands, that is a direct, quantitative confirmation of failure mode #2 above, and the fix is in the *reward trade-off between settling and army spend*, not in map exploration or combat micro. **Importance: critical** — this is the single most actionable indicator requested.
**Red flag set:** (a) founding a new city while 0 combat units exist and an enemy unit has been sighted within, say, 6 tiles of any owned city in the last 3 turns; (b) city count still rising past the point where the opponent's visible army value exceeds 1.5× your own army value; (c) mean non-capital city level flat near 1 past turn 15 while city count is ≥4 (very likely over-expansion without consolidation).

Sources: Steam "General Strategy Roadmap" (scaling rule: "only engage if enemy cities + current opponents ≤ your own city count" — a useful if rough heuristic, written for Domination against multiple bots rather than 1v1 Might, so treat the exact ratio loosely), [Strategies — Polytopia Wiki], general 1v1-Might-specific reasoning above (synthesized from the mechanics in §1.5, not a direct quote from any single source).

### 2.7 Leveraging newly researched techs immediately — diagnosing "didn't exploit new techs"

This is the human owner's second concern, and it has a clean operational definition given §1.8's tech-to-action latency signal. To make it actionable, break it into the specific sub-patterns worth separately tracking, because "didn't exploit new techs" could mean any of these:

- **Build-latency failure:** tech researched, qualifying tile and sufficient stars both exist, but the corresponding improvement isn't built for several turns (or ever). *Likely root cause if present broadly:* the policy doesn't have good "what just became legal" awareness, or the improvement-placement sub-policy is undertrained relative to movement/combat.
- **Unit-latency failure:** tech unlocks a unit (e.g., Riding→Rider, Smithery→Swordsman, Mining→Mining→Smithery→Swordsman chain) but the player doesn't train one for many turns despite stars being available and a use for it existing (open front, undefended city). *Likely root cause:* training-decision sub-policy undervalues new unit types, possibly because self-play under-sampled games where that tech path was taken, so the policy never learned when the new unit is good.
- **Research-without-plan failure:** a tech is researched that leads nowhere useful for this game (no matching terrain, no matching strategic need) — wasted stars that should have gone to a tech that mattered, or to direct spending. This is different from the two above: it's not under-using what was unlocked, it's unlocking the wrong thing to begin with.
- **Same-turn compounding failures:** research → build → train chains that *could* happen in one highly efficient turn (research a cheap tech, immediately build the now-legal improvement, immediately train the now-legal unit, if stars allow) but are instead spread across several turns for no apparent reason, losing tempo even if nothing is technically "wasted."

**Signal set:**
- `tech_to_first_build_latency` and `tech_to_first_train_latency`, per tech, aggregated across the self-play corpus as a distribution (median, 90th percentile, and fraction that's "never" within the game).
- `fraction_of_researched_techs_with_zero_matching_terrain_or_use` — direct measure of research-without-plan.
- `same_turn_research_build_rate`: fraction of tech-research events where the newly unlocked improvement (if its prerequisites were already otherwise met) is built the same turn.
**Target:** latency medians near 0–1 turns for techs whose prerequisites are already satisfied; "never built" rate should be low and concentrated only in techs that genuinely don't apply to that game's map. **Importance: critical** — directly named by the human owner as an observed weakness, and cheap to compute as a per-tech histogram across the whole self-play dataset, which should make it easy to tell whether this is a general policy weakness or concentrated in a few specific tech→action transitions (e.g., maybe the network is fine exploiting Farming but consistently sits on Smithery).

Sources: synthesized from §1.8's tech-cost mechanics plus general strategy-guide framing of "use techs as soon as you get them" (community consensus, e.g. Steam guides and wiki strategy pages cited above); the specific instrumentation design here is original synthesis for this document's stated purpose, not drawn from any single source.

### 2.8 Consolidated red-flag checklist (economy/expansion)

For quick scanning when triaging a batch of games:

| Red flag | Area | Importance |
|---|---|---|
| Stars banked 3+ turns with a legal purchase available | Income discipline | Critical |
| Visible unguarded village uncaptured 2+ turns after a unit is adjacent | Expansion tempo | Critical |
| City count rising while opponent's army value already exceeds ~1.5× own army value | Over-expansion vs. military switch | Critical |
| Mean non-capital city level flat near 1 past turn ~15 with city count ≥4 | Width-without-depth expansion | Critical |
| Tech researched, prerequisites met, matching improvement never built in-game | Tech exploitation | Critical |
| Tech researched, unlocked unit never trained despite available use and stars | Tech exploitation | Critical |
| Windmill/Sawmill/Forge/Market built before its adjacency prerequisite exists | Build-order waste | Important |
| Tech researched with no matching terrain/use in this game | Research planning | Important |
| City Wall taken at level 3 with no active/recent threat | Reward-choice context sensitivity | Minor–Important |
| Border Growth taken at level 4 with no valuable tile in new radius | Reward-choice context sensitivity | Minor–Important |
| Exploration frontier stalls (no new tiles revealed 2+ turns) in the first 6 turns | Scouting tempo | Critical |
| New city founded with zero combat units in the empire while an enemy unit was recently sighted nearby | Expansion/defense balance | Important |

---

## 3. Tech tree

### 3.1 Full reference (standard 12-tribe tree)

All unlock lists below are read directly from `techData` in `GameLogicData28.txt` (ground truth); in-game display names sometimes differ from the internal keys (noted where known). Tier = relative cost bracket; absolute star cost depends on city count per §1.8.

| Internal key | Display name (if different) | Tier | Unlocks techs | Unlocks units | Unlocks improvements/abilities |
|---|---|---|---|---|---|
| basic | — | 0 | Organization, Climbing, Fishing, Hunting, Riding | Warrior, Giant, Scout(explorer), Transport ship | City, Ruin examination |
| riding | Riding | 1 | Roads, Free Spirit | Rider | — |
| climbing | Climbing | 1 | Mining, Meditation | — | Mountain defense bonus, 2× mountain movement |
| fishing | Fishing | 1 | Sailing, Ramming | — | Port, Fishing Hut, 2× water movement |
| hunting | Hunting | 1 | Archery, Forestry | — | Hunting Hut |
| organization | Organization | 1 | Farming, Strategy(shields) | — | Harvest Fruit |
| free spirit | Free Spirit | 2 | Chivalry | — | Temple, Disband ability |
| roads | Roads | 2 | Trade | — | Road, Bridge |
| strategy (shields) | Strategy | 2 | Diplomacy | Defender | Peace Treaty ability |
| farming | Farming | 2 | Construction | — | Farm |
| mining | Mining | 2 | Smithery | — | Mine |
| meditation | Meditation | 2 | Philosophy | — | Mountain Temple |
| sailing | Sailing | 2 | Navigation | Scout ship | ocean movement +1 |
| archery | Archery | 2 | Spiritualism | Archer | forest defense bonus |
| forestry | Forestry | 2 | Mathematics | — | Lumber Hut, Hidden Sanctuary, Clear Forest |
| chivalry | Chivalry | 3 | — | Knight | Destroy ability |
| trade | Trade | 3 | — | — | Market, "Wealth" task |
| diplomacy | Diplomacy | 3 | — | Cloak | Embassy, Capital Vision abilities |
| construction | Construction | 3 | — | — | Windmill, Burn Forest |
| smithery | Smithery | 3 | — | Swordsman | Forge |
| philosophy | Philosophy | 3 | — | Mind Bender | Literacy (−33% research cost) ability, "Genius" task |
| navigation | Navigation | 3 | — | Bomber ship | Starfishing |
| spiritualism | Spiritualism | 3 | — | — | Grow Forest, Forest Temple |
| mathematics | Mathematics | 3 | — | Catapult | Sawmill |

(Aquatism and the water-temple branch exist in the data but their normal prerequisites — `ramming`, `sailing`, `aquaculture`/`hydrology` — belong more to the naval/companion document; mentioned here only because Aquatism unlocks the Water Temple, relevant to the economy table in §2.5.)

### 3.2 What's worth it, what's a trap, in a 1v1 Lakes Small Might game specifically

- **Always worth it early:** whichever Tier-1 tech directly unlocks the resource you're sitting on (Organization for fruit, Hunting for game, Fishing for fish/water — doubly so on Lakes, where water tiles are guaranteed near most of the map — Climbing if mountains dominate your start). These are cheap and pay back almost immediately.
- **Usually worth it, timed deliberately:** Roads→Trade (Market) once you have 2+ Windmill/Forge/Sawmill-eligible tiles developed — Market is wasted stars before that (§2.5); Philosophy for the Literacy discount in a game you expect to run long enough to recoup the research-time investment (less clear-cut in a fast 1v1 than in a multi-opponent game, since 1v1 Might games can end quickly if one side snowballs — don't over-invest in long-game economy techs if a short, decisive war is realistically in reach).
- **Situational / can be a trap:** Meditation→Mountain Temple and the other Temple lines are population+score investments with a 20-star price tag and slow payback (temples "grow" over time rather than paying instantly) — fine in a long, peaceful build-up, a poor use of 20 stars if you're already racing toward a military timing. Border-relevant Diplomacy (Embassy/Peace Treaty) is far more valuable in multi-opponent games than in a 1v1 where there's no one to make peace with or embassy for economic bonus from — in strict 1v1 Might, Diplomacy is close to a trap unless its Cloak unit specifically is wanted for scouting/sabotage (a companion-document combat question).
- **Military techs, timed to the switch point in §2.6:** Archery (Archer) and Strategy/shields (Defender) are cheap, early, dual-purpose (economy-adjacent: Strategy also leads toward Diplomacy, Archery toward Spiritualism) and reasonable to grab opportunistically even before committing to war; Chivalry (Knight) and Smithery (Swordsman) are the core 1v1 "go to war" techs and should be timed to when §2.6's switch-to-military trigger fires, not researched reflexively early (a Knight tech path researched turn 3 with no army to use it yet is stars that could have been expansion instead).

**Signal:** fraction of games where a Temple-line or Diplomacy tech is researched before the game's own military resolution turn (i.e., it never got "used" in time) — a direct trap-detector. **Importance: important.**

Sources: GameLogicData28.txt `techData` (ground truth for unlocks/tiers), [Technology — Polytopia Wiki], community strategy guides cited in §2, tribe-specific pages in §4 for tribe-conditioned tech paths.

---

## 4. Tribes

### 4.0 How to read each entry

For each tribe: starting tech/unit/resources (ground truth from `tribeData`), terrain/resource bias (ground truth `terrainModifier`/`resourceModifier`), strengths/weaknesses, opening plan, tech path, Lakes-1v1-specific notes, how to beat it, and competitive standing. **Tier-list caveat:** tier lists found online are overwhelmingly written for either general Domination-vs-bots play or multi-player/team Perfection, not specifically 1v1 Might on Lakes Small — I flag this every time a tier claim is quoted, since it's a real and recurring source of mismatch with this book's actual target format.

### 4.1 Xin-xi

- **Start:** Climbing tech, Warrior, 7 stars, 2× Metal / 1.5× Mountain terrain.
- **Strengths:** Early mountain vision and movement (2× on mountains) is a strong scouting tool on a map with any elevation; metal-rich territory sets up a Mining→Smithery Swordsman-economy path; mountain defense bonus helps hold chokepoints.
- **Weaknesses:** Climbing is not an economy tech and doesn't lead to one directly — no T0 capital upgrade is typical, giving Xin-xi one of the slower starts among the 12. Needs the opponent to also have Climbing to fight at parity on mountains (otherwise Xin-xi has a one-sided terrain advantage, which cuts the other way if the opponent already has Climbing).
- **Opening plan:** Scout mountains aggressively for villages/ruins/early enemy sighting; if metal is present, go Mining→Smithery for a Swordsman timing; otherwise pivot to whatever resource dominates the revealed start.
- **Lakes 1v1 notes:** Mountains plus lake water is a common Lakes-map combination; Xin-xi's mountain vision can partially compensate for the slower economic start by finding the opponent (and their capital direction) earlier than they find you.
- **Countering Xin-xi:** Punish the slow start — an opponent who can level their capital on turn 0 (Imperius/Bardur/Kickoo) has a tangible tempo lead in the first several turns; press that lead before Xin-xi's metal economy comes online.
- **Competitive standing:** Mixed — one tier-list source placed Xin-xi in C tier for regular 1v1 land maps (contested; tier lists vary a lot by source and are mostly not Lakes-1v1-Might-specific).

Sources: GameLogicData28.txt `tribeData.xinxi`, [Xin-xi — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.2 Imperius

- **Start:** Organization tech, Warrior, 5 stars, 2× Fruit / 0.5× Game.
- **Strengths:** Organization is a genuine economy tech (unlocks Farming/Strategy) and commonly lets Imperius hit a T0 capital upgrade; sets up naturally toward Strategy→Diplomacy for longer games, and a Shields/Navigation-adjacent naval-leaning path is cited by some guides.
- **Weaknesses:** The 2× fruit / 0.5× game bias means less access to Farms specifically (fruit tiles give population directly via Harvest Fruit rather than feeding the denser Farm improvement, which needs Crop tiles, not Fruit) — a subtlety worth remembering: fruit and crop are different resources in the data (`fruit` vs `crop`), so an Imperius-heavy-fruit start does not automatically mean lots of Farms. This can leave Imperius needing more total tech investment to reach the same city levels as a Farm-rich tribe.
- **Opening plan:** Take the T0 upgrade when available; research toward whichever of Farming/Strategy fits the revealed terrain; expand opportunistically.
- **Lakes 1v1 notes:** No special water bias, so Lakes play is "generic" for Imperius — plan around whatever the specific map seed gives rather than a tribe-conferred water strength.
- **Countering Imperius:** Their early tempo is real but not overwhelming; a tribe with its own T0 upgrade (Bardur, Kickoo) can match pace, and Imperius's fruit-heavy, Farm-poor terrain can be exploited by racing the tech tree if the map doesn't hand them crop tiles.
- **Competitive standing:** Frequently rated S/A tier for 1v1 land maps in community tier lists (one source placed it S tier) — one of the more consistently well-regarded tribes across sources, though again these lists are not specifically Lakes-1v1-Might-calibrated.

Sources: GameLogicData28.txt `tribeData.imperius`, [Imperius — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.3 Bardur

- **Start:** Hunting tech, Warrior, 5 stars, 1.5× Fruit / 0.8× Forest / 0× Crop.
- **Strengths:** Widely regarded as one of the best, easiest, most consistently viable tribes — strong early economy (T0 capital upgrade available), good fruit+game population income, and a clean Organization/Forestry→Mathematics tech path praised for long-game logistics (Sawmill economy, Catapults). Multiple sources put Bardur at or near the top of tier lists across map types.
- **Weaknesses:** Zero crop bias means no native Farm economy — fine, since fruit/game substitute, but it does mean Bardur can't lean on Farms specifically; reduced forest (0.8×) slightly limits Lumber Hut density relative to a forest-biased tribe like Hoodrick.
- **Opening plan:** Move the Warrior turn 1 to reveal tiles, research Organization immediately, harvest fruit/game to level the capital, add a second Warrior only once SPT has actually gone up (not before) — this exact sequencing is explicitly recommended in a community guide and is a good concrete opening template for a training curriculum's "reference policy" comparisons.
- **Lakes 1v1 notes:** No special water terrain bias; generic Lakes play, same caveat as Imperius.
- **Countering Bardur:** There isn't a clean terrain-based counter; the main lever is tempo — Bardur's strength is a steady, low-variance economy, so a sharper, faster aggressive tribe (Vengir, or an early-Archer Hoodrick) that can threaten the capital before Bardur's economy compounds is the more promising counter-plan than trying to out-economy them.
- **Competitive standing:** Consistently high — SSS/S tier in multiple sources, A tier in the most recent search result for regular 1v1 land maps. One of the safest defaults if the AI's opponent pool needs a strong, "generically correct" baseline tribe.

Sources: GameLogicData28.txt `tribeData.bardur`, [Bardur — Polytopia Wiki] (via r.jina.ai), [Strategies — Polytopia Wiki] (Bakalol tier list), recent tier-list search synthesis.

### 4.4 Oumaji

- **Start:** Riding tech, Rider, 6 stars, 0.2× Forest / 0.2× Game / 0.5× Mountain.
- **Strengths:** Immediate mobility — the starting Rider (2 movement) is excellent for fast scouting and reaching distant villages before any other tribe can, which matters a lot for the §1.6/§2.1 scouting race on a small map. Good hit-and-run raiding tool once a second unit joins it.
- **Weaknesses:** Riding is explicitly *not* an economy tech and doesn't lead toward one — captured villages won't level up quickly without separately investing in Farming or another economy tech, so Oumaji's fast land-grab can turn into a wide-but-shallow (§2.2 red flag) empire if the player doesn't consciously pivot to economy tech after the early land rush.
- **Opening plan:** Use the Rider's speed to grab the 1–2 closest villages immediately, then pivot hard into Organization/Farming (whichever fits terrain) before the new cities stagnate at level 1.
- **Lakes 1v1 notes:** Speed advantage is most valuable when there's a lot of explorable land between the two capitals, which Lakes maps (broken up by water) can either help (chokepoint control) or hurt (water blocks the Rider's path, requiring a Port/boat pivot earlier than a pure-land tribe would need).
- **Countering Oumaji:** Expect an early rush at your villages; a tribe with its own fast unit or an early defensive tech (Strategy for Defender) can blunt the land grab; since Oumaji's own economy is a known weak point, out-teching them in the mid-game (by turn 10–15) is a realistic win condition even after losing the early scouting race.
- **Competitive standing:** Lower-mid tier in most sources ("comparable to Ai-Mo and Quetzali" per the wiki's own self-assessment); B tier in the most recent tier-list search result for regular 1v1 land maps.

Sources: GameLogicData28.txt `tribeData.oumaji`, [Oumaji — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.5 Kickoo

- **Start:** Fishing tech, Warrior, 5 stars, 2× Water / 1.5× Fish / 0.5× Mountain.
- **Strengths:** One of the strongest tribes per multiple sources — T0 capital upgrade available, and starting Fishing (now Tier-1 post-rework, confirmed in the JSON) gives immediate water movement and a fast path to Port/Scout-ship-based exploration, which is unusually good specifically on a **Lakes** map, since water is guaranteed nearby. Strong fish-based population income.
- **Weaknesses:** Halved mountain resource rate limits a Mining/Smithery pivot if the map happens to be mountain-heavy instead of water-heavy — situational, not a constant weakness.
- **Opening plan:** T0 upgrade, immediate water scouting via Fishing's unlocked movement, aggressive early expansion across lake shores and islands/peninsulas, Port economy once Trade is available.
- **Lakes 1v1 notes:** This is arguably the single best-suited regular tribe for the book's exact target map (Lakes). The format's map choice should make Kickoo a priority tribe to study in self-play — if the network doesn't already favor it or at least play it well, that's worth checking directly, since its play pattern (naval economy, Port/Market synergy) stresses exactly the mechanics in §2.5.
- **Countering Kickoo:** Deny water-adjacent village grabs early if possible; a land-mobility tribe (Oumaji) can sometimes out-race Kickoo to purely-inland villages while Kickoo is busy consolidating the shoreline.
- **Competitive standing:** Rated S tier for 1v1 Archipelago maps in one recent source, B tier for regular 1v1 land maps in the same source — i.e., explicitly water-map-dependent strength, which directly supports treating Kickoo as a strong Lakes-map pick even though "Lakes" isn't literally "Archipelago."

Sources: GameLogicData28.txt `tribeData.kickoo`, [Kickoo — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.6 Hoodrick

- **Start:** Archery tech, Archer, 7 stars (`startingStars: 7` in the data), 1.5× Forest / 0.5× Mountain.
- **Strengths:** Most forest-dense tribe, which is both a Lumber Hut/Sawmill economy base and (via Archery's own unlock) a forest defense bonus for units fighting in forest terrain — a genuinely synergistic bias (the terrain that's abundant is also the terrain that favors their starting unit defensively). Strong mid-to-late game power spike once Sawmill/Super Unit economy comes online.
- **Weaknesses:** The starting Archer (10 defense, ranged) is fragile in a direct early fight compared to a Warrior/Rider/Swordsman opener — guides explicitly warn to be careful with early Archer positioning, since losing it to an early skirmish also risks losing whatever village it was headed toward.
- **Opening plan:** Conservative early movement (don't blunder the Archer into an unscouted fight), lean into Forestry early for Lumber Huts, aim for the forest-economy mid-game spike rather than an early rush.
- **Lakes 1v1 notes:** No special water bias; a Lakes map's water tiles don't especially help or hurt Hoodrick beyond the generic effect of water splitting up the forest landmass.
- **Countering Hoodrick:** Punish the early fragility — an aggressive opener (Vengir, or any tribe with a cheap early rush) can end the game before Hoodrick's forest economy spikes; if that window is survived, expect Hoodrick to get stronger relatively as the game goes long.
- **Competitive standing:** C tier for regular 1v1 land maps in the most recent tier-list source — generally regarded as a slower, more passive pick, consistent with the "best played conservatively early" framing above.

Sources: GameLogicData28.txt `tribeData.hoodrick`, [Hoodrick — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.7 Luxidoor

- **Start:** No starting tech (`basic` only), Warrior, 2 stars, a large one-time bundle of pre-collected starting resources (2 game, 2 fruit, 2 fish, 1 crop, 1 metal), `bonus: 1` flag in the data (reflecting its unique capital mechanic).
- **Strengths:** Flexible — since Luxidoor has no tech bias, it can freely pick whichever Tier-1 tech best matches the terrain actually revealed, unlike tribes locked into a specific starting tech that may not match their land. The pre-harvested resource bundle front-loads population, letting the capital reach higher levels faster than its low starting-star count would suggest.
- **Weaknesses:** Most expensive tribe to unlock (a monetization fact, irrelevant to the AI but explains why it's less represented in casual data if you're drawing from broad human replay sets). Historically **overpowered and nerfed** — older sources describe a level-3-capital-with-walls start; patch 2.0.49 reduced this to a lower-level capital without the wall. **Version flag:** treat any claim about Luxidoor's exact starting capital level/stars with caution and prefer the JSON's `startingStars: 2` plus the resource bundle as ground truth over older prose descriptions. Entirely dependent on capital survival — lore and mechanics both lean on "safety of the mighty capital," implying a defensive, capital-centric rather than wide-expansion playstyle.
- **Opening plan:** Immediately check which Tier-1 tech the revealed terrain rewards most (no default bias to follow), harvest the pre-collected resources for a fast early level-up, otherwise play similarly to a generic economy-tribe opening.
- **Lakes 1v1 notes:** No terrain bias at all means no special Lakes interaction either way — purely map-seed dependent.
- **Countering Luxidoor:** Since the tribe is explicitly built around capital safety, a direct, early rush at the capital before Luxidoor's flexible tech choice pays off is a reasonable counter-plan; Luxidoor's low starting star count (2) is also a real early vulnerability if the opponent can apply any pressure before the pre-collected resources convert into a city-level lead.
- **Competitive standing:** B tier for regular 1v1 land maps in the most recent tier-list source — middling, consistent with "flexible but not exceptional" once the historical overpowered version was patched out.

Sources: GameLogicData28.txt `tribeData.luxidoor`, [Luxidoor — Polytopia Wiki] (via r.jina.ai, including the explicit 2.0.49 nerf note), tier-list search synthesis.

### 4.8 Vengir

- **Start:** Smithery tech, **Swordsman** (not Warrior), 5 stars, 2× Metal / 0.1× Fruit / 0.1× Fish / 0.1× Game.
- **Strengths:** By far the strongest *military* opener among the 12 — starting with a Swordsman instead of a Warrior is a large, immediate combat advantage in the first several turns, explicitly described as able to win fights "without unit losses" early and well-suited to small, crowded maps where early conquest is realistic — which is a reasonable description of a 14×14 1v1 map.
- **Weaknesses:** Deliberately crippled economy — 0.1× on three of the four basic population resources (fruit/fish/game) means Vengir's native land is close to the worst early-economy terrain of any tribe; the tribe is designed to need to take the opponent's cities/villages to have any real economy at all. This is a genuine, sharp, high-variance tribe: dominant if the early rush connects, weak if it doesn't.
- **Opening plan:** Immediate aggression — scout for the opponent or nearby villages and press the Swordsman's advantage before the economy gap becomes decisive; there is little reason to play a slow, economy-first game as Vengir, since the terrain won't reward it.
- **Lakes 1v1 notes:** If water separates the two capitals meaningfully on a given Lakes seed, Vengir's land-based early-aggression plan is weakened (needs a Port/boat tech detour it's not naturally biased toward), which is a real risk specific to this map type — worth flagging for self-play curriculum (Vengir games on highly-aquatic Lakes seeds may behave very differently from Vengir games on land-dominant Lakes seeds).
- **Countering Vengir:** Defensive tech (Strategy for Defender, City Walls) and simply surviving the early window are explicitly cited as the counter — once the early rush is blunted, Vengir's terrible economy becomes a decisive long-game liability, so a defender who trades time for safety should expect to win the late game.
- **Competitive standing:** Rated at or near the bottom in most sources (Z tier in an older list, D tier in the most recent one for regular 1v1 land maps) *despite* the strong early-aggression case above — the community consensus is that the economic weakness outweighs the opening-tempo strength over a full game, though this specific tension (strong 1v1-rush case vs. weak overall tier ranking) is worth testing directly in self-play rather than assuming the general tier list transfers cleanly to the specific 1v1-Might-Lakes-Small format, since rush tribes are generally undervalued by tier lists built around longer or multi-opponent games.

Sources: GameLogicData28.txt `tribeData.vengir`, [Vengir — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.9 Zebasi

- **Start:** Farming tech, Warrior, 5 stars, 0.5× Forest / 0.5× Mountain / 0.5× Fruit.
- **Strengths:** Farming is the tech that unlocks the Farm improvement directly (+2 population, denser than the +1 from Fishing/Hunting/Harvest-Fruit huts), so Zebasi can reach higher city levels faster than huts-based tribes wherever crop tiles exist — a real, quantifiable economic edge per the improvement table in §2.5.
- **Weaknesses:** Starting on a Tier-2 tech (`farming.cost == 2` in the JSON) rather than a cheap Tier-1 tech means Zebasi is relatively behind on *reaching other parts* of the tree — Farming only leads onward to Construction, a narrower follow-up than some Tier-1 starts offer. Reduced forest/mountain/fruit all somewhat narrow alternate economic paths if crop tiles are scarce on a given map seed.
- **Opening plan:** Explicitly terrain-dependent — move the first Warrior and scout before committing to a second unit; if forests are abundant, pivot toward Forestry/Hunting instead of leaning on the Farming bias; only add a second Warrior once tech and city-upgrade spending for the next few turns is already accounted for.
- **Lakes 1v1 notes:** No water bias; generic Lakes interaction.
- **Countering Zebasi:** The narrow tech follow-up (Farming→Construction only) is the exploitable weakness — a tribe that reaches broader tech faster can out-tech Zebasi in the mid-game even if Zebasi's early Farm economy looks strong on paper.
- **Competitive standing:** A tier for regular 1v1 land maps in the most recent tier-list source — solidly good, consistent with the real economic edge Farms provide.

Sources: GameLogicData28.txt `tribeData.zebasi`, [Zebasi — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.10 Ai-Mo

- **Start:** Philosophy tech, **Mind Bender** (not a combat starter), 5 stars, 1.5× Mountain / 0.1× Crop.
- **Strengths:** Philosophy's Literacy ability (−33% research cost) is a real, if slow-burning, compounding economic edge — valuable in a game that runs long enough to recoup it. Mountain bias pairs with defensive/vision value similar to Xin-xi.
- **Weaknesses:** This is a **reworked tribe** — it used to start with Meditation, explicitly criticized by the wiki itself as "neither a resource nor a military technology" and a cause of Ai-Mo's older "lower-mid-tier" reputation; the data file confirms the *current* pinned version already has the Philosophy rework (`startingTech: ["basic","philosophy"]`), so older sources describing Ai-Mo via Meditation are talking about a previous version and should be discounted. Even post-rework, a Mind Bender is not a combat-capable opener (0 attack in the unit data), so Ai-Mo has no early offensive pressure at all. The wiki's own strategy page is thin, and sources explicitly flag that "the real power of Ai-Mo is unclear due to lack of documented experimentation" — treat tier-list claims about current Ai-Mo with extra caution, since much of the available commentary predates the rework.
- **Opening plan:** Lean into the Literacy discount with a longer-horizon tech plan; the 0.1× crop penalty means avoiding over-reliance on Farms; use the Mind Bender defensively/for healing rather than expecting it to fight.
- **Lakes 1v1 notes:** No water bias; generic.
- **Countering Ai-Mo:** No early offensive pressure to worry about — an aggressive early rush from any tribe with a real starting combat unit (Vengir especially) should be a strong plan against Ai-Mo specifically, since it has nothing to punish the aggression with in the opening.
- **Competitive standing:** **Genuinely uncertain/contested** post-rework — this is one of the clearer "I couldn't confirm" cases in this document; worth treating Ai-Mo as a priority tribe to let self-play data speak for, rather than trusting any pre-existing tier list.

Sources: GameLogicData28.txt `tribeData.aimo` (confirms current rework), [Ai-Mo — Polytopia Wiki] (via r.jina.ai, including explicit note about the rework and documented uncertainty).

### 4.11 Quetzali

- **Start:** Strategy (internal key `shields`) tech, **Defender**, 7 stars, 2× Fruit / 0.1× Crop.
- **Strengths:** High starting star count (7, tied for highest); Strategy leads to Diplomacy, useful in longer/multi-opponent games (less so in strict 1v1 Might, per §3.2's framing of Diplomacy as situational). Fruit abundance is a real, if secondary, population source.
- **Weaknesses:** A Defender (10 attack, can't effectively initiate fights) is a poor opener for offense — explicitly called out as "not particularly useful early-game since they cannot effectively attack enemy units." The 0.1× crop penalty hurts specifically because Farms (+2 pop each) are the denser population improvement compared to Fruit's harvest-fruit (+1 pop) — so despite 2× fruit, Quetzali's *total* population-income ceiling is arguably lower than a Farm-rich tribe's, a subtlety the wiki itself draws out explicitly.
- **Opening plan:** Use the Defender for safe scouting-adjacent holding rather than offense; lean on the high starting star count to buy an early tempo play (second unit, or rush toward a cheap useful tech) that the terrain bias alone doesn't provide; expect to need more total tech investment than a Farm-biased tribe to hit the same city levels.
- **Lakes 1v1 notes:** No water bias; generic.
- **Countering Quetzali:** Similar to Ai-Mo — no early offensive pressure, so an aggressive opener can press an advantage; the high starting star count is Quetzali's main compensating factor, so denying them an efficient early purchase (by contesting villages/tiles fast) blunts that advantage too.
- **Competitive standing:** "Lower-mid-tier" per the wiki's own framing; B tier for regular 1v1 land maps in the most recent tier-list source — consistent, unlike Ai-Mo, since Quetzali's starting kit wasn't recently reworked in the same way.

Sources: GameLogicData28.txt `tribeData.quetzali`, [Quetzali — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.12 Yadakk

- **Start:** Roads tech, Warrior, 7 stars, 1.5× Fruit / 0.5× Forest / 0.5× Mountain.
- **Strengths:** Roads is a genuinely useful early infrastructure tech (movement + the path to Trade/Market), described as "upper-mid-tier" with "decent resources and a useful starting technology"; high starting star count (7) and solid fruit bias support an early tempo play.
- **Weaknesses:** Reduced forest and mountain both narrow alternate economic paths (less Lumber Hut/Sawmill potential, less Mining/Smithery potential) if the map doesn't hand Yadakk enough fruit/crop terrain to compensate.
- **Opening plan:** Use the high star count for an efficient early purchase; build roads toward any nearby villages/second-city sites to speed overland movement; aim toward Trade for Market once production buildings exist (per §2.5's adjacency caveat — don't buy Market early).
- **Lakes 1v1 notes:** No water bias; generic, though Roads' movement benefit is land-only, so a highly-aquatic Lakes seed blunts Yadakk's main starting-tech advantage more than it would a water-biased tribe.
- **Countering Yadakk:** No specific sharp weakness beyond the generic forest/mountain reduction; a fairly "balanced-strong" tribe to treat as a solid opponent baseline, similar in spirit to Bardur/Imperius though without the T0 capital-upgrade tempo those two get.
- **Competitive standing:** B tier for regular 1v1 land maps in the most recent tier-list source, consistent with "upper-mid-tier" framing from the wiki.

Sources: GameLogicData28.txt `tribeData.yadakk`, [Yădakk — Polytopia Wiki] (via r.jina.ai), tier-list search synthesis.

### 4.13 Special tribes (brief — outside the championship pool)

These four are not part of the 12-tribe championship pool but are worth a short entry since self-play or human-replay data may still include them, and because they illustrate tech/unit override mechanics worth knowing about generally.

- **Aquarion:** Starts with the unique `aquabase` tech (not a reskinned basic tech) and a Mermaid Warrior; its entire kit is unit/tech/improvement-*overridden* (per `tribeData.aquarion`'s `unitOverrides`/`techOverrides`/`improvementOverrides` fields — e.g., its "Chivalry" is internally `spearing`, its "Sailing" is `marinelife`) to support a fully amphibious playstyle: buildings can be placed on water, land units gain amphibious movement, and ocean ruins give free level-3 cities instead of normal rewards. Reduced forest bias (0.5×). Tier lists rate it very differently by map: D tier for regular 1v1 land, S tier for 1v1 Archipelago — a strong signal that its value is almost entirely conditional on water-heavy maps. On a Lakes map specifically, it likely sits somewhere between those extremes depending on how much water the seed has; worth testing empirically rather than assuming either extreme.
- **Elyrion:** Starts with Forest Magic (overriding Hunting) and a Warrior; its defining mechanic is turning wild animals into Polytaur units (strong early free-unit generator) instead of harvesting them for population, plus "Rainbow Vision" (sees ruins through fog) and access to Dragon-line units. Crop-biased, mountain-penalized. Rated "upper-mid-tier" (A tier in the most recent 1v1-land tier list). Cannot hunt for population in the normal way, a real structural economy difference from the 12 regular tribes.
- **Polaris:** Starts with Frostwork and a Mooni unit; its unique mechanic actively *terraforms* its surroundings to ice terrain (`tribeAbilities: alienclimate, freeze, spread` in the data) rather than having a fixed terrain bias — if every player in a game is Polaris, the map defaults to Imperius-like terrain instead. Strong ice/naval-adjacent tech overrides (Sailing→`icefishing`, etc.). Rated upper-mid-tier generally, S tier for 1v1 Archipelago in the most recent list. Its climate-overwriting mechanic makes it an unusual, high-variance tribe to include in a mixed-tribe training pool, since it changes the *map itself*.
- **Cymanti:** Starts with Farming (unlocking the unique `fungi` improvement rather than a normal Farm) and a Shaman unit; heavily overridden kit (15 unique units per the wiki) with a poison-based combat identity and a guaranteed-Spores-resource capital. Rated as one of the strongest tribes overall in some sources ("S tier" for regular 1v1 land in the most recent list) though a source also notes it was recently toned down outside of water maps specifically. Its distinct `spores` resource economy (via `fungi`/`clathrus` improvements, both overridden from the normal `farm`/`market`) means none of this document's §2.5 improvement-cost table applies to Cymanti directly — it needs its own improvement-cost table if it's ever included in training, which this document does not attempt to build (out of scope for the championship-pool-focused mandate).

Sources: GameLogicData28.txt `tribeData.{aquarion,elyrion,polaris,cymanti}`, [Aquarion — Polytopia Wiki], [∑∫ỹriȱŋ (Elyrion) — Polytopia Wiki], [Polaris — Polytopia Wiki], [Cymanti — Polytopia Wiki] (all via r.jina.ai wikitext fetches), tier-list search synthesis.

---

## 5. Long-term strategy: tempo, snowballing, war timing, converting a lead

### 5.1 Tempo and snowballing

Every mechanic covered above compounds: a city founded one turn earlier pays research-discount dividends for the rest of the game (§1.8), a tech exploited the same turn it's researched captures tempo a delayed one doesn't (§2.7), and a border claimed first is land the opponent can never claim at all (§1.7). In a 1v1 Might game with no allies and no team dynamics to dilute this, **small per-turn edges compound unusually cleanly into a decisive lead** compared to multi-player formats, because there's no third party to punish the leader or rescue the trailing side. This is the mechanical reason tempo-focused diagnostics (§2.6–§2.8) are worth weighting heavily in training signal design for this specific format.

**Signal:** a simple, aggregate "economic lead" curve — `(own_stars_per_turn - opponent_stars_per_turn)` integrated over time — tends to diverge monotonically once a real lead opens up in a well-played game; a network that lets this curve flatten or reverse after an early lead (without a corresponding military trade explaining it) is leaking tempo somewhere upstream. **Importance: critical** as a top-level health check, even though it's a lagging indicator of the more specific defects in §2.6–§2.8.

### 5.2 When to go to war

See §2.6 for the detailed trigger logic. At the strategic level: in 1v1 Might, "going to war" has an unusually clean win condition (capture the one enemy capital) and an unusually clean cost (every unit and turn spent on the war is a unit/turn not spent on economy, and economy compounds per §1.8/§5.1) — so the decision is close to a pure timing optimization: go to war once the expected value of continuing to out-economy the opponent (which they can also be doing) is lower than the expected value of converting current military strength into ending the game outright. Waiting too long risks the opponent reaching the same or a better economic/military state first (since both sides compound); going too early risks an underfunded assault that fails and hands the opponent a free initiative (lost units are pure negative score and, more importantly in Might, a failed assault often strips the attacker's remaining economy's defense).

### 5.3 Defending while expanding

A 1v1 map offers no ally to screen an expanding player, so defense cannot be fully deferred the way it sometimes can be in team formats. The practical resolution multiple sources converge on: keep a small, cheap defensive garrison (even a single Defender/Warrior with a City Wall) in border-adjacent cities once the opponent's direction is known, rather than leaving new cities at zero defense while expansion continues elsewhere — this is a direct, cheap insurance policy against the "new city founded with zero combat units nearby an enemy sighting" red flag in §2.8.

### 5.4 Converting an economic lead into a win

A lead in stars-per-turn or city count is not itself a win in Might — it has to be converted into an army that can reach and take the capital. The failure mode of *not* converting (described by the human owner as the network's actual observed weakness) looks like: a real economic lead exists on the scoreboard, but it never gets spent on the military buildup needed to finish the game, so the game drags on until a sharper opponent finds and punishes a soft point (§2.6's failure mode #2), or simply runs the clock/score in their own favor. The conversion step is mechanically simple — stars become units become an army become a siege of the capital — but it requires the policy to recognize *when* the lead is big enough to spend down safely without surrendering the lead itself, which is exactly the §2.6 switch-point question restated at the macro level. **This is likely the single highest-leverage thing to verify directly in self-play logs**: for games with a clear, sustained economic lead (say, SPT advantage sustained for 10+ turns), check whether army value as a fraction of total stars spent rises accordingly in the second half of the game, or stays flat (the latter being the "farming simulator" pattern a Steam guide explicitly warns against, see §1.1).

---

## Part I open questions

1. **Market's exact star-income coefficient and whether a Port adjacency still doubles it.** Sources disagree (1 star/level vs 2 stars/level; port-doubling described as both current and "former-only" depending on source); the ground-truth JSON confirms the adjacency *structure* but not the income *formula*. Recommend resolving empirically from your own replay logs rather than trusting any single web source — see §2.5.
2. **Luxidoor's exact original/current starting capital level and star production.** Sources describe a historical level-3-walled capital nerfed in patch 2.0.49; the pinned-version JSON gives `startingStars: 2` plus a large pre-collected resource bundle, which should be treated as authoritative over older prose, but I could not find a current source stating the exact resulting capital level in explicit, unambiguous terms for the present version.
3. **Ai-Mo's current competitive standing.** The tribe was reworked (Meditation → Philosophy start); most tier-list commentary available online either predates this rework or explicitly flags the post-rework power level as undocumented. Treat Ai-Mo as a tribe to let your own data judge rather than trusting any tier list.
4. **General tier-list applicability to this exact format.** Essentially all tier lists found are built for either Domination-vs-bots, Perfection score racing, or multi-player/team maps, or at best "regular 1v1 land maps" / "1v1 Archipelago" as the two reference buckets — none are specifically "1v1 Might, Lakes, Small." I used the regular-1v1-land list as the closer proxy throughout §4 and flagged water-sensitive tribes (Kickoo, Aquarion, Polaris) as cases where the Archipelago numbers might transfer better than the land numbers, but this is informed judgment, not a confirmed mapping.
5. **Border mechanics detail beyond the level-4 Border Growth reward.** I could not retrieve a dedicated wiki article with full border-growth tile-claim mechanics (the "Border" page appears not to exist as a standalone article; content is folded into the City page and scattered forum threads); §1.7's description is a reasonable synthesis but has lower confidence than the ground-truth-backed sections.
6. **Default `startingStars` for tribes with no explicit value in the JSON** (Imperius, Bardur, Kickoo, Vengir, Zebasi). Inferred as 5 by pattern-matching against tribes that do have an explicit value and consistent wiki figures, but not independently confirmed against an engine default constant.

---

## Part I sources

- GameLogicData28.txt — `/Users/ben/polytopia/game/dump/gamedata/GameLogicData28.txt` (ground truth for tribeData, techData, improvementData, terrainData, resourceData, unitData — ground truth throughout, no external citation needed per section but noted inline above)
- https://polytopia.fandom.com/wiki/Technology
- https://polytopia.fandom.com/wiki/City
- https://polytopia.fandom.com/wiki/Score
- https://polytopia.fandom.com/wiki/Village (redirects to City#Village)
- https://polytopia.fandom.com/wiki/Ruin
- https://polytopia.fandom.com/wiki/Game_Modes
- https://polytopia.fandom.com/wiki/Market
- https://polytopia.fandom.com/wiki/Port
- https://polytopia.fandom.com/wiki/Customs_House
- https://polytopia.fandom.com/wiki/Strategies
- https://polytopia.fandom.com/wiki/Xin-xi
- https://polytopia.fandom.com/wiki/Imperius
- https://polytopia.fandom.com/wiki/Bardur
- https://polytopia.fandom.com/wiki/Oumaji
- https://polytopia.fandom.com/wiki/Hoodrick
- https://polytopia.fandom.com/wiki/Kickoo
- https://polytopia.fandom.com/wiki/Luxidoor
- https://polytopia.fandom.com/wiki/Vengir
- https://polytopia.fandom.com/wiki/Zebasi
- https://polytopia.fandom.com/wiki/Ai-Mo
- https://polytopia.fandom.com/wiki/Quetzali
- https://polytopia.fandom.com/wiki/Y%C4%83dakk
- https://polytopia.fandom.com/wiki/Aquarion
- https://polytopia.fandom.com/wiki/%E2%88%91%E2%88%AB%E1%BB%B9ri%C8%B1%C5%8B (Elyrion)
- https://polytopia.fandom.com/wiki/Polaris
- https://polytopia.fandom.com/wiki/Cymanti
- https://steamcommunity.com/sharedfiles/filedetails/?id=2187949888 (General Strategy Roadmap — note: written pre-naval-rework, naval specifics outdated)
- https://steamcommunity.com/sharedfiles/filedetails/?id=2217846492 (A not too simple guide)
- https://theahura.substack.com/p/on-towards-the-stars-your-first-ten (general 4X framing, used cautiously — not Polytopia-exclusive content)
- https://en.wikipedia.org/wiki/The_Battle_of_Polytopia
- Community tier-list aggregation via web search (titles varied; exact source page for the most-cited recent 1v1-land/1v1-Archipelago split tier list was not independently re-verified beyond the search engine's synthesis — treat that specific tier list as indicative, not authoritative)

---

# Part II — Units, combat, tactics, naval play and the 1v1 meta


## 1. Combat mechanics

### 1.1 The damage formula (verified against the game)

*(Editor: this section replaces the researcher's version, which used the defender's attack stat for
retaliation and squared the attack force. The formula below matched the game's own
`BattleHelpers.GetBattleResults` on all 27 open-field attacks I sampled on 2026-10-24, including
wounded attackers and defenders. Stats and health are in game units: a Warrior has attack 2,
defence 2 and 10 HP; the data file stores them ×10.)*

```
attackForce  = attacker.attack  × attacker.health / attacker.maxHealth
defenceForce = defender.defence × defender.health / defender.maxHealth × defenceBonus
total        = attackForce + defenceForce
damage       = round(attackForce  / total × attacker.attack  × 4.5)      whole HP, halves up
retaliation  = round(defenceForce / total × defender.defence × 4.5)      same forces, before the hit
```

- **Retaliation uses the defender's defence stat** and its health *before* the hit.
- **There is no retaliation** if the defender dies, can't reach the attacker (e.g. an archer
  shooting from 2 tiles away at a range-1 unit), or the defender is `stiff` (or, per community
  sources, the attacker has `surprise`).
- **Examples:**
  - Warrior vs Warrior: 5 damage, 5 back.
  - Rider (4/10 HP) vs Warrior (8/10): 3 damage, 6 back. A wounded unit is much worse at both.
  - Rider vs Giant: 3 damage, 12 back.
- **The play app's yellow attack marks** come from the game's own function, so they are exact in
  every situation, including the bonus cases below.

Consequences:

- **Wounded units are worse on both sides of combat:** a smaller attack force deals less damage
  *and* draws more retaliation. Committing a damaged unit to another fight is usually bad.
- **High defence pays twice:** it cuts the damage taken and raises the retaliation dealt.
- **Ranged attacks from outside the defender's range take no retaliation.**

**Indicator:** none needed for the formula itself (the engine is exact); use it to read games.
**Importance: CRITICAL** as background: every tactical principle below rests on it.

### 1.2 Defence bonus multipliers

| Situation | Multiplier | Requirement |
|---|---|---|
| Open ground, no bonus | 1.0× | — |
| Forest | 1.5× | Archery tech researched |
| Mountain | 1.5× | Climbing tech researched |
| Shallow/coastal water tile (naval units) | 1.5× | relevant naval tech |
| Own city, no wall | 1.5× | defender must have the `fortify` skill |
| Own city **with** City Wall | 4.0× | defender must have the `fortify` skill |

Bonuses do not stack (city + forest doesn't multiply further) — the governing one is the terrain/structure the defending unit is standing on. This is converging community-sourced information (Fandom "Combat" page summary, corroborated by a second independent fan wiki/blog); I was not able to pull the literal game code (fandom.com blocked `WebFetch` with HTTP 402 in this session) so treat the exact "bonuses don't stack" claim as **high-confidence but not file-verified**.

The critical, non-obvious fact, confirmed directly from `GameLogicData28.txt`'s `unitAbilities` field: **the city/wall bonus only applies to units with the `fortify` skill**, and this is a *per-unit* flag, not a blanket rule. In the current data:

- **Has `fortify`** (gets the city/wall bonus): Warrior, Archer, Rider, Knight, **Defender**, Polytaur, Ice Archer, Dragon Egg, and the Aquarion amphibious equivalents Mermaid Warrior / Mermaid Archer / Mermaid Defender.
- **Does NOT have `fortify`** (gets zero bonus from sitting in a city or behind a wall): **Swordsman**, Catapult, Mind Bender, Giant, Amphibian, Tridention, Mermaid Swordsman, and all pure naval units (Raft/Scout/Rammer/Bomber).

This is a genuinely easy-to-miss rule and directly relevant to the front-line question raised about the network: **Defender (defence 30, has fortify) is strictly better than Swordsman (defence 30, no fortify) as a city garrison**, even though they show the same base defence stat — in a walled city, Defender's effective Defence Force is 4× its stat while Swordsman's stays at 1×. Swordsman is the better *field* front-line unit (mobile defence), Defender is the better *city* front-line unit. A policy that garrisons cities with Swordsmen instead of Defenders is leaving a large, measurable amount of defence on the table.

Also confirmed from the file: **Catapult's defence stat is 0**, so no bonus of any kind ever helps it — "protect the catapult" always has to mean "keep other units between it and the enemy," never "stand it on a mountain."

**Indicator**: for the agent's cities, fraction of turns where the garrison unit has the `fortify` skill (Warrior/Archer/Rider/Knight/Defender/etc.) vs. a non-fortify unit (Swordsman/Catapult/Mind Bender) sitting there instead. **Good range [heuristic]**: ≥90% of turns a contested city's primary garrison should be a fortify-capable unit, ideally Defender once available.
**Red flag**: a city under threat garrisoned by a Swordsman or Catapult while a fortify-capable unit (even a cheap Warrior) is idle elsewhere.
**Importance: IMPORTANT.**

### 1.3 Unit skills glossary (confirmed against `unitAbilities` in the data file)

| Skill | Effect |
|---|---|
| `dash` | Unit may move, then attack, in the same turn (normally attacking consumes all further action for most non-dash units only in the sense that they can't move first; dash removes that restriction). Present on almost every basic combat unit. |
| `escape` | Unit may attack, then still move afterward, in the same turn — the mirror of dash. Riders, Knights, Battlesleds, Hexapods, Amphibians, Tridentions, and dragons have this; it is what lets a hit-and-run unit retreat to safety after striking. |
| `persist` | After an attack that **kills** the target, the unit may attack again (no cap on chain length) as long as it still has an eligible target and, if it needs to move to reach it, has remaining movement (via dash). Knight and Tridention have this. This is the mechanical basis of "knight chaining" (see §4.1). |
| `fortify` | Grants the city/open-field "fortify" defence bonus; see §1.2. Also usable as a manual stand-still action in some contexts. |
| `splash` | The attack also damages all enemy units adjacent to the primary target, at the same damage roll. Confirmed on Bomber, Fire Dragon, and the Cymanti Exida — **not** on the regular-tribe Catapult in this data version (older guides sometimes imply catapults have splash; the current file does not support that — catapult's only skills are `stiff` and `land`). |
| `stiff` | Unit never deals retaliation damage when it is the one being attacked. Present on Catapult, Mind Bender, all Rafts/Scout/Rammer/Bomber hulls, Ice Archer, Cloak, and others — generally the "glass" ranged/support/naval units. |
| `surprise` | When this unit attacks, the target cannot retaliate (the attacker's point of view, opposite of `stiff`). Present on Dagger, Cloak-line units, Pirate, Shark — ambush-style units. |
| `convert` | On a successful attack, instead of dealing damage, the unit instantly flips the target to the attacker's side, regardless of target HP. Mind Bender and the Cymanti Shaman have this; it is deterministic, not a percentage roll, per converging sources (no in-file percentage field exists either). |
| `heal` | Grants a "Heal Others" action: once per turn, heal all adjacent friendly units by up to 4 HP (separate from the unit's own passive regen). Mind Bender only among regular-tribe units. |
| `poison` | Inflicts a damage-over-time status on the target in addition to the attack; poisoned units lose HP each turn until cured (healing in a city, or a cure tech/structure). Present on Cymanti units (Kiton, Exida, Phychi) — not on any of the 12 regular tribes' base units. |
| `creep` | Unit ignores both Zone of Control restrictions and terrain movement penalties. |
| `sneak` | Unit ignores the Zone-of-Control movement *penalty* (doesn't lose all remaining movement when passing near an enemy) but can still be physically blocked by units occupying tiles. |
| `hide` | Unit becomes invisible to the enemy once it moves, and while hidden it also ignores Zone of Control, effectively moving through enemy lines. |
| `infiltrate` | Used with Cloak-type units; lets the unit "appear" already positioned past the front line, functionally an ambush insertion. |
| `carry` | Unit (a naval hull) can hold one embarked land unit inside it. |
| `amphibious` | Unit can move on both land and water, usually at a movement penalty on land. |
| `static` | Appears on most summoned/rewarded "super" units, all naval hulls, and a scattering of promotable units (e.g. Ice Fortress). Its exact mechanical effect was **not confirmed** from available sources in this session (candidate theories: non-purchasable origin, or a flag unrelated to promotion, since it does not correlate cleanly with `promotionLimit = 0` in the data). Flag as **unconfirmed** — do not hard-code behavior around it without further verification.
| `grow` | Dragon Egg → Baby Dragon → Fire Dragon evolution chain triggers based on this skill. |
| `stomp` | Juggernaut (Giant-on-raft) damages all adjacent tiles whenever it moves, including on disembarking. |
| `swarm`, `doubleattack`, `eat`, `explode`, `tentacles`, `freeze`/`freezearea`/`autofreeze`, `skate` | Cymanti/Polaris/Elyrion/Aquarion special mechanics; out of scope for the 12 regular tribes' core kit but noted briefly in §2.5 since they shape what a regular-tribe army might face from certain tribes if the format ever widens past the 12 regular tribes (it currently will not, per the championship format, so treat these as background only). |

**Sources**: [Unit Skills — Polytopia Wiki](https://polytopia.fandom.com/wiki/Unit_Skills) (content reached via search snippets; direct `WebFetch` returned HTTP 402 in this session), [Combat — Polytopia Wiki](https://polytopia.fandom.com/wiki/Combat), ground truth `unitAbilities` field per unit in `GameLogicData28.txt`.

### 1.4 Retaliation — exact exclusion rules

No retaliation occurs if **any** of the following hold:
1. The attack kills the defender.
2. The defender cannot reach the attacker (ranged attack from outside the defender's own range, e.g. Catapult/Archer striking from 2–3 tiles away against a range-1 melee unit).
3. The defender's computed retaliation damage rounds to 0.
4. The defender has the `stiff` skill.
5. The attacker has the `surprise` skill.

*(Editor: verified exclusions: kill, out of reach, `stiff`. The others are community-sourced.)* **Practical rule for the agent**: attacking a melee unit (range 1) from adjacent is always a two-way trade unless one of the exclusions applies; attacking from range 2+ with Archer/Catapult/Bomber/Ice Archer is "free" damage. A policy that routinely walks Archers adjacent to attack when it could have struck from 2 tiles away is giving up free damage avoidance for no reason — this is a cheap, high-signal check.

**Indicator**: fraction of Archer/Catapult/Bomber attacks thrown from a tile where the defender could retaliate (i.e., attacker ended the move within the defender's range) vs. from outside it. **Good range [heuristic]**: ≥80% of ranged-unit attacks should be thrown from outside the defender's range when terrain/position allows it (it isn't always possible — defender may have moved first, or the attacker needed the tile for the model's pathing). **Importance: IMPORTANT.**
**Red flag**: ranged units ending their attack adjacent to a live melee defender when an unoccupied tile 2 tiles away was reachable instead.

### 1.5 Veterans and healing

- A unit is promoted to **Veteran** after `promotionLimit` kills (3, for every basic regular-tribe unit that can promote at all — Warrior, Archer, Rider, Knight, Defender, Swordsman, Catapult, Mind Bender). Super/summoned units and naval hulls have `promotionLimit = 0` and never promote.
- Promotion grants **+5 max HP** and fully heals the unit on the spot.
- Passive per-turn healing (for a unit that takes no action that turn): **+4 HP in friendly territory, +2 HP elsewhere**, if sources are correctly converged (Fandom wiki; corroborated by independent strategy write-ups). This was not independently verifiable in the decompiled dump available in this session.
- Mind Bender's active `heal` action heals adjacent friendly units by up to 4 HP and does not require the Mind Bender itself to stand still.

**Indicator**: average HP% of the agent's units at the start of an attack (are units being thrown into fights while wounded, when retreating one turn to heal in friendly territory was available and tactically free?). **Importance: MINOR–IMPORTANT** depending on how often the agent has the tempo to retreat; mostly relevant in grinding mid-game fights rather than early rushes.

**Sources for §1**: [Combat — Polytopia Wiki](https://polytopia.fandom.com/wiki/Combat), [Unit Skills — Polytopia Wiki](https://polytopia.fandom.com/wiki/Unit_Skills), [Polytopia Damage Calculator (poly.bylab.app)](https://poly.bylab.app/), [Polytopia Battle Calculator (mikaelmayer/frothfrenzy fork)](https://mikaelmayer.github.io/polytopiacalculator/), ground truth `GameLogicData28.txt`.

---

## 2. One-hit-kill tables

*(Editor: recomputed with the verified formula in §1.1; full-health attacker against full-health
defender; numbers are damage in HP, **K** = kills in one hit. The ×1.5 and ×4 bonus values are
community-sourced; the open-field table is formula-verified.)*

#### Open field (no bonus)

| Attacker ↓ / defender → | Warrior (10 HP) | Archer (10 HP) | Rider (10 HP) | Knight (10 HP) | Defender (15 HP) | Swordsman (15 HP) | Catapult (10 HP) | Mind Bender (10 HP) | Giant (40 HP) |
|---|---|---|---|---|---|---|---|---|---|
| Warrior | 5 | 6 | 6 | 6 | 4 | 4 | 9 | 6 | 3 |
| Archer | 5 | 6 | 6 | 6 | 4 | 4 | 9 | 6 | 3 |
| Rider | 5 | 6 | 6 | 6 | 4 | 4 | 9 | 6 | 3 |
| Knight | **10 K** | **12 K** | **12 K** | **12 K** | 8 | 8 | **16 K** | **12 K** | 7 |
| Defender | 2 | 2 | 2 | 2 | 1 | 1 | 5 | 2 | 1 |
| Swordsman | 8 | **10 K** | **10 K** | **10 K** | 7 | 7 | **14 K** | **10 K** | 6 |
| Catapult | **12 K** | **14 K** | **14 K** | **14 K** | 10 | 10 | **18 K** | **14 K** | 9 |
| Giant | **16 K** | **19 K** | **19 K** | **19 K** | 14 | 14 | **23 K** | **19 K** | 13 |

#### Defence bonus ×1.5 (forest with Archery, mountain with Climbing, or a fortify unit in an unwalled city)

| Attacker ↓ / defender → | Warrior (10 HP) | Archer (10 HP) | Rider (10 HP) | Knight (10 HP) | Defender (15 HP) | Swordsman (15 HP) | Catapult (10 HP) | Mind Bender (10 HP) | Giant (40 HP) |
|---|---|---|---|---|---|---|---|---|---|
| Warrior | 4 | 5 | 5 | 5 | 3 | 3 | 9 | 5 | 2 |
| Archer | 4 | 5 | 5 | 5 | 3 | 3 | 9 | 5 | 2 |
| Rider | 4 | 5 | 5 | 5 | 3 | 3 | 9 | 5 | 2 |
| Knight | 8 | **11 K** | **11 K** | **11 K** | 7 | 7 | **16 K** | **11 K** | 6 |
| Defender | 1 | 2 | 2 | 2 | 1 | 1 | 5 | 2 | 1 |
| Swordsman | 7 | 9 | 9 | 9 | 5 | 5 | **14 K** | 9 | 5 |
| Catapult | **10 K** | **13 K** | **13 K** | **13 K** | 8 | 8 | **18 K** | **13 K** | 7 |
| Giant | **14 K** | **17 K** | **17 K** | **17 K** | 12 | 12 | **23 K** | **17 K** | 10 |

#### Walled city ×4 (fortify units only)

| Attacker ↓ / defender → | Warrior (10 HP) | Archer (10 HP) | Rider (10 HP) | Knight (10 HP) | Defender (15 HP) |
|---|---|---|---|---|---|
| Warrior | 2 | 3 | 3 | 3 | 1 |
| Archer | 2 | 3 | 3 | 3 | 1 |
| Rider | 2 | 3 | 3 | 3 | 1 |
| Knight | 5 | 7 | 7 | 7 | 4 |
| Defender | 1 | 1 | 1 | 1 | 0 |
| Swordsman | 4 | 6 | 6 | 6 | 3 |
| Catapult | 6 | 9 | 9 | 9 | 5 |
| Giant | 9 | **13 K** | **13 K** | **13 K** | 7 |

- **No regular unit one-shots a Defender**, even in the open: a Catapult does 10 of 15 HP and a
  Knight 8.
- **Behind a wall** (×4), only a Giant hurts anything much.
- **Swordsmen survive a Knight's first hit** (8 of 15 HP) in the open. Warriors, Archers, Riders,
  Catapults and Mind Benders don't. That is why a front line of Warriors in the open feeds knight
  chains.

**Indicator:** when a garrison dies in one hit, compare with these tables (a mismatch points to a
missed bonus or a rules question). **Importance: IMPORTANT.**

---

## 3. Units — stats, roles, counters, mistakes

All stats below are from `GameLogicData28.txt`. *(Editor: the tables in this section use the data file's ×10 units: a Warrior's 100 HP and attack 20 are 10 HP and attack 2 in the game and in §1–2.)* Cost is in stars at a city (Raft/Juggernaut/Pirate/etc. are obtained by conversion/event, not bought, hence cost 0).

### 3.1 Land units (regular-tribe core kit)

| Unit | HP | Atk | Def | Mov | Rng | Cost | Key skills | Role |
|---|---|---|---|---|---|---|---|---|
| Warrior | 100 | 20 | 20 | 1 | 1 | 2 | dash, fortify | Cheapest front line; early rush and garrison filler |
| Archer | 100 | 20 | 10 | 1 | 2 | 3 | dash, fortify | Ranged softening; weak if caught in melee (def 10) |
| Rider | 100 | 20 | 10 | 2 | 1 | 3 | dash, escape, fortify | Fast raider/scout-fighter; hit-and-run, village grabbing |
| Knight | 100 | 35 | 10 | 3 | 1 | 8 | dash, persist, fortify | Breakthrough/chain-killer; very low defence, dies fast if it fails to kill |
| Defender | 150 | 10 | 30 | 1 | 1 | 3 | fortify | The core front-line/garrison unit; low offence, best defence-to-cost of any fortify unit |
| Swordsman | 150 | 30 | 30 | 1 | 1 | 5 | dash | Mobile front line with real offence; no fortify (see §1.2) |
| Catapult | 100 | 40 | 0 | 1 | 3 | 8 | stiff | Artillery; highest base attack of the basic kit, zero defence, must never be left exposed |
| Mind Bender | 100 | 0 | 10 | 1 | 1 | 5 | heal, convert | Support; converts enemies, heals allies, cannot deal damage itself |

**Common mistakes (per unit):**
- **Warrior**: fine early, but keeping it as the main front line past the midgame wastes the star economy — it has the same defence as Archer/Rider but no ranged or mobility edge.
- **Archer**: attacking from range 1 when range 2 was reachable (gives up free damage avoidance, §1.4); leaving it unescorted since its defence (10) is the lowest of any fortify-capable melee-adjacent unit.
- **Rider**: using it as a stand-and-fight front-line piece — its value is escape+mobility (hit something, retreat before retaliation window closes next enemy turn), not trading blows.
- **Knight**: sending it in alone against a line that has even one unit it can't one-shot (see §4.1) — the chain stops and the Knight (defence 10, HP100) is then exposed mid-field to a counter-attack.
- **Defender**: using it as an attacker — attack 10 means it almost never wins an exchange on offence; its entire value is defence stat + fortify.
- **Swordsman**: the best "generalist" front-line unit at 5 stars, but it is not a good city garrison per §1.2 (no fortify) — pair with Defender for static defence and use Swordsman for the mobile line.
- **Catapult**: the single most commonly misused unit — zero defence means any contact at all (melee or even a weak sniping unit) can kill it outright if unguarded. The flagged weakness "catapult-only armies with no front line" is a direct instance of this.
- **Mind Bender**: forgetting it can't fight — moving it into a position where it is the frontmost unit converts "support piece" into "free kill/free conversion target" for the opponent.

### 3.2 Naval units (current "Path of the Ocean" system)

The 2023 naval rework replaced the old Boat → Ship → Battleship line with a branching upgrade tree from a single base hull. Ground truth internal names: `transportship` = Raft, `scoutship` = Scout(ship), `rammership` = Rammer, `bombership` = Bomber.

| Unit | HP | Atk | Def | Mov | Rng | Cost to upgrade | Key skills | Role |
|---|---|---|---|---|---|---|---|---|
| Raft (`transportship`) | 100 | 0 | 10 | 2 | 2 | — (built at a Port) | carry, stiff | Pure transport; cannot attack |
| Scout (`scoutship`) | 100 | 20 | 10 | 3 | 2 | 5★ (needs Sailing) | dash, carry, scout | Fast, long-vision ranged skirmisher; vision role replaces the old Battleship's sight |
| Rammer (`rammership`) | 100 | 30 | 30 | 3 | 1 | 5★ (needs Ramming) | dash, carry | Naval melee bruiser; best stat-for-cost naval unit, good for killing weak land units from the shore and capturing coastal cities |
| Bomber (`bombership`) | 100 | 30 | 20 | 2 | 3 | 15★ (needs Navigation) | carry, splash, stiff | Slow, long-range, splash artillery; the unit that "snipes catapults/archers from the water" |
| Juggernaut (`juggernaut`) | 100 | 40 | 40 | 2 | 1 | — (Giant boarding a Port) | stiff, stomp, carry | Rare, giant-tier naval brawler; damages everything adjacent whenever it moves |

A Raft can be freely re-upgraded between Scout/Rammer/Bomber (paying the difference) as long as it's in friendly territory, so a single hull isn't permanently locked into one role — useful for an agent to know it can retool its navy, not just build one type and commit forever.

**Common naval mistakes**: building Rafts and never upgrading them (a pure transport contributes zero combat value and dies for free to anything); upgrading everything to Bomber (slow, mov 2, needs escort — a navy of only Bombers is as exposed as a land army of only Catapults, same underlying bug pattern as §3.1's Catapult mistake); never building a Port at all on a map that is ≥25% water (see §5).

### 3.3 Special-tribe and super units (brief — regular tribes only compete with the 12, but the engine / opponent pool may include others, and some of these appear as city-growth rewards for regular tribes too)

- **Giant** (HP 400, Atk 50, Def 40, cost 10, no fortify, never promotes): a slow, extremely tanky "super unit" awarded as a population/city-growth reward on some tribes (not purchasable in the regular build queue). Its lack of `fortify` means it gets **no** bonus from sitting in your own city — it is a field brawler, not a defensive trump card, contrary to what its raw stats might suggest.
- **Polytaur** (HP 150, Atk 30, Def 10, Mov 1, cost 3, has fortify, independent): Ai-Mo's distinctive unit; fortify-capable unlike most "independent" wandering units.
- **Cymanti line** (Hexapod, Doomux, Phychi, Kiton, Exida, Centipede, Segment) and **Polaris line** (Mooni, Battle Sled, Ice Fortress, Ice Archer, Gaami) and **Aquarion line** (Mermaid Warrior/Archer/Swordsman/Defender/Cloak/Dagger, Crab, Tridention, Amphibian) belong to special (non-regular) tribes and are out of this book's main scope (the championship format uses the 12 regular tribes only), but worth knowing if the opponent pool ever includes them: Doomux (explode+poison, very cheap for its stats per a recurring Reddit/Steam-community balance complaint) and Centipede/Segment (near-unkillable spam) are the units most often flagged by the community as overtuned relative to the regular-tribe kit. Treat any claim about these as **contested/community-opinion**, not an official balance statement.
- **Dragon line** (Dragon Egg → Baby Dragon → Fire Dragon): Elyrion's growth-based flyers; Fire Dragon (HP200, Atk40, Def30, Mov3, Rng2, splash+fly) is a strong late-game harasser immune to Zone-of-Control-by-terrain concerns because it flies.

**Sources for §3**: ground truth `unitData` in `GameLogicData28.txt`; [Raft — Polytopia Wiki](https://polytopia.fandom.com/wiki/Raft); [Path of the Ocean — Polytopia Wiki](https://polytopia.fandom.com/wiki/Path_of_the_Ocean); [Steam Community Guide — The Ultimate Sequel to the Tribe Guide (updated for the Ocean)](https://steamcommunity.com/sharedfiles/filedetails/?id=3005850953); [Steam announcement — Experience the Path of the Ocean](https://steamcommunity.com/games/874390/announcements/detail/3735230544088443158).

---

## 4. Army composition — the front-line question

This section directly addresses the owner's note: *the network built catapult-only armies with no front-line troops, and needs warriors "in a pinch," swordsmen in some situations, and knights only in narrow, carefully played situations.*

### 4.1 Why a front line is non-negotiable [CRITICAL]

Catapult has 0 defence and range 3; Mind Bender has 0 attack. Neither can survive contact and neither should ever be the unit an enemy can reach first. Every regular-tribe army composition that wins fights at the format's power level has at least one melee unit with real defence (Warrior ≥ 20, Swordsman/Defender = 30) standing between the enemy's melee units and any Catapult/Archer/Mind Bender. This is not a style choice — it falls directly out of §2's one-hit-kill tables: almost everything one-shots a Catapult (defence 0 means the `DefenceForce` term in the formula is always driven only by the HP ratio, never boosted by terrain or city bonus), so an unguarded Catapult that any enemy unit can reach is just a free kill or, worse, a free capture of your highest-value piece.

**Indicator**: for every Catapult/Archer/Mind Bender the agent owns, on every turn, is there a friendly unit with defence ≥ 20 occupying a tile between it and the nearest visible enemy unit (or simply: is the ranged/support unit NOT the closest friendly unit to any enemy)? Define `exposed_ranged_unit_turns` = count of turns where a Catapult/Archer/Mind Bender is the closest (or only reachable) friendly unit to an enemy combat unit.
**Good range [heuristic]**: `exposed_ranged_unit_turns` should be a small fraction of total unit-turns for those unit types — target under 10%, trending to near 0% once the opponent has any melee units in the area. A single-digit percentage still happens because of fog-of-war surprises; a number in the 30%+ range (which is roughly consistent with "catapult-only army, no front line") is the signature of this exact bug.
**Red flag**: a game where the agent's army, at any point past turn ~15, contains Catapults/Archers but zero Warrior/Swordsman/Defender/Rider/Knight in the same region of the map.
**Importance: CRITICAL.** This was explicitly the mechanism by which the network lost to the human despite a 96% win rate vs. Crazy — a single opponent willing to punish this specific, checkable pattern beat it easily.

### 4.2 Which front-line unit, when

- **Warrior** "in a pinch": cheapest fortify-capable melee body, 2 stars. Correct early (turns 1–10ish) when stars are scarce and you need bodies to screen Catapults/Archers or hold a just-captured village. Becomes a poor long-term choice once 5★ Swordsman is available, since Swordsman has more defence (3 vs 2), more attack (3 vs 2) and 15 HP instead of 10 for not much more cost relative to the army's overall star budget.
- **Swordsman** "in some situations": the best mobile front-line unit — defence 30, attack 30, dash. The situation where it is *not* the right call: static city defence, because it lacks `fortify` (§1.2) — a Defender holds a walled city far better per star spent. Use Swordsman to escort Catapults/Archers in the field and to push the line forward; use Defender to hold ground.
- **Defender**: the default garrison and "anchor" unit. Low offence (10) means it should not be your primary damage dealer, but it is the unit that should be standing in your cities and at the front of any defensive position, because of the fortify multiplier (§1.2, §2.3).
- **Knight** "only in very specific, carefully played situations": high attack (35) and movement (3) with persist make it the best breakthrough tool in the kit, but defence 10 and HP 100 mean it dies to almost any retaliation or counter-attack if a chain fails (§4.3/§4.4). It is a *finishing* unit, not a front-line unit — it should enter combat only when the opponent's line has already been softened (by Catapult/Archer fire) enough that the Knight is very likely to one-shot its first few targets, and it should have an exit path (its own mobility, or allies to retreat behind) if the chain stops early. Using Knights as the main body of an army, un-softened, against an intact enemy line is the single most common way to lose several Knights (40–64 stars) in one bad turn.

**Indicator**: ratio of melee front-line units (Warrior+Swordsman+Defender+Rider, excluding Knight) to ranged/support units (Archer+Catapult+Mind Bender) in the agent's standing army over time.
**Good range [heuristic]**: roughly 1:1 to 2:1 front-line:ranged by midgame (turn ~15–30) in a 1v1 Small-map game, trending toward more front-line as the map's combat intensifies; a ratio below roughly 0.5:1 (half as many melee bodies as ranged/support units) is the catapult-heavy-army red flag region.
**Importance: CRITICAL** — this ratio is probably the single cheapest, highest-value number to monitor every training run against, given the explicit failure mode reported.

### 4.3 Knight chaining — mechanism

Knight has `dash` (move then attack), `persist` (attack again after a kill), movement 3, and attack 35. A Knight that can reach and one-shot a target, then still has movement left to reach the *next* target and one-shot that too, keeps going — "no limit on the number of kills in a single turn" per converging community sources. Against an unprotected line of 100-HP, low-defence units (Warriors in the open: §2.1 shows Knight does 100 damage to a full-HP Warrior, a kill), a single Knight with enough open movement can erase 2–3 units in one turn for 8 stars, a catastrophic exchange rate.

The chain **stops** the moment an attack fails to kill its target. That failure can come from:
- The target having enough effective HP/defence that 1 hit isn't lethal (Defender behind a wall never dies to a Knight in one hit per §2.3; even an open-field Defender takes only 85, surviving its 150 HP).
- The Knight running out of movement before reaching the next target (dash lets it move after attacking, but only with movement it has left — 3 total per turn).
- Zone of Control from an adjacent enemy unit cutting off the path to the next target (any enemy unit, not just the next victim, exerts ZoC that halts further movement through that tile).

### 4.4 Denying/countering knight chains [CRITICAL]

1. **Don't leave a line of one-shot-able units adjacent to each other in the open.** Spacing (gaps too wide for a Knight's remaining movement to cross after each kill) breaks the chain even without a dedicated counter-unit.
2. **Put a Defender (or any unit the Knight can't one-shot) at the front.** Once the first attack fails to kill, the chain is over for that turn regardless of how many more units are behind it.
3. **Use terrain.** Forest/mountain (1.5×) is often enough to push a would-be one-shot into "survives" territory, especially for 150-HP bodies.
4. **Zone of Control screening.** A unit merely standing adjacent to the path the Knight would need to dash through can stop the chain even if that screening unit is never attacked itself, because ZoC halts movement.
5. **Kill or threaten the Knight on the preceding or following turn.** Its own defence is only 10 — any decent attacker kills it easily once it's used its movement and is stranded mid-field without escape (Knight has `dash`, not `escape`, so it cannot move again after whatever its last action was that turn).

**Indicator**: `multi_kill_turns` = count of opponent turns in which 2+ of the agent's units died to the same enemy unit. This is the most direct measurable trace of a knight-chain (or any persist-chain) actually happening to the agent.
**Good range [heuristic]**: should be rare — a handful of occurrences across many games is expected (sometimes a chain is genuinely unavoidable or correct for the opponent to take), but a `multi_kill_turns` rate that's elevated specifically against Knight-owning opponents, and specifically early in a loss, is a strong, specific red flag matching the owner's exact report.
**Red flag**: 2+ agent units lost in a single enemy turn to a single Knight, especially when (a) the lost units were in the open with no terrain/fortify bonus and (b) no friendly unit's defence stat was high enough to have survived the first hit (compare the actual HP lost to the §2 tables to confirm it was a "my units were simply one-shot-able" failure rather than bad luck).
**Importance: CRITICAL** — this is one of the two explicitly reported failure modes.

**Sources for §4**: [Knight — Polytopia Wiki](https://polytopia.fandom.com/wiki/Knight); [Strategies/Units — Polytopia Wiki](https://polytopia.fandom.com/wiki/Strategies/Units); Steam Community discussion threads on Knight persist counters; ground truth `unitAbilities`/stats from `GameLogicData28.txt`.

---

## 5. Naval play, water control, and why Lakes demands a navy

This section directly addresses the second reported weakness: *no navy and no ports were built on Lakes, and water control is what lets a player snipe the enemy's catapults.*

### 5.1 Why water control matters mechanically

A Catapult (range 3, defence 0) or Archer (range 2, defence 10) sitting on the shore to bombard your front line is a prime target for anything that can reach the water next to them *without* a land path, i.e., a navy. A Scout (range 2, movement 3) or Rammer (range 1, movement 3) that controls the water adjacent to the enemy's firing line can kill an undefended Catapult/Archer the enemy correctly assumed was safe behind their own front line — because naval units approach from the water side, where the enemy's land front line provides zero protection. A Bomber (range 3, splash) can do this from further out and hit multiple units if they're clustered on the shore. This is exactly the "sniping catapults and archers from the water" mechanism flagged by the owner, and it is the main reason naval presence is not optional on a map type where water reaches into the middle of the fighting, which is precisely what Lakes maps do (§6).

### 5.2 Ports and getting on the water

- A **Port** costs 7★ (ground truth: `improvementData.port.cost = 7`), must be built on a water/wetland-adjacent tile, and grants the `embark` ability — a land unit moving into/through a Port tile becomes a Raft.
- Without at least one Port, the agent has **zero** ability to project force onto water, contest enemy naval sniping, move units across a lake faster than walking around it, or threaten the enemy's coastal cities from the water. On a Lakes map this is close to a self-imposed handicap.
- Rafts upgrade in friendly territory to Scout (5★, needs Sailing), Rammer (5★, needs Ramming), or Bomber (15★, needs Navigation) — see §3.2.

**Indicator**: `turn_of_first_port` and `ports_built` by end of game, and `naval_units_built` (Scout/Rammer/Bomber count, not counting un-upgraded Rafts) on any map tagged Lakes (or with water fraction above some threshold, e.g. 20%).
**Good range [heuristic]**: on a Small (14×14) Lakes 1v1 map, with land generating on the map edges and every tribe connected to at least two villages across water per the map-generation rules (§6), a competent plan should have a Port up by roughly turn 10–15 once the relevant economy allows it, and at least 2–3 upgraded naval units by the point ranged-unit exchanges start happening near the shore. Treat `ports_built = 0` for an entire Lakes game as an unambiguous red flag rather than a borderline case — there is no legitimate "I chose to skip the navy entirely" line of play once contact with the enemy's water-adjacent territory is established.
**Red flag**: `ports_built = 0` on a Lakes map; or a Port built but never upgraded past Raft (0 combat-capable naval units); or the agent's Catapults/Archers dying to attacks that arrived from a water tile with no friendly naval unit anywhere on the map to contest it.
**Importance: CRITICAL** on Lakes specifically (this is the target map type); importance drops to MINOR/IRRELEVANT on a map type with no water contact at all, so this indicator should be conditioned on map type, not applied uniformly.

### 5.3 Naval unit roles recap (see §3.2 for stats)

- **Scout**: vision + light ranged harassment; good for spotting and picking off isolated weak units, and for the vision radius that replaces what the old Battleship provided.
- **Rammer**: best stat-for-cost naval combat unit; use for killing exposed units near shore and for threatening/softening coastal cities.
- **Bomber**: slow, expensive, high-value splash damage from range 3 — the dedicated "snipe the artillery/ranged cluster" tool, but needs escort (it has `stiff`, no retaliation ability, and movement 2) just like a land Catapult needs a front line. The same underlying principle from §4 (never leave a zero/low-retaliation high-value ranged unit unescorted) applies at sea.
- **Juggernaut**: rare, situational; treat as a bonus unit if a Giant happens to be available, not a planned part of the standard naval composition.

### 5.4 Defending against enemy naval harassment

- Keep land Archers/Catapults that must operate near a shoreline at least one tile back from the water's edge when possible, or escorted by a unit that can reach the water tile the enemy naval unit would need to attack from.
- Maintain at least one naval unit (even just an upgraded Scout) patrolling your own coastline once the enemy has any navy, purely to deny free water-side attacks — the cost of losing a Catapult/Archer to an unanswered naval snipe vastly exceeds the cost of a Scout.
- A Port in your own territory is also a soft target — losing it removes your embark capability at a bad moment; don't leave it totally undefended if the enemy navy is active nearby.

**Indicator**: `ranged_units_lost_to_naval_attack` — count of Catapult/Archer/Mind Bender deaths where the killing unit's last position before attacking was a water tile. A non-trivial count of these, especially repeated across games on Lakes, directly confirms the reported failure mode.
**Importance: CRITICAL** on Lakes.

**Sources for §5**: ground truth `improvementData.port` and `unitData` (`transportship`/`scoutship`/`rammership`/`bombership`) in `GameLogicData28.txt`; [Path of the Ocean — Polytopia Wiki](https://polytopia.fandom.com/wiki/Path_of_the_Ocean); [Raft — Polytopia Wiki](https://polytopia.fandom.com/wiki/Raft); [Steam announcement — Experience the Path of the Ocean](https://steamcommunity.com/games/874390/announcements/detail/3735230544088443158); GitHub issue discussion on naval play/embarkation mechanics (community, not official, used only to corroborate general embark/Port behavior).

---

## 6. Move-level tactics (beyond knight chains and army composition, covered above)

### 6.1 Zone of Control and blocking [IMPORTANT]

Every unit projects a passive Zone of Control (ZoC) onto all tiles adjacent to it. An enemy unit that enters a tile adjacent to your unit immediately loses all further movement that turn, even with movement points left, *unless* it has `creep` (full ignore), `hide` (ignore + can pass through enemy units while hidden), or `sneak` (ignores the movement-stopping penalty but can still be physically blocked by occupied tiles). This is the mechanical basis for using cheap units as "tripwires" to stop an enemy Knight/Rider dash from reaching a high-value target even without fighting it directly (§4.4).

**Indicator**: for threatened high-value units (Catapult/Mind Bender/weak Archer), count of turns where at least one friendly unit occupies a tile that would force an enemy fast-mover (Rider/Knight/Battle-sled-equivalent) to either stop short or detour around, versus turns where the path is completely open.
**Importance: IMPORTANT** — this is the structural tool behind both "protect the catapult" and "deny the knight chain."

### 6.2 Attack ordering: soften then finish [IMPORTANT]

Because damage scales with the attacker's *current* HP fraction and the defender's *current* HP fraction (§1.1), and because a kill denies the defender any retaliation, the efficient sequence against a tough target is: hit it first with units that take no/low retaliation risk (ranged from outside its range, or units with `surprise`/`stiff`), then finish with a melee unit once the target's remaining HP is low enough to guarantee the kill (checking the §2-style math for its now-reduced current HP, not full HP). Attacking with your melee "finisher" first, before softening, risks it eating a full-strength retaliation for no reason if it fails to kill outright.

**Indicator**: for multi-attack kills (a target that took 2+ attacks from different units before dying), fraction where a ranged/no-retaliation attack preceded the melee attack vs. the reverse order.
**Good range [heuristic]**: should lean heavily toward ranged-then-melee when both unit types were available and able to reach — this is close to a dominant-strategy pattern, not a soft preference, so deviations are worth investigating individually rather than just tracking the rate.
**Importance: IMPORTANT.**

### 6.3 Retreating to heal, and tempo of moving vs. attacking [MINOR–IMPORTANT]

A unit that doesn't act at all in a turn heals (+4 HP in own territory, +2 elsewhere, per §1.5). Pulling a damaged front-line unit back one tile to let a fresher unit take its place in the line is often worth more than squeezing one more weak attack out of it, especially once its current-HP-scaled Defence Force has dropped enough that it's losing exchanges. This trades off against tempo — every unit that retreats-and-heals instead of attacking is a turn where the opponent's line isn't under pressure, so this is situational, not a universal "always retreat wounded units" rule.

**Indicator**: average HP% of units at the moment they're committed to an attack where retreating instead was a legal, available option (i.e., not required to defend a critical tile). Not a strong standalone signal; mostly useful when cross-referenced with whether the attack was then lost.
**Importance: MINOR**, rising to IMPORTANT only in attritional mid-game stand-offs rather than rush openings.

### 6.4 City capture timing and defence; sieges [IMPORTANT]

Capturing a city takes two steps: a unit **enters** an undefended (or just-cleared) city, and then must **survive** there until the start of your next turn to actually **take over** it (gain its stars/production). An entered-but-not-yet-taken-over city can still be retaken by the opponent killing your occupying unit before the turn boundary — so the attacker needs to protect the capturing unit for one more enemy turn, typically by clearing out or blocking nearby enemy units, the same way a front line protects a Catapult.

A city **under siege** (an enemy unit occupying or threatening it such that it isn't under the owner's control) produces no income for its owner that turn — denying economy is a valid alternative to an outright capture attempt when a full assault isn't worth it yet.

**Indicator**: `capture_attempts_failed_to_retake` = count of times the agent entered an enemy city but lost the occupying unit before the city flipped, versus successful takeovers. A high failure rate suggests the agent isn't bringing enough escort/clearing force before committing the capture, or is capturing into a position it can't hold.
**Importance: IMPORTANT** — city captures are the actual win condition in Might/conquest mode, so the mechanics of securing one, not just reaching one, matter directly for the score.

### 6.5 Roads, chokepoints, and using terrain for approach [MINOR]

Roads halve movement cost on the tiles they connect (0.5 move per tile vs. 1.0), which matters for exactly the kind of multi-target dash/persist chains in §4 and for getting reinforcements to a contested front faster than the opponent can. A Bridge is the water-crossing equivalent for land units, buildable only where two land tiles flank a single water tile orthogonally. On a Small/Lakes map, a well-placed road network from the capital to the contested shore can matter more than on a larger map simply because distances are short enough that one extra movement point is a meaningfully larger fraction of the map's total size.

**Importance: MINOR** as a standalone indicator (hard to cleanly separate from general economic/infrastructure development), but worth keeping in mind when interpreting why a unit reached a tile "early" or "late" relative to expectation.

### 6.6 Ambush and mind-bender tempo [MINOR]

`hide`/`infiltrate` units (Cloak-line, out of the regular-tribe kit) and `surprise` units (Dagger, Pirate) can threaten without warning from fog of war; against the 12 regular tribes specifically this mostly shows up as Rider/Knight mobility creating de facto ambushes (a fast unit appearing from outside current vision range) rather than a dedicated stealth mechanic, since no regular tribe's base kit has `hide`. Mind Bender conversions change the tempo calculation of a fight mid-combat — converting an enemy unit removes it from their side and adds it to yours in the same action, a double-value swing worth prioritizing against a key enemy unit (e.g. an enemy Catapult or Knight) rather than defaulting to killing it with a combat unit when a Mind Bender can reach it instead.

**Importance: MINOR** for the regular-tribe 1v1 format specifically (ambush mechanics matter far more when Cymanti/other special tribes are in the opponent pool).

**Sources for §6**: [Movement — Polytopia Wiki](https://polytopia.fandom.com/wiki/Movement); [Zone of control — background concept, Wikipedia](https://en.wikipedia.org/wiki/Zone_of_control); [City — Polytopia Wiki](https://polytopia.fandom.com/wiki/City); [Bridge — Polytopia Wiki](https://polytopia.fandom.com/wiki/Bridge); [Roads (Building) — Polytopia Wiki](https://polytopia.fandom.com/wiki/Roads_(Building)); musingsandroughdrafts.com blog post on capture mechanics (community, corroborating, not official); GitHub `polytopia-move-advisor` issue discussion on capture rules (community).

---

## 7. Map types, with Lakes on Small 14×14 in depth

### 7.1 Map size and water fraction

A **Small** map is 14×14 = 196 tiles (confirmed via converging sources). **Lakes** as a map type is characterized by roughly 25–30% water coverage arranged as scattered inland lakes rather than one dominant ocean, by the clustering described in available sources, with land generation favoring the map's edges and every tribe's starting position connected to at least two villages across the generated terrain. I was not able to find an official, numerically precise water-percentage or lake-count specification beyond this approximate figure, so treat the exact percentage as **approximate/contested** rather than a verified constant — if the game's own map generator source is ever available in the dump, prefer that over this figure.

### 7.2 What this means for a 1v1 Small Lakes game specifically

- **Contact comes early.** On a 14×14 board for exactly 2 players, the available land is limited and lakes compress the usable land further, so scouting/first-contact between the two capitals typically happens well before the midgame — players and guides consistently describe small 1v1 maps as fast, aggressive formats where opening tech and unit choices (not long-term economic snowballing) decide a large fraction of games.
- **Water is not peripheral — it's a central tactical feature**, not a border like it often is on bigger/Continents-style maps. Because lakes sit inside the contested land area rather than only around the edges, a navy on a Lakes map is positioned to directly interact with the main land battle (shore-side sniping, §5.1) rather than being a separate side-theater, which is exactly why skipping the navy here is costlier than it would be on a drier map type.
- **Chokepoints**: land bridges between lake-separated landmasses, and the handful of tiles bordering both players' territory, become the natural places fighting concentrates — holding or contesting these with a front-line unit (§4) plus naval support (§5) covering the water side of the same chokepoint is the composite skill this format rewards.

**Indicator**: `turn_of_first_contact` (first turn either side's units are within sighting/attack range of the other) as a sanity check that the agent is treating the opening with appropriately high urgency on this map type; compare against whatever the second researcher's economy/tech-tree part documents as typical opening tech timing.
**Importance: IMPORTANT** as a calibration check, not a tactic in itself.

**Sources for §7**: [Game Modes — Polytopia Wiki](https://polytopia.fandom.com/wiki/Game_Modes); [Map Generation — Polytopia Wiki](https://polytopia.fandom.com/wiki/Map_Generation); general community strategy commentary on small 1v1 map pacing (Steam Community discussions, Fandom Strategies page) — flagged as approximate where noted above.

---

## 8. The competitive 1v1 meta: openings, mid-game plans, punishing greed, and a blunder checklist

### 8.1 Openings and rush timings

Observed/discussed opening patterns from competitive-adjacent community sources (these are tribe-flavored, so cross-reference with the economy/tech researcher's per-tribe section):
- **Giant/early-super-unit rushes** on tribes that can reach one quickly (reported turn 5–7 on skilled play for at least one tribe) aim to grab contested villages before the opponent and convert an early tempo lead into a stars/city-count lead.
- **Rider rush**: research the mobility tech early and use Riders' movement 2 + escape to grab villages and harass before the opponent's defence is set up; a well-known generic early-aggression pattern not tied to one tribe.
- Competitive players are reported to adapt their opening specifically to the generated map layout and to the visible opponent tribe(s) in the lobby before the game starts, rather than running one fixed build order — i.e., the "best" opening is conditional on map/opponent information available at setup, which is directly relevant if the agent's self-play opponent pool/curriculum varies tribe matchups.

**Importance: IMPORTANT**, but this overlaps substantially with the economy/tech-tree researcher's scope; the combat-relevant takeaway is narrower: *whichever opening is chosen, it should produce at least one real front-line unit before committing to any ranged/support unit purchase*, which is a direct instance of §4.1 applied to the opening specifically.

### 8.2 Mid-game plans and punishing greed

- A player who over-expands economically without matching military investment is vulnerable to a focused strike that takes 1–2 cities and swings the long-run star economy decisively — this is the generic "greedy economy, weak military" punish pattern repeatedly described across beginner-guide and strategy sources.
- A player who over-invests in one unit type (most commonly: ranged/artillery without escorts, per §4; or a pure naval or pure land army on a map type that punishes the omission, per §5) is vulnerable to the specific counters already detailed in §4 and §5 — these aren't separate meta facts so much as the same two reported failure modes described from the attacker's point of view.
- Converting tempo (captured villages, early kills) into permanent advantage requires following up with new tech/unit access promptly — a common documented blunder is researching or unlocking a new unit tier and then not building/using it for several turns, losing the tempo the tech investment was supposed to buy.

### 8.3 Consolidated blunder checklist

| # | Blunder | Section | Importance |
|---|---|---|---|
| 1 | Leaving units open to a knight (or any persist-unit) chain: no front-line body able to survive the first hit, no spacing/ZoC screen | §4.3–4.4 | CRITICAL |
| 2 | Catapult/Archer/Mind Bender-only army composition with no melee front line | §4.1–4.2 | CRITICAL |
| 3 | No Port / no navy on a water-heavy map (Lakes especially) | §5.2 | CRITICAL on Lakes |
| 4 | Garrisoning a city with a non-`fortify` unit (Swordsman/Catapult/Mind Bender) instead of a `fortify` unit (Defender ideally) | §1.2 | IMPORTANT |
| 5 | Throwing ranged attacks from inside the defender's range instead of outside it, eating avoidable retaliation | §1.4 | IMPORTANT |
| 6 | Leaving a high-value ranged/support unit (land Catapult or naval Bomber) as the closest unit to the enemy | §4.1, §5.3 | CRITICAL |
| 7 | Researching/unlocking new unit tiers and not following up by building/deploying them promptly | §8.2 | IMPORTANT |
| 8 | Entering an enemy city without enough escort/clearing to survive the one extra enemy turn needed to take it over | §6.4 | IMPORTANT |
| 9 | Over-expanding economy without matching military, inviting a focused military punish | §8.2 | IMPORTANT (shared with economy researcher) |
| 10 | Committing a melee "finisher" before softening a tough target with ranged/no-retaliation attacks | §6.2 | IMPORTANT |

**Sources for §8**: [Ai-Mo/Strategy — Polytopia Wiki](https://polytopia.fandom.com/wiki/Ai-Mo/Strategy); [Strategies — Polytopia Wiki](https://polytopia.fandom.com/wiki/Strategies); [Strategies/Archive — Polytopia Wiki](https://polytopia.fandom.com/wiki/Strategies/Archive); [Polytopia World Championship site](https://championship.polytopia.io/); [The Battle of Polytopia Beginner's Guide — Level Winner](https://www.levelwinner.com/the-battle-of-polytopia-beginners-guide-tips-tricks-strategies-to-rule-the-world/); musingsandroughdrafts.com strategy write-up (community); various Steam Community discussion threads (community, used only for corroborating widely-repeated claims, never as a sole source for a numeric fact).

---

## 9. Summary table — indicators by importance

For quick reference when deciding what to monitor first in a training/evaluation pipeline.

| Indicator | Section | Importance |
|---|---|---|
| Front-line : ranged/support unit ratio | §4.2 | CRITICAL |
| Exposed ranged/support unit-turns (Catapult/Archer/Mind Bender as closest unit to enemy) | §4.1 | CRITICAL |
| Multi-kill enemy turns (knight-chain signature) | §4.4 | CRITICAL |
| Ports built / naval units built on Lakes maps | §5.2 | CRITICAL (Lakes) |
| Ranged units lost to attacks launched from water | §5.4 | CRITICAL (Lakes) |
| Fortify-capable garrison fraction in contested cities | §1.2 | IMPORTANT |
| Ranged attacks thrown from outside vs. inside defender's range | §1.4 | IMPORTANT |
| Attack-order (ranged-then-melee) on multi-attack kills | §6.2 | IMPORTANT |
| City-capture attempts that fail to hold through the take-over turn | §6.4 | IMPORTANT |
| Turn of first contact (pacing calibration) | §7.2 | IMPORTANT (calibration) |
| Average HP% at time of committing to an attack | §1.5, §6.3 | MINOR–IMPORTANT |
| Road/terrain usage for reinforcement speed | §6.5 | MINOR |

---

## Part II open questions

1. *(Resolved by the editor: the formula in §1.1 matches the game's own battle calculation on 27 open-field samples; the ×1.5/×4 bonus values remain community-sourced.)*
2. The `static` skill's precise mechanical effect could not be confirmed from available sources in this session; do not assume it's purely cosmetic.
3. Lakes map water-percentage and generation specifics (§7.1) are approximate, sourced from secondary wiki/community content, not an official numeric spec.
4. Whether defence bonuses from different sources (terrain vs. fortify) can ever combine (rather than the higher one simply applying) is stated by sources as "no stacking," but this was not independently file-verified.
5. The "good range" numbers throughout are my own heuristics derived from the unit math and general game-design reasoning, not sourced competitive benchmarks (no source in this session gave quantitative thresholds suitable for an AI-training eval) — treat them as a reasonable starting point to refine empirically against your own self-play data, not as an external ground truth.
6. Fandom wiki pages repeatedly returned HTTP 402 to direct `WebFetch` in this session; all fandom-sourced content here was reconstructed from search-engine snippets rather than full-page reads, which raises (modestly) the chance of a missed nuance on any given wiki page relative to reading it directly.
