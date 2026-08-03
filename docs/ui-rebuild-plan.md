# Rebuild plan: make it actually playable

Written 2026-08-03 after the first playtest failed. Supersedes the UI portions of
`docs/phase0-plan.md` §6 and the `legal`-array contract in `docs/wire-protocol.md`.

Sources: owner playtest feedback (4 question rounds), and `docs/ui-reference.md` — 1562 lines
derived from the game's own localisation string table, Harmony mod source compiled against
shipped build 122, frame-by-frame video, and full-resolution official screenshots.

---

## 1. The diagnosis

I wired the human UI directly to the agent's legality mask. Those are two different products
and I shipped one interface for both.

A legality mask is a set of permitted actions. It can only ever say **no**. It structurally
cannot say *"Farm — 5★, you have 3"* or *"Mine — needs Mining"* or *"Warrior — city is full"*,
because by the time an action is illegal it has already been deleted from the set. Every
symptom the playtest found is that one fact:

| Symptom | Cause |
|---|---|
| Most tiles unclickable | Not in the mask → no affordance at all |
| Actions silently missing rather than greyed | Filtered out before reaching the UI |
| A "legal actions" sidebar | The mask leaking into the interface as a list |
| Opponent stars and income visible | The mask has no concept of what a player may *know* |

**The real game does the opposite, and its own code proves it.** `ui-reference.md` §2 documents
the shipped API: `CommandUtils.GetTrainableUnits(..., bool includeUnavailable)` — and the UI
passes **`true`**. Every button carries *two* callbacks: the action, and an *unavailable*
callback whose only job is to explain the refusal. Refusals often ship a fix — the missing-tech
popup replaces its commit button with a `TECH TREE` jump.

The game renders the full option set and annotates it. I rendered the filtered set and deleted
everything else.

---

## 2. The architectural fix: one enumerator, two projections

The naive repair — have the UI compute its own menus — would put legality in two places and
guarantee drift. `README.md` §4 names that as the invariant the project cannot break: *"The
rules exist in exactly one place. If the browser and the trainer can disagree, the project is
already broken."*

So instead: **the option enumerator becomes the single source, and the legality mask becomes a
projection of it.**

```cpp
enum class Availability : uint8_t {
  Available,        // do it
  CannotAfford,     // right shape, not enough stars -- show cost, commit button dark red
  MissingTech,      // needs research  -- "(locked)", no commit button, offers a TECH TREE jump
  Blocked,          // structurally impossible right now -- annotated in place, e.g. "(too many)"
};

struct Option {
  Action action;            // the slot index; valid only when Available
  Verb verb;
  int cost;                 // always populated, so the price stays legible when unaffordable
  Availability status;
  Tech required_tech;       // when MissingTech
  BlockReason reason;       // when Blocked: CityFull, WrongTerrain, NoResource, AlreadyBuilt, ...
};

// The ONE place that knows what can happen on a tile.
void enumerate_options(const GameState&, const Rules&, TileIdx, Layer, std::vector<Option>&,
                       bool include_unavailable);
```

Then:

- **Agent:** `legal_actions()` = `enumerate_options(include_unavailable=false)` folded into the
  existing bitmask. Unchanged signature, unchanged action space, unchanged ~187 head.
- **Human:** the UI receives the full annotated list.

They cannot disagree, because there is only one enumerator. This is the same discipline as
`Rules` being loaded once and shared — and it means the human and the agent still play the
same game, which is the property that made "the owner plays it first" a correctness instrument
in the first place.

**Note this does not change the action space.** The agent's interface is unaffected. What
changes is that the engine gains a *richer* query alongside the mask, not instead of it.

---

## 3. The knowledge model

Currently `state_view_json` sends every player's stars, income, score and tribe to everyone.
That is wrong in a way that matters beyond UI polish: it is the same leak that would
invalidate any fog-of-war agent trained against this engine.

**New state:** `std::uint16_t met[kMaxPlayers]` — a bitmask per player of who they have
encountered. Trivially copyable, 32 bytes, keeps `GameState` a POD.

**Meeting is an event, not a flag** (`ui-reference.md` §5.4): first sight of another tribe's
unit or city fires a popup, pays out **stars scaled to that tribe's score**, and reveals their
identity. This is a real mechanic with an economic effect, currently absent entirely.

Visibility rules, per `ui-reference.md` §5 and owner confirmation:

| Datum | Own | Opponent met | Opponent unmet |
|---|---|---|---|
| Score | ✓ | ✓ | ✓ **always public** |
| City count | ✓ | ✓ | ✓ always public |
| Tribe identity | ✓ | ✓ | ✗ until met or Diplomacy |
| **Stars** | ✓ | ✗ | ✗ **never** |
| **Income** | ✓ | ✗ | ✗ never (Embassy may reveal — UNRESOLVED) |
| Techs | ✓ | ✗ | ✗ |
| City population | ✓ | ✗ | ✗ |
| City level / walled / capital | ✓ | ✓ when visible | — |
| Unit type, HP, effects | ✓ | ✓ when visible | — |

**One conflict to settle.** Owner says clicking an enemy *city* opens a panel showing its star
production, point value and capital status. The research agent says enemy cities render a bare
name plate with no income or population — and specifically corrected itself here after a
screenshot overturned its own inference, restoring the 2020 paper's claim. These are probably
compatible: bare plate on the **map**, detail in the **info panel**. Implement the panel per
owner, the map decoration per research, and flag it.

