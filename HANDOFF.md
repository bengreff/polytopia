# Handoff

State of the project, what to do next, and why. Read this before `README.md` — that file is the
original plan and parts of it have been overtaken by what the build actually found.

**Run it:** `./run.sh`, then open <http://localhost:8080>.

```
./build/cli/poly-cli --selfplay 20             whole-engine smoke test
./build/cli/poly-cli --match rulebased random --games 20
./build/cli/poly-cli --match rulebased rb_base --games 400 --playtest    A/B a bot change
./build/cli/poly-cli --ladder --games 6         round robin, ~4 min
./build/core/tests/poly_tests                   89 tests
```

**`--playtest` runs the setup a player actually gets** (16×16, four tribes, fog on). Without it
you get the 2020 paper's format (11×11, two players, fog off) — correct for the Phase 1
anchors, but not what anyone plays. Every match prints its setup line; read it. See §3.

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

**Measured.** Seven agents and an Elo ladder that runs the published experimental setup in
about four minutes. The playable baseline (`rulebased`) now beats **every other agent 4-0** and
closes games in 14–20 turns where the search agents run to the 50-turn limit:

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

**Verified.** 89 tests, clean under `-Werror -Wconversion` and ASAN/UBSAN.

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

Added `--playtest`, plus `--size`, `--players`, `--fog`/`--no-fog`, `--turns`. Every match now
prints its setup line first, so a result can never again be read without knowing what produced
it. The paper defaults are unchanged — they must stay for the Phase 1 anchors to mean anything.

### The results do hold, but only after fixing how they were counted

**Read A/B results against a measured control, never against 50%.** Mirror controls scatter:
`random` 49.8%, `paper2020` 52.1%, `rb_base` 45.5% (400 games each, ±2.5%).

| 400 games | paper format | playtest format |
|---|---|---|
| control (`rb_base` mirror) | 45.5% | 45.5% |
| `rb_kill` (kill-not-wound only) | 59.8% | 56.0% |
| **`rulebased` (shipped)** | **63.0%** | **60.0%** |
| `rulebased` vs `random` | 100% (200-0) | 97% (97-3) |

**A trap worth recording, because I walked into it.** My first four-player design was "A takes
one seat, B takes the other three" — the shape of the playtest itself. Two *identical* agents
then score **25%**, not 50%. I read the resulting 31.5% as a catastrophic regression before
noticing it was above the null. Seats are now split evenly between the two agents at every
player count, so 50% means "equal strength" everywhere and the numbers stay comparable to the
two-player ladder.

### Two things the playtest format shows that the paper format cannot

- **Games run much longer: mean 63 turns against 23 in the paper format**, with many reaching
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

## 5. Known gaps

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
- **`rulebased` is duplicated content-wise with `paper2020`** in places; both are scripted and
  share targeting patterns. Worth extracting a common scripted base if a third one appears.

---

## 6. Rules confidence

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

## 7. What the build actually taught us

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
