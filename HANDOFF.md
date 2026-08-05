# Handoff

State of the project, what to do next, and why. Read this before `README.md` — that file is the
original plan and parts of it have been overtaken by what the build actually found.

**Run it:** `./run.sh`, then open <http://localhost:8080>.

```
./build/cli/poly-cli --selfplay 20             whole-engine smoke test
./build/cli/poly-cli --match rulebased random --games 20
./build/cli/poly-cli --match rulebased rb_base --games 400   A/B a bot change
./build/cli/poly-cli --ladder --games 4         round robin, ~18 min
./build/cli/poly-cli --ladder --games 6 --paper 2020 AIIDE format, ~4 min
./build/core/tests/poly_tests                   98 tests
./build/cli/poly-cli --audit 100                fuzz + census: FINDS BUGS (see §6)
python3 tools/content_audit.py                  declared-but-never-consulted content
```

**Everything defaults to the setup a player actually gets**: 16×16, four tribes, fog on — in
the browser and in the harness. `--paper` asks for the 2020 AIIDE format (11×11, two players,
fog off) explicitly. Every match prints its setup line first.

That value had **six homes and four of them were wrong**, including the browser, which opened
an 11×11 duel. `GameConfig`, `MatchConfig` and `MapConfig` now share `kDefaultMapDims` /
`kDefaultPlayerCount` from `types.hpp`; `web/js/app.js` and the New Game form read the new
`defaults` RPC at boot. A default written in six places is not a default.

**A/B a bot change against `rb_base` AND against an `rb_base` mirror control** — mirror controls
sit anywhere in 45–52%, so 50% is not the null. See §3.

---

## 1. Where things stand

**Opponent quality (2026-08-04).** `rulebased` beats `random` **100% (200-0)** and beats its own
pre-session self **63%**. It is wired into the browser, which it previously was not.

**Playable.** Full rules for the 12 regular tribes: movement with terrain and zone of control,
combat with the verified damage formula, capture, harvest, build, train, research, city growth
and level rewards, naval embark/upgrade, roads and city connections, temples, veterancy,
explorers, super units, healing, conversion, disband and destroy. Fog with a server-side
knowledge model. Default environment is the base game's 16×16 with four tribes.

**Measured.** Seven agents and an Elo ladder. The playable baseline (`rulebased`) beats **every
other agent 4-0** in both formats. Under `--paper` (11×11, 2p, no fog) it closes games in 14–20
turns where the search agents run to the 50-turn limit:

```
  agent             elo   games          rulebased 100% vs all six
  rulebased        3057      24          paper2020 100% vs all but rulebased
  paper2020        2275      24          random 0% vs everything
  mcts             2223      24
  mc               1991      24
  osla             1945      24
  rhea             1734      24
  random           1000      24
```

The gap to the next agent is 782 Elo, so the ladder no longer resolves `rulebased` at all —
every pairing is saturated. Measuring the next change to it needs `rb_base` (see §2), not this.

That table is `--paper`. In the **default** format (16×16, four tribes, fog on) `rulebased`
still wins every pairing 4-0, but the rest of the field reorders — `paper2020` drops from 2nd
to 6th and loses a game to `random`. See §3; run both.

**Verified.** 98 tests, clean under `-Werror -Wconversion` and ASAN/UBSAN. Eight of those
tests are new and were written to reproduce defects found by PLAYING — see §5.

---

## 2. The bot: what was actually wrong (resolved 2026-08-04)

The previous entry here listed three priorities and a target of "~90% against random". The
target is met — 100% (200-0) — but **only one of the three priorities was real, and it was not
on the list.** Recording this properly because the list was confidently wrong.

### What fixed it: the bot never ended its own turn

`rulebased` declines legal verbs — `Attack` when no worthwhile attack exists, `Recover` when
not badly hurt. Having declined every verb an actor offers, it answered `Deselect`, and
`legal_select` then offered that same actor again, unchanged, forever. The global pseudo-actor
made it worse: `EndTurn` was missing from the verb priority list entirely, so the bot had no
way to end a turn even when it wanted to.