Enforcement stays server-side, as now — unexplored tiles are omitted from the payload
entirely, so no UI bug can leak the map. Extend that to player-level data.

---

## 4. Rules corrections the playtest exposed

| # | Fix | Authority |
|---|---|---|
| C1 | **Initial reveal is 5×5 explored, 3×3 owned** (radius 2 vision) | Owner + research + turn-0 score identity — **all three now agree**; the earlier R18 "1 tile" was wrong |
| C2 | **Per-tribe starting stars** — never implemented at all; everyone starts on income | Official 2025 balance-pass dev blog |
| C3 | Meeting a tribe pays stars scaled to their score | ui-reference §5.4 |
| C4 | Combat preview: exact two-way damage with death warnings for both sides | ui-reference §4 |

**C2 table** (the wiki's flat 5 is stale and wrong):
Xin-xi 7 · Oumaji 6 · Hoodrick 7 · Quetzali 7 · Yădakk 7 · **Luxidoor 2** · all others 5.

Luxidoor's 2 is the trade against its level-3 capital, which independently corroborates R12.

**Movement markers must be computed by the engine from TRUE state, not from what the viewer
can see.** Zone of control from a fog-hidden enemy still truncates movement (ui-reference §4.3).
A client computing reachability from the visible tiles would draw markers the engine then
refuses — and would leak the hidden unit's position to anyone reading the highlight.

---

## 5. The UI rebuild

Delete the legal-actions sidebar. Replace the single menu with the real game's three
simultaneous surfaces (`InteractionBar`, ui-reference §1):

1. **Description panel**, bottom-left — what is selected, and its info.
2. **Floating action button**, in world space above the selected tile.
3. **Ability tray**, bottom-left — the selected unit's actions.

**Selection.** Tapping cycles unit → city. **There is no tile layer on a city tile.** Tapping a
unit standing on a city selects the unit. With a unit already selected, tapping a reachable
tile **moves** rather than selects — ui-reference flags this as the single largest source of
misclicks in the real game, so it must be replicated, not improved.

**Every discovered tile is selectable.** Selection is not gated on the action mask. That was
the core interaction bug.

**Option rendering** — three states, no padlock icon anywhere in this game:

| State | Presentation |
|---|---|
| Available | Normal button, price shown |
| Cannot afford | Price still legible, **commit button dark red**, click explains |
| Missing tech | `(locked)` suffix, grey italic, **no commit button**, offers a `TECH TREE` jump |
| Blocked | Annotated in place — a full city still lists the unit, suffixed `(too many)` |

**Map markers.** Cyan rings = reachable. Red rings under enemies = attackable. A distinct
highlight for one-hit kills. Hover or press-and-hold gives the two-way damage preview. Spent
units render greyed; a unit that has moved but can still attack keeps a white outline.

**Other required behaviour:** two-tap confirm by default (info popup, then commit) because
**there is no undo**; End Turn turns blue and relabels to "No more moves available, end turn"
when every unit is spent; city training is a **radial menu** on the tile, not a screen; the
population strip encodes three things at once — level progress (blue), unit slots (dots), debt
(red); full-screen tech tree from the HUD, each node showing what it unlocks.

**HUD, permanent:** own stars, own income, turn number, own score, End Turn, tech tree.
Never any opponent's stars or income.

---

## 6. Sequencing

Engine before UI — the UI cannot be right while the data it renders is wrong.

| # | Step | Gate |
|---|---|---|
| S1 | `enumerate_options` + `Availability`; re-derive `legal_actions` from it | Self-play unchanged; 61 tests still green |
| S2 | Knowledge model: `met[]`, per-viewer projection, meeting event | A viewer's payload never contains another player's stars, income or techs — asserted by test |
| S3 | Rules C1–C4: 5×5 reveal, per-tribe stars, meeting payout, combat preview | Turn-0 score identity reproduces all 16 published scores |
| S4 | Wire protocol v2: `options` replaces `legal`; add `preview_combat` | Documented and frozen before UI work starts |
| S5 | UI rebuild against v2 | **Owner plays a full game and it feels like Polytopia** |

S1–S3 are mine. S5 is delegable once S4 is frozen; the previous UI agent did good work against
a bad contract, and its `projection.js` isolation and glyph-function structure survive intact.

---

## 7. Carried forward

- **Live version is 2.17.2**, not the 2.16 I briefed. Nothing invalidated; diff release notes
  from there.
- **A booby trap, named so it cannot leak in:** a widely-circulated post presenting itself as
  the 2.8.6 changelog contains *"Seeing opponent's score now only available with Strategy."* It
  is an **April Fools joke**. It reads exactly like a real gating rule.
- 19 UNRESOLVED items in `ui-reference.md`. The most consequential is whether opponent income
  is gated behind Diplomacy or the Embassy. A throwaway PolyMod patch logging
  `InteractionBar.Show` and `PlayerInfoPopup.Refresh` would settle four of them in ~20 minutes;
  the public written record is exhausted.
- Still missing from the engine and unchanged by this plan: naval movement and embarking, city
  connections and roads, temple growth, veteran promotion, super-unit spawning, and the
  Heal/Convert/Destroy/Disband verbs.
