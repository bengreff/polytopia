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

`polytopia.fandom.com` HTML is Cloudflare-gated: plain `WebFetch` returns HTTP 402 and `curl` gets a
JS challenge. Two routes work:

1. **MediaWiki API** (not gated):
   `curl "https://polytopia.fandom.com/api.php?action=parse&page=<Title>&prop=text&format=json&formatversion=2"`
2. **Wayback Machine** via `curl` (note: `WebFetch` is blocked from `web.archive.org`, so use
   `curl`, and gzip-decompress the `id_` endpoint's response yourself).

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
records the conflict. Known bad infoboxes are listed in [§13.2](#132-known-stale-wiki-infoboxes).

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