Measured before the fix: **88% of all player-turns ended at the harness's 300-decision
backstop**, at ~269 decisions each, rather than by the bot's choice. The spin could start
*mid-turn*, so a single stubborn unit would consume the whole turn and the rest of the army
never moved and the stars never got spent. That last part is why priority 3 below ("the bot
hoards stars") looked true — it was a symptom, not a cause.

Fixing it (agent-side: a per-turn set of declined actors, plus a real `EndTurn`) moved
`rulebased` from **77.5% → 100%** against random, cut decisions per game 5×, and dropped mean
turns-to-win from 26.3 to 15.5. Forced turn-ends: **3057 → 0**.

This is §3's no-op defect. It was not just degrading the search agents; it was degrading the
playable baseline itself, which is why the baseline looked mediocre.

### What else helped: kill, not wound

Only one strategy change out of four measured positive. `worthwhile_attack` used to accept any
attack it survived; it now declines a wound of ≤4 HP that does not clear one of our own cities,
because Recover restores 4 HP in own territory and the wound is simply undone. **+9.8 points**
(59.8% vs the same bot without it, 400 games).

Note the distinction from the failed experiment recorded previously: that gate compared the two
sides' *damage ratio*, so a well-defended target was never attackable at all and the bot sat on
its army. This gate is absolute rather than comparative.

### What did not help — do not retry these blind

All measured at 400 games against an identical bot with only the named term removed, so ±2.5%.

| Change | Result | Note |
|---|---|---|
| **Defensive term near the capital** | **47.5%** | The previous handoff called this "almost certainly the single largest win available". It is not. Implemented narrowly (one elected defender per threatened city, so it does not recall the whole army) and it still lost ground. Expansion tempo is worth more than a garrison in this matchup. |
| **Don't walk wounded units into contact** | **48.0%** | Both halves — a retreat penalty in the move scorer and Recover-before-Move when safe — measured neutral-to-negative. |
| **Resources (5 stars) over City Wall at level 3** | **50.2%** | Exactly neutral. **Kept anyway**: the competitive consensus is emphatic ("Resources are almost always the better choice"), it is threat-conditioned so it only fires when the city is safe, and mirror matches cannot see a long-game economy edge. This is the one place I kept something the measurement did not endorse, and it is flagged rather than hidden. |

The pattern is now three-for-three: **strong design instincts about this bot keep measuring
wrong.** Units-first selection cost 55 points, the damage-ratio gate cost tempo, and the
defensive term cost 2.5. Change one thing, measure at 400 games, keep or revert.

`rb_base` and the single-flag probes (`rb_kill`, `rb_reward`) stay registered so the next change
can be A/B'd against a frozen reference rather than a remembered number. They are not on the
ladder.

---

## 3. The harness could only measure a format nobody plays

Everything in §2 was first measured in the **2020 paper's format** — 11×11, two players, fog
**off**, 50-turn cap — because that was the only format the harness could express. `play_one`
hardcoded `player_count = 2`, and the CLI had no flag for size, seats or fog. The setup a
player actually gets from New Game — **16×16, four tribes, fog on, no turn limit** — had never
been measured at all.

`game.cpp`'s own `GameConfig` comment called this exact shot: *"testing against the wrong
default hides whole classes of problem (village density, multi-front war, naval relevance)."*

**The harness now defaults to the game's own setup**, and `--paper` asks for the AIIDE format
explicitly. The first pass here got that backwards — it added `--playtest` as an opt-in and
left 11×11 as the default, which leaves the correct measurement as the one you have to remember
to type. Anything measured on autopilot should be measured on the thing being shipped.

The map size and seat count now come from `kDefaultMapDims` / `kDefaultPlayerCount` in
`types.hpp`. `GameConfig`, `MatchConfig` and `MapConfig` each used to declare them separately
and two of the three were wrong; a default that is only correct in one of the three places it
is written is not a default, it is a coincidence.

Also added `--size`, `--players`, `--fog`/`--no-fog`, `--turns`, which compose with the preset.
Every match prints its setup line first, so a result can never again be read without knowing
what produced it.

Note that the paper format is **not a gate** — §7 records that reproducing the published
ordering was dropped deliberately. Its remaining role is as a stable relative yardstick, which
is exactly the kind of thing that should be requested by name.

### The results do hold, but only after fixing how they were counted

**Read A/B results against a measured control, never against 50%.** Mirror controls scatter:
`random` 49.8%, `paper2020` 52.1%, `rb_base` 45.5% (400 games each, ±2.5%).

| 400 games | `--paper` | default (the real game) |
|---|---|---|
| control (`rb_base` mirror) | 45.5% | 48.5% |
| `rb_kill` (kill-not-wound only) | 59.8% | 56.0% |
| **`rulebased` (shipped)** | **63.0%** | **60.5%** |
| `rulebased` vs `random` | 100% (200-0) | 100% (100-0) |

Re-measured after the §5 combat fixes (Escape, Persist), which change every agent's play:
control 48.5%, `rulebased` 60.5%. The edge holds.

**A trap worth recording, because I walked into it.** My first four-player design was "A takes
one seat, B takes the other three" — the shape of the playtest itself. Two *identical* agents
then score **25%**, not 50%. I read the resulting 31.5% as a catastrophic regression before
noticing it was above the null. Seats are now split evenly between the two agents at every
player count, so 50% means "equal strength" everywhere and the numbers stay comparable to the
two-player ladder.

### The ladder reorders when you change the format

Same seven agents, same code, 4 games per pairing. `rulebased` is dominant in both, but
**everything below it moves**:

| | `--paper` (11×11, 2p, no fog) | default (16×16, 4p, fog) |
|---|---|---|
| 1 | rulebased 3057 | rulebased 2660 |
| 2 | **paper2020 2275** | mcts 1905 |
| 3 | mcts 2223 | mc 1693 |
| 4 | mc 1991 | rhea 1647 |
| 5 | osla 1945 | osla 1375 |
| 6 | rhea 1734 | **paper2020 1375** |
| 7 | random 1000 | random 1000 |

`paper2020` falls from 2nd to joint-last-but-one, and **loses a game to `random`**. Its policy
is nearest-target greedy, which holds up on a small two-player map and comes apart in a
four-player game on 16×16. That is the concrete instance of "whole classes of problem" the
`GameConfig` comment warned about, and it was invisible for as long as the harness could only
run one format.

**Do not over-read the ordering below `rulebased`.** 4 games per pairing is ±25%; only the top
seat is resolved. The point is that the ordering is *format-dependent at all*, not the specific
ranks. The default ladder also takes ~18 minutes against ~4 for `--paper`.

### A caveat on those Elo numbers: most pairings never actually finish

Six of the 21 pairings ran to **exactly 100.0 turns** — the backstop — and were settled by the
score tie-break rather than by anyone winning. Among the non-`rulebased` agents that is most of
the table, so those ratings largely measure score accumulated at the cap, not victories.
`rulebased`'s pairings all resolve in 30–45 turns, which is why it is the one rating to trust.

If the mid-table ever needs to be resolved properly, raising `--turns` is the wrong instinct;
the finding is that these agents cannot close a game on a big map, and that is the thing to fix.

### Two things the default format shows that `--paper` cannot

- **Games run much longer: mean 63 turns against 23 under `--paper`**, with many reaching
  the 100-turn backstop and being settled on *score* rather than domination. The bot is tuned
  and measured almost entirely on domination play. Score-mode endgame is unexamined.
- **The bot sees through fog.** Fog is applied at *serialisation* time — `state_view_json(...,
  viewer)` — for the human UI only. Agents always receive the true `GameState`, in the ladder
  and in `advance_bots` alike. So with `fog: true`, the human is fogged and the opponent is
  not. That may be an acceptable choice (many games let bots cheat) but it is currently
  undocumented, and it means "fog on" is not a symmetric condition. There is no fogged-state
  projection for agents to consume if we ever want one.

---

## 4. The browser opponent was playing at random

**This was the largest gap between "the bot is good" and "the game is playable", and no amount
of agent work would have closed it.** `advance_bots()` in `cli/main.cpp` — the function that
runs every non-human player for the web UI — picked uniformly from the legality mask. The
ladder's floor agent, ~1200 Elo below `rulebased`. A comment promised `bots/scripted.cpp` would
replace it; that file was never written, and `poly_agents` was already linked into the binary.

It now calls `make_agent("rulebased")`, the same agent the ladder measures, with the same
per-decision budget and the same forced-EndTurn backstop the ladder carries. Verified over RPC:
the bot trains, researches, and hands the turn back at `turn=2, current_player=0`.

Worth noting how this survived: every measurement in this project ran through `--match` and
`--ladder`, and none of them touch this code path. The thing being measured and the thing being
played were different programs.

### The action-space defect itself is still open

The engine still permits `Select` → `Deselect` as a free no-op. `rulebased` no longer *enters*
the cycle, but the fix is in the agent, not the engine, so it protects one policy rather than
all of them. It has now degraded five agents (MCTS, paper2020, RHEA, rulebased v1, and
rulebased v2 by a different route).

I did not close it in the engine because **WORKFLOW.md puts action-space changes on the
stop-and-ask list** and the owner was present. The options, for that conversation:

- *Per-turn "passed" set in the engine.* Deselecting an actor that has done nothing removes it
  from `legal_select` for that turn. Bounded and monotone. Costs the human "back out and
  immediately re-select the same unit".
- *Clear the passed set on any real action.* Restores that for the human; the bound becomes
  actors × real-actions rather than actors.
- *Leave it, keep the harness backstop.* Cheapest; leaves the trap armed for Phase 2 search.

---

---

## 5. Defects found by PLAYING, fixed 2026-08-05

Five reports from the owner playing the build. **Every one was invisible to the 89 tests that
passed at the time**, and the reason is uncomfortable: the suite tested the paths the engine
offers, and each of these is a case where the engine offered the wrong path, or none. §7's
"almost every real bug was found by running the game, not by testing it" now has five more.

Each has a regression test, and the tests are written to fail on the *specific* condition —
which took two attempts, because the obvious repros all passed:

| Report | What it actually was |
|---|---|
| "Healing can only be done for an entire turn, moving takes it away" | Recover was gated on `TurnEnded`, which `Verb::Move` sets for every unit **without Dash** — Defender, Catapult, Mind Bender, Giant. My first test used the starting Warrior, which *has* Dash, and passed. (R33) |
| "Abilities like escape do not work" | `Verb::Move` zeroes `moves_left` unconditionally and nothing restored it, so a Rider that **rode in** and attacked could not retreat — while attacking from a standing start worked fine. My first test attacked from a standing start, and passed. (R34) |
| — (found while investigating Escape) | **Persist was never implemented.** Declared in `content.hpp`, assigned to the Knight in `rules.cpp`, asserted in `test_content.cpp`, and `game.cpp` never mentioned it. The kill was computed two lines away and discarded. A Knight could never chain. (R35) |
| "Population rewards for lvl 4 does not work" | The reward menu read the city's **current** level, but a city banks a *count* of level-ups. Crossing two thresholds at once answered every outstanding choice with the top level's menu, so level 4's Population Growth was unreachable. (R36) |
| "You should see what you get from a ruin" | Two defects. `live.json` lists six ruin rewards and four were implemented — no Explorer, no naval unit, so a **water ruin** could only ever give stars/tech/population. And the "here is what you found" popup sat at `z-index: 80` under the level-up modal at `95`, while `commit()` raised the level-up *before* reporting — so a ruin granting Population buried its own popup. (R37) |

All five are written up with authority levels in `docs/rules-research/resolutions.md` (R33–R37).
R33 is an `OWNER` ruling that **overrides the wiki**, which phrases recovery as happening
"instead of moving and/or attacking"; it is tagged `_unverified` accordingly.

**Verified in the running game, not just in tests** — four distinct ruin rewards observed by
walking a unit onto ruins across 25 seeds, including the new Explorer, all in the
`Ruin: <title> -- <body>` shape the popup parses.

### What this says about the test suite

The suite is not weak, it is *aimed wrong*. It asserts that mechanics work when invoked, and
every one of these bugs is about **reachability**: a verb the engine never offers, a skill never
consulted, a menu that cannot show an entry, a popup rendered under an opaque modal. None of
that is visible from inside a test that sets up the state it wants and calls the thing directly.

The cheapest correction is what the new tests do deliberately: assert on the **enumerator**
(`enumerate_options` / `legal_*`) rather than only on `apply()`, and pick fixtures adversarially
— the Defender rather than the Warrior, move-then-attack rather than attack-from-rest.

---

## 6. How to find the bugs: `--audit` and `content_audit.py`

Five defects arrived as play reports against a build whose 89 tests passed, and patching them
one at a time was not converging. They all share a shape the unit suite structurally cannot
see, so the answer was a tool rather than more patches.

**The shape.** Every one was a REACHABILITY defect — an option the engine never offers, a skill
it never consults, a menu entry that cannot appear, an outcome that cannot be rolled. A unit
test sets up the state it wants and calls the thing directly, so it proves a mechanism works
*when invoked* and says nothing about whether the game ever invokes it. Worse, the natural
fixture hides the bug: the starting Warrior has Dash, so it heals after moving; an attack from
a standing start leaves `moves_left` intact, so Escape looks fine. **Four of my five first
regression tests passed against the broken engine.**

### `./build/cli/poly-cli --audit 100` — dynamic

Plays 100 games across five map shapes, three-quarters driven by `rulebased` and a quarter
uniform-random (the scripted agent reaches the content; random reaches the strange positions).
After every action it checks invariants, and it keeps a census.

The load-bearing check is **`offer/applies`**: everything `enumerate_options` reports as
Available must survive `apply_compound`. That one assertion turns "a button that does nothing"
from a play report into a failing build, without knowing anything about healing or ruins.
Alongside it: the state bijection, HP bounds, monotonic city level, turn termination, and
`projection/*`, which tests the architecture's own claim that `legal_actions` is a projection of
`enumerate_options` — a claim that lived in a comment and was false.

The **census** is the other half. It counts every verb, unit, reward and ruin outcome offered
and taken, and lists what never happened. A zero is a question, not a verdict — some content is
genuinely rare — but Persist, level-4 Population Growth and the water Rammer are all zeros, and
none of them is a failing assertion anywhere.

Findings carry a seed; `--audit-seed N` replays one game with every check on.

### `python3 tools/content_audit.py` — static

Answers what playing cannot: content **declared but never consulted**. `Skill::Persist` was in
`content.hpp`, assigned to the Knight in `rules.cpp`, and asserted in `test_content.cpp` — a
test that *passed* — while `game.cpp` never referenced the symbol. Three files agreed the
feature existed and the one that had to implement it did not know. No dynamic check can tell
that from bad luck.

It deliberately excludes `rules.cpp` (the data parser mentions every enum member by
construction — `{"persist", Skill::Persist}` was there the whole time) and the tests. Getting
that wrong is what made its first version report a clean bill of health.

### What the two tools found on their first run

| | |
|---|---|
| **Destroy was unreachable** | `enumerate_options` offered it on any improved tile; `legal_select` only handed out tiles that could harvest or build. A finished Farm has neither, so the button appeared and `act` answered "action refused". |
| **Build drifted on city tiles** | `legal_verbs` re-derived Build with its own `can_build` loop, sitting directly under the comment promising the mask is a projection of the enumerator. `tile_layers` gives a city tile no Tile layer, so the agent could build there and the human had no menu to do it from. The loop was redundant as well as wrong; deleted. |
| **A pending level-up did not freeze its tile** | `legal_verbs` returns `{ChooseReward, Deselect}` and the enumerator did not know, so a unit standing on a city awaiting a choice was still offered Move. |
| **Promote was unreachable after moving** | Same class as Recover. Fixing these one verb at a time was the signal to fix the class: unit selection now ASKS the enumerator instead of re-deriving. |
| **The Scout does not scout** | `Skill::Scout` — "units with the scout skill can see two tiles in every direction" — was consulted nowhere. A unit whose entire purpose is vision saw as far as a Warrior. Static audit; invisible from play, because "the Scout revealed no more than anything else" does not look like a bug from inside a fogged game. |
| **Bomber splash damage is not implemented** | `Skill::Splash`, formula known and in `live.json`. Now recorded as a gap; it was in no gap list before. |

### The first version of this audit was structurally blind, and the owner found the proof

It checked REACHABILITY — can you get to the option, does it apply, does it change something —
and **never once asked whether the resulting number was right**. So it reported a clean engine
while Windmills were broken, because Windmill adjacency is pure computation over the board and
had exactly zero coverage.

The reported symptom was "windmills don't work for diagonals or when building after the
windmill". Diagonals were fine (`count_adjacent_improvements` is 8-way). The real defect was
that `Verb::Build` computed the adjacency yield **once, at build time**, and added it to the
city — so a Windmill placed before its Farms was worth zero forever, and a Farm built beside an
existing Windmill fed it nothing. Destroy did not take the population back either.

**The general form of the bug is "a derived value stored as an event."** The general form of
the check is an **oracle**: recompute the value independently from the board and assert
equality after every action. `derived/tile-population-matches-board` does that, and it catches
every path that mutates the board without resyncing rather than the one path someone remembered.

Two more checks came out of the same thinking:

- **`derived/build-order-independent`** — a metamorphic test. Two builds on different tiles are
  independent, so A-then-B must equal B-then-A. It needs no oracle and no expected value, only
  the observation that the game should not care about the order of unrelated things. It catches
  the Windmill bug *without knowing anything about windmills*.

- Re-running the audit against a deliberately **reintroduced** bug, which is the only way to
  know a check works. The oracle fired immediately — *"tile 38 banked 0 population but the board
  says 1"* — while the 100-test suite still reported 99/100. I did not do this the first time,
  which is why the first audit shipped looking clean.

**It then found a second bug on its own**, at 200 games: *"tile 40 banked 3 population but the
board says 2."* Ownership is an input to every adjacency yield, so **capturing a city** silently
changes what the border buildings are worth on both sides — and nothing recomputed it. Border
growth had the same hole. Neither was reported by anyone; both now resync.

### Cost, honestly

Making unit selection ask the enumerator costs **MCTS about 33%** (48s vs ~36s for a 4-game
pairing) even with a fast-accept path for units that have not acted. Legality generation is the
documented search bottleneck and this project already paid to fix it once. I think correctness
wins here and the audit is what makes the trade visible, but it is a real cost and a future
search phase may want to revisit it.

### Standing invariants

- `--audit 200` reports **0 invariant violations**. Anything above zero is a regression.
- **When you add a check, verify it by reintroducing the bug it is meant to catch.** A check
  that has only ever been run against fixed code has not been tested.
- `content_audit.py` exits non-zero on **1 finding**: the 66 unparsed rule keys (§7). Everything
  else is either implemented or accounted for with a stated reason.
- `verb/convert` is offered but never taken — `rulebased` never chooses it. Agent behaviour,
  not an engine defect.

## 7. Known gaps

- **No map-type presets.** Generation is smoothed-noise blobs against a land quota. Continents,
  Pangea, Archipelago, Lakes, Waterworld and their water bands do not exist. `land_ratio` is the
  only lever.
- **Village and ruin densities are UNVERIFIED.** The documented formula
  `(floor(w/3)² − capitals) × 0.3` is integer-quantised and ignores height; on an 11×11 it gave
  2 villages against 4 ruins. Replaced with area-based density in `Rules`, so real counts can be
  dialled in as data without a rebuild. Measure them in game.
- **`web/server.js --mock` speaks wire protocol v1** and cannot serve the current UI. Port or
  delete.
- **Cloak/Dagger infiltrate, Stomp damage, and the special tribes** are unimplemented.
- **Bomber splash damage is unimplemented.** `Skill::Splash` is declared and carried by the
  Bomber, the formula is in the wiki and `live.json` has `combat.splash_divisor`, and nothing
  consults the skill — so a Bomber is a weak single-target attacker. Found by
  `content_audit.py` 2026-08-05; it was in no gap list before that.
- **`rulebased` is duplicated content-wise with `paper2020`** in places; both are scripted and
  share targeting patterns. Worth extracting a common scripted base if a third one appears.

---

## 8. Rules confidence

`docs/rules-research/resolutions.md` is the file to read before touching any rule. It records
every open question with an authority level and, critically, **59 values flagged `_unverified`
in `data/rules/live.json`** — the set of things we might be wrong about is queryable rather
than folklore.

Highest-value things still to check in the live client, cheapest first:

1. Whether a besieged city really produces nothing (R4, the owner marked this uncertain).
2. Splash rounding — settles whether HP must be fractional (UNRESOLVED #1; currently sidestepped
   by storing HP in half-point units).
3. Building costs: Lumber Hut / Port / Market.
4. Real village counts per map size.
5. Whether an enemy city's info panel shows its star production (U-H; implemented as visible
   per the owner, which contradicts the UI research).

