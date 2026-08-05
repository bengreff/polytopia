# Rule resolutions

Companion to `live-rules.md` (wiki-derived spec) and `polyenv-audit.md` (MIT cross-check).
That file records what the sources say. **This file records what the engine will do, and why** —
every rule that was open after research, with its resolution and the authority behind it.

Authority levels, strongest first:

| Level | Meaning |
|---|---|
| `PRIMARY` | Stated outright by wiki/official source. Implement as written. |
| `CORROBORATED` | Two independent sources agree (wiki + PolyEnv). Strong. |
| `OWNER` | Answered by Ben from play experience. Trusted, unverified against the client. |
| `DERIVED` | Inferred from a documented outcome by arithmetic. Strong but not stated. |
| `ASSUMED` | Chosen from conflicting or absent sources. **Must carry a flag in the rules JSON.** |

Every `ASSUMED` and `OWNER` rule is tagged `"_unverified": true` in `data/rules/live.json` so the set
of things we might be wrong about is queryable, not folklore. That list is the verification backlog.

---

## Resolved by the owner (2026-08-03)

**R1 — Star income is credited at the START of a player's turn.** `OWNER`
Closes UNRESOLVED #24, which the researcher flagged as the single highest-priority gap. Income from a
city captured last turn is spendable this turn.

**R2 — A newly trained unit CANNOT act on the turn it is bought.** `OWNER`
Closes UNRESOLVED #25. The community universally repeats this but it traces to no citable source;
owner confirmation is the best authority available and is good enough to build on.

**R3 — Border Growth expands a city to 5×5, but never into already-claimed tiles.** `OWNER`
Closes UNRESOLVED #9 and, more valuably, **UNRESOLVED #10** — which the researcher could not resolve
at all. The claim rule is first-come-first-served: growth does not steal owned tiles, and this applies
equally to a newly founded city's 3×3. **Consequence: territory is not a square.** Borders are an
arbitrary tile set per city, so the engine stores an owner id per tile rather than a city rectangle.
Any implementation that assumed a rectangle would have been wrong in most real games.

