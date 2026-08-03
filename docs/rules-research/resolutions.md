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

**R4 — A besieged city produces no stars that turn.** `OWNER`, flagged uncertain — Ben said "I think".
Implement it, tag `_unverified`, and put it near the top of the in-game verification list. It is
cheap to check and materially changes siege value.

**R5 — A Cloak used on a city steals that city's star production for a turn.** `OWNER`
Cross-reference `live-rules.md` §2.5 (Cloak and Dagger, Diplomacy T3) before implementing; record the
interaction precisely rather than paraphrasing. Tagged `_unverified`.

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