### Verified this session (2026-08-04)

- **R4, besieged cities produce nothing — CONFIRMED, now `PRIMARY`.** The wiki states it
  outright, and adds a clause nobody had asked about: *a Market in the territory of a besieged
  city still produces.* The engine already does exactly this, because Market income is summed
  in `player_income()` rather than inside `city_income()`. Untagged in `live.json`; 59 → 58.
- **Combat is correct against the published formula.** `resolve_combat()` matches the wiki's
  `attackForce`/`defenceForce` derivation term for term, including all four no-retaliation
  conditions, defence bonus as a non-stacking scalar (1.0 / 1.5 / 4.0), the city bonus applying
  only to Fortify units, terrain bonuses gated on Archery/Climbing/Aquatism, and melee-only
  advance-after-kill. No changes needed.
- **Starfish = 8 stars is right, and the strategy page that says 10 is stale.** The dedicated
  Starfish Harvesting page: *"In the subsequent Patch of the Ocean update, Starfish were changed
  to provide 8 stars instead of 10."* Another instance of §6's "converging sources can be
  confidently wrong" — the stale number is the one that appears in the general strategy guide,
  which is the page you would naturally check first.

### Found this session, NOT fixed — the rules JSON is only half-live

`WORKFLOW.md` and `README.md` both promise "change a value, reload, play the change". That is
true for about a third of the file.