**R4 — A besieged city produces no stars that turn.** ~~`OWNER`, flagged uncertain~~ → **`PRIMARY`,
CONFIRMED 2026-08-04.** The wiki states it outright, in the Exploration and Combat section of
[Strategies](https://polytopia.fandom.com/wiki/Strategies): *"Besieged cities do not provide any
income. However, a Market in the territory of a besieged city still provides income."*

The second sentence is the valuable half, because it is a rule we were never asked about and it
happens to be what the engine already does — `city_income()` returns 0 for a besieged city, while
Market income is summed separately in `player_income()` and is untouched by siege. Confirmed by
reading, not by luck, but worth recording: had Market income been folded into `city_income()` the
first clause would still have looked right and the second would have been silently wrong.

Untag `_unverified` in `data/rules/live.json`. This was the top entry on the verification backlog.

**R5 — A Cloak used on a city steals that city's star production for a turn.** `OWNER`
Cross-reference `live-rules.md` §2.5 (Cloak and Dagger, Diplomacy T3) before implementing; record the
interaction precisely rather than paraphrasing. Tagged `_unverified`.

---

## Resolved by the owner — round 2 (2026-08-03)

This round changed the data model three times. Each is recorded with the consequence, because in
every case the obvious implementation would have compiled, passed tests, and been wrong.

**R15 — Surplus population carries over on level-up.** `OWNER`
Closes UNRESOLVED #8. Population is a running total that a level-up subtracts from, not a bar that
resets. Overshooting a threshold wastes nothing.

**R16 — A captured city keeps its territory, its partial population, and its Border Growth.** `OWNER`
Closes UNRESOLVED #13 and #14. All three transfer intact.

**R17 — City level is monotonic. Population can go NEGATIVE relative to the current level.** `OWNER`
This is the most consequential answer of the round and no source hints at it.

Capture moves a city into the new owner's trade network, which recomputes its connection population —
and a captured capital can lose a great deal. But **the city does not drop a level**. Instead the
population bar goes negative, displayed as red markers, and the owner must climb back to zero before
making progress toward the next level.

Consequences, both load-bearing:
- `population` is a **signed** running total, not an unsigned counter. An unsigned type here would
  underflow to a huge positive on the exact turn a capital is captured — a spectacular bug that would
  only ever fire in the single most important moment of a Might game.
- `level` is **monotonically non-decreasing** and must never be recomputed from population. Deriving
  level from population — the natural implementation — would silently demote captured cities.

**R18 — Fog: once revealed, a tile is permanently and LIVE visible.** `OWNER`
Closes UNRESOLVED #27 and #28, and substantially simplifies the imperfect-information model.

- At game start each player sees a 1-tile radius around their capital. That initial reveal is the
  only city-based vision; cities do not project vision thereafter.
- Units and territory reveal fog.
- **Revealed tiles never re-fog, and the player continues to see live events in them** — enemy
  movement included. There is no "last known state" memory to maintain.
- Unit vision varies by type (Scout notably), and **standing on a mountain grants +1 vision**.
- A unit reveals every tile it passes through during a multi-tile move.

Consequence: fog is a **single `explored` bitset per player**, not the usual explored + currently-
visible pair with remembered stale state. Imperfect information in this game is exclusively about
*unexplored* regions, never about staleness in explored ones. That is a materially easier belief-state
problem than README §5 Phase 5 assumes, and it should make the Phase 5 fog work cheaper than planned.

**R19 — Movement is destination-only; the engine chooses the path.** `OWNER`
Unlike Civ, the player picks a destination and the game routes the unit. **This confirms the
SELECT→VERB→TARGET action space is the right shape**: TARGET is a destination tile, and no path
encoding is needed in the action space at all. Had paths been player-controlled, the action space
would have needed either per-step decisions or a path enumeration, and the ~201 head would not hold.

**R20 — Zone of Control: moving into a tile adjacent to an enemy unit consumes ALL remaining
movement.** `OWNER` Consistent with the wiki's ZoC section.

**R21 — Market: one per city (verified in game), and income is the SUM OF LEVELS of nearby
Windmills, Sawmills and Forges.** `OWNER`, verified
Closes UNRESOLVED #16 and settles the Market income conflict flagged in the PolyEnv audit — with an
instructive twist. PolyEnv contains two incompatible Market implementations: `StarsSystem` uses
`2 × unique adjacent building types` and is the one actually wired up, while `BuildingSystem`
computes a sum-of-levels model that is dead code. **The dead code was the correct one.** Had we
lifted PolyEnv's live behaviour as a cross-check we would have taken the wrong model; had we taken
the wiki's silence as agreement with the other adjacency buildings, likewise.

**R22 — Trade routes / city connections.** `OWNER`, partial
- Sea routes **cannot** pass through enemy territory; they **can** pass through neutral territory.
- Capture re-parents the city into the captor's network and recomputes connection population.
- Roads and bridges **can** be built outside your own territory.

## Resolved by the owner — round 3 (2026-08-03)

**R23 — A city connection gives +1 population to BOTH the capital and the connected city.** `OWNER`
This is why capturing a capital is devastating: it collapses the entire network at once, and combined
with R17 drives the captured capital's population negative rather than dropping its level.

**R24 — Temples: 2-turn growth, 5 levels, 8 turns to max, 100 points per level, 500 maximum.**
`OWNER`, verified in game. Closes UNRESOLVED #11 — and corrects the research.

A Temple produces 1 population **only when built**, then starts at level 1 and grows every 2 turns to
level 5. Score is 100 per level.

**Every wiki source said max level arrives at 12 turns, and that figure is wrong.** The researcher
used it to argue for a 3-turn interval, since 12 is divisible by 3 and not by 2 — sound reasoning
from a false premise. The real answer is 4 growths × 2 turns = 8. Both the interval *and* the figure
that was being used to cross-check it were wrong, which is a good argument for preferring one
in-game measurement over four agreeing documents.

**R25 — There is no city-level capture cooldown. The restriction is on units.** `OWNER`
Closes UNRESOLVED #12, and the correct model is more general than the question assumed.

**Capturing a city or exploring a ruin cannot happen on the turn the unit moved onto that tile.**
The unit must *start* its turn there. Combined with the turn-ending rules below, this produces the
observed rhythm: move onto a city turn N, capture turn N+1, an enemy kills the unit and steps on
turn N+2, recaptures turn N+3. Cities genuinely can ping-pong — just never within one turn.

**R26 — The unit turn-ending model.** `OWNER` These actions end a unit's turn:

| Action | Ends turn unless |
|---|---|
| Moving | the unit has **Dash** |
| Attacking | the unit has **Escape** |
| Capturing a city | always |
| Taking a temple | always |

This is the rule the SELECT→VERB→TARGET machine enforces at the VERB phase: after a turn-ending
action the unit leaves the legal SELECT set entirely. Dash and Escape are not stat modifiers, they
are **action-space modifiers** — they change which verbs remain legal after another verb resolves.

**R27 — Super-unit spawning displaces whatever occupies the city ("giant-popping").** `OWNER`
Spawning a super unit pushes the unit standing on that city to an adjacent tile, **or kills it if the
city is completely surrounded**. This is a real counter to the R25 sit-on-the-city rhythm and is not
documented in any source consulted. Displacement direction is described as random — the exact rule
goes on the verification list, since a deterministic engine needs it pinned down.

**R28 — Ruin rewards are terrain-gated.** `OWNER`, partial
Swordsmen come only from **land** ruins and Pirates only from **sea** ruins. Units granted by ruins
are believed to always be **veterans**. The underlying probability distribution remains UNRESOLVED
#17 — Ben has no read on it, so it stays flagged and uniform-by-default.

## Resolved by the owner — round 4 (2026-08-03)

**R29 — A naval unit is one entity with the land unit; damage carries both ways.** `OWNER`
Closes UNRESOLVED #3. HP persists through embark → sail → upgrade → disembark in both directions.
Confirms PolyEnv's `maxHealth: 0` encoding, where the hull carries no HP of its own.

**R30 — Backwards research unlocks the direct parent, applied transitively.** `OWNER`
Closes UNRESOLVED #6. Strict parent↔child is the rule, but Ben's clarification matters: the tree is
only three tiers deep, so owning a T3 tech transitively makes its T2 parent and T1 grandparent
researchable — the whole branch *upward*. The two readings the research could not distinguish turn
out to coincide in the upward direction, which is why no source ever needed to disambiguate them.
Implement parent↔child; the branch behaviour falls out.

**R31 — Roads and bridges can be built on your own and neutral territory, never enemy territory.**
`OWNER` Closes U-B.

**R32 — In-game bot behaviour.** `OWNER` Closes UNRESOLVED #23 to the extent anything can, since no
datamine exists. This is the specification for the `bots/scripted.cpp` sparring opponent:

- **Purely reactive; no forward planning.**
- **Expands hard and early, fights badly.** Strong at grabbing villages and teching, poor at combat
  value assessment.
- **But not naive.** It has real target selection: it moves units to attack other units in order to
  take a city, and it will **pursue a city across multiple turns** — so it holds a persistent goal
  even without lookahead. It also uses boats competently and can execute **Knight chaining**.
- **Its characteristic failure is over-commitment:** it will throw every unit it owns at a Giant that
  captured one of its cities.

That profile is more specific than "greedy utility scorer". A faithful clone needs a per-unit
persistent objective (a target city) layered over reactive combat scoring, plus a threat-response
term with no cost ceiling. Cloning it as pure one-step greedy would produce a visibly *different*
bot — it would never walk a unit three turns toward a city.

## New UNRESOLVED raised by this round

Ben flagged these as things he does not know. They go on the verification list rather than into code:

- **U-A: Can a unit move through a friendly unit?** He is confident units cannot pass through *enemy*
  units. Friendly pass-through is unknown and affects pathing on crowded fronts.
- ~~U-B~~ RESOLVED by R31.

- **U-C: Do another player's buildings contribute to your adjacency bonuses?** Affects Market,
  Sawmill, Windmill and Forge scoring along contested borders.
- **U-D: Which direction does a giant-pop displace the occupying unit?** (R27) Described as random.
  A deterministic engine needs the exact selection rule, and "random" must be drawn from the state's
  own PCG32 stream so replays reproduce.
- **U-E: Can the neutral roaming Explorer traverse water without the relevant tech?** Ben believes it
  is gated on the tech but flagged it for double-checking.
- **U-F: Are ruin-granted units always veterans?** Believed yes, not certain.

---

## Resolved by cross-check (wiki + PolyEnv agreeing independently)

**R6 — The combat formula is confirmed.** `CORROBORATED`
Both sources give `atk × (hp/maxHp)` vs `def × (hp/maxHp) × bonus`, the 4.5 constant, and no
retaliation on a kill. PolyEnv additionally carries an independent Python reimplementation in its
tests whose six hand-computed damage vectors agree. **Those six vectors get lifted into our test
suite verbatim** — PolyEnv is MIT, so this is clean, and an independently derived expected-value table
is exactly the fixture we would otherwise have to invent.

**R7 — Naval is definitively the modern Raft/Scout/Rammer/Bomber line.** `CORROBORATED`
Boat/Ship/Battleship and the `Float` skill are deleted. PolyEnv also encodes `maxHealth: 0` with HP
carried from the embarked unit, and upgrades in own territory at 5/5/15★. Seven naval units total
including Juggernaut, Dinghy and Pirate.

**R8 — The tech tree is the modern 25-tech, post-Path-of-the-Ocean shape.** `CORROBORATED`
Tier costs 5/10/10, `Ramming` at tier 2 from Fishing. Verify only two edges independently:
Aquatism←Ramming, and Spiritualism←Archery.

**R9 — The 9 regular land units' stats are confirmed.** `CORROBORATED`
Triple agreement inside PolyEnv (`Units.json`, its test table, its scoring system) plus the wiki.
The strongest data available anywhere; treat as settled.

---

## Resolved by derivation

**R10 — Damage rounding is round-half-away-from-zero: `floor(x + 0.5)`.** `DERIVED`
From a documented Warrior-vs-Warrior outcome landing on exactly 4.5 → observed 5. 4.5 is exactly
representable in IEEE-754, so there is no float slop to hide behind. This **rules out banker's
rounding**, which is what `std::nearbyint` and C#'s `Math.Round` do by default — the trap this note
exists to prevent.

**R11 — The Literacy discount is exactly ⅓: `ceil(cost × 2 / 3)`.** `DERIVED`
The wiki says "33%". Tested against all 111 rows of the wiki's own cost table: `ceil(cost*2/3)`
matches 111/111, `ceil(cost*0.67)` fails 28. The stated percentage is a rounding of the real rule.

**R12 — Luxidoor's capital starts at level 3.** `DERIVED`
The wiki flip-flopped five times during 2026. Resolved via the turn-0 score identity, which
reproduces all 16 published turn-0 scores. **That identity is now an engine self-test**: if our
turn-0 scores do not match the published 16, tribe setup is wrong.

---

## Engine-architecture decisions taken from unresolved rules

**R13 — HP is a scaled integer in half-point units, not a float.** `ASSUMED`
UNRESOLVED #1 is a direct contradiction between primary sources: the Combat page says splash is
`attackResult / 2` *without* rounding and documents the resulting fractional HP as a display bug,
while the Bomber and Fire Dragon pages say "rounded down". Rather than bet on either, store HP
×2 as an integer. `.5` is exactly representable, state stays bit-hashable for the determinism test,
and if measurement later says "floor" it becomes a one-line change at one call site. Sidesteps the
question instead of guessing it.

**R14 — Float determinism is a build requirement.** `PRIMARY` (of the design, not the game)
The damage formula genuinely needs floats — Knight attack is 3.5, Cloak defence 0.5. IEEE-754 basic
operations are reproducible across macOS ARM64, Windows x64 and WASM **only if** the compiler is
forbidden from contracting `a*b+c` into an FMA and from reordering under fast-math. `-ffp-contract=off`
and no `-ffast-math`, on every target. Without this the browser build and the trainer can compute
different damage from the same position, which is precisely the divergence
`README.md` §4 calls "the project is already broken".

---

## Defaulted, with the recommendation logged

Each is tagged `_unverified` in the rules JSON.

| # | Rule | Default | Why |
|---|---|---|---|
| 5 | Creep negates Zone of Control | **No** | 2 of 3 wiki statements, including the newest edit and the dedicated skills page. Near-moot in regular-tribe scope. |
| 6 | Backwards research adjacency | **Strict parent↔child** | Both officially named examples are direct-parent. The narrower rule is the safe one. |
| 7 | Tech cost falls when a city is lost | **Yes** | The formula is a pure function of current city count; asymmetry would need a source and there is none. |
| 17 | Ruin reward distribution | **Uniform** | The wiki says uniform; v2.16.3 says "changed how ruin rewards are calculated" with no detail. |
| 30 | Explorer move count | **12** | The v2.16.3 patch note is newer and explicit; the wiki page simply was not updated. |
| 11 | Temple max score / growth | **500, every 3 turns** | Most recent and officially corroborated; all sources agree max level at 12 turns, which favours 3. |

## Do not trust — PolyEnv is wrong or self-contradictory here

Use the wiki, and verify in-game:

- **Building costs.** PolyEnv's engine says LumberHut 3 / Port 7 / Market 5; its own "wiki-sourced"
  test table says 2 / 10 / 6. Both cite the same page. The tests never catch it because those
  buildings are never legal in the sampled turns.
- **Market income.** Two incompatible implementations coexist in the repo, only one of which is used.
- **Tech cost `tier × cities + 4`.** Single-sourced from one code comment. Highest-leverage
  unverified number in the ruleset — it drives the entire economy curve. Verify first.

## Legal note

PolyEnv's MIT license is real and covers `src/`, `data/Units.json`, `python/`. But `assets/` contains
**1,428 ripped Polytopia textures the author had no right to license**. That directory is never
vendored, referenced, or copied. This is exactly the contamination risk `README.md` §10 raises about
our own license hygiene.

---

## Verification backlog

Ben has Polytopia on **mobile, and is installing the Mac App Store build**. No Steam.

**Consequence for the plan:** Polyfish's memory extractor targets the Steam build, so that specific
path to real-game ground truth is unavailable. `README.md` §10 ranks it as the single highest-value
thing a license grant would buy — that ranking no longer applies, which is worth knowing before
spending more effort on outreach for it.

**But the Mac App Store build may unblock more than Steam would have.** `WORKFLOW.md` deferred the
base-game AI report on the grounds that Polytopia is absent from this Mac, and notes that **static
decompilation is sufficient and does not require running the game**. A locally installed Mac build
puts `Assembly-CSharp.dll` (Mono) or `GameAssembly` + `global-metadata.dat` (IL2CPP) on this machine.
That is a static-analysis target for both the AI report and rules ground truth, and it lands on the
dev machine rather than the Windows box. Worth checking once installed.

Measure by playing, in priority order — cheapest and highest-impact first:

1. Tech cost formula (drives the whole economy curve; PolyEnv's is single-sourced)
2. R4 — does a besieged city really produce no stars
3. Splash rounding — settles UNRESOLVED #1 and confirms or retires R13
4. Building costs — LumberHut / Port / Market
5. Temple growth interval and score maximum
6. Intra-turn phase ordering beyond income (UNRESOLVED #26)
7. Whether a captured city keeps partial population and territory (#13, #14)

---

## Verified in the live client

**V1 — `cost(tier, numCities) = tier * numCities + 4`.** `OWNER`, verified in game (2026-08-03)
The PolyEnv audit flagged this as "the single highest-leverage unverified number" because it drives
the entire economy curve, and it was single-sourced from one code comment there. Now confirmed
against the live client, which also confirms the 111/111 table match behind the Literacy variant
`ceil(cost * 2/3)` (R11). The economy formula is settled.

---

## Corrections from the first playtest (2026-08-03)

**C1 — Initial reveal is 5×5 explored / 3×3 owned. R18's "1 tile radius" was WRONG.**
Owner correction on playtest, the UI research, and the turn-0 score identity now all agree on
radius 2. This retires the conflict both parallel agents independently flagged — and the losing
answer was the owner's own earlier one, which is worth recording: `OWNER` authority is strong
but not infallible, and a derived arithmetic identity that reproduces 16 published values beat
it. When a self-test disagrees with a remembered rule, the self-test deserves a real hearing.

**C2 — Starting stars are PER TRIBE and were never implemented at all.**
Xin-xi 7 · Oumaji 6 · Hoodrick 7 · Quetzali 7 · Yădakk 7 · **Luxidoor 2** · all others 5.
Source: the official 2025 balance-pass dev blog. **The wiki's flat 5 is stale and wrong**, as
is every pre-2025 secondary source. Map generation flagged that it was not setting
`Player::stars` and I did not follow up, so every tribe has been starting on income alone.
Luxidoor's 2 is the trade against its level-3 capital, independently corroborating R12.

**C3 — Meeting a tribe is an EVENT, not a flag.** First sight of another tribe's unit or city
fires a popup, reveals their identity, and **pays stars scaled to that tribe's score**. An
economic mechanic, currently absent entirely.

**C4 — Opponent information is far more restricted than implemented.** Score and city count are
permanently public. Stars, income, techs and city population are never visible. Tribe identity
is unknown until met or until Diplomacy. See `docs/ui-rebuild-plan.md` §3 for the full matrix.

**C5 — Movement markers must be computed from TRUE state, not visible state.** Zone of control
from a fog-hidden enemy still truncates movement. Computing reachability client-side from
visible tiles would both draw markers the engine rejects and leak the hidden unit's position.

## New UNRESOLVED from the UI research

- **U-G: Is opponent income revealed by Diplomacy or by the Embassy?** Narrowed to the Embassy
  by elimination; not confirmed. The public written record is exhausted — settle it with a
  PolyMod patch logging `PlayerInfoPopup.Refresh`.
- **U-H: Does an enemy city's info panel show its star production and point value?** Owner says
  yes; the UI research says enemy cities render a bare name plate. Probably compatible — bare
  plate on the map, detail in the panel — but implement both readings explicitly and verify.

---

## Resolved by the owner — round 5 (2026-08-05), from PLAYING the game

Every entry here was reported by the owner playing the build, and every one was invisible to the
89 tests that passed at the time. The suite tested the paths the engine offers; these are all
cases where the engine offered the wrong path, or none.

**R33 — A unit that has MOVED may still Recover.** `OWNER`
Reported as "healing can only be done for an entire turn, moving takes it away as an option."

Recover was gated on `UnitFlag::TurnEnded`, and `Verb::Move` sets that flag for every unit
*without* Dash — Defender, Catapult, Mind Bender, Giant. So walking a wounded Defender back
behind the line and then healing it, the ordinary defensive move, was impossible: the unit had
to stand still for a whole turn instead.

Recover is now gated on its own terms — not freshly trained, has not attacked, has not already
healed — and needs `UnitFlag::HasRecovered` to hold that last clause, because `TurnEnded` is no
longer load-bearing here and without a dedicated bit a unit could heal repeatedly.

*Note the boundary:* attacking still precludes healing, since the attack is the unit's action.
The wiki phrases this as recovering "instead of moving and/or attacking", which reads the
stricter way; the owner's ruling from play is the stronger authority and is recorded as such.

**R34 — Escape grants a FRESH move after attacking.** `DERIVED` + `OWNER`
Reported as "abilities like escape do not work."

`Verb::Move` zeroes `moves_left` unconditionally, and nothing restored it, so a Rider that rode
in and attacked could not retreat — while attacking from a standing start worked. `moves_left`
was silently doing double duty as an undeclared "has moved" gate, contradicting
`live-rules.md` §2.3: *"A Dash+Escape unit (the Rider) therefore gets move → attack → move."*
The attack now restores the move budget for an Escape unit. Self-limiting: the retreat re-zeroes
it and sets `TurnEnded`, so there is no second escape and no attack afterwards.

**R35 — Persist lets a Knight attack again after a KILL.** `CORROBORATED`
`Skill::Persist` was declared in `content.hpp`, assigned to the Knight in `rules.cpp` and
asserted in `test_content.cpp` — and `game.cpp` never referenced it, so a Knight's turn ended on
its first kill like anything else. The kill was even computed two lines earlier and discarded.
Bounded by construction: it re-arms only on a kill, and kills are finite.

**R36 — The level-up menu follows the level the reward was EARNED at, not the city's current
level.** `DERIVED`
Reported as "population rewards for lvl 4 does not work."

A city banks a *count* of outstanding level-ups, and both reward menus read `c.level` directly.
A city that crossed two thresholds in one step — a fat harvest, a capture, or Population Growth
itself pushing it over — answered *every* outstanding choice with the menu for its current
level, so the lower level's reward could never be taken. Reaching 5 in the same step offered
Park / Super Unit twice and level 4's Population Growth never appeared.

No new state was needed: with `pending` outstanding and the city at `level`, those rewards were
earned at levels `level-pending+1 .. level`, so the next to resolve is the first of those.

**R37 — Ruins grant Explorer and, on water, a veteran Rammer.** `CORROBORATED`
Reported as "you should see what you get from a ruin."

Two separate defects. The reward pool in `live.json` lists six outcomes and only four were
implemented — no Explorer, and no naval unit — so a ruin on water could yield nothing but stars,
tech or population. `run_explorer()` already existed and was reachable only from a city
level-up. Both are now in the pool, and the land/water gift is hulled for the terrain.

The second was pure UI: the "here is what you found" popup sits at `z-index: 80` and the
level-up modal at `95`, and `commit()` called `setState` (which raises the level-up) *before*
reporting the log. A ruin granting Population can level a city, so the find opened underneath an
undismissable modal and was only seen much later, out of context. Reporting now happens first
and the result scrim outranks the level-up.

## Source hazard

A widely-circulated post presenting itself as the 2.8.6 changelog contains *"Seeing opponent's
score now only available with Strategy."* It is an **April Fools joke** (2024-04-01). It reads
exactly like a real gating rule and must never enter the ruleset.
