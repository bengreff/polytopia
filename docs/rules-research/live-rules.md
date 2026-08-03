# The Battle of Polytopia — Live Rules Reference (implementation spec)

Compiled 2026-08-03 for the C++ engine clone. Target: **the live game, version 2.16.3** (bug-fix
release, Feb 2026) — [polytopia.io/bug-fixes-in-version-2-16-3](https://polytopia.io/bug-fixes-in-version-2-16-3/).

**This document is not about the 2020 Java "Tribes" academic framework.** That framework models a
much older ruleset (Boat/Ship/Battleship naval line, pre-Diplomacy tech tree, no Cloak/Dagger, no
Path of the Ocean). Do not implement from it. See `docs/research-findings.md` for what Tribes is.

## How to read this document

- Every number carries a source URL. Where a number is stated verbatim by a source, it is quoted.
- **Nothing in this document is guessed.** Anything that could not be confirmed is in the
  [UNRESOLVED](#unresolved) section at the end, with what was found and why it is uncertain.
- Source priority: (1) official Polytopia wiki `polytopia.fandom.com`, (2) `polytopia.io` official
  site and patch notes, (3) community resources — **explicitly marked SECONDARY** and never used as
  the sole basis for a number.

### Source access note (for whoever refreshes this doc later)

`polytopia.fandom.com` **rendered HTML** is Cloudflare-gated: `WebFetch` returns HTTP 402 and a plain
`curl` gets a JS challenge. **The MediaWiki API is not gated** — it just rejects some default user
agents. This works and returns live current content:

```sh
curl -sS -A "PolytopiaResearchBot/1.0" \
  "https://polytopia.fandom.com/api.php?action=parse&page=Combat&prop=wikitext&format=json&formatversion=2"
```

`prop=text` (rendered HTML, needed for `#dpl`-generated tables), `action=query&prop=revisions`, and
`action=query&list=search` all work the same way. **Prefer this over the Wayback Machine** — it gives
live data and page revision dates.

A Wayback fallback also works via `curl` (`WebFetch` is blocked from `web.archive.org`); if you use
the `id_` raw endpoint you must gzip-decompress the response yourself. Both routes were used for this
document and cross-checked against each other on the Combat page — **identical output**.

Where a number came from a Wayback snapshot, the snapshot date is given next to it, because the wiki
is actively edited and at least one rule (Creep vs. Zone of Control, [§9.3](#93-zone-of-control))
was reversed on the wiki between January and April 2026.

Exact wiki page titles, from the wiki's own navbox (several are not what you would guess —
`Ruin` is singular, `Temples` is plural, and there are dedicated `City Upgrades`, `City Connection`,
`Map Generation`, and `List of Units` pages):

> Combat, Easter Eggs, Game Modes, Map Generation, Movement, Ruin, Score, Star, Technology, Terrain,
> Tribes, Burn Forest, Clear Forest, Destroy, Grow Forest, Starfish Harvesting, Whale Hunting,
> Buildings, Bridge, Embassy, Roads, Temples, City, City Connection, City Upgrades, Population,
> Diplomacy, Peace Treaty, Tribe Relations, Units, List of Units, Unit Skills, Super Unit, Disband,
> Update History

### ⚠ Wiki reliability warning

Several unit and technology **infoboxes are stale relative to their own page bodies**. Where an
infobox and body text disagree, this document follows the body text and the `Technology` page, and
records the conflict. Known bad infoboxes are listed in [§13.2](#132-known-stale-wiki-infoboxes-and-pages).

---

## 1. Combat

Source for this whole section unless noted: [Combat](https://polytopia.fandom.com/wiki/Combat)
(verified against Wayback snapshot 2026-03-01 and a live MediaWiki API pull 2026-08-03 — identical).

### 1.1 The damage formula

Reproduced verbatim from the wiki's code block:

```
attackForce = attacker.attack * (attacker.health / attacker.maxHealth)
defenseForce = defender.defense * (defender.health / defender.maxHealth) * defenseBonus
totalDamage = attackForce + defenseForce
attackResult = round((attackForce / totalDamage) * attacker.attack * 4.5)
defenseResult = round((defenseForce / totalDamage) * defender.defense * 4.5)
```

`attackResult` is the damage dealt by the attacker; `defenseResult` is the retaliation damage dealt
by the defender.

Key properties, all stated on that page:

- **The attacker's defence stat and the defender's attack stat are irrelevant.** There is no term
  for either.
- `defenceBonus` is applied to `defenceForce`, **not** to `defenceResult`. The wiki calls this out
  explicitly because it is counter-intuitive: *"attacking a 2-defense unit with 1.5 defenceBonus
  deals the same damage as that on a 3-defense unit without defence bonus, but the retaliation
  damage does not equal to that a 3-attack unit deals on a 2-defence unit without defence bonus."*
- **"Damaged units are weaker in all regards."** They deal less offensive and defensive damage and
  receive more damage when attacked.
- **Both results are computed from the pre-combat state, then applied.** The defender's HP loss from
  `attackResult` does not reduce its own retaliation. (SECONDARY corroboration: the FrothFrenzy
  battle calculator FAQ, [frothfrenzy.github.io/polytopiacalculator](https://frothfrenzy.github.io/polytopiacalculator/)
  — *"the defender's HP loss doesn't affect its retaliation result. Instead, it deals damage based
  on HP it had before combat began."*)

### 1.2 The 4.5 constant ("accelerated damage")

The constant is literally **4.5** in the wiki formula.

There is **no mechanic called "accelerated damage" in the live game**. A wiki full-text search for
that phrase returns nothing. What the term almost certainly refers to is this 4.5 multiplier, which
the community battle calculator names `accelerator = 4.5` (SECONDARY,
[frothfrenzy.github.io/polytopiacalculator](https://frothfrenzy.github.io/polytopiacalculator/); its
FAQ claims the pseudocode originated in a developer post on the old wikia forums, which could not be
retrieved because Fandom discussion threads are Cloudflare-gated).

Consequence: **maximum possible damage from any unit is `stat * 4.5`**, reached when the opposing
force term is zero.

### 1.3 Rounding — round-half-away-from-zero

The wiki says only *"Rounding is done to the nearest whole number"* and does not state the tie rule.
It is resolved by a documented outcome on the same page:

> *"a full-health Warrior can deal 5 or more damage even though its attack and defence stats are
> both 2"*

Warrior is attack 2 / defence 2 / HP 10. Warrior vs Warrior, both at full HP:
`attackForce = 2`, `defenseForce = 2`, `totalDamage = 4`, raw result `= (2/4) * 2 * 4.5 = 4.5`
exactly (4.5 is exactly representable in IEEE-754, so there is no float slop to hide behind). The
documented outcome is **5**.

Therefore `round(4.5) == 5`. **Implement `std::floor(x + 0.5)`**, not `std::nearbyint` with the
default banker's-rounding mode and not C#'s `Math.Round`, both of which would give 4. All damage
values are non-negative, so half-up and half-toward-+∞ are indistinguishable here.

This is an inference from a documented outcome rather than a published statement of the tie rule —
see [UNRESOLVED #4](#unresolved).

### 1.4 Implementation notes

- **Stats are not integers.** Confirmed fractional stats: Knight attack **3.5**
  ([Knight](https://polytopia.fandom.com/wiki/Knight)), Cloak defence **0.5**
  ([Cloak](https://polytopia.fandom.com/wiki/Cloak)). Use floating point for `attack` and `defence`.
- **Guard `totalDamage == 0`.** If the attacker has 0 attack and the defender has 0 defence, the
  division is 0/0. Clamp the result to 0. (SECONDARY: the community calculator does exactly this
  with an `isNaN` check.)
- **0-defence units always take maximum damage**, regardless of either side's HP: `defenseForce = 0`
  makes the attacker's ratio 1. This is why the Catapult (defence 0) is so fragile.
- **Range check uses Chebyshev (chessboard) distance**, the same metric as movement: *"A unit can
  attack enemy units whose chessboard distance is not more than the unit's range"*
  ([Units](https://polytopia.fandom.com/wiki/Units)).

### 1.5 Defence bonus

`defenceBonus` is a **single scalar selected by the situation — it does not stack or multiply.**

| Situation | `defenceBonus` |
|---|---|
| No bonus | **1** |
| Standard bonus: forest / mountain / shallow water / ocean / city without a wall | **1.5** |
| City **with a City Wall** | **4** |

Poisoned units (Cymanti mechanic — excluded, noted for completeness) have these replaced by
**0.5 / 0.7 / 2** respectively.

Terrain bonuses **require the corresponding technology** to be researched:

| Terrain | Unlocked by |
|---|---|
| Forest | Archery |
| Mountain | **Climbing** |
| Shallow water | Aquatism |
| Ocean | Aquatism |

The mountain bonus was **moved from Meditation to Climbing** in a past update
([Update History](https://polytopia.fandom.com/wiki/Update_History)). Older community sources
(including the 2019 battle calculator) still say Meditation and are wrong on this point.

City rules:

- Cities without a wall give 1.5; cities with a wall give 4.
- **Uncaptured villages give no bonus at all.**
- *"invading units besieging a city do not receive any defence bonus"* — an enemy unit standing on
  your city gets 1.
- **Cities grant their bonus only to units with the `Fortify` skill.** *"a Giant will never receive
  a defence bonus in a city because it does not have the fortify skill, although it may receive a
  defence bonus on other tiles."* This gates the city and city-wall bonus only; non-city terrain
  bonuses apply to everyone.
- Fortify holders among regular-tribe units: Warrior, Archer, Defender, Rider, Knight. Notably
  **the Swordsman lost Fortify** in Path of the Ocean 2.8
  ([Path of the Ocean](https://polytopia.fandom.com/wiki/Path_of_the_Ocean)).

**The attacker's own tile bonus does nothing.** There is no bonus term in `attackForce`, so a unit
attacking out of a forest forfeits the bonus entirely and takes full retaliation. (SECONDARY
confirmation of the intent: calculator FAQ, *"Contrary to popular belief, this has no effect on the
result."*)

There is **no separate "fortified" multiplier**. Unlike Civ-style games, Polytopia has no
fortify-in-place action; `Fortify` is a passive skill that gates city-bonus eligibility.

### 1.6 Retaliation

`defenseResult = round((defenseForce / totalDamage) * defender.defense * 4.5)`

The defender's **defence** stat takes the multiplier position that the attacker's **attack** stat
occupies in `attackResult`. `defenseForce` already contains the defender's HP ratio and the defence
bonus. Note that **the attacker's HP affects retaliation** through `totalDamage` — a damaged
attacker takes *more* retaliation.

**No retaliation occurs when** (verbatim from the wiki):

> *"No retaliation occurs if the unit being attacked is killed by the attacker, cannot reach or see
> the attacker (the attacker is outside the unit's range or hidden by fog), deals 0 retaliation
> damage, has Stiff skill or defends from a unit with Surprise."*

Broken out for implementation:

1. The defender dies from `attackResult`.
2. `chebyshev(attacker, defender) > defender.range`. **Retaliation is not keyed to the attacker
   being "ranged"** — it is keyed to whether the attacker is inside the *defender's* range. Two
   Archers (range 2) attacking each other at distance 2 **do** retaliate against each other.
3. The attacker is not visible to the defender's player (hidden by fog, or a hidden Cloak).
4. `defenseResult` computes to 0.
5. The defender has **`Stiff`**. Regular-tribe holders: Catapult, Mind Bender, Cloak, Raft, Bomber,
   Juggernaut, Dinghy.
6. The attacker has **`Surprise`**. Regular-tribe holders: Dagger, Pirate.
7. Frozen defenders (Polaris — excluded).

**`Escape` is not a no-retaliation condition.** Escape is post-attack movement; retaliation has
already resolved by then.

**Retaliation kills count toward veteran progress.**

### 1.7 Melee vs ranged: advance after kill

> *"If a melee unit (like a Warrior) kills an adjacent enemy unit, it will take the place of the
> killed unit (barring movement restrictions imposed by terrain). This movement can move a unit onto
> a friendly port, where it will be carried by a Raft. Ranged units (like an Archer) will not move
> after a kill, even when killing at melee range."*

Engine rule — advance-after-kill requires all of: (a) the attacker is melee, (b) the target died,
(c) the target's tile is legally enterable by the attacker. The wiki does not define "melee"
numerically, but every melee unit has `range == 1` and the Archer (`range == 2`) is the given
counterexample, so **`range == 1` ⇒ melee** is the working definition.

If the advance lands on a friendly Port, the unit embarks (and, being a Giant, would become a
Juggernaut and fire Stomp).

### 1.8 Action-ordering skills

Source: [Unit Skills](https://polytopia.fandom.com/wiki/Unit_Skills)

| Skill | Verbatim definition | Ordering |
|---|---|---|
| **Dash** | *"Allows a unit to attack or break ice after moving in the same turn"* | move → attack |
| **Escape** | *"Allows a unit to move after attacking in the same turn. The unit cannot attack again in the same turn if they have dash."* | attack → move |
| **Persist** | *"Allows a unit to attack again immediately after killing an enemy unit. There is no limit on the number of kills in a single turn."* | attack chain, gated on kills |

- **Rider has both Dash and Escape** → move, attack, move again — but it may not attack twice.
- **Knight has Dash + Persist** → move, then attack repeatedly as long as each attack kills.
- **Giant has neither** → *"without the Dash skill, they cannot move and attack in the same turn"*
  ([Giant](https://polytopia.fandom.com/wiki/Giant)).
- Zone of Control **does not** block Dash: *"This also does not prevent units with the Dash skill
  from attacking after moving, and the Zone of Control goes away when the enemy unit is destroyed."*
  ([Movement](https://polytopia.fandom.com/wiki/Movement))
- Embarking onto a Port **cancels Dash** and ends the unit's turn.

### 1.9 Splash

`Splash` — *"Allows a unit to damage or poison enemy units adjacent to the targeted unit."*
Among regular tribes, **only the Bomber has it.** (Also Fire Dragon — Elyrion; Exida — Cymanti.)

The wiki's splash formula:

```
attackForce = attacker.attack * (attacker.health / attacker.maxHealth)
defenseForce = defender.defense * (defender.health / defender.maxHealth) * defenseBonus
totalDamage = attackForce + defenseForce
attackResult = round((attackForce / totalDamage) * attacker.attack * 4.5)
attackSplash = attackResult / 2
```

**Critically: splash is a full damage calculation run separately against each adjacent unit**, using
*that unit's own* defence, HP and defence bonus, and then halved. It is **not** the main target's
damage copied across, and it is **not** equivalent to running the formula with half the attacker's
attack — the Bomber page says so explicitly (*"This is not the same as just calculating the damage
as if the bomber had 1.5 attack"*).

**Only the main target can retaliate** (and the Bomber has Stiff, so in practice nothing retaliates
against it).

⚠ The **rounding of splash is contradicted across primary sources** — see
[UNRESOLVED #1](#unresolved). It determines whether HP must be a float in your engine.

### 1.10 Stomp

`Stomp` — *"Causes a unit to deal damage to all adjacent enemy units when it moves."* Among regular
tribes, only the **Juggernaut** has it. It fires on every move **and additionally on embark
(a Giant moving onto a Port) and on disembark**
([Juggernaut](https://polytopia.fandom.com/wiki/Juggernaut)).

⚠ **No damage formula or multiplier for Stomp is published anywhere.** See
[UNRESOLVED #2](#unresolved). Do not guess it.

### 1.11 Mechanics NOT present in regular-tribe play

- **Poison** — Cymanti only. (Effect if you ever add it: replaces the `defenceBonus` ladder with
  0.5 / 0.7 / 2.)
- **Freeze** — Polaris only.
- **Explode** — Cymanti only.
- **Tentacles** — Aquarion only.
- **Fire** is not a combat mechanic; Burn Forest is a terrain ability.
- The **Catapult has no splash** — it is a plain single-target unit.

So the only area-damage sources available to the 12 regular tribes are **Bomber (Splash)** and
**Juggernaut (Stomp)**.

### 1.12 Healing

> *"A damaged unit can recover (heal) instead of moving and/or attacking on any given turn. Healing
> restores up to 4 HP in friendly territory and 2 HP in neutral or enemy territory. (A unit cannot
> have more than its maximum HP.)"*

```
health = min(maxHealth, health + (tileIsOwnTerritory ? 4 : 2))
```

Neutral and enemy territory both give 2. Additional rules from
[Units](https://polytopia.fandom.com/wiki/Units):

- **Auto-recover:** *"If a unit does not take any action in a given turn, it will automatically heal
  if it can do so."*
- A **poisoned** unit's Recover removes the poison and heals **0 HP** that turn.
- **Heal Others** (Mind Bender only): heals **all adjacent friendly units by up to 4 HP**, capped at
  max HP. Consumes the Mind Bender's entire turn.
- SECONDARY: multiple Mind Benders and a unit's own Recover **stack** in the same turn (calculator
  FAQ).
- Upgrading a naval unit (Raft → Scout/Rammer/Bomber) does **not** heal it.

### 1.13 Veteran / promotion

Source: [Units § Veteran Units](https://polytopia.fandom.com/wiki/Units), verbatim:

> *"A unit can be promoted to a veteran after it kills three enemy units (through attacks or
> retaliation). Promoting a unit increases its maximum health by five and then heals the unit
> completely. Therefore, a player may choose to not immediately promote a unit when it becomes
> eligible to do so because the promotion can be used as a 'free' heal later."*

- Threshold: **3 kills**, attacks or retaliation.
- Effect: **`maxHealth += 5`, then `health = maxHealth`**. **No other stat changes.**
- **Player-triggered and deferrable** — the promotion is banked and can be cashed in later as a free
  full heal. **Once per unit only.**
- **No technology is required.** (Confirmed absence across `Units`, `Unit Skills`, `Combat` and wiki
  full-text search — not a positive statement in a source.)
- **Veteran progress survives conversion**: a unit at 2/3 kills converted by a Mind Bender stays at
  2/3 ([Mind Bender](https://polytopia.fandom.com/wiki/Mind_Bender)).
- Veteran status can be *granted* to units that cannot *earn* it — ruins give a veteran Swordsman
  (land) or a veteran Rammer carrying a Warrior (water).

**Who cannot be promoted:** use the **`Static` skill as the single authoritative flag**
([Unit Skills](https://polytopia.fandom.com/wiki/Unit_Skills): *"Prevents a unit from becoming a
veteran"*). Among regular tribes that is: **all Carry naval units** (Raft, Scout, Rammer, Bomber,
Juggernaut, Dinghy, Pirate), **all super units** (Giant), **Cloak**, and **Dagger**.

### 1.14 Convert (Mind Bender)

`Convert` — *"Allows a unit to convert an enemy unit into a friendly unit by attacking it.
Converted units take up population in the mind bender's capital (except units with Independent
skill). If no capital is owned, then a random city starting from the southernmost to northernmost
city is chosen."*

- The Mind Bender has **attack 0**, so the "attack" deals no damage; the target keeps its HP.
- **Range 1**, and it has no Dash — it cannot move and convert in the same turn.
- **Population**: converted units occupy a slot in the Mind Bender's capital. *"the process can go
  over the limit, although the city won't be able to train any new units"*
  ([Population](https://polytopia.fandom.com/wiki/Population)). Units with `Independent` (Dagger,
  Pirate) consume no slot.
- **Super units can be converted.** The only unit in the game that cannot be converted is the Bunny
  ([Bunny](https://polytopia.fandom.com/wiki/Bunny)).
- **Converting counts as an attack** for other systems: it resets the Pacifist task and destroys
  embassies.
- Since update 2.0.31, 0-attack units **can** capture cities.

---

## 2. Units

Scope: the 12 regular tribes — Xin-xi, Imperius, Bardur, Oumaji, Kickoo, Hoodrick, Luxidoor, Vengir,
Zebasi, Ai-Mo, Quetzali, Yădakk. Tribe **skins are cosmetic only** and change no stat.

Primary sources: [List of Units](https://polytopia.fandom.com/wiki/List_of_Units),
[Units](https://polytopia.fandom.com/wiki/Units),
[Unit Skills](https://polytopia.fandom.com/wiki/Unit_Skills), plus each unit's own page.

### 2.1 Land units

| Unit | Cost | HP | Atk | Def | Move | Range | Tech | Skills | Vet? |
|---|---|---|---|---|---|---|---|---|---|
| **Warrior** | 2★ | 10 | 2 | 2 | 1 | 1 | *none* | Dash, Fortify | Yes |
| **Rider** | 3★ | 10 | 2 | 1 | 2 | 1 | Riding (T1) | Dash, Escape, Fortify | Yes |
| **Archer** | 3★ | 10 | 2 | 1 | 1 | 2 | Archery (T2) | Dash, Fortify | Yes |
| **Defender** | 3★ | 15 | 1 | 3 | 1 | 1 | Strategy (T2) | Fortify | Yes |
| **Swordsman** | 5★ | 15 | 3 | 3 | 1 | 1 | Smithery (T3) | Dash | Yes |
| **Mind Bender** | 5★ | 10 | 0 | 1 | 1 | 1 | Philosophy (T3) | Heal, Convert, Stiff | Yes |
| **Catapult** | 8★ | 10 | 4 | 0 | 1 | 3 | Mathematics (T3) | Stiff | Yes |
| **Knight** | 8★ | 10 | **3.5** | 1 | 3 | 1 | Chivalry (T3) | Dash, Persist, Fortify | Yes |
| **Cloak** | 8★ | 5 | 2 | **0.5** | 2 | 1 | Diplomacy (T3) | Hide, Infiltrate, Dash, Scout, Creep, Stiff, Static | **No** |
| **Dagger** | not trainable (theoretical 2★) | 10 | 2 | 2 | 1 | 1 | spawned by Cloak Infiltrate | Dash, Surprise, Independent, Static | **No** |
| **Giant** (super) | not trainable (theoretical 10★) | 40 | 5 | 4 | 1 | 1 | city level 5+ reward | Static | **No** |

Notes:

- **Swordsman has no Fortify** — removed in Path of the Ocean 2.8.
- **Knight history**: 8★ → 10★ in 2.0.58, then reverted to **8★ in 2.2 but with HP cut 15 → 10**.
  Current is 8★ / 10 HP.
- **Catapult history**: reworked in 1.7 — attack 6 → 4, cost 5★ → 8★. Current is 4 atk / 8★.
- The Dagger's "theoretical 2★" is derived from its disband refund and score value; it is not a
  purchasable price.
- Starting units: Oumaji → Rider, Hoodrick → Archer, Vengir → Swordsman, Ai-Mo → Mind Bender,
  Quetzali → Defender; every other regular tribe → Warrior.

### 2.2 Naval — the current line is Raft / Scout / Rammer / Bomber

**Definitively resolved: the live game uses the Path of the Ocean naval rework (2.8, 2023-11-21).
Boat / Ship / Battleship have been removed from the game.**

| Unit | Cost | HP | Atk | Def | Move | Range | Tech | Skills |
|---|---|---|---|---|---|---|---|---|
| **Raft** | free (auto on Port) | = carried unit | 0 | 1 | 2 | 0 | **Fishing** (T1) | Water, Carry, Stiff, Static |
| **Scout** | 5★ upgrade from Raft | = carried unit | 2 | 1 | 3 | 2 | **Sailing** (T2) | Water, Dash, Carry, Scout, Static |
| **Rammer** | 5★ upgrade from Raft | = carried unit | 3 | 3 | 3 | 1 | **Ramming** (T2) | Water, Dash, Carry, Static |
| **Bomber** | 15★ upgrade from Raft | = carried unit | 3 | 2 | 2 | 3 | **Navigation** (T3) | Water, Carry, Splash, Stiff, Static |
| **Juggernaut** (naval super) | Giant on a Port | **40** | 4 | 4 | 2 | 1 | Fishing (for the Port) | Water, Carry, Stiff, Stomp, Static |
| **Dinghy** | Cloak on a Port | **5** | 2 | 0.5 | 2 | 1 | Diplomacy + Fishing | Water, Carry, Hide, Infiltrate, Dash, Scout, Static, Stiff |
| **Pirate** | Dagger on a Port | **10** | 2 | 2 | 2 | 1 | Diplomacy + Fishing | Water, Carry, Dash, Surprise, Independent, Static |

Naval tech branch shape:
`Fishing (T1) → { Sailing (T2) → Navigation (T3) ; Ramming (T2) → Aquatism (T3) }`

Independent confirmation from the official patch notes: *"Tech tree: moved Fishing to become the
first tech in that branch and it includes Port improvement"*
([Path of the Ocean](https://polytopia.fandom.com/wiki/Path_of_the_Ocean),
[polytopia.io/path-of-the-ocean](https://polytopia.io/path-of-the-ocean/)).

#### The old line (removed — reference only, do NOT implement)

| Unit | Cost | HP | Atk | Def | Move | Range | Tech | Skills |
|---|---|---|---|---|---|---|---|---|
| Boat | free (auto on Port) | = carried unit | 1 | 1 | 2 | 2 | Sailing | Carry, Dash, Float |
| Ship | 5★ from Boat | = carried unit | 2 | 2 | 3 | 2 | Sailing | Carry, Dash, Float |
| Battleship | 15★ from Ship | = carried unit | 4 | 3 | 3 | 2 | Navigation | Carry, Dash, Scout, Float |

Differences that matter if you are porting from an old reference (such as the Tribes framework):
old path was Boat →5★→ Ship →15★→ Battleship (20★ total, sequential); Giants became **Boats**, not
Juggernauts; Navigation granted ocean movement; the `Float` skill existed (it was split into
`Water` and `Amphibious` in the 2.10.0 Aquarion rework and no longer exists).

#### Embark / upgrade / disembark rules

1. **Embark.** A **Port** converts a land unit that moves onto it. Most units → **Raft**.
   Exceptions: **Giant → Juggernaut**, **Cloak → Dinghy**, **Dagger → Pirate**. Enemy Ports cannot
   be used. ([Port](https://polytopia.fandom.com/wiki/Port))
2. **Embarking ends the turn.** *"Most land units moving into a Port will be turned into Rafts and
   will be unable to perform actions for the rest of that turn, even if they had excess movement
   points or had not attacked."* ([Movement](https://polytopia.fandom.com/wiki/Movement))
3. **HP.** The naval unit's max HP **equals the carried unit's max HP, including any veteran bonus**:
   *"a warrior in a raft can become a 10 hit point scout, rammer, or bomber. A defender raft creates
   a 15 hit point naval unit. If you put a veteran swordsman in a raft you can turn it into a 20 hit
   point bomber."* All of the land unit's other stats (atk / def / move / range / skills) are
   **discarded** while embarked. This matters for combat because the damage formula uses the HP
   ratio.
4. **Upgrade.** Raft → Scout (5★) / Rammer (5★) / Bomber (15★), **only inside your own or allied
   territory**. Upgrading **does not heal**. Once upgraded you cannot switch hulls. Juggernaut,
   Dinghy and Pirate **cannot be upgraded at all**. Note this differs from the old line: upgrades
   now branch from the Raft rather than chaining Boat→Ship→Battleship.
5. **Disembark.** Moving a Carry unit onto a land tile adjacent to water releases the carried unit
   and **ends the turn**. An **upgraded hull permanently loses its upgrade** — re-boarding a Port
   makes it a Raft again and the stars are gone. There is also an explicit **Disembark action**
   available to a naval unit on a *flooded* tile.
6. **Juggernaut Stomp fires on embark and disembark**, not just on ordinary moves.
7. **Ocean vs shallow water:** Fishing grants shallow-water movement; **Sailing** grants ocean
   movement.
8. **Bridges** (5★, Roads tech) let land units cross one water tile; naval units pass under them.

⚠ Whether a *damaged* naval unit's current HP carries back to the land unit on disembark is not
stated anywhere — see [UNRESOLVED #3](#unresolved).

### 2.3 Ability reference (implementation-level)

Primary source: [Unit Skills](https://polytopia.fandom.com/wiki/Unit_Skills).

#### Available to regular tribes

| Skill | Trigger | Exact effect |
|---|---|---|
| **Dash** | after moving, same turn | May still attack (or break ice) after moving. |
| **Escape** | after attacking, same turn | May move after attacking. If it also has Dash, it may not attack again that turn. |
| **Persist** | on killing with an attack | Attack again immediately. **No limit** on kills per turn. |
| **Fortify** | standing in a friendly city | Grants the city defence bonus (1.5, or 4 with a wall). **Without Fortify a unit gets no city bonus at all.** Does not affect non-city terrain bonuses. |
| **Stiff** | being attacked | Deals **no retaliation damage**. |
| **Surprise** | attacking | The defender does **not retaliate**. |
| **Heal** | unit action, replaces move/attack | Heals **all adjacent friendly units by up to 4 HP**, capped at max HP; removes poison. |
| **Convert** | Mind Bender attacks an enemy unit | See [§1.14](#114-convert-mind-bender). |
| **Scout** | passive vision | Reveals a **5×5** area instead of the normal 3×3 ("sees two tiles in every direction"). |
| **Carry** | passive, naval | Carries one unit. Moving onto a land tile adjacent to water releases it and ends the turn. |
| **Stomp** | whenever the unit **moves**, incl. embark/disembark | Damages **all adjacent enemy units**. Damage value unpublished — [UNRESOLVED #2](#unresolved). |
| **Static** | passive | **Cannot be promoted to veteran.** |
| **Splash** | on a successful attack | Damages all enemy units adjacent to the target; see [§1.9](#19-splash). |
| **Infiltrate** | attack targeting an adjacent **enemy city** only | See [§2.5](#25-cloak-and-dagger-diplomacy-t3). Unit is consumed. |
| **Hide** | on moving | Unit becomes **invisible to enemies**; also **ignores Zone of Control and can move through enemy units**. |
| **Creep** | passive movement | Ignores terrain movement penalties **except mountains**; also **disables road bonuses**. ⚠ Its interaction with Zone of Control is contradicted across the wiki — [UNRESOLVED #5](#unresolved). |
| **Independent** | passive | Takes **no population slot** and belongs to no city. |
| **Water** | passive, all naval units | Restricts the unit to water tiles. Naval units *without* Carry may enter cities and villages. Slowed by flooded tiles. |

#### Removed / special-tribe abilities (do not implement)

| Skill | Status |
|---|---|
| **Float** | **REMOVED** in 2.10.0 — split into `Water` and `Amphibious`. This is the old Boat/Ship/Battleship movement skill; it appears in old references and must not be implemented. |
| **Navigate** | **REMOVED** with the Navalon in Path of the Ocean. |
| **Boost** | **Not a skill name in the live game.** The nearest real mechanics are `Swarm` (Cymanti Shaman action, +1 move to adjacent friendlies), `Glide` (Polaris, +1 move on ice) and `Bubbled` (Aquarion, +1 move on flooded tiles). |
| **Explode** | Cymanti only. |
| **Tentacles**, **Auto Flood**, **Amphibious** | Aquarion only. |
| **Freeze**, **Auto Freeze**, **Freeze Area**, **Skate** | Polaris only. |
| **Poison**, **Double Attack**, **Eat**, **Grow**, **Sneak**, **Drench**, **Algae** | Cymanti / Elyrion only. |
| **Air** | Elyrion (dragons) only. |

### 2.4 Super units

Obtained by **upgrading a city to level 5 or above** and choosing "Super Unit" over "Park". Spawning
one in a besieged city triggers a forced unit spawn that **pushes the besieger out**. Theoretical
cost **10★** (disband refunds 5★; worth 50 score points). **No longer obtainable from ruins** —
removed in 2.0.58. ([Super Unit](https://polytopia.fandom.com/wiki/Super_Unit))

**All 12 regular tribes get the Giant.** No regular tribe has a different super unit. (Crab, Dragon
Egg line, Gaami and Centipede belong to the four excluded special tribes.)

Giant specifics that catch implementers out:

- **No Dash** → cannot move and attack in the same turn.
- **No Fortify** → **never** benefits from a city or city-wall defence bonus, even its own capital.
- **Static** → never a veteran.
- Moving onto a Port makes it a **Juggernaut**, not a Raft, and Stomp fires on that embark.

### 2.5 Cloak and Dagger (Diplomacy, T3)

#### Cloak — 8★, HP 5, Atk 2, Def 0.5, Move 2, Range 1

Skills: Hide, Infiltrate, Dash, Scout, Creep, Stiff, Static.
([Cloak](https://polytopia.fandom.com/wiki/Cloak))

**What reveals a Cloak:**

- It is invisible to enemies **after it moves**.
- It is **visible** when trained (its spawn turn), when capturing a village, when exploring a ruin,
  and when breaking ice.
- It is **revealed to all players** when an enemy unit attempts to move onto its tile — *the enemy's
  move is cancelled and that enemy's action is **not** spent* — and the Cloak becomes attackable.
- It is **revealed** when an enemy tries to break ice it is standing on; the ice does not break.
- **Adjacency detection:** every unit detects an enemy Cloak on an adjacent tile, shown as a small
  eye icon on that unit's map icon. This reveals adjacency only, not the exact tile.
- Cloaks belonging to a player you are **at peace** with are always visible and produce no eye icon.

**Combat:** the Cloak **cannot attack units at all**. Its attack can only target an adjacent enemy
city (Infiltrate). Stiff means it never retaliates, and 5 HP means almost anything kills it.

**Infiltrate — exact effect.** The Cloak is **consumed**, then:

1. Any enemy unit in the city takes damage computed from **the Cloak's attack stat (2) at full
   health**, regardless of the Cloak's actual current HP.
2. **Daggers spawn** within the city's borders. **Count = city level, capped at 5.** Spawn priority:
   the city tile itself (including if step 1 killed its occupant), then tiles with a defence bonus
   (forest / mountain / water — depending on which of Archery / Climbing / Aquatism you have). A
   Dagger may spawn on **water** if no empty land tile remains, in which case it spawns as a
   **Pirate**. Daggers cannot spawn on terrain the player has not unlocked movement for. **If there
   is no empty tile inside the city's borders, no Dagger spawns.** Spawned Daggers **cannot act
   until their owner's next turn**.
3. The infiltrator **immediately gains stars equal to the city's income**, and that city produces
   **0 stars** on its owner's next turn. Market / Ice Bank / Sanctuary income is unaffected.

**Restrictions:** cannot infiltrate a city another Cloak already infiltrated this turn-cycle, nor a
city **under siege**. Infiltrating does **not** count as an attack for the Pacifist task.

**Dash + Hide interaction:** a Cloak may move and then infiltrate in the same turn **only if it
began the turn already invisible**. A revealed Cloak that is already adjacent to a city may still
infiltrate. With move 2 and range 1, a Cloak anywhere in the **7×7 square centred on a city** can
infiltrate it in one turn.

**Naval form:** Cloak on a Port → **Dinghy** — same stats, keeps Hide / Infiltrate / Scout,
**loses Creep**, gains Water / Carry. Cannot be upgraded.

#### Dagger — HP 10, Atk 2, Def 2, Move 1, Range 1

Skills: Dash, Surprise, Independent, Static. **Not trainable** — only spawned by Cloak infiltration.
([Dagger](https://polytopia.fandom.com/wiki/Dagger))

- **Surprise** → its attacks never draw retaliation; with Dash this is a clean hit-and-kill unit.
- **Independent** → occupies no population slot in any city, so infiltrating immediately frees the
  producing city to train another Cloak.
- **Static** → never a veteran.
- On a Port → **Pirate**: keeps all Dagger skills including Surprise against land targets, gains
  **+1 movement** (1 → 2) and Water / Carry, **cannot be upgraded**, and **reverts to a Dagger** if
  it lands.

### 2.6 Recent unit stat changes (patch history)

| Version | Date | Regular-tribe unit impact |
|---|---|---|
| 2.8 **Path of the Ocean** | 2023-11-21 | Boat/Ship/Battleship **deleted**; Raft/Scout/Rammer/Bomber/Juggernaut added. **Swordsman lost Fortify.** Roads 2★→3★, Lumber Hut 2★→3★. Naval tech branch reshuffled. |
| 2.8.5 **Patch of the Ocean** | 2024-02-26 | **Bomber attack 4 → 3.** Aqua Farms removed. Fishing moved to first tech in its branch and given the Port. Burn Forest → Construction, Destroy → Chivalry. Bridges 7★→5★. Starfish 10★→8★. Water-ruin reward changed to a **veteran Rammer**. Temples grow every 2 turns (was 3). |
| 2.10.0 Aquarion rework | — | **`Float` split into `Water` and `Amphibious`.** All tribes gained manual disembark on bridges/flooded tiles and the Landfill ability. Tech **Aquaculture renamed to Ramming** for non-Aquarion tribes. |
| **2025 Balance Pass** | 2025-09-01 | **No regular-tribe unit stat changes.** Starting stars retuned. **Ai-Mo now starts with Philosophy + a Mind Bender.** Techs researchable backwards. Forges buildable on forests. Temples 100 pop/growth level (was 50). Burn Forest → 3★. Embassy cost scales with quantity owned. ([polytopia.io/2025-balance-pass](https://polytopia.io/2025-balance-pass/)) |
| Cymanti rework | 2025-10-29 | Cymanti only. No regular-tribe impact. |
| **2.16.3** | 2026-02-16 | Bug fixes. **Explorer moves reduced 15 → 12.** New visuals for poisoned/frozen/splash. ([polytopia.io/bug-fixes-in-version-2-16-3](https://polytopia.io/bug-fixes-in-version-2-16-3/)) |

---

## 3. Technology

Primary source: [Technology](https://polytopia.fandom.com/wiki/Technology) plus each individual tech
page. Last update that **changed the tech tree** was 2.15.0 "Cymanti Rework" (2025-10-29); last
update that changed **research rules** was the 2025 Balance Pass (2025-09-09).

### 3.1 Tree shape

**25 techs: 5 branches × (1 T1 + 2 T2 + 2 T3).** Verified against the wiki's
[Navbox Technology](https://polytopia.fandom.com/wiki/Template:Navbox_Technology) template and
cross-checked against every individual tech page's `preceded_by` / `leads_to` infobox fields.

```
T1 Climbing      ├─ T2 Mining       ─ T3 Smithery
                 └─ T2 Meditation   ─ T3 Philosophy

T1 Fishing       ├─ T2 Sailing      ─ T3 Navigation
                 └─ T2 Ramming      ─ T3 Aquatism

T1 Hunting       ├─ T2 Archery      ─ T3 Spiritualism
                 └─ T2 Forestry     ─ T3 Mathematics

T1 Organization  ├─ T2 Farming      ─ T3 Construction
                 └─ T2 Strategy     ─ T3 Diplomacy

T1 Riding        ├─ T2 Roads        ─ T3 Trade
                 └─ T2 Free Spirit  ─ T3 Chivalry
```

**Removed techs — do not implement:** `Whaling`, `Engineering`. Both are listed under "Removed" in
the navbox and struck through on the Technology page.

### 3.2 ⚠ Prerequisites: backwards research IS allowed (the wiki is stale)

The Technology page still says *"T2 and T3 technology can be only researched after the previous
technology in their branch is unlocked."* **This changed in September 2025.** Official patch notes,
verbatim:

> *"Tech can be researched 'backwards,' so Vengir can go from Smithery to Mining without Climbing"*
> — [2025 Balance Pass](https://polytopia.io/2025-balance-pass/)

The beta announcement is more explicit:

> *"You can now research 'backwards' from a T3 technology, so Vengir can get Mining and Ai-Mo can
> get Meditation without needing Climbing first"*
> — [Cymanti Rework and Balance Pass Beta, 2025-06-18](https://store.steampowered.com/news/app/874390/view/810198487112095848)

**Engine rule:** a tech is researchable if it is a T1, **or** if a tech adjacent to it in the branch
graph (parent *or* child) is already owned.

The [Smithery](https://polytopia.fandom.com/wiki/Smithery) page's line *"Starting with Smithery does
not allow researching Mining, as technologies cannot be researched backwards"* is **stale and
directly contradicted by the official note naming that exact case.** The Score page's turn-1 entry
"Ai-Mo 930, Meditation Research" (730 + 200 for a T2) independently corroborates the new rule.

See [UNRESOLVED #6](#unresolved) for the precise adjacency semantics.

### 3.3 Cost formula

```
cost(tier, numCities) = tier * numCities + 4          // tier ∈ {1,2,3}
```

Verbatim from the Technology page: *"the cost of a technology is calculated as follows: (technology
tier) × (number of cities) + 4"*, with the accompanying prose *"Initially, tier one technology cost
five stars, tier two cost six, and tier three cost seven. For each additional city the player
controls, research costs increase by one, two, or three stars for T1, T2, and T3 technology,
respectively."*

#### Literacy discount — the wiki's "33%" is wrong as an implementation rule

```
literacyCost(tier, numCities) = ceil( (tier * numCities + 4) * 2 / 3 )
```

The wiki says *"Literacy, an ability unlocked by the Philosophy technology, reduces research costs by
33%. (Discounted costs are rounded up to the nearest star.)"* Taken literally as `ceil(cost * 0.67)`
this is **wrong**. Both candidates were tested against all **37 rows** of the wiki's own Technology
Costs table (0–36 cities × 3 tiers = **111 data points**):

| Candidate | Matches |
|---|---|
| `ceil(cost * 2/3)` | **111 / 111** ✅ |
| `ceil(cost * 0.67)` | 83 / 111 ❌ |

Discriminating case: **T1 at 2 cities**, base cost 6. The wiki table says **4**.
`ceil(6 * 2/3) = 4` ✅; `ceil(6 * 0.67) = ceil(4.02) = 5` ✗. Other discriminating rows: T2@1 (6→4),
T1@5 (9→6), T2@4 (12→8), T1@8 (12→8).

**Implement as an exact one-third reduction, ceiling-rounded.** Use integer arithmetic
(`(cost * 2 + 2) / 3` in C++ integer division) to avoid float error at the boundaries.

The base formula also matched 111/111.

#### Cost table (subset)

| Cities | T1 | T1+Lit | T2 | T2+Lit | T3 | T3+Lit |
|---|---|---|---|---|---|---|
| 0 | 4 | 3 | 4 | 3 | 4 | 3 |
| 1 | 5 | 4 | 6 | 4 | 7 | 5 |
| 2 | 6 | 4 | 8 | 6 | 10 | 7 |
| 3 | 7 | 5 | 10 | 7 | 13 | 9 |
| 4 | 8 | 6 | 12 | 8 | 16 | 11 |
| 5 | 9 | 6 | 14 | 10 | 19 | 13 |
| 10 | 14 | 10 | 24 | 16 | 34 | 23 |
| 20 | 24 | 16 | 44 | 30 | 64 | 43 |
| 36 | 40 | 27 | 76 | 51 | 112 | 75 |

Note the 0-city degenerate row: all three tiers cost 4 (`tier * 0 + 4`). This matters if a player
loses every city.

#### Cost updates on city count change

- **Gain: immediate.** Confirmed by [Strategies](https://polytopia.fandom.com/wiki/Strategies):
  *"Tech becomes more expensive after you take a new village. Therefore, consider researching useful
  tech just before taking a new village. Avoiding the tech cost penalty can save 1, 2 or 3 Stars
  depending on tech tier."*
- **Loss: no source.** The formula is a pure function of the *current* city count so it should fall
  symmetrically, but nothing states it. See [UNRESOLVED #7](#unresolved).

#### Other discounts — exhaustive check

- **Literacy is the only research-cost modifier in the game.** No building, monument or other tech
  reduces tech cost.
- A **Library** building that reduced tech cost existed in pre-release beta and was **scrapped**,
  replaced by Literacy ([Philosophy](https://polytopia.fandom.com/wiki/Philosophy)).
- **Free Spirit gives no discount** — it unlocks Temple and Disband.
- A tribe's starting tech is **granted free at game start**, not discounted.
- **Ruins can grant a free technology** ("Scrolls of Wisdom") — see [§8](#8-ruins).

### 3.4 Per-tech unlocks

#### Tier 1

| Tech | Unlocks |
|---|---|
| **Climbing** | Movement on mountain tiles; **defence bonus on mountain**; reveals **metal** resource |
| **Fishing** | **Fishing** harvest action (2★ → 1 pop); **Port** (7★, 1 pop); **Raft**; movement on **shallow water** |
| **Hunting** | **Hunting** action on wild animals (2★ → 1 pop) |
| **Organization** | **Harvest Fruit** action (2★ → 1 pop); reveals **crop** resource |
| **Riding** | **Rider** unit |

#### Tier 2

| Tech | Parent | Unlocks |
|---|---|---|
| **Mining** | Climbing | **Mine** (5★, 2 pop; mountain + metal only) |
| **Meditation** | Climbing | **Mountain Temple**; **Pacifist** task → Altar of Peace (no attacks for 5 consecutive turns) |
| **Sailing** | Fishing | **Scout** unit; **movement on ocean tiles** |
| **Ramming** | Fishing | **Rammer** unit |
| **Archery** | Hunting | **Archer** unit; **defence bonus on forest** |
| **Forestry** | Hunting | **Clear Forest** ability (forest → field, **gives 1★**); **Lumber Hut** (3★, 1 pop) |
| **Farming** | Organization | **Farm** (5★, 2 pop; crop tiles only) |
| **Strategy** | Organization | **Defender** unit; ability to **send** Peace Treaties (not required to respond) |
| **Roads** | Riding | **Roads** (3★); **Bridge** (5★); **Network** task → Grand Bazaar (5 cities connected to capital) |
| **Free Spirit** | Riding | **Temple**; **Disband** ability (removes a unit, refunds half its cost rounded down) |

#### Tier 3

| Tech | Parent | Unlocks |
|---|---|---|
| **Smithery** | Mining | **Swordsman** unit; **Forge** |
| **Philosophy** | Meditation | **Mind Bender** unit; **Literacy** (⅓ tech discount); **Genius** task → Tower of Wisdom (research all tech) |
| **Navigation** | Sailing | **Bomber** unit; **Starfish Harvesting** (**8★** per starfish) |
| **Aquatism** | Ramming | **Water Temple**; **defence bonus on shallow water AND ocean** |
| **Spiritualism** | Archery | **Grow Forest** (**5★**, field → forest); **Forest Temple** (**15★** — 5★ cheaper than other temples) |
| **Mathematics** | Forestry | **Catapult** unit; **Sawmill** (1 pop per adjacent Lumber Hut) |
| **Construction** | Farming | **Windmill** (1 pop per adjacent Farm); **Burn Forest** (**3★**, forest → field **with crop**) |
| **Diplomacy** | Strategy | **Cloak** unit; **Embassy**; **Capital Vision**; reveals which players are at peace/war with each other |
| **Trade** | Roads | **Market**; **Wealth** task → Emperor's Tomb (accumulate 100★) |
| **Chivalry** | Free Spirit | **Knight** unit; **Destroy** ability (removes a building, free) |

### 3.5 Techs that change terrain access, vision, or terrain itself

#### Vision — no tech changes any unit's sight radius

Base sight radius is **1 tile** (a 3×3 area). Only two things modify it:

- **Mountains give sight radius 2** (a 5×5 area). *"Mountains provide extra sight (a two tile radius
  instead of the normal one)"* ([Combat § Vision](https://polytopia.fandom.com/wiki/Combat));
  [Terrain](https://polytopia.fandom.com/wiki/Terrain) confirms *"All units on this terrain have a
  sight radius of two tiles."* **This is a property of the tile, not of Climbing.** Climbing gates
  *access* to mountains, so it enables the effect indirectly, but it is not a vision tech.
- **The `Scout` skill** gives 2 tiles in every direction. A unit skill, not a tech.

**Diplomacy → Capital Vision** reveals the capital tiles of all discovered tribes (including
captured capitals). This is a **global reveal flag**, not a radius change.

#### Terrain access gates

| Terrain | Gate |
|---|---|
| Mountain | **Climbing** |
| Shallow water | **Fishing** (via Port/Raft) |
| Ocean | **Sailing** |

⚠ **Wiki self-conflict on the ocean gate — resolved in favour of Sailing.** The
[Terrain](https://polytopia.fandom.com/wiki/Terrain) and
[Movement](https://polytopia.fandom.com/wiki/Movement) pages both say ocean requires *"Navigation or
Free Diving"*. But [Sailing](https://polytopia.fandom.com/wiki/Sailing) and the Technology table
both say Sailing unlocks *"movement in ocean"*, and
[Navigation](https://polytopia.fandom.com/wiki/Navigation) lists only Bomber + Starfish Harvesting.
The Terrain/Movement pages retain **pre-Path-of-the-Ocean wording** — Navigation *was* the
ocean-movement tech before the rework. The Aquarion, Cymanti and Polaris pages all list ocean
movement on the *Sailing slot*, corroborating. **Use Sailing.**

Note: **Free Diving is an Aquarion-only tech**, not a general one.

#### Terrain transformation abilities

| Ability | Tech | Cost | Effect |
|---|---|---|---|
| **Clear Forest** | Forestry | — | forest → field, **gains you 1★** |
| **Grow Forest** | Spiritualism | 5★ | field → forest (own territory, no buildings on tile) |
| **Burn Forest** | Construction | **3★** | forest → field **with crop** |
| **Destroy** | Chivalry | 0★ | removes a building |

Forestry does **not** turn forest into field by itself — Clear Forest is an active ability that
*gives* 1★. Chivalry does **not** affect terrain; Destroy removes buildings only.

⚠ **Burn Forest cost was a live conflict, now resolved at 3★.** The
[Burn Forest](https://polytopia.fandom.com/wiki/Burn_Forest) infobox says 3★; its body says "two
stars"; its trivia says "raised once more to five stars";
[Construction](https://polytopia.fandom.com/wiki/Construction) says 5★. The official
[2025 Balance Pass](https://polytopia.io/2025-balance-pass/) settles it: *"Cost of 'Burn Forest'
down to 3"*. A wiki edit dated 2026-06-06 with the comment *"Update cost to 3 stars (from 5)"*
confirms. **Use 3★.**

### 3.6 Research gives no stars; score is 100 per tier

- **Researching a technology grants no stars.** Star sources are city income, harvests, Clear Forest
  (1★), ruins (10★), Starfish Harvesting (8★), tribe encounters, and Disband refunds.
- **Score: 100 points per tier** — T1 = 100, T2 = 200, T3 = 300
  ([Score](https://polytopia.fandom.com/wiki/Score): *"Technology: 100 points per tier of the
  technology."*).

Cross-check against the Score page's turn-0 tribe table: base with no tech ≈ 415 (Aquarion, no
starting tech). Xin-xi 515 = 415 + 100 (T1). Zebasi/Yădakk 615 = 415 + 200 (T2). Vengir 730 =
415 + 300 (T3 Smithery) + 15 (Swordsman 5★ × 5 pts/★ = 25, minus the Warrior's 10). Ai-Mo 730 = the
same arithmetic with Philosophy + Mind Bender. All internally consistent.

### 3.7 Tech-tree history (what changed, so you can spot stale references)

Three sequential reworks, all confirmed by official patch notes
([Path of the Ocean](https://polytopia.fandom.com/wiki/Path_of_the_Ocean)):

**Pre-Path-of-the-Ocean (before Nov 2023)** — this is roughly what the 2020 Tribes framework models:
T1 Fishing (fishing action only); T2 Sailing (Boat/Ship, shallow water, Port at 10★/2 pop); T2
Whaling (Whale Hunting 10★); T3 Navigation (**Battleship**, **ocean movement**); T3 Aquatism (Water
Temple, water defence). Trade unlocked the **Customs House**.

**Path of the Ocean v2.8.0 (2023-11-21):** Sailing ↔ Fishing swapped; Whaling renamed
**Aquaculture** (Rammer + Aqua Farm), Whale → **Starfish** (10★); Navigation → Bomber + Starfish
Harvesting; Battleship → Bomber, Boat/Ship → **Raft/Scout/Rammer**, Giant-in-Port → **Juggernaut**;
Roads gained the **Bridge** (7★); Customs House → **Market**; Port 10★/2pop → 7★/1pop; Roads 2★→3★;
Lumber Hut 2★→3★; **Swordsman lost Fortify**.

**Patch of the Ocean v2.8.5 (2024-02-26):** **Fishing moved back to T1 and absorbed the Port** —
this is the current state. Aqua Crop/Aqua Farm removed (later returned Aquarion-exclusive).
**Burn Forest moved to Construction; Destroy moved to Chivalry.** Bridge 7★→5★; Starfish 10★→**8★**;
**Bomber attack 4→3**; water-ruin reward Warrior-Bomber → **veteran Rammer**; Markets simplified.

**Aquarion rework v2.10 (Aug 2024):** *"'Aquaculture' tech renamed to 'Ramming'"*. Aquarion kept the
name Aquaculture for its Aqua Farm version. `Float` split into `Water` and `Amphibious`.

---

## 4. Cities

Primary sources: [City](https://polytopia.fandom.com/wiki/City),
[Population](https://polytopia.fandom.com/wiki/Population),
[City Upgrades](https://polytopia.fandom.com/wiki/City_Upgrades),
[City Connection](https://polytopia.fandom.com/wiki/City_Connection).

### 4.1 Population thresholds

> *"Upgrading a city to level n from the previous level requires n population."* — City
>
> *"The amount of population required to upgrade a city is one more than the city's current level,
> e.g., it takes two population to upgrade a city from level 1 to level 2."* — Population

```
popNeededForStep(n)      = n                 // population for the (n-1) -> n upgrade
cumulativePopFromLevel1(n) = n*(n+1)/2 - 1
```

| Transition | Pop for this step | Cumulative from L1 |
|---|---|---|
| 1 → 2 | 2 | 2 |
| 2 → 3 | 3 | 5 |
| 3 → 4 | 4 | 9 |
| 4 → 5 | 5 | 14 |
| 5 → 6 | 6 | 20 |
| 6 → 7 | 7 | 27 |
| 7 → 8 | 8 | 35 |
| 8 → 9 | 9 | 44 |
| 9 → 10 | 10 | 54 |
| (n-1) → n | n | n(n+1)/2 − 1 |

**No level cap is documented.** A captured village becomes a **level 1 city with 0 population**.

**Negative population is a real state.** *"A city may have negative population, shown as red
population bars. (This typically occurs to a former capital that has lost the population produced
from city connections.) Each negative population decreases the income the city produces by one star
per turn up to the point where the city no longer produces any income. A city cannot produce
negative income. However, negative population does not impact the number of units a city can
support."*

### 4.2 What every level-up gives unconditionally

> *"Upgrading a city increases the income it produces by one star per turn, its unit capacity by
> one, and its point value by 50 points."*

- **+1 star/turn**
- **+1 unit capacity.** Unit capacity `= city level + 1`.
- **+50 score.** A city is worth `100 + 50 * (level - 1)`.

Score decomposition (needed if you model score exactly): each population gained is worth 5 points,
plus a flat level bonus of `50 - 5n` for reaching level *n*. So L2 bonus = 40, L3 = 35, L4 = 30 …
**L10 = 0, and levels above 10 give a negative bonus**
([Score](https://polytopia.fandom.com/wiki/Score)).

### 4.3 Level-up reward choice

| New level | Option A | Option B |
|---|---|---|
| **2** | Workshop | Explorer |
| **3** | City Wall | **Resources (five stars)** |
| **4** | **Population Growth (+3 population)** | Border Growth |
| **5+** | Park | Super Unit |

Level 5 and every level above it repeat the Park / Super Unit choice.

| Reward | Exact effect |
|---|---|
| **Workshop** | **+1 star/turn** permanently to that city. Rendered among the city buildings. ([Workshop](https://polytopia.fandom.com/wiki/Workshop)) |
| **Explorer** | Spawns an auto-moving fog-clearing figure with **12 moves** (reduced from 15 in v2.16.3, Feb 2026 — the [Explorer](https://polytopia.fandom.com/wiki/Explorer) wiki page still says 15 and is stale). Cannot enter mountain / shallow water / ocean without Climbing / Fishing / Sailing respectively. Can move over other units including enemies. |
| **City Wall** | Raises `defenceBonus` from 1.5 to **4** for friendly units **with the Fortify skill** in that city. ([City Wall](https://polytopia.fandom.com/wiki/City_Wall)) |
| **Resources** | **+5 stars**, one time. ([Star](https://polytopia.fandom.com/wiki/Star): *"Resources city upgrade bonus — 5"*) |
| **Population Growth** | **+3 population** to that city, free. |
| **Border Growth** | Expands that city's territory — see [§4.5](#45-borders-and-territory). No stars, no population. |
| **Park** | **+250 score** *and* **+1 star/turn**. (Pre-Path-of-the-Ocean it was points only.) ([Park](https://polytopia.fandom.com/wiki/Park)) |
| **Super Unit** | Spawns the tribe's super unit — **the Giant for all 12 regular tribes**. Free, but carries a theoretical cost of 10★ → disband value 5★, score value 50. Spawning it in a besieged city force-pushes the invader out. |

### 4.4 Star production per city

```cpp
int cityIncome(const City& c) {
    if (c.underSiege) return 0;              // an enemy unit standing on the city
    int income  = c.level;                   // 1 star/turn per level
    income += c.capitalBonus;                // see table below
    income += c.numWorkshops;                // +1 each
    income += c.numParks;                    // +1 each
    income -= std::max(0, -c.population);    // each NEGATIVE population costs 1 star
    return std::max(0, income);              // "A city cannot produce negative income"
}
```

All four clauses are sourced: *"Cities produce one star per turn per level."* / *"Workshops and
Parks increase a city's income by one extra star per turn."* / *"Cities that are under siege by an
enemy unit do not produce any income."* (all [City](https://polytopia.fandom.com/wiki/City)), and
the negative-population clause from [Population](https://polytopia.fandom.com/wiki/Population).

**Capital bonus** — the City page gives a level-1 capital's *total* income by controller type; the
bonus is that minus 1. **This is also the AI difficulty handicap** — see [§12](#12-the-in-game-ai).

| Controller | Level-1 capital income | Implied capital bonus |
|---|---|---|
| Easy bot | 1 | **+0** |
| **Human player** | **2** | **+1** |
| Normal bot | 2 | +1 |
| Hard bot | 3 | +2 |
| Crazy bot | 5 | +4 |

> *"The starting income of a capital varies by the type of player that controls it (human or bot).
> Capitals of easy bots produce one star per turn, those of human players and normal bots produce
> two, those of hard bots produce three, and those of crazy bots produce five."*

Worked example straight from the wiki's own image caption: *"It is a capital and has a Workshop, so
it produces 2+1+1=4 stars per turn"* (level 2 capital + Workshop). Cross-check: Luxidoor's level 3
capital gives **4 SPT** (3 level + 1 capital).

### 4.5 Borders and territory

**A new city claims 3×3 = 9 tiles** (Chebyshev radius 1, city at centre). Primary-source confirmed
by the [Score](https://polytopia.fandom.com/wiki/Score) worked example *"level 1 city + 9 tiles
territory = 280 points"* → 100 (city) + 9 × 20 (territory at 20 pts/tile) = 280.

**Border Growth expands to 5×5 = 25 tiles** (Chebyshev radius 2). ⚠ **SECONDARY SOURCES ONLY** — the
wiki never states the number. Two independent secondary sources agree (a Polytopia Fandom forum
thread, *"your city will cover a 5x5 area (unless there is another city in the way)"*, and
NamuWiki, *"border growth expands the city area to the surrounding 2 spaces"*), and it is
corroborated indirectly by [Map Generation](https://polytopia.fandom.com/wiki/Map_Generation):
*"All resources spawn only within two tiles of cities or villages"* — the generator only ever seeds
resources out to Chebyshev radius 2, exactly the maximum border reach. Treat as high-confidence but
not officially confirmed. See [UNRESOLVED #9](#unresolved).

**Tile claiming** appears to be first-come-first-served — the forum quote's *"unless there is
another city in the way"* implies growth does **not** steal already-owned tiles, and
[Strategies](https://polytopia.fandom.com/wiki/Strategies/Observations_and_General_Strategies) lists
Border Growth as a way to *pre-emptively deny* tiles to a village the opponent has not captured yet.
Exact overlap tie-breaking is undocumented — [UNRESOLVED #10](#unresolved).

Territory is worth **20 points/tile**; explored tiles are worth **5 points each**.

### 4.6 Unit capacity

> *"The maximum number of units a city can support is equal to the number of population bars (shown
> below the city's name) it has, which is one more than the city's level."*

`capacity = level + 1`. Additional rules from
[Population](https://polytopia.fandom.com/wiki/Population):

- A unit that captures a village or city **migrates its origin city** to the newly captured one.
- Units removed by death or Disband free their slot.
- Units gained from **ruins** inherit the exploring unit's origin city but **do not count** against
  capacity.
- Units **converted** by a Mind Bender take a slot in the Mind Bender's city and **may exceed the
  limit**, in which case the city simply cannot train anything new.
- Units with `Independent` (Dagger, Pirate) belong to no city and consume no slot.
- **On capture, units the city produced do not disappear — they lose their origin city.** If the
  city is recaptured they do **not** re-count against its capacity. *"This effectively allows a
  potentially infinite number of units."*

### 4.7 Capital

- The capital is the player's **starting city**; crown icon and underlined name.
- **+1 star/turn** for a human player (bot values above).
- Contains a decorative **city castle**, unique per tribe. **It remains if the capital is captured.**
- **City connections anchor on the capital**: each connection grants **+1 population to the capital
  and +1 to the connected city** ([City Connection](https://polytopia.fandom.com/wiki/City_Connection)).
- The **Population ruin reward (+3 pop) goes to the capital**; if you no longer hold your original
  capital but hold captured ones, it goes to the **highest-level capital**
  ([Ruin](https://polytopia.fandom.com/wiki/Ruin)).
- **Discovering a Lighthouse gives +1 population to your capital.**
- **Embassies can only be built in a capital still held by its original owner** — not in a captured
  capital ([Embassy](https://polytopia.fandom.com/wiki/Embassy)).
- Diplomacy grants **Capital Vision**.

**Losing your capital** stops it being a capital, so all city-connection population evaporates —
this is the documented cause of negative population.

### 4.8 City capture

⚠ **The wiki has no dedicated capture-mechanics page.** Everything below is assembled from scattered
sourced statements; the gaps are in [UNRESOLVED](#unresolved) and must not be filled by guessing.

| Rule | Source |
|---|---|
| **Any unit that started the turn on a village/city captures it. Capturing ends the unit's turn.** | [City § Village](https://polytopia.fandom.com/wiki/City) |
| Cities are acquired **only** by capturing villages or enemy cities. | City |
| The capturing unit **migrates its origin city** to the captured city. | City, Population |
| **Units the city produced survive capture** but lose their origin city; on recapture they do not re-count against capacity. | Population |
| **A besieged city produces 0 income** — this is the mechanical basis of "a captured city can't produce". | City |
| **City walls persist** — a captured walled city still confers its bonus to the new owner's Fortify units. | Combat, City |
| **The city castle persists** when a capital is captured. | City |
| **City level is retained on capture** — capturing a level 5+ city completes the Metropolis task exactly as upgrading one does. | [Park of Fortune](https://polytopia.fandom.com/wiki/Park_of_Fortune) |
| **Buildings persist.** Implied by the documented scorched-earth counterplay: [Destroy](https://polytopia.fandom.com/wiki/Destroy) — *"Destroy can be used in 'scorched-earth' tactics before a city is captured by an enemy"* — which only makes sense if the attacker otherwise inherits everything. | Destroy, Strategies |
| **Capturing a connected city severs every connection routed through its territory**, removing that population from the capital and the far cities — often driving the capital negative. | [City Connection](https://polytopia.fandom.com/wiki/City_Connection) |
| **Roads under a neutral village are deleted when the village is captured.** | [Roads (Building)](https://polytopia.fandom.com/wiki/Roads_(Building)) |
| **A capture cooldown exists** — v2.16.3 bugfix list includes *"Capture city allowed during cooldown"*. Duration undocumented. | [v2.16.3](https://polytopia.io/bug-fixes-in-version-2-16-3/) |

**There is no resistance, unrest or rebellion mechanic.** No such system appears anywhere in the
wiki, in Game Modes, or in any patch notes. Captured cities are fully and immediately yours.

**Villages:** *"A village becomes a city when captured by a unit. Any unit can capture a village if
it started the turn on the village, and capturing a village ends the unit's turn."* Villages are
randomly distributed at map generation, **never spawn on map edges**, and **no more than one village
spawns in any 3×3 area**. Uncaptured villages give **no defence bonus**.

---

## 5. Buildings and improvements

Primary: [Buildings](https://polytopia.fandom.com/wiki/Buildings). Tech attributions use
[Technology](https://polytopia.fandom.com/wiki/Technology) as authoritative, because the Buildings
and Population pages carry a stale Port attribution.

### 5.1 Resource buildings

| Building | Cost ★ | Population | Tech (tier) | Terrain requirement | Per-city limit |
|---|---|---|---|---|---|
| **Lumber Hut** | **3** | **+1** | Forestry (T2) | Forest | none |
| **Sawmill** | **5** | **+1 per adjacent friendly Lumber Hut** | Mathematics (T3) | Field | **1 per city** |
| **Farm** | **5** | **+2** | Farming (T2) | Field **with Crop** | none |
| **Windmill** | **5** | **+1 per adjacent friendly Farm** | Construction (T3) | Field | **1 per city** |
| **Mine** | **5** | **+2** | Mining (T2) | Mountain **with Metal** | none |
| **Forge** | **5** | **+2 per adjacent friendly Mine** | Smithery (T3) | **Field or Forest** ⚠ | **1 per city** |
| **Port** | **7** | **+1** | **Fishing (T1)** ⚠ | Shallow water, or flooded Field | none |
| **Market** | **5** | 0 (income only) | Trade (T3) | Field | not documented |

⚠ **Forge terrain:** the [2025 Balance Pass](https://polytopia.io/2025-balance-pass/) explicitly
added *"Forges can be built on Forests."* The Buildings-page cell reading "Field, Forest adjacent to
a Lumber Hut" is a **copy-paste error from the Sawmill row** — a Forge has **no adjacency
prerequisite for placement**; adjacency only determines its output.

⚠ **Port tech:** [Buildings](https://polytopia.fandom.com/wiki/Buildings) and
[Population](https://polytopia.fandom.com/wiki/Population) both say Sailing — **both are stale**.
[Technology](https://polytopia.fandom.com/wiki/Technology),
[Fishing](https://polytopia.fandom.com/wiki/Fishing) and
[Port](https://polytopia.fandom.com/wiki/Port) all say **Fishing (T1)**, and the Patch of the Ocean
notes confirm it. **Use Fishing.**

### 5.2 Temples

All temples produce **+1 population** regardless of level.

| Temple | Cost ★ | Tech | Terrain |
|---|---|---|---|
| Temple | **20** | Free Spirit (T2) | Field |
| **Forest Temple** | **15** | Spiritualism (T3) | Forest |
| Mountain Temple | **20** | Meditation (T2) | Mountain |
| Water Temple | **20** | Aquatism (T3) | Shallow water, Ocean |
| Ice Temple | 20 | Polarism (Polaris — excluded) | Ice |

**Temple score growth.** Temples level up on **elapsed turns since construction**, max level 5.
Score per [Score](https://polytopia.fandom.com/wiki/Score): *"Temples: 100 points plus 100 points per
level above level 1 with a maximum of 500 points at level 5."*

```cpp
int templeScore(int level) { return 100 * level; }   // 100/200/300/400/500
```

⚠ **Temple scoring is a genuine three-way conflict.** Four sources give three different maxima:

| Source | Formula | Max |
|---|---|---|
| [Score](https://polytopia.fandom.com/wiki/Score) (edited 2026-01-15) | 100 + **100 per level** above 1 | **500** at level 5 |
| [2025 Balance Pass](https://polytopia.io/2025-balance-pass/) (official) | *"Temples give 100p per level of growth instead of 50p"* | implies **500** |
| [Temples](https://polytopia.fandom.com/wiki/Temples) table (edited 2025-12-30) | 100 / 150 / 200 / 250 / 300 | **300** |
| [polytopia.io strategy article](https://polytopia.io/news/strategy-tip-build-temples/) | *"A fully grown temple takes 12 turns to develop and is worth 400 points!"* | **400** |

**Recommendation: use 500** — the Score page is the most recently edited and is directly corroborated
by the official 2025 Balance Pass note. The Temples table pre-dates the balance pass; the strategy
article is undated and probably older still. But **this is a recommendation, not a settled fact** —
see [UNRESOLVED #11](#unresolved).

⚠ **The growth *rate* is also unresolved.** Path of the Ocean says *"Temples grow quicker, reduced
from 3 turns→2"*; the Temples page's own prose says *"100 points every two turns"* but its table
uses 3-turn buckets (0–2, 3–5, 6–8, 9–11, 12+); and
[Strategies/Perfection](https://polytopia.fandom.com/wiki/Strategies/Perfection) says *"Temples level
up every three turns, including the turn they were placed"*. **What all sources agree on: max level
is reached 12 turns after construction**, which is consistent with 3-turn buckets and inconsistent
with 2. Do not hardcode the interval without measuring it.

### 5.3 Other improvements

| Improvement | Cost ★ | Pop | Tech | Notes |
|---|---|---|---|---|
| **Road** | **3** | 0 | Roads (T2) | Field and Forest only. **Not a building** — it modifies the tile, and other buildings can sit on top. Buildable on **neutral** land. **Cannot be destroyed.** Not allowed on mountain / water / ice. (Was 2★ pre-Path-of-the-Ocean.) |
| **Bridge** | **5** | 0 | Roads (T2) | A water tile with **two land tiles orthogonally opposite** — **no diagonal bridges**. Spans exactly one shallow-water tile. Buildable on neutral water. **Can** be destroyed, unlike roads. Naval units pass under. Units on a bridge still get the Aquatism defence bonus. (7★ in Path of the Ocean, cut to 5★ in Patch of the Ocean.) |
| **Embassy** | ⚠ scaling | 0 | Diplomacy (T3) | Built in another tribe's **originally-owned** capital, at peace only. **+2 SPT to both parties, +4 each under an active Peace Treaty.** Reveals the 3×3 around the host capital. One per tribe. Destroyed if either party attacks the other. |
| **Monuments** | **0** | **+3** | task-gated | Field, shallow water or ice. See [§5.6](#56-monuments). |

⚠ **Embassy cost is UNRESOLVED.** Buildings says 4★; the Embassy page says 5★; and the 2025 Balance
Pass says *"Cost of new Embassies begins cheaper, but increases based on the number of Embassies you
have"* — a scaling cost with **no published formula**. Do not hardcode.

### 5.4 The exact adjacency rules

#### Neighbourhood: 8-neighbour (Moore), diagonals included

No wiki sentence states this outright, but the evidence is conclusive and mutually reinforcing:

1. The **Market is capped at level 8** ([Market](https://polytopia.fandom.com/wiki/Market)); a cap
   of exactly 8 only makes sense with 8 adjacent tiles.
2. **Sawmill sprites exist for levels 1–8** ([Sawmill](https://polytopia.fandom.com/wiki/Sawmill)
   gallery). Sawmill level = number of adjacent friendly Lumber Huts, so 8 is the achievable max.
3. **Forge sprites exist for levels 1–8** ([Forge](https://polytopia.fandom.com/wiki/Forge) gallery).
4. The Embassy reveal is defined as *"the surrounding eight tiles"* = a 3×3 area, establishing that
   "adjacent"/"surrounding" in this game means the 8-cell Moore neighbourhood.

**Use 8-neighbour.** The one documented exception is the Bridge: *"This has to be horizontal or
vertical (diagonal bridges cannot be built)"*.

#### Scope: your territory, NOT the same city

> *"If a building relies on adjacent tiles for its function, only the tiles within the player's
> territory are considered."* — [Buildings](https://polytopia.fandom.com/wiki/Buildings)

Combined with the per-building wording "**friendly** adjacent Lumber Hut / Farm / Mine":

- Source buildings **outside your borders do not count** (neutral or enemy tiles).
- Source buildings belonging to **a different city of yours DO count**, provided the tile is in your
  territory. The constraint is territory-scoped, never city-scoped.
- The one-per-city limit applies to the **consumer** building (Sawmill / Windmill / Forge), not to
  where its sources live.

#### Per-building yields

| Consumer | Counts | Yield per source | Cap |
|---|---|---|---|
| **Sawmill** | adjacent friendly **Lumber Huts** | **+1 population each** | none stated; physical max 8 |
| **Windmill** | adjacent friendly **Farms** | **+1 population each** | none stated; physical max 8 |
| **Forge** | adjacent friendly **Mines** | **+2 population each** | none stated; physical max 8 → up to +16 pop |
| **Market** | adjacent **Sawmills, Windmills, Forges** — 1 star per **LEVEL** of each | 1 SPT per level | **Market level capped at 8** → max 8 SPT |

**The Market sums LEVELS, not counts.** *"A Market provides 1 star per turn for each level of each
adjacent sawmill, windmill or forge, with a maximum level of 8."* A Market beside one level-8 Sawmill
yields 8 SPT; beside a level-3 Sawmill and a level-2 Windmill it yields 5 SPT.

⚠ The [Star](https://polytopia.fandom.com/wiki/Star) page still says *"2 per each level"* — **stale**.
Patch of the Ocean states the simplification: *"Simplified Markets: 1✯ for each level of each nearby
production building. Removed doubling for ports."* **Use 1.**

Two earlier Market designs exist in old references and must **not** be reintroduced: the first gave
1★ per adjacent building (count, not level); the second gave 2★ per *unique* adjacent building type,
doubled next to a Port.

#### Dynamic recomputation, and level 0

Levels are **recomputed live, not latched.** Destroying sources drops the consumer's level, and
**level 0 is a legal state** for Sawmill and Forge. Correspondingly: *"If a building is destroyed or
a city connection is lost, then the population it produces will also be lost."*

### 5.5 Removed buildings (do not implement)

- **Customs House** — 5★, +2 SPT per friendly adjacent Port, one per city, field only. **Replaced by
  the Market** in Path of the Ocean. It notably kept producing while its city was under siege.
- **Library** — 5★, −2 research cost per adjacent temple. Scrapped pre-release; replaced by Literacy.
- **Lantern** — never released.

### 5.6 Monuments

Every monument: **cost 0 stars**, **+3 population**, **worth 400 score points**, **buildable once per
game**, placeable on field, shallow water or ice.

| Monument | Task | Task condition | Task unlocked by |
|---|---|---|---|
| **Altar of Peace** | Pacifist | Make **no attacks for 5 consecutive turns**. Retaliation and Mind Bender conversion do **not** count as attacks. | Meditation |
| **Emperor's Tomb** | Wealth | Hold **100 stars** at one time | Trade |
| **Eye of God** | Explorer | Discover **all 4 Lighthouses** (was: explore every tile, pre-Path-of-the-Ocean) | finding a lighthouse |
| **Gate of Power** | Killer | **Kill 10 enemy units.** Conversion does not count; forced-spawn removals do not count. | killing an enemy unit |
| **Grand Bazaar** | Network | **Connect 5 cities to the capital** | connecting your first city |
| **Park of Fortune** | Metropolis | Have a **level 5+ city** — by upgrading **or by capturing** one | having a level 2 city |
| **Tower of Wisdom** | Genius | **Research all technologies** | Philosophy |

(Cymanti replaces Pacifist/Altar of Peace with a Converter task — excluded.)

---

## 6. Terrain and resources

### 6.1 Terrain types

Source: [Terrain](https://polytopia.fandom.com/wiki/Terrain)

| Terrain | Movement | Defence bonus | Resources it can host | Buildings |
|---|---|---|---|---|
| **Cloud** | impassable until explored | — | — | — |
| **Field** | free | — | Fruit, Crop | almost anything; Farm needs Crop |
| **Forest** | **stops movement** (unless creep/fly, or a road) | with **Archery** | Game (wild animal) | Lumber Hut, Forest Temple, Road, **Forge** (post-2025) |
| **Mountain** | impassable until **Climbing**; then **stops movement** | with **Climbing** | Metal | Mine (needs Metal), Mountain Temple. **Sight radius 2.** No roads. |
| **Water** (shallow) | naval only; shares ≥1 edge with land | with **Aquatism** | Fish, Starfish | Port, Bridge, Water Temple, Monuments |
| **Ocean** (deep) | naval **and** requires **Sailing**; shares no edge with land | with **Aquatism** | Fish, Starfish | Water Temple |
| **Ice** | Polaris (excluded) | — | Fish, Starfish | Ice Bank, Ice Temple, Outpost |
| **Flooded** | Aquarion (excluded) — land **and** naval units may enter | — | retains underlying | underlying + Atoll + Port |

Freezing and flooding **preserve resources, buildings and structures** on the tile.

### 6.2 Resource harvest table

| Resource | Terrain | Tech to harvest | Cost ★ | Yield |
|---|---|---|---|---|
| **Fruit** | Field | **Organization** (T1) | **2** | **+1 population** |
| **Crop** | Field | revealed by Organization; harvested by building a **Farm** (Farming T2) | **5** | **+2 population** |
| **Game** (wild animal) | Forest | **Hunting** (T1) | **2** | **+1 population** |
| **Fish** | Water / Ocean | **Fishing** (T1) | **2** | **+1 population** (removes the fish) |
| **Starfish** | Water / Ocean | **Navigation** (T3) | **0** | **+8 stars** (was 10; cut to 8 in Patch of the Ocean). Any naval unit landing on it collects next turn, **even in neutral or enemy territory**; consumes the unit's turn. |
| **Metal** | Mountain | revealed by Climbing; harvested by building a **Mine** (Mining T2) | **5** | **+2 population** |
| **Ruin** | any terrain **except shallow water** | none | **0** | random — see [§8](#8-ruins) |
| **Village** | land | none | **0** | becomes a level-1 city |
| **Lighthouse** | the 4 map corners | none | **0** | **+1 population to your capital** on discovery |
| ~~**Whale**~~ | ~~Ocean~~ | ~~Whaling~~ | — | **REMOVED** — replaced by Starfish |

**Spawn rules** ([Map Generation](https://polytopia.fandom.com/wiki/Map_Generation)):
*"All resources spawn only within two tiles of cities or villages."* Fish spawn at a 50% base rate
among shallow-water tiles. Starfish spawn on any water tile at roughly 1 per 25 water tiles, never
adjacent to another starfish, lighthouse or city. Villages never spawn on map edges and no more than
one spawns per 3×3 area. Ruins cannot spawn adjacent to a capital or to another ruin.

### 6.3 Terrain manipulation

| Ability | Tech | Cost ★ | Effect |
|---|---|---|---|
| **Clear Forest** | Forestry (T2) | **0 — gives you +1 star** | Forest → Field |
| **Burn Forest** | Construction (T3) | **3** ⚠ | Forest → **Field with Crop** |
| **Grow Forest** | Spiritualism (T3) | **5** | Field → Forest |
| **Destroy** | Chivalry (T3) | **0** | Removes a building or ruin, and the population/points it gave. **Monuments cannot be rebuilt** once destroyed. |

All three terrain abilities require the tile to be **in your territory and free of buildings**.
Clear Forest and Burn Forest **forfeit any wild animal** on the tile.

⚠ **Burn Forest cost history is a minefield** and the wiki page is internally self-contradictory
(infobox 3, body 2, trivia 5). Actual history: 5 → 2 (v2.0.58) → 5 (Patch of the Ocean) → **3**
(2025 Balance Pass, *"Cost of 'Burn Forest' down to 3"*). **Use 3.**

### 6.4 Population efficiency table

Useful for AI evaluation functions. Source:
[Population](https://polytopia.fandom.com/wiki/Population).

| Method | Pop | Stars | Stars per pop |
|---|---|---|---|
| Harvest fruit / Hunt animal / Fish | 1 | 2 | **2.0** |
| Farm (on natural crop) | 2 | 5 | **2.5** |
| Mine | 2 | 5 | **2.5** |
| Forge (per adjacent mine) | 2 | 5 | 2.5 / #mines |
| Lumber Hut | 1 | 3 | 3.0 |
| Burn Forest + Farm | 2 | 8 | 4.0 |
| Sawmill (per adjacent hut) | 1 | 5 | 5 / #huts |
| Windmill (per adjacent farm) | 1 | 5 | 5 / #farms |
| Port | 1 | 7 | 7.0 |
| Grow Forest + Lumber Hut | 1 | 8 | 8.0 |
| Grow Forest + Burn Forest + Farm | 2 | 13 | 6.5 |
| Forest Temple | 1 | 15 | 15.0 |
| Temple / Water / Mountain Temple | 1 | 20 | 20.0 |
| Monument | 3 | 0 | ∞ |
| Population city upgrade / ruin reward | 3 | 0 | ∞ |
| Lighthouse discovery | 1 | 0 | ∞ |
| City connection | +1 to capital **and** +1 to the city | varies | varies |

---

## 7. Economy

### 7.1 Per-turn star income

```
totalIncome = Σ cityIncome(c) over all cities        // see §4.4
            + Σ marketIncome                          // 1 SPT per LEVEL of each adjacent
                                                      //   Sawmill/Windmill/Forge, market level cap 8
            + Σ embassyIncome                         // +2 SPT each, +4 under an active Peace Treaty
```

### 7.2 One-time star sources

Source: [Star](https://polytopia.fandom.com/wiki/Star)

| Source | Stars |
|---|---|
| Clear Forest | **1** |
| Disband a unit | **floor(unit cost / 2)** |
| Meeting an enemy player | 1 + 2 per 1000 points of their score (rounded up to the next 1000); **max 11** |
| Resources city upgrade (level 3 reward) | **5** |
| Starfish Harvesting | **8** |
| Resources ruin reward | **10** |
| Cloak infiltration | stars equal to the target city's income |

### 7.3 Star sinks

Unit training, technology research (`tier × numCities + 4`, or `ceil(cost × 2/3)` with Literacy),
buildings and improvements, resource harvesting, terrain manipulation, and naval upgrades.

**Stars do not decay and there is no cap.** Holding 100 stars at once completes the Wealth task.

---

## 8. Ruins

Source: [Ruin](https://polytopia.fandom.com/wiki/Ruin) (Wayback snapshot 2026-02-17).

### 8.1 Mechanics

- *"Examining a ruin requires a unit to start the turn on the ruin and consumes the unit's actions
  for that turn (like capturing a village)."*
- *"Ruins can spawn on all types of terrain **except shallow water**. However, ruins cannot spawn
  next to a capital city or another ruin."*
- *"Tiles with ruins cannot be developed until the ruin is examined."* Resources **can** spawn on a
  ruin tile but are invisible and unusable until the ruin is cleared.
- *"If the ruin provides a unit as a reward, the examining unit is pushed in a forced unit spawn."*
  (See the push-order rules in [§9.4](#94-forced-unit-spawns).)

### 8.2 Reward table

> *"The rewards for ruins are chosen randomly, **with no special weighting accorded to any one
> option**."*

That is the only statement of the distribution the wiki makes: **uniform over the currently-eligible
options**. There is no published probability table. Note that the eligible set is state-dependent, so
the effective distribution changes with game state.

| Name | Reward | Condition |
|---|---|---|
| **Resources** | **10 stars** | always available |
| **Scrolls of Wisdom** | a free technology | tech tree not completed. *"The technology is chosen at random from all possible researchable techs (i.e. not a tech that is behind another unresearched tech)"* |
| **Population** | **+3 population to the capital** | player owns a capital. If they no longer own their original capital but hold captured ones, it goes to the **highest-level capital** |
| **Explorer** | a free Explorer | any tile within the **5×5 area centred on the ruin** is unexplored. Consequently **never obtainable from a ruin on a mountain**, because moving onto the mountain (sight radius 2) already reveals that whole 5×5 |
| **New Friends** | a **veteran Swordsman** | ruin is **NOT** on shallow water or ocean |
| **Rammer** | a **veteran Rammer carrying a Warrior** | ruin **is** on shallow water or ocean |
| ~~Lost City~~ | level 3 city + city wall + 4 adjacent shallow water tiles | **Aquarion only** (excluded) |

Historical, so you do not reintroduce it: before update 2.0.58 a land ruin granted a **Giant**, not a
veteran Swordsman — a Giant could be had as early as turn 5. The water reward was a Warrior
Battleship, changed to a Warrior Bomber in Path of the Ocean, then to a veteran Rammer in Patch of
the Ocean. Also, ruins used to be allowed next to villages.

⚠ The v2.16.3 patch notes include the line *"Changed how ruin rewards are calculated"*, with no
detail. The "no special weighting" wording may therefore be stale — see
[UNRESOLVED #17](#unresolved).

---

## 9. Movement and vision

Source: [Movement](https://polytopia.fandom.com/wiki/Movement) (Wayback snapshot 2026-01-28).

### 9.1 The movement model

> *"If not impacted by any other factors, a unit can move to tiles whose distance to the unit's
> starting tile is not more than its movement stat, as measured using **Chebyshev distance**. […]
> A unit may only move once per turn (besides the use of the dash and escape skills), and **each tile
> can only be occupied by one unit at a time**."*
>
> *"Moving from one tile to an adjacent tile incurs a cost of 1, but moving between tiles with roads
> has a cost of 0.5."*

So this is a **point-budget pathfinding model over an 8-connected grid**, not a simple radius test.

| Terrain / situation | Cost |
|---|---|
| Ordinary tile → ordinary adjacent tile | **1** |
| Between two road tiles (or road ↔ city/village) | **0.5** |
| Between two bridge/road tiles | **0.5** |
| Forest, Mountain | 1 to enter, **and movement ends** |
| Cloud (unexplored) | impassable |
| Water / Ocean | land units cannot enter (except via a Port or a bridge) |

**Rough terrain stops movement, it does not cost extra:**

> *"Rough terrain, including mountains and forests (without roads) block movement. Units cannot move
> **through** them (they can move **onto** them, but no further in the same turn). Also, units cannot
> move into clouds (the fog of war)."*

**Movement-point rounding:**

> *"When calculating movement cost of a tile, the game **rounds up** — meaning even if you have 0.5
> movement points remaining, you can still move onto a tile costing 1 or more movement points. The
> place the unit ends up is irrelevant — you don't need a road at the final tile and movement is not
> blocked if final tile is an obstacle."*

### 9.2 Roads and bridges

> *"Movement between two tiles with Roads (or a Road and a city) requires only 0.5 moves, ignoring
> the movement penalty from Forests, but **not** Zone of Control (it is impossible to place Roads on
> Mountains). This only applies to Roads in **friendly territory or in neutral territory, but not
> when moving onto Roads in enemy territory**. Bridges follow these same rules, but with the added
> benefit of allowing land units to cross one tile of water."*

- **City and village tiles function as road tiles.**
- Both endpoints need a road: *"you need to place a road where your troop is standing and then on
  the following tile."*
- A road on a forest tile **neutralises the forest movement restriction**. Roads cannot be placed on
  mountains, so mountains always stop movement.

### 9.3 Zone of Control

> *"**All units exert a passive Zone of Control onto enemy units.** This takes priority over movement
> bonuses conferred by Roads or Polarism. **A unit that moves to a tile adjacent to an enemy unit
> cannot move any further**, even if the unit had excess movement points."*

Additional clauses, all from the same page:

- *"This … does not prevent units with the Dash skill from attacking after moving, and the Zone of
  Control goes away when the enemy unit is destroyed."*
- *"A unit that starts its turn within Zone of Control may be able to move out and then back into
  it, occasionally leading to seemingly impossible but valid movement destinations."*
- **ZoC is projected by enemy units hidden in fog too** — the page's own example caption reads
  *"An enemy hidden by the fog is blocking movement."* This is important and easy to miss: ZoC is
  computed on true state, not on observed state.

**Creep and ZoC — the wiki was edited mid-research and now says Creep does NOT bypass ZoC.**
This is worth spelling out because it is a live, dated reversal:

- The Wayback snapshot of **2026-01-28** has the Movement page saying Creep bypasses ZoC in three
  places, including a worked example (*"This Cloak can travel freely past the defending enemy Warrior
  because of its Creep ability, ignoring the warrior's zone of control"*) and, in its Unit Skills
  section, *"Movement restrictions from zone of control are **also eliminated**."*
- A **live pull of 2026-08-03** (page last edited **2026-04-05**) has that same Unit Skills section
  reading *"movement restrictions from zone of control are **not eliminated**."*
- [Unit Skills](https://polytopia.fandom.com/wiki/Unit_Skills) agrees with the newer text:
  *"Note that zone of control cannot be negated."*

So the page now **contradicts itself internally** (its Zone of Control section still says "completely
bypass"), but 2 of 3 statements — including the newest edit and the dedicated skills page — say
**Creep does NOT bypass ZoC**. That is the recommended implementation. See
[UNRESOLVED #5](#unresolved).

**Two skills unambiguously do bypass ZoC**, and they differ:

- **`Sneak`** — *"bypass barriers presented by tiles adjacent to enemies at no cost, **but they can't
  move through enemies**"*
- **`Hide`** — *"bypass barriers presented by tiles adjacent to enemies **as well as move through
  enemies**"*

The Cloak has both `Hide` and `Sneak`, so its ZoC bypass is guaranteed regardless of how Creep is
resolved.

### 9.4 Forced unit spawns

> *"A forced unit spawn occurs when a unit is spawned in a tile that already has a unit on it. The
> unit that is already on the tile is pushed in a specific direction. Forced unit spawns can happen
> when a **super unit spawns in a city**, a **Ruin spawns a unit**, or ∑∫ỹriȱŋ spawns a Polytaur."*

Push order, verbatim:

1. **Friendly units that previously moved** are pushed **in the same direction** as their movement.
2. **Enemy units that previously moved** are pushed **in the opposite direction** of their movement.
3. **Ranged units** are pushed in the direction of their last move **or last attack**.
4. **Units that had not previously moved** are pushed **toward the centre of the map**.
5. If the city is at the exact centre of the map, the unit is pushed **south**.
6. *"If the tile where the unit is supposed to go is occupied or impassable, it will try
   **counterclockwise and then clockwise** one tile at a time until it finds a free tile, or if there
   is no free tile, **it will be removed from the game entirely**."*
7. A removal this way **does not count as a kill** for the Gate of Power, nor **as an attack** for
   the Altar of Peace.

This is the mechanism behind the standard "spawn a super unit to push the besieger out of my city"
play.

### 9.5 Movement-affecting skills

| Skill | Effect |
|---|---|
| **Carry** (Raft, Scout, Rammer, Bomber, Juggernaut, Dinghy, Pirate) | Carries a unit and travels on water. Moving to a land tile adjacent to water converts it back into the carried unit and **ends the turn**. |
| **Creep** (Cloak; plus Cymanti units) | Ignores movement penalties from mountains and forests, but **disables road bonuses**. ZoC interaction disputed — [UNRESOLVED #5](#unresolved). |
| **Sneak** (Cloak; plus Hexapod) | *"allows a unit to bypass barriers presented by tiles adjacent to enemies at no cost"* — i.e. a second, independent ZoC bypass. |
| **Fly**, **Navigate**, **Skate**, **Boost** | Special tribes only (Elyrion / Cymanti / Polaris) — excluded. |

### 9.6 Vision and fog of war

- **Base sight radius is 1 tile** (a 3×3 area centred on the unit).
- **A unit on a mountain has sight radius 2** (a 5×5 area): *"Mountains provide extra sight (a two
  tile radius instead of the normal one)"* ([Combat](https://polytopia.fandom.com/wiki/Combat));
  *"All units on this terrain have a sight radius of two tiles"*
  ([Terrain](https://polytopia.fandom.com/wiki/Terrain)). **This is a property of the tile, not of
  the Climbing tech.**
- **The `Scout` skill gives sight radius 2** in every direction. Regular-tribe holders: Scout (naval),
  Cloak, Dinghy.
- **The Explorer** is a level-2 city reward: an auto-moving figure that makes **12 moves** (reduced
  from 15 in v2.16.3) clearing fog along its path. It cannot enter mountain / shallow water / ocean
  without Climbing / Fishing / Sailing, and it can move over other units including enemies.
- **Starting vision is 25 tiles** (5×5 around the capital) — derived from the turn-0 score arithmetic
  in [§10.2](#102-turn-0-score-cross-check) and consistent across all 16 tribes.
- **Explored tiles stay revealed** (terrain is remembered), but unit positions are only known where
  you currently have vision — this is why ZoC from a fogged enemy can block you unexpectedly.
- **Diplomacy grants Capital Vision**, a global reveal of all discovered tribes' capital tiles.
- **Embassies reveal the 3×3 around the host capital.**
- **Lighthouses** sit in the 4 map corners and must be revealed by exploration; discovering one gives
  **+1 population to your capital** and progresses the Explorer task.

---

## 10. Tribes

### 10.1 The 12 regular tribes

All 12 **share the same technology tree, units and buildings**. Confirmed three ways:

- [Tribes](https://polytopia.fandom.com/wiki/Tribes): *"**Regular tribes**, formerly known as human
  tribes, **all share the same technology tree, units, and buildings.** **Special tribes** each have
  a unique technology tree, units, and buildings."*
- [Units](https://polytopia.fandom.com/wiki/Units): *"**The super unit for all regular tribes is the
  Giant.** Special tribes have unique super units."*
- [List of Units](https://polytopia.fandom.com/wiki/List_of_Units) indexes per-tribe unique unit
  rosters only under the four special tribes.

**No regular tribe has a unique unit.** The only per-tribe variation among the 12 is cosmetic: unit
colour, helmet, and the animal that mounted units ride.

**Engine implication:** one shared unit table and one shared tech tree for all 12. Tribe identity
affects only map generation, starting technology, starting unit, starting stars, and — for
Luxidoor — capital level.

| Tribe | Starting tech | Tier | Starting unit | Starting ★ |
|---|---|---|---|---|
| Xin-xi | Climbing | T1 | Warrior | **7** |
| Imperius | Organization | T1 | Warrior | **5** |
| Bardur | Hunting | T1 | Warrior | **5** |
| Oumaji | Riding | T1 | Rider | **6** |
| Kickoo | Fishing | T1 | Warrior | **5** |
| Hoodrick | Archery | T2 | Archer | **7** |
| **Luxidoor** | **none** | — | Warrior | **2** |
| Vengir | Smithery | T3 | Swordsman | **5** |
| Zebasi | Farming | T2 | Warrior | **5** |
| **Ai-Mo** | **Philosophy** ⚠ | **T3** | **Mind Bender** | **5** |
| Quetzali | Strategy | T2 | Defender | **7** |
| Yădakk | Roads | T2 | Warrior | **7** |

Starting stars are **directly from the official dev blog**, not just the wiki —
[polytopia.io/2025-balance-pass](https://polytopia.io/2025-balance-pass/) states verbatim: *"Xin-xi
begins with 7 Stars / Oumaji begins with 6 Stars (that's not a typo, they begin with 6) / Hoodrick
begins with 7 Stars / Quetzali begins with 7 Stars / Yădakk begins with 7 Stars / Luxidoor begins
with 2 Stars in addition to their upgraded Capital / All other tribes begin with 5 Stars."*
**Pre-2025 sources all say a flat 5 and are wrong.**

⚠ **Ai-Mo is the one entry the wiki disagrees with itself on.** The
[Technology](https://polytopia.fandom.com/wiki/Technology) and
[Meditation](https://polytopia.fandom.com/wiki/Meditation) pages still say Meditation. The
[Ai-Mo](https://polytopia.fandom.com/wiki/Ai-Mo) and [Tribes](https://polytopia.fandom.com/wiki/Tribes)
pages say **Philosophy + Mind Bender**, and the official patch note is unambiguous: *"Ai-Mo now
starts with Philosophy instead of Meditation, and a Mind Bender to go with it"*. The 730 turn-0 score
corroborates. **Use Philosophy.**

**Engine consequence: Ai-Mo starts with Literacy active from turn 0**, so all its research uses
`ceil(cost × 2/3)` from the first turn.

### 10.2 Turn-0 score cross-check

Every tribe's published turn-0 score is reproduced exactly by:

```
405                                   // 100 city + 180 (9 territory tiles @20) + 125 (25 explored @5)
  + 5   * (starting unit cost in stars)
  + 100 * (starting tech tier)
  + 50  * (capital level - 1)
```

Verified against all 16 published values ([Score](https://polytopia.fandom.com/wiki/Score)):
Aquarion 415 · Xin-xi/Imperius/Bardur/Kickoo/Luxidoor/Elyrion 515 · Oumaji 520 · Polaris 530 ·
Zebasi/Yădakk 615 · Quetzali/Hoodrick 620 · Cymanti 630 · Vengir/Ai-Mo 730.

This is a **useful engine self-test**: if your turn-0 scores do not match these numbers exactly, your
starting territory, starting vision, unit costs, tech tiers or capital levels are wrong.

Note: *"If a tribe spawns in a corner, due to the update where lighthouses have to be revealed by
exploration, that corner tile will be obscured by the clouds. This will result in 5 points less than
usual."* So a 510 score means a corner spawn of a 515 tribe.

### 10.3 Luxidoor — the special case

> *"Luxidoor is the only regular tribe that does not have any starting technology; instead, they
> start with a **level 3 capital city** (without a Workshop or Explorer or Wall, and so giving
> **+4 spt**). A Park graphic can also be seen, however it is **purely cosmetic and does not give any
> points**."* — [Luxidoor](https://polytopia.fandom.com/wiki/Luxidoor)

**The level-3 figure was genuinely disputed and is resolved arithmetically, not by trusting the
wiki.** [Update History](https://polytopia.fandom.com/wiki/Update_History) records that update 2.0.49
(Balance Pass 1, 2021-04-21) *reduced* Luxidoor's capital from level 3 + city wall to **level 2**, and
no patch note reverses it. The wiki then flip-flopped repeatedly through 2026 (revision history shows
level 2 → 3 → 2 → 3 across five editors between February and July 2026, one edit justified only as
*"based on what I am looking at in game in my last 2 games"*).

The turn-0 score settles it. Luxidoor's published turn-0 score is **515**. With a Warrior (10 pts)
and no tech (0): `405 + 10 + 50 × (L − 1) = 515` ⟹ **L = 3**. A level-2 capital would score 465.
**Confirmed level 3**; the Update History entry is stale.

Engine consequences:

- **4 SPT at start** (3 for city level + 1 human-capital bonus).
- **Unit capacity 4** (level + 1).
- Already has 2 population banked, so fewer resources are needed to reach level 5 and a Giant.
- **Luxidoor skips the level-2 and level-3 upgrade reward choices** — it gets no Workshop, no
  Explorer, and no Wall.

### 10.4 Map generation — terrain and resource biases

Source: [Map Generation](https://polytopia.fandom.com/wiki/Map_Generation).

**Base rates** (these are Luxidoor's — Luxidoor *is* the baseline). "Inner city" = tiles adjacent to
a city or village; "outer city" = everything else. Each column sums to 100%.

| | Inner city | Outer city |
|---|---|---|
| **Field (total)** | **48%** | **48%** |
| — Fruit | 18% | 6% |
| — Crop | 18% | 6% |
| — Empty field | 12% | 36% |
| **Forest (total)** | **38%** | **38%** |
| — Game (animal) | 19% | 6% |
| — Empty forest | 19% | 32% |
| **Mountain (total)** | **14%** | **14%** |
| — Metal | 11% | 3% |
| — Empty mountain | 3% | 11% |

- **Fish: 50% of shallow-water tiles.** Fish can also spawn in ocean inside borders.
- **Starfish** spawn on any water tile at roughly **1 per 25 water tiles**, never adjacent to another
  starfish, lighthouse or city.
- **All resources spawn only within two tiles of a city or village.**
- Since update 2.0.58 terrain is generated to **match** these percentages (a deterministic quota, not
  a per-tile roll): *"Luxidoor will always have 48% of their biome (or as close as possible) as field
  tiles."*

**Per-tribe multipliers.** These multiply the base rates. Anything not listed is 1.0.

| Tribe | Mountain | Forest | Fruit | Crop | Animal | Metal | Fish | Water |
|---|---|---|---|---|---|---|---|---|
| Xin-xi | **1.5** | 1.0 | 1.0 | 1.0 | 1.0 | **1.5** | 1.0 | 1.0 |
| Imperius | 1.0 | 1.0 | **2.0** | 1.0 | **0.5** | 1.0 | 1.0 | 1.0 |
| Bardur | 1.0 | **0.8** | 1.0 | **0.0** | 1.0 | 1.0 | 1.0 | 1.0 |
| Oumaji | **0.5** | **0.2** | 1.0 | 1.0 | **0.2** | 1.0 | 1.0 | 0.5 ¹ |
| Kickoo | **0.5** | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | **1.5** | 2.0 ² |
| Hoodrick | **0.5** | **1.5** | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| Luxidoor | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| Vengir | 1.0 | 1.0 | **0.1** | 1.0 | **0.1** | **2.0** | **0.1** | 1.0 |
| Zebasi | **0.5** | **0.5** | **0.5** | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| Ai-Mo | **1.5** | 1.0 | 1.0 | **0.1** | 1.0 | 1.0 | 1.0 | 1.0 |
| Quetzali | 1.0 | 1.0 | **2.0** | **0.1** | 1.0 | 1.0 | 1.0 | 1.0 |
| Yădakk | **0.5** | **0.5** | **1.5** | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |

¹ ² **Both water multipliers are dead values.** The wiki footnotes them explicitly: Oumaji's —
*"Reductions to the amount of water are found in the source code but are not translated into the
world generation and do not impact water rates."* Kickoo's — *"On continents, Kickoo and Aquarion
should have 40% and 30%, respectively, of their tiles replaced with water after the initial map
generation, but this effect is bugged and does not currently apply to map generation."*
**Implement as no-ops** unless you deliberately want to reproduce the intended-but-broken behaviour.

This table was cross-verified against the wiki's own `Resource rates by tribe.png` spreadsheet image
(revision 2025-06-08) — all 12 regular tribes match exactly.

**Multiplier application order** — load-bearing, because these do **not** compose naively:

1. Apply the **mountain** modifier first. If it differs from base, it rescales **both forest and
   field** proportionally.
2. Then apply the **forest** modifier. Because it runs after step 1 it no longer affects mountain —
   only fields.
3. **The field rate is never modified directly** by any tribe; it is derived as
   `100% − mountain% − forest%`.

Resource sub-rates (fruit / crop / animal / metal / fish) are then applied within their terrain type,
with the "empty" variant absorbing the remainder.

Also documented: *"if forced in a 121-tile map, surrounding tribes can affect the spawn rates in the
capital city (e.g. a naturally spawning Bardur crop)."*

**Guaranteed capital starting resource:** Xin-xi → Metal; Imperius → Fruit; Bardur → Animals;
Oumaji → Fruit; Kickoo → Fish; Hoodrick → Animals; Luxidoor → Animals; Vengir → Animals;
Zebasi → Crop; Ai-Mo → Animals; Quetzali → Fruit; Yădakk → Fruit.
[Strategies/Tribes](https://polytopia.fandom.com/wiki/Strategies/Tribes) confirms that the four
"turn-0" tribes (Bardur, Imperius, Kickoo, Zebasi) *always* spawn with enough resources to upgrade
the capital on turn 0, force-spawning them if world generation did not provide.

⚠ **Ignore the [Bardur](https://polytopia.fandom.com/wiki/Bardur) page's prose** claiming "Bardur has
1.5x fruit, and no crop" — that is wrong. Bardur's own icon table on the same page, the Map
Generation table, and the spreadsheet image all agree on **0.8× forest, 0.0× crop, 1.0× fruit**.

### 10.5 Map generation parameters

Source: [Map Generation](https://polytopia.fandom.com/wiki/Map_Generation).

**Map sizes:** Tiny **121** (11×11) · Small **196** (14×14) · Normal **256** (16×16) ·
Large **324** (18×18) · Huge **400** (20×20) · Massive **900** (30×30).

**Size by mode:** Perfection is always **Normal**. Domination: 1 opponent → Tiny; 2 → 196; 3 →
Normal; 4+ → Large. Multiplayer and Creative: the host chooses. **Tiny supports a maximum of 9
players; every other size supports 16.**

**Water proportion by map type:** Drylands **0–10%** · Lakes **25–30%** · Continents **40–70%** ·
Pangea **40–60%** · Archipelago **60–80%** · Waterworld **90–100%**.

**Generation order:** capitals → villages → terrain → resources → ruins and starfish.

**Capital placement quadrants** (Drylands, Lakes, Archipelago, Waterworld only): 1–4 players →
**4 domains**; 5–9 → **9 domains**; 10–16 → **16 domains**. Continents and Pangea do not use
quadrants — villages are placed first and some are converted to capitals, preferring maximum mutual
distance and coastal tiles.

**Village generation.** Pre-terrain village count:
`(floor(mapWidth / 3))² − (capitals + suburbs)) × densityCoefficient`, where the density coefficient
is **0.3** (Lakes, Archipelago) or **0.1** (Waterworld). Pre-terrain villages must be **2 tiles from
other villages** and **at least 1 tile from the map edge**. Post-terrain villages must be at least
**2 tiles from the map edge and 2 tiles from other villages**; all Drylands villages are
post-terrain. **Suburbs** (Lakes and Archipelago only) place up to **2 per capital** before terrain.
**Tiny-island villages** (Continents and Pangea): Tiny 0 · Small 1 · Normal 2 · Large 3 · Huge 4 ·
Massive 9.

**Continents:** land masses have **30–200 tiles** and must be **at least 1 tile apart**; the count
depends on player count, wetness and map size.

**Ruin counts by map size:** Tiny **4** · Small **5** · Normal **7** · Large **9** · Huge **11** ·
Massive **23**. Ruins are placed **after** villages, resources and lighthouses. On Lakes maps **at
most one third** of ruins may spawn on water.

⚠ The Ruin and Map Generation pages disagree on ruin adjacency: [Ruin](https://polytopia.fandom.com/wiki/Ruin)
says ruins cannot spawn next to *a capital city* or another ruin;
[Map Generation](https://polytopia.fandom.com/wiki/Map_Generation) says next to another ruin *or a
village*. The Ruin page itself notes the village rule applied only **before** update 2.0.58, so Map
Generation is probably the stale one. [UNRESOLVED #29](#unresolved).

### 10.6 Starting conditions common to all tribes

- **Capital:** exactly one city, **level 1** — *"Each player starts with one level 1 city, their
  capital"* — **except Luxidoor at level 3**.
- **Capital buildings: none.** Buildings arrive only via level-up rewards.
- **Starting units: exactly one**, in the capital — *"Every tribe starts the game with one unit in
  its capital. Most tribes start with a Warrior. Some tribes start with the unit associated with
  their starting technology."*
- **Starting stars:** 5 for most; 7 for Xin-xi / Hoodrick / Quetzali / Yădakk; 6 for Oumaji; 2 for
  Luxidoor.
- **Starting income:** 2 SPT (1 level + 1 human-capital bonus); Luxidoor 4 SPT.
- **Starting territory:** 9 tiles (3×3). **Starting vision:** 25 tiles (5×5).
- **Unit capacity:** `level + 1` — so 2, or 4 for Luxidoor.
- **Starting technology:** one, free, at game start, for every tribe except Luxidoor.

### 10.7 The 4 excluded special tribes — what you are deferring

| Tribe | Structural deltas an engine must special-case |
|---|---|
| **Aquarion** | **All land units are amphibious mermaid variants** — every land unit moves on water without a boat; there is no plain Warrior. **New tile state "Flooded"**, passable by both land and naval units, spreadable and reversible at runtime. Buildings placeable on **water/ocean**. **No roads and no Ports** — Roads is replaced by *Waterways*, city connection uses the **Atoll**. A deep-ocean ruin can yield a **Lost City** (level 3 city + wall). Renamed techs: Free Diving, Aquaculture, Marine Life, Waterways, Spearing. |
| **∑∫ỹriȱŋ (Elyrion)** | **Lightest special.** Same naval system, same roads. Hunting → **Forest Magic**: cannot harvest animals for population; instead **enchants** them into free **Polytaur** units. **Cannot Clear Forest or Burn Forest at all.** Unique building **Sanctuary**. Super unit is a **3-stage growth chain** (Dragon Egg → Baby Dragon → Fire Dragon) rather than an instant Giant. Sees ruins through clouds. |
| **Polaris** | **Has no terrain of its own at world generation** — the *other* tribes determine the land and Polaris converts it at runtime. Core mechanic **Freeze Area** converts adjacent water to ice, converts adjacent land to Polaris land, and applies a **frozen status effect** to units. Any non-freezer unit can **Break Ice**. Captured Polaris cities convert terrain to the capturing tribe. Unique building **Ice Bank** gives income based on **world-wide frozen tile count** — a global-state income source, which is a real architectural wrinkle. |
| **Cymanti** | **Most divergent.** 15 unique units, 8 unique unit skills, 4 unique buildings, ~17 modified techs. **Cannot spawn crop at all** — replaced by a **spore** rate. **No roads** (Mycelium instead) and **no Ports or Rafts**. Entirely different naval layer. **Poison** status (−50% defence + slows) and **Explode** self-destruct on multiple units. Reworked 2025-10-29, so much of the wiki still describes the pre-rework version. |

### 10.8 Skins are cosmetic only

[Tribes § Tribe Skins](https://polytopia.fandom.com/wiki/Tribes) describes skins as *"a **cosmetic**
skin, which can be purchased through microtransactions. These skins change the tribe's music, units,
cities, and sometimes terrain."* Every documented skin is a pure art/audio/naming swap with no stat
or rule change.

The one trap: **special-tribe skins rename mechanics**, which reads like a rule change but is not.
[Polaris § Solaris Skin](https://polytopia.fandom.com/wiki/Polaris) lists ~25 renames — ice → "Lava",
`Break Ice` → `Break Lava`, Frostwork → Heatwork, Ice Bank → Lava Bank, "Frozen" → "Petrified". All
functionally identical. **Solaris is not a 17th tribe**; it is a Polaris skin. There are 16 tribes,
12 regular.

Two skins (**Forgotten** for Aquarion, **New Dawn** for Cymanti) change the tribe **colour**, which
affects player-colour rendering only.

**Engine conclusion: model skins as a pure presentation-layer lookup. Zero rules impact.**

---

## 11. Game modes, victory conditions and scoring

Source: [Game Modes](https://polytopia.fandom.com/wiki/Game_Modes) (Wayback snapshot 2026-03-05).

There are **6 game modes**: 4 single-player (Perfection, Domination, Creative, Boot Camp) and 2
multiplayer (Glory, Might).

### 11.1 Victory conditions and limits

| Mode | Players | Objective | Turn limit |
|---|---|---|---|
| **Perfection** | single | *"obtain the highest score possible in **30 turns**"* | **30** |
| **Domination** | single | *"defeat all opponents"* — a tribe is destroyed when you capture its **last city**, not specifically its capital | **none** |
| **Glory** | multi | *"be the first player to achieve a score of **10,000 points**"* | none |
| **Might** | multi | *"capture **every tribe's capital**. Not all cities have to be captured to win (only the capitals do)"* | none |
| **Creative** | single | Customise opponents, map type and map size (including **Huge = 400 tiles** and **Massive = 900 tiles**). Play by Perfection or Domination rules, or **Infinity** with no win condition | per chosen ruleset |
| **Boot Camp** | single | Scripted tutorial; static world generation, 14×14 Lakes map, easy difficulty, sole opponent Xin-xi, player is Imperius with 6 stars and no Warrior | — |

**Tie-breaking in Glory:** *"If multiple players get 10,000 points in the same turn, the player with
the most points at the end of the turn wins, even if that player was not the first to get 10,000
points."*

**Elimination:** *"A player is eliminated when they have lost all of their cities."*
([City](https://polytopia.fandom.com/wiki/City))

**Might vs Score.** Note the wiki does not name a "Might vs Score" mode. What exists is the
**Might/Glory pairing** (capital capture vs. 10,000 points) as the two multiplayer modes, and the
Might mode's distinctive UI: *"in the Game Stats menu, the number of capitals each tribe controls is
shown instead of the number of cities each tribe has, and tribes are first ordered by the number of
capitals they control instead of the number of points they have."*

### 11.2 The score formula — complete per-item table

Source: [Score](https://polytopia.fandom.com/wiki/Score) (Wayback snapshot 2026-03-08).

Points are earned in four categories, and **lost when a city is captured by an enemy, a building is
destroyed, or a unit is killed or disbanded.**

| Item | Points |
|---|---|
| **Units** | **5 per star of the unit's cost** |
| **Super units** | **50 each** (theoretical cost 10 stars) |
| **Territory** | **20 per tile** in the player's territory |
| **Exploration** | **5 per explored tile** |
| **Cities** | **100, plus 50 per level above level 1** |
| — decomposed | each population gained = **5**; plus a flat level bonus of `50 − 5n` for reaching level *n* (L2 = 40, L3 = 35, L4 = 30 … **L10 = 0, above 10 negative**) |
| **Park** | **250 each** |
| **Monuments** | **400 each** |
| **Temples** | **100, plus 100 per level above level 1, max 500 at level 5** |
| **Technology** | **100 per tier** (T1 = 100, T2 = 200, T3 = 300) |

Verbatim caveats from that page:

- *"Ship and battleship upgrades do not give extra score."* ⚠ Those hulls no longer exist; whether
  this still applies to Scout/Rammer/Bomber upgrades is **not stated** — [UNRESOLVED #18](#unresolved).
- *"Converted units do not change score for either player."*

### 11.3 Perfection difficulty bonus

Applied to the **final** score in Perfection only:

> *"This bonus is equal to **100% plus 41% of the natural logarithm of the number of opponents**,
> with an **additional 20%, 40%, or 80%** when the difficulty level is **normal, hard, or crazy**,
> respectively. The maximum difficulty bonus, with 15 crazy-level opponents, is **291%**."*

```
difficultyBonus = 1.00 + 0.41 * ln(numOpponents) + {easy: 0, normal: 0.20, hard: 0.40, crazy: 0.80}
```

Published easy-difficulty table (use as a unit test):

| Opponents | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Bonus % | 100 | 128 | 145 | 157 | 166 | 174 | 180 | 185 | 190 | 195 | 199 | 202 | 205 | 208 | 211 |

**This is a scoring multiplier and is entirely separate from the in-game star handicap in
[§12](#12-the-in-game-ai). Do not conflate them.**

Star thresholds: 3,000 points earns one star for that tribe; 10,000 two; 50,000 three; 100,000 a
halo. The global leaderboard resets every Monday.

### 11.4 Turn structure and ordering constraints

⚠ **This is the weakest-documented area of the entire ruleset.** Several sub-questions have no source
at all and are listed in [UNRESOLVED](#unresolved). Do not fill them in by guessing — they are
observable in-game in a few minutes each and are worth measuring directly.

**Confirmed:**

- **One move per turn:** *"A unit may only move once per turn (besides the use of the dash and escape
  skills)."*
- **Action flags are two independent booleans.** `hasMoved` and `hasAttacked`.
  Attack is legal iff `!hasMoved || hasDash`. Move is legal iff `!hasAttacked || hasEscape`.
  A Dash+Escape unit (the Rider) therefore gets move → attack → move, but **not**
  attack → move → attack.
- **Auto-heal on idle:** *"If a unit does not take any action in a given turn, it will automatically
  heal if it can do so."*
- **Several actions consume the whole turn regardless of remaining movement:** capturing a village
  or city, examining a ruin, embarking at a Port, disembarking, Heal Others, and Recover.
- **A besieged city produces 0 income.**
- **City level-up is immediate, not turn-gated.** Gaining the n-th population instantly levels the
  city and presents the reward choice; a level-5+ super unit spawns immediately, which is what makes
  the "spawn a Giant to push the besieger out" play work.
- **Temples and other growth buildings are turn-indexed:** *"Temples level up based on how many turns
  have passed since their construction, including the turn of their construction."* Sanctuary
  attracts an animal *"every 2 turns"*.

**Multiplayer is sequential, not simultaneous.** Turns follow a fixed player order. The evidence is
indirect but conclusive: *"In multiplayer games, if a piece of neutral road is connected to more than
one player's city, only the player that comes first in host order (listed first in the multiplayer
games menu) can use that piece of neutral road"*
([City Connection](https://polytopia.fandom.com/wiki/City_Connection)), and Glory's tiebreak resolves
"at the end of the turn", which requires a defined ordering. Both async (long per-turn timers) and
"Live Games" (short timer) pacing exist, and the timer is host-configurable.

**Unresolved and important:** whether star income is credited at the **start or the end** of a
player's turn ([UNRESOLVED #24](#unresolved)), whether a **newly trained unit can act the same turn**
([UNRESOLVED #25](#unresolved)), and the full intra-turn phase ordering
([UNRESOLVED #26](#unresolved)).

### 11.5 Domination rating

Not a victory condition — a post-game rating, four **equally weighted** parts:

- **Speed skills** — turns taken to destroy all opponents. 100% requires **≤ 10 turns per enemy**;
  below that it scales as `maxTurnsFor100% / turnsTaken`.
- **Battle skills** — `(enemyUnitsKilled + 1) / (friendlyUnitsLost + enemyUnitsKilled + 1)`.
- **Tribes destroyed** — `tribesDestroyed / totalOpponents`, where "destroyed" means you captured its
  last city.
- **Difficulty rating** — **25% easy, 50% normal, 75% hard, 100% crazy**.

20% earns one star, 70% two, 95% three, 100% a halo.

---

## 12. The in-game AI

### 12.1 The difficulty handicap — exact, and narrower than commonly believed

> *"The difficulty level determines the **aggressiveness and starting income** of bot opponents. Bots
> of higher difficulties are more aggressive. For example, **easy bots will almost always be peaceful
> until provoked, while crazy bots may be immediately hostile.** The difficulty also determines the
> income bots receive from **their capital city at the beginning of the game**. Easy bots receive one
> star per turn (SPT), normal bots (and human players) receive two, hard bots receive three, and
> crazy bots receive five."*
> — [Game Modes](https://polytopia.fandom.com/wiki/Game_Modes), corroborated verbatim on
> [City § Capital](https://polytopia.fandom.com/wiki/City)

| Difficulty | Level-1 capital income | Capital bonus vs. base level income |
|---|---|---|
| Easy | **1 SPT** | **+0** |
| Normal | **2 SPT** | **+1** (identical to a human player) |
| Hard | **3 SPT** | **+2** |
| Crazy | **5 SPT** | **+4** |

**Two corrections to the common framing of this rule:**

1. **It is not a flat "bonus stars per turn" added to the bot's whole economy.** It is a modifier to
   the **capital's** income, exactly like the human player's +1 capital bonus. Every other city a bot
   owns produces `level` stars like anyone else's. So the handicap is a **constant additive offset of
   +0/+1/+2/+4 SPT**, not a percentage and not a per-city bonus.
2. **Normal bots have no handicap at all** — they get exactly the same +1 capital bonus as a human.

**This is confirmed by a developer, verbatim.** Zoythrus (tagged `[developer]`), Steam, 2020-08-16
([thread](https://steamcommunity.com/app/874390/discussions/0/4390400383727805950/)):

> *"It affects how many stars the AI get from their capital every turn. Normally, everyone starts out
> with +2 stars/turn from their capital, but on Easy, Normal, Hard, and Crazy, the AI get
> **+1/2/3/5 stars/turn**. (yes, +5 stars/turn on Crazy, that's not a typo) **They also have higher
> aggression ratings.**"*

As the bot's capital levels up, the usual +1 per level stacks on top normally.

**Superseded claim — do not use:** a 2017 Fandom forum post claimed "At Hard, +1 income in its
capital. At Crazy, an additional +4", implying Crazy = 6 SPT. It predates the developer statement by
three years and is contradicted by two current wiki pages.

### 12.2 What else differs by difficulty

| Difference | Status |
|---|---|
| **Capital star income** | **CONFIRMED** (dev quote + two wiki pages) |
| **Aggression rating** | **CONFIRMED** — dev quote above, plus *"easy bots will almost always be peaceful until provoked, while crazy bots may be immediately hostile"*. **No numeric aggression values are published.** |
| **Diplomacy bias** | [Tribe Relations](https://polytopia.fandom.com/wiki/Tribe_Relations) documents two difficulty-linked opinion modifiers: **"Charming"** (*"a lot more rare on higher level bots"*) and **"Annoying"** (*"Penalty for playing on higher difficulties"*). |
| **Fog-of-war cheating** | **Contested.** The only direct statement is Midjiwan, 2016: *"The AI have to explore in order to get vision as well."* A 2024 Zoythrus comment is vaguer: *"It does cheat, but actually very little. Just enough to give it a chance"* — and he never itemises anything beyond capital income. Widespread player claims that the AI sees everything are SECONDARY and contradict the only dev statement on record. See [UNRESOLVED #20](#unresolved). |
| **Extra starting units** | **No evidence in any source.** A 2024 Steam thread *requested* extra units on high difficulty as a feature that does not exist. |
| **Free or cheaper technology** | **No evidence.** |
| **Smarter decision-making** | **Explicitly denied** by the wiki guide: *"Although they are more aggressive and have a greater income in their capital, they're still pretty, well, dumb. **Notice that harder difficulties don't make the bots any smarter!**"* ([Strategies/Perfection](https://polytopia.fandom.com/wiki/Strategies/Perfection)) |

**Bottom line for the engine: model difficulty as exactly two knobs — capital base SPT (1/2/3/5) and
an aggression scalar.** Everything else is unevidenced.

The Domination **difficulty rating** (25/50/75/100%) and the Perfection **difficulty bonus**
(+0/20/40/80%) are *scoring* consequences of difficulty, not behavioural ones. Do not conflate them
with the star handicap.

### 12.3 Documented AI behaviour

There is no planner and no machine learning — the shipped bot is hand-authored objectives with, per
community analysis, static priorities. The only substantive developer description is Zoythrus on
r/Polytopia (~May 2022,
[mirror](https://devtrackers.gg/polytopia/p/e387ab32-littles-changes-to-ai-that-improves-the-singleplayer-game)):

> *"Concerning Defenders, before a big AI upgrade, AI had the opposite problem — they'd always attack
> Defenders, and would lose most of their units to them. It was common to spam Defenders and just let
> the AI kill their own troops. **We changed it to stop this exploitive behavior.**"*
>
> *"Why does the AI spam Knights or MB's? **The AI is programmed to train units to counter what
> you/their target is training.** We don't say 'just churn out Knights', but 'Hey, the player is
> making Archers, so make Knights to counter them.'"*
>
> (on the AI not using Battleships) *"That's just an issue with the AI. We just haven't fixed it
> yet."*

Derivable decision rules:

1. Production is **reactive counter-picking** against the current target's unit composition.
2. There is a **special-cased de-prioritisation of Defenders** as attack targets.
3. Naval usage is a known under-implemented branch.

Uncontested community observations (**SECONDARY**): bots build Parks and Temples even in Domination
where score is irrelevant; they rarely use Catapults or Archers; city-upgrade choices look close to
random rather than mode-aware; they underdevelop city economies even after 50 turns.

**No datamine of the shipped bot exists.** Two traps: `GAIGResearch/Tribes` is the excluded 2020
academic framework (and Zoythrus explicitly denied any DeepMind integration — *"We never had any
plans to integrate DeepMind into Polytopia"*), and `HenBOMB/Polyfish` is a from-scratch MCTS/NN
replica with its own heuristics. Neither reflects the real bot.

### 12.4 The Explorer's pathfinding — the one fully published algorithm

Worth implementing verbatim, since it is the only game algorithm the developers have documented
(Midjiwan, reproduced on [Explorer](https://polytopia.fandom.com/wiki/Explorer)):

- Scans for fog tiles within a **movement range of 4 steps** (*"the vision range of the explorer is
  actually 5 tiles"*).
- Moves **1 step toward the nearest fog**.
- Ties are broken by **most fog uncovered**, capped at 4 (5 would force diagonals).
- A **lighthouse bonus** worth *"a little over clear-3-fog-tiles"*.
- A **small penalty on recently-traversed tiles** to discourage backtracking.
- Remaining ties broken **randomly**.
- Tile scores run **0–1000**; the BFS is **capped at 3 iterations**, **+100 per step**.
- Documented score constants: 110 = 4/5 tiles + lighthouse, 120 = 3 + lighthouse, 130 = 2 +
  lighthouse, 140 = 1 + lighthouse, 143 = 4/5 tiles, 153 = 3, 163 = 2, 173 = 1.

If you want a published baseline to compare your engine's AI against, the only peer-reviewed work is
the 2020 Java **Tribes** framework — but it models a much older ruleset and its best agent loses
100% of games against a moderately good human. See `docs/research-findings.md`.

---

## 13. Source quality notes

### 13.1 How conflicts were resolved

Order of authority used throughout this document:

1. **Official patch notes** (`polytopia.io`, Steam announcements) — these override the wiki
   unconditionally, and did so for: Literacy discount, Burn Forest cost, temple score, Ai-Mo's
   starting tech, Explorer moves, backwards research, Market income, starting stars.
2. **Wiki page body text** — overrides the page's own infobox.
3. **Wiki infoboxes** — least reliable; see below.
4. **Arithmetic cross-checks** — the turn-0 score identity in [§10.2](#102-turn-0-score-cross-check)
   independently confirmed Luxidoor's capital level and every tribe's starting tech tier.

### 13.2 Known stale wiki infoboxes and pages

Do **not** trust these specific lines:

| Page | Stale claim | Correct value |
|---|---|---|
| [Technology](https://polytopia.fandom.com/wiki/Technology) | "T2/T3 only after the previous tech in their branch" | backwards research allowed since Sept 2025 |
| [Technology](https://polytopia.fandom.com/wiki/Technology) | Literacy "reduces research costs by 33%" | exactly ⅓, `ceil(cost × 2/3)` |
| [Technology](https://polytopia.fandom.com/wiki/Technology), [Meditation](https://polytopia.fandom.com/wiki/Meditation) | Ai-Mo starts with Meditation | **Philosophy** |
| [Smithery](https://polytopia.fandom.com/wiki/Smithery) | "technologies cannot be researched backwards" | they can |
| [Sailing](https://polytopia.fandom.com/wiki/Sailing) infobox | unlocks "Raft, Port" | those belong to **Fishing**; Sailing gives Scout + ocean movement |
| [Scout](https://polytopia.fandom.com/wiki/Scout) infobox | tech "Fishing" | **Sailing** |
| [Port](https://polytopia.fandom.com/wiki/Port) infobox, [Buildings](https://polytopia.fandom.com/wiki/Buildings), [Population](https://polytopia.fandom.com/wiki/Population) | Port tech "Sailing" | **Fishing** |
| [Raft](https://polytopia.fandom.com/wiki/Raft) | Rammer needs "Aquaculture" | tech was renamed **Ramming** |
| [Terrain](https://polytopia.fandom.com/wiki/Terrain), [Movement](https://polytopia.fandom.com/wiki/Movement) | ocean gated on "Navigation" | **Sailing** |
| [Burn Forest](https://polytopia.fandom.com/wiki/Burn_Forest), [Construction](https://polytopia.fandom.com/wiki/Construction) | cost 2★ / 5★ (page is self-contradictory) | **3★** |
| [Temples](https://polytopia.fandom.com/wiki/Temples) | score 100/150/200/250/300 | **100/200/300/400/500** |
| [Star](https://polytopia.fandom.com/wiki/Star) | Market gives 2★ per level | **1★** per level |
| [Explorer](https://polytopia.fandom.com/wiki/Explorer) | 15 moves | **12** (v2.16.3) |
| [Buildings](https://polytopia.fandom.com/wiki/Buildings) | Forge "adjacent to a Lumber Hut" | copy-paste error from the Sawmill row; Forge has **no placement adjacency requirement** |
| [Update History](https://polytopia.fandom.com/wiki/Update_History) | Luxidoor capital level 2 | **level 3** (proven by turn-0 score) |
| [Update History](https://polytopia.fandom.com/wiki/Update_History) | — | stops at v2.10.0; misses the 2025 Balance Pass and Cymanti rework entirely |
| [Bardur](https://polytopia.fandom.com/wiki/Bardur) prose | "1.5x fruit, no crop" | **0.8× forest, 0.0× crop, 1.0× fruit** |
| [Strategies/Tribes](https://polytopia.fandom.com/wiki/Strategies/Tribes) | Luxidoor "3 SPT", Starfish "10 stars" | **4 SPT**, **8 stars** |

### 13.3 Secondary sources used

Used only for cross-checking or where explicitly flagged in the text; **no number in this document
rests solely on a secondary source except the Border Growth radius**, which is flagged as such.

- FrothFrenzy Battle Calculator — [frothfrenzy.github.io/polytopiacalculator](https://frothfrenzy.github.io/polytopiacalculator/).
  Last updated 2019-08-14, so stale on Meditation/Climbing and on Mind Bender veteran eligibility.
  Useful for the `accelerator = 4.5` naming and the `isNaN` guard.
- Polytopia Fandom forum thread on border growth, and NamuWiki — the only sources for the 5×5 figure.
- SteamDB / perfectly-nintendo patch-note mirrors — used only to confirm dates already established
  from official sources.

---

## UNRESOLVED

**These are the things that could not be confirmed. Do not implement any of them from a guess.**
The project rule is that a wrongly-stated rule is worse than a missing one.

### Combat

1. **Splash / explosion rounding — primary sources directly contradict each other.**
   [Combat](https://polytopia.fandom.com/wiki/Combat) says splash is `attackResult / 2`
   ***without rounding***, and documents the resulting fractional HP as a bug (*"this will cause the
   received splash damage to be a .5 decimal, which in turn will make the receiving unit's hp a .5
   decimal … the n.5 hp is rounded up on the map display and rounded down in the unit info box"*).
   But [Bomber](https://polytopia.fandom.com/wiki/Bomber) and
   [Fire Dragon](https://polytopia.fandom.com/wiki/Fire_Dragon) both say *"halved and **rounded
   down**"*, and the 2019 community calculator uses `floor`.
   **This determines whether HP must be a floating-point type in your engine.** Resolve empirically:
   fire a Bomber splash that would produce an odd `attackResult` and check whether the victim lands
   on `n.5` HP.

2. **Stomp damage (Juggernaut) — no formula or number published anywhere.** The skill is defined
   ("damages all adjacent enemy units when it moves") but the magnitude is not. A wiki full-text
   search for "stomp damage" returns only pages that mention the skill without a number. A plausible
   but **entirely unsourced** hypothesis is that it reuses the splash `attackResult / 2` path.
   Needs in-game measurement.

3. **Does a damaged naval unit's current HP carry back to the land unit on disembark?** Not stated on
   any wiki page. The strong implication (single entity, shared HP pool, "upgrading does not heal
   it") is that current HP is preserved through embark → upgrade → disembark, but no source says so.

4. **The rounding tie-rule in the damage formula.** The wiki says only "nearest whole number".
   [§1.3](#13-rounding--round-half-away-from-zero) infers **round-half-away-from-zero** from a
   documented outcome (Warrior vs Warrior → exactly 4.5 → observed 5), which is strong but is an
   inference, not a published statement. It rules out banker's rounding. Worth confirming against a
   second documented case.

5. **Does `Creep` negate Zone of Control?** The [Movement](https://polytopia.fandom.com/wiki/Movement)
   page contradicts *itself*: its Zone of Control section says Creep "completely bypass[es]" ZoC,
   while its Unit Skills section (edited 2026-04-05) says restrictions are "not eliminated", and
   [Unit Skills](https://polytopia.fandom.com/wiki/Unit_Skills) says "zone of control cannot be
   negated". **Recommended: Creep does NOT bypass ZoC** (2 of 3 statements, including the newest
   edit and the dedicated skills page). Moot for the Cloak specifically, which has `Hide` and
   `Sneak`, but it matters if you implement Creep generically. Note this was reversed on the wiki
   between January and April 2026, so older references will disagree.

### Technology

6. **Backwards-research adjacency semantics.** Confirmed that backwards research exists. Not
   confirmed whether the rule is strictly parent↔child adjacency, or whether owning *any* tech in a
   branch unlocks the whole branch. The two officially named examples (Smithery→Mining,
   Philosophy→Meditation) are both direct-parent cases, so parent↔child is the safe implementation —
   but it is not stated.

7. **Does tech cost fall when you lose a city?** The formula is a pure function of the *current* city
   count so it should fall symmetrically, and cost rising on city *gain* is explicitly confirmed. No
   source states the loss case.

### Cities and buildings

8. **Surplus population carry-over on level-up.** If a city needing 1 more population receives a +3
   grant, does the excess 2 carry into the next level? Never stated.

9. **Border Growth = 5×5 (25 tiles).** **Secondary sources only** (a Fandom forum thread and
   NamuWiki), corroborated indirectly by the "resources spawn within two tiles" generation rule. The
   3×3 = 9 tiles for a *new* city **is** primary-confirmed via the Score worked example.

10. **Border overlap and tile-claim tie-breaking.** Evidence points to first-come-first-served
    (growth does not steal owned tiles), but the exact rule when growth would overlap another
    player's territory is undocumented.

11. **Temple scoring and growth rate — both disputed.** *Score:* four sources give three maxima —
    500 ([Score](https://polytopia.fandom.com/wiki/Score) + the 2025 Balance Pass),
    400 ([polytopia.io strategy article](https://polytopia.io/news/strategy-tip-build-temples/)),
    300 ([Temples](https://polytopia.fandom.com/wiki/Temples) table). **500 is recommended** as the
    most recent and officially corroborated, but it is not settled.
    *Rate:* Path of the Ocean says every 2 turns; the Temples table uses 3-turn buckets;
    Strategies/Perfection says every 3 turns. All sources agree max level is reached at **12 turns**,
    which favours 3. Measure both in-game.

12. **Capture cooldown duration.** Its existence is confirmed only by a v2.16.3 bugfix line
    (*"Capture city allowed during cooldown"*). Duration and exact semantics unstated — most likely
    "cannot capture a city on the turn it changed hands", but that is a guess.

13. **What happens to a captured city's accumulated population.** City *level* is retained (proven by
    the Metropolis task completing on capture). Whether the partial population bar toward the next
    level transfers or resets is undocumented.

14. **Whether territory tiles transfer with a captured city**, and whether Border Growth is retained.

15. **Embassy cost.** Buildings says 4★; the Embassy page says 5★; and the 2025 Balance Pass changed
    it to a scaling cost (*"begins cheaper, but increases based on the number of Embassies you
    have"*) with **no published formula**.

16. **Market per-city limit.** Sawmill, Windmill, Forge and the removed Customs House all carry an
    explicit "only one per city"; the [Market](https://polytopia.fandom.com/wiki/Market) page
    conspicuously does not.

### Ruins and scoring

17. **Ruin reward distribution.** The wiki says rewards are chosen *"randomly, with no special
    weighting accorded to any one option"*, but v2.16.3's notes say *"Changed how ruin rewards are
    calculated"* with no detail. The uniform-random claim may be stale.

18. **Naval upgrade score.** [Score](https://polytopia.fandom.com/wiki/Score) says verbatim *"Ship
    and battleship upgrades do not give extra score"* — but those hulls no longer exist. Whether the
    same applies to Scout / Rammer / Bomber upgrades is **not stated**. Recorded verbatim rather than
    resolved by assumption.

### Map generation

19. **Sub-resource redistribution when a rate is zeroed.** The wiki documents the
    mountain→forest→field cascade, but not where the freed probability mass goes when a *sub*-resource
    is zeroed (Bardur's 0.0× crop). Presumably into the "empty field" bucket, but that is inference.

### AI

20. **Does the AI cheat on vision in current builds?** The only direct statement is Midjiwan in
    **2016**: *"The AI have to explore in order to get vision as well."* A 2024 Zoythrus comment says
    only *"It does cheat, but actually very little. Just enough to give it a chance"* without
    itemising anything beyond capital income. Widespread player claims that the AI sees the whole map
    are SECONDARY and contradict the only developer statement on record. Ten years is a long time for
    a 2016 claim to still hold.

21. **Does the AI capital income bonus persist all game, or only at the start?** The developer said
    "every turn" and the wiki calls it "starting income", which reads as the level-1 baseline rather
    than a temporary buff. Strongly indicated, never literally stated.

22. **Numeric aggression values.** "Higher aggression ratings" is developer-confirmed; no numbers are
    published.

23. **The bots' actual decision rules** beyond reactive counter-picking and Defender
    de-prioritisation. Entirely undocumented; no datamine exists.

### Turn structure — the least documented area

24. **Is star income credited at the START or the END of a player's turn?** Every source says only
    "produced every turn" / "stars per turn". **No source anywhere states the phase.** This is
    probably the single highest-priority gap for engine correctness, and it is trivially observable
    in-game.

25. **Can a newly trained unit act on the same turn it is bought?** Not documented on any wiki page.
    The widely repeated "no" could not be traced to the wiki, a developer statement, or any citable
    source.

26. **The full intra-turn phase ordering** — income → growth → actions, versus actions → growth →
    income. Undocumented.

### Vision

27. **Do cities or territory have a vision radius independent of units?** No source states one. The
    turn-0 score arithmetic is consistent with a radius-1 initial reveal around the capital (a corner
    spawn loses exactly 5 points = 1 tile), but that is derivation, not a quoted rule.

28. **Does capturing a city reveal any tiles** beyond the capturing unit's own radius? No source.

### Map generation

29. **Ruin adjacency rule.** [Ruin](https://polytopia.fandom.com/wiki/Ruin) says ruins cannot spawn
    next to a **capital** or another ruin; [Map Generation](https://polytopia.fandom.com/wiki/Map_Generation)
    says next to another ruin or a **village**. The Ruin page notes the village rule applied only
    before update 2.0.58, so Map Generation is probably stale — but the Ruin page itself was last
    edited 2024-08-08 and is the stalest page cited in this document.

### Minor

30. **Explorer move count.** [Explorer](https://polytopia.fandom.com/wiki/Explorer) says **15**;
    the official [v2.16.3 notes](https://polytopia.io/bug-fixes-in-version-2-16-3/) say *"Reduced
    number of moves an explorer makes: 15 → 12"*. **Use 12** — the patch note is newer and explicit,
    and the wiki page simply was not updated. Listed here only because the wiki still contradicts it.

31. **Multiplayer turn-timer options.** A host-configurable timer demonstrably exists (a 2025 patch
    note fixes it falling back to "60 days"), but the selectable values are undocumented.

32. **Luxidoor's purchase price** — $2.99 ([Tribes](https://polytopia.fandom.com/wiki/Tribes)) vs
    $3.99 ([Luxidoor](https://polytopia.fandom.com/wiki/Luxidoor)). No gameplay impact.