**67 of the 101 numeric keys in `data/rules/live.json` are never parsed by `rules.cpp`.** Some
are genuinely documentation — a bool like `attacker_defence_stat_unused` asserts a behaviour
rather than configuring one — but many are live-looking numbers that are hardcoded at the point
of use, so editing them does nothing and gives no warning:

| Key in `live.json` | Reality |
|---|---|
| `heal_own_territory: 4`, `heal_neutral_or_enemy_territory: 2` | `game.cpp` Verb::Recover: `const int amount = (...) ? 4 : 2;` |
| `veteran_kills_required: 3`, `veteran_hp_bonus: 5` | Exist as `Rules` fields with matching C++ defaults, but the JSON values are never read |
| `resources_upgrade_stars: 5` | `grant_reward()`: `stars += 5` |
| `road_move_cost: 0.5`, `clear_forest_stars: 1`, `disband_refund_divisor: 2`, and ~60 more | same pattern |

Every current value happens to *agree* with its hardcoded counterpart, so nothing is wrong
today. The hazard is the next tuning session: someone edits `veteran_kills_required`, reloads,
sees no change, and concludes the rule is wrong rather than that the knob is disconnected.

I did not fix it. Wiring 67 keys is a rules-semantics and schema change, which `WORKFLOW.md`
puts on the stop-and-queue list, and it deserves a decision about which keys should be live
config versus spec documentation rather than a blanket wiring. Cheapest useful step is probably
a test that fails when a numeric key is present but unparsed, so the two cannot drift silently.

**A lesson worth carrying.** `OWNER` authority is strong but not infallible: the initial vision
radius was answered as "1 tile" and was actually 5×5, and a derived arithmetic identity that
reproduced 16 published turn-0 scores beat the recollection. When a self-test disagrees with a
remembered rule, give the self-test a real hearing.

---

## 9. What the build actually taught us

Recorded because these change how the remaining phases should be approached.

- **Almost every real bug was found by running the game, not by testing it.** The action-slot
  collision, the `GameState` padding trap, the turn-limit regression, the fog-viewer leak, and
  four agent failures — none caught by unit tests. The README's argument for building the UI
  before the agent held up better than expected.
- **A legality mask can only say *no*.** Wiring the human UI to it produced an interface where
  most tiles were unclickable and actions silently vanished. The fix — one enumerator, two
  projections — is the architecture to preserve: `legal_actions()` is a *projection* of
  `enumerate_options()`, so the agent's view and the human's menu cannot drift.
- **Legality generation, not the forward model, was the search bottleneck.** `apply()` was
  recomputing `legal_actions` to validate, so every search step paid twice. Fixing that plus
  hoisting a territory test gave 3.75×. Profile before optimising; the obvious culprit was not
  the real one.
- **Converging sources can be confidently wrong.** All four wiki sources said temples reach max
  at 12 turns; in-game measurement says 8. PolyEnv contains two Market implementations and the
  *dead* one was correct. The wiki's flat-5 starting stars is stale for every tribe.
- **The published 2020 ordering is no longer a gate.** It was dropped deliberately: the
  baselines are weak, the scripted agents beat them, and reproducing a six-year-old ranking is
  not what this project is for. The ladder remains valuable as a *relative* measure.
