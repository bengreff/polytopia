# PolyEnv Audit — Read-Only Source Cross-Check

**Repo:** https://github.com/FryOne2137/PolyEnv
**Clone:** shallow, read-only, into scratchpad. Never built, never executed.
**HEAD audited:** `0b586a7637b0b793a740550393d6cf24d7660ebe` — "error hangling update", 2026-07-20, sole author `FryOne`.
**Audit date:** 2026-08-03

**Framing:** the live game (wiki-sourced) is our canonical ruleset. PolyEnv is a *cross-check corpus*, not a source of truth. This document exists to say which of its numbers are worth diffing against wiki research and which are stale, invented, or plainly wrong.

---

## 1. License confirmation

An actual `LICENSE` file exists at repo root. Verbatim:

```
MIT License

Copyright (c) 2026 FryOne2137

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Standard, unmodified MIT. The **source and data files are clean to read and to draw ideas from**, with attribution.

### ⚠ License hygiene caveat the MIT file does not cover

`assets/textures/Polytopia_game_engine_textures/` contains **1,428 binary asset files** — subdirectories `misc/`, `water/`, `tribes/`, `common/`, with filenames like `poly phone green.png`, `trophyIcon.png`, `bgComplete.png`. These are ripped Battle of Polytopia game art. The MIT grant is the author's, and the author does not own this art. Midjiwan's copyright is unaffected by someone else's LICENSE file.

**Action for this project: do not clone `assets/` into anything we ship, vendor, or commit.** Restrict any reuse to `src/`, `data/Units.json`, and `python/`. Our own audit clone should be treated as ephemeral. This is the single biggest license-hygiene footgun in the repo and it is not flagged anywhere in the README.

Secondary note: `README.md` credits map generation to `QuasiStellar/Polytopia-Map-Generator`. That upstream's license was **not verified** in this audit (out of scope — we were told to clone only PolyEnv). If we ever lift map-gen structure from `src/world/MapGenerator.cpp`, that derivation chain needs its own check.

---

## 2. Layout, size, and state representation

### Directory structure

```
CMakeLists.txt          10.8 KB, scikit-build-core + pybind11
data/Units.json         the only externalized rule data (701 lines)
src/
  core/Ids.h            PlayerId=u8, UnitId=u16, CityId=u16, sentinels 0xFF/0xFFFF
  world/                Map, Tile, Pos, MapGenerator
    terrain/            BaseTerrain, Resources, Building, RoadBridge,
                        Settlement{Id,Type}, Visibility enums
  content/              units/ tech/ buildings/ tribes/ settlements/ skills/
  systems/              22 systems — THE RULES LIVE HERE
  game/Game.{h,cpp}     façade: turn order, action entry points, win check
  ai/                   Action, ActionSpace, GameStateSerializer/Adapter, ModelClient
  runtime/              GameSession, BeliefWorldBuilder (ISMCTS determinization)
  python/bindings.cpp   8,240 lines — pybind11 surface, vector env, MCTS pool
  gui/                  raylib-ish renderer, 4,324 lines in MapRenderer.cpp alone
  replay/               ReplayRecorder
apps/cli, apps/gui
python/PolyEnv/         thin wrapper + tribe constants
python/tests/           17 pytest files, 4,284 lines
docs/                   18 markdown files, mkdocs + readthedocs
```

### Line counts

| Scope | Lines |
|---|---|
| All `src/**/*.{cpp,h}` | 29,096 |
| **Rules-bearing** (`systems/` + `content/` + `world/`) | **10,478** |
| `src/python/bindings.cpp` alone | 8,240 |
| `src/gui/MapRenderer.cpp` alone | 4,324 |
| Python tests | 4,284 |

So of ~29k lines of C++, **only about a third is actual game rules**. Nearly 45% is Python-binding plumbing and a GUI. Useful calibration: the rules surface of Polytopia is genuinely small — ~10k lines of unoptimized C++ covers most of it.

### Core state representation

Plain **AoS, `std::vector`-of-struct, no bitboards, no SoA, no ECS despite the `systems/` naming.**

```cpp
// src/game/Game.h:222-238
Map map;
std::vector<Player> players;
std::vector<Unit>   units;      // UnitId == index; dead units are tombstoned, never erased
std::vector<City>   cities;     // CityId == index
PlayerId currentPlayer = 0;
uint32_t turnNumber = 0;
std::array<uint16_t, 4> lighthouseDiscoveredBy = {0,0,0,0};
std::deque<PendingCityUpgrade> pendingCityUpgrades;
PlayerId winner = kNoPlayer;
std::vector<std::vector<Pos>>     visibleTilesByPlayer;
std::vector<std::vector<uint8_t>> visibleLookupByPlayer;
```

`Map` (`src/world/Map.h:53-59`) is two parallel flat vectors — `std::vector<Tile> tiles` and `std::vector<UnitId> unitAt` — indexed `y * width + x`. That `unitAt` sidecar is the one genuinely good decision here: O(1) occupancy without scanning units.

`Tile` (`src/world/Tile.h:56-64`) is 9 scalar members, no bitpacking:

```cpp
VisibilityEnum visibility = VisibilityEnum::None;   // uint16_t reveal bitmask, 1 bit/player
CityId territoryCityId = kNoCity;                   // uint16_t
RoadBridgeEnum roadBridge;   BuildingTypeEnum buildingType;
SettlementTypeEnum settlementType;  SettlementId settlementId;
ResourcesEnum resource;      BaseTerrainEnum baseTerrain;
TribeType tribe;                                    // biome, for rendering + resource rolls
```

Roughly 12 bytes padded to ~14–16. A 16×16 map is ~4 KB of tile state. Perfectly adequate; nothing clever, nothing to copy.

`Unit` (`src/content/units/Unit.h:184-220`) is ~56 bytes: ids, `Pos`, int HP/maxHP, **float** attack/defense, movePoints, range, cost, visionRange, `TechId requiredTechToSpawn`, two `Pos` direction memos, kill counter, three bools, and `uint64_t skillMask`.

Two observations worth carrying into our design:

- **`uint64_t skillMask` with 35 defined bits** (`src/content/skills/UnitSkill.h:11-48`) is the right shape. We should do the same and it will fit comfortably.
- **`float attack/defense` is a mistake to avoid.** Values are all halves (2.0, 3.5, 0.7, 0.1). Store as fixed-point tenths in an integer and the whole combat path becomes exactly reproducible across compilers. PolyEnv's damage path is `double`-based and rounds with `std::llround`, which is *probably* stable but is a needless determinism risk for RL replay.
- **`VisibilityEnum` is `uint16_t` → hard cap of 16 players.** Matches the stated 2–16 range but it's an implicit coupling; `LighthouseSystem.cpp:140` re-checks `idx >= 16` by hand.

### Architecture style

Systems are stateless namespaces of free functions taking `Game&` — `CombatSystem::attack(Game&, UnitId, Pos)`. Data classes (`Unit`, `City`, `Tile`) are near-pure getter/setter bags. `Game` is a thin façade that checks turn ownership and delegates.

This is a reasonable shape and the one thing about the codebase I'd call unambiguously sound. The problem is the execution *inside* the systems (see §11).

---

## 3. Rule constants: hardcoded vs. externalized

**Almost entirely hardcoded in C++.** Exactly one data file exists.

| Rule domain | Where | Form |
|---|---|---|
| Unit stats & skills | `data/Units.json` | **Externalized JSON**, 53 units, schema v1 with `_defaults` |
| Tech tree + tiers + prereqs | `src/content/tech/TechDB.cpp:13-44` | `constexpr std::array<TechData, 25>` |
| Tech pricing formula | `src/content/tech/TechDB.cpp:94-106` | hardcoded arithmetic |
| Building cost/tech/terrain/pop | `src/content/buildings/BuildingDB.cpp:7-95` | static array built by an immediately-invoked lambda |
| Tile action costs | `src/systems/TileActionSystem.cpp` | `static constexpr int kCost` **inside each function, duplicated between the `can*` and the doer** |
| Combat constants | `src/systems/CombatSystem.cpp` | magic numbers inline |
| City thresholds / income | `src/content/settlements/City.cpp` | inline arithmetic in methods |
| City level-up rewards | `src/systems/CityRewardSystem.cpp:150-213` | `switch` with literals |
| Monument unlock thresholds | `src/systems/MonumentSystem.cpp` | literals in each hook |
| Score weights | `src/systems/ScoreSystem.cpp:131-180` | one function per weight |
| Tribe start stars/tech/unit/score | `src/content/tribes/Tribe.cpp:64-153` | four `switch` statements |
| Map-gen tuning | `src/world/MapGenerator.cpp:13-46` | anonymous-namespace `constexpr` |

### On `data/Units.json`

Format is clean and worth imitating structurally:

```json
{ "_schemaVersion": 1,
  "_defaults": { "maxHealth": 10, "attack": 1.0, "defense": 1.0, "movePoints": 1,
                 "range": 1, "cost": 0, "visionRange": 1,
                 "requiredTechToSpawn": null, "skills": [], "veteranEligible": true },
  "units": { "Warrior": { "cost": 2, "maxHealth": 10, ... , "skills": ["Dash","Fortify"] }, ... } }
```

Completeness: **all 53 unit types have full stat blocks.** Zero stubs. Coverage extends to all four special tribes plus Elyrion dragons.

But the externalization is **half-done in a way that undermines its value**:

- Loader `src/systems/GameDataSystem.cpp:20-135` hand-writes three `if`-chains mapping strings → `UnitType`, `TechId`, `UnitSkill`. Adding a unit to JSON without editing C++ throws `std::runtime_error`. So it is data-driven for *tuning* but not for *content*.
- `veteranEligible` is **parsed nowhere.** `applyJsonToUnit` (lines 137-166) never reads it. Veterancy is gated instead by `UnitSkill::StaticSkill` in `UnitUpgradeSystem.cpp:36`. The JSON field is decorative and the two encodings can silently disagree.
- `ScoreSystem::costStarsByUnitType` (`ScoreSystem.cpp:199-228`) **re-hardcodes** Warrior=2, Archer=3, Defender=3, Rider=3, MindBender=5, Swordsman=5, Catapult=8, Cloak=8, Knight=8, Raft=2 as a "minimal fallback." The comment says *"Replace with UnitDB lookup when you have it"* — the UnitDB exists. So unit costs live in two places and `Raft=2` here contradicts `Raft: cost 0` in the JSON.
- Nothing else is externalized: no `Techs.json`, no `Buildings.json`, no `Tribes.json`.
- The path is resolved as the literal relative string `"data/Units.json"` (`Game.cpp:312`) with a Python override — a working-directory dependency.

**Takeaway for us:** externalize *everything* (units, techs, buildings, tribes, tile actions, combat constants) into data, generate the enum⇄string mapping rather than hand-writing it, and never let a constant exist in both a table and a `switch`.

---

## 4. Completeness matrix

### Terrain — complete, but coarse

`BaseTerrainEnum` (`src/world/terrain/BaseTerrainEnum.h:11-17`): `Ocean=0, Water=1, Land=2, Mountain=3, Forest=4`. Five values, all implemented and all load-bearing.

`ResourcesEnum` (`ResourcesEnum.h:10-18`): `None, Fish, Whale, Fruit, Crops, Animal, Metal`. **`Whale` is declared and never used anywhere** — no spawn in `MapGenerator`, no harvest in `TileActionSystem`, no starfish/whale distinction. Absent in practice.

`SettlementTypeEnum`: `None, Village, City, Starfish, Ruin` — all five generated and handled.

`RoadBridgeEnum`: `None, Road, Bridge` — implemented, with the 0.5-move-point rule.

**Missing terrain concepts vs. live game:** ice tiles (Polaris), flooded tiles (Aquarion `AutoFlood`), algae (Cymanti), and the fog/cloud distinction. `Whale` is the only base-game gap and it's a small one.

### Units — data complete, behavior 12/16 tribes

| Group | In `Units.json` | Trainable in engine | Behavior implemented |
|---|---|---|---|
| Regular land (Warrior, Archer, Defender, Rider, MindBender, Swordsman, Catapult, Cloak, Knight) | ✅ 9 | ✅ all 9 | ✅ |
| Naval (Raft, Scout, Rammer, Bomber) | ✅ 4 | ⛔ upgrade-only, by design | ✅ |
| Special/spawned (Dagger, Pirate, Giant, GiantSuper, Juggernaut, Dinghy, Bunny, Bunta) | ✅ 8 | ⛔ | Partial — Dagger/Pirate spawn via infiltration, Giant via city reward; Bunny/Bunta/Dinghy/Juggernaut are **data-only, never instantiated** |
| Aquarion (12) | ✅ | ⛔ | ⛔ stat blocks only |
| Elyrion (4) | ✅ | ⛔ | ⛔ stat blocks only |
| Polaris (5) | ✅ | ⛔ | ⛔ stat blocks only |
| Cymanti (12) | ✅ | ⛔ | ⛔ stat blocks only |

`UnitSpawnSystem.cpp:16-31` restricts city training to exactly the 9 regular land units. README states this outright: *"does not implement special tribes."*

**Skills: 35 declared, ~15 implemented.**

Actually referenced by game logic (`grep UnitSkill:: src/systems src/game`): `WaterOnly, Escape, Stomp, Infiltrate, Dash, StaticSkill, Hide, Heal, Creep, Stiff, Splash, Scout, Persist, Fortify, Carry, Surprise(1 ref), Independent(1 ref)`.

Parsed from JSON but with **zero gameplay implementation**: `Convert, Poison, Freeze, FreezeArea, AutoFreeze, AutoFlood, Explode, Grow, Amphibious, Air, Algae, Skate, Swarm, DoubleAttack, Drench, Tentacles, Sneak, Eat, Boost`. These appear only in `GameDataSystem.cpp`'s string→enum table.

Consequence: `Poison` is read in `CombatSystem::computeDefenceBonus` (a unit *can be* poisoned and suffer the penalty) but **nothing ever sets the poisoned flag** — no unit with the `Poison` skill applies it on attack. The defense-penalty branch at `CombatSystem.cpp:95-103` is dead code in practice.

### Techs — 25/25, structurally correct

All 25 live-game techs present with correct tier and prerequisite (see §10.2). No stubs. Every tech is referenced by at least one gate.

### Buildings — 8 economic + 7 monuments + 1 orphan

`BuildingTypeEnum.h:10-32` declares `Farm, Forge, LumberHut, Market, Mine, Port, Sawmill, Windmill` (all implemented in `BuildingDB` and `BuildingSystem`), the 7 monuments (implemented), and `Lighthouse = 16`.

**`Lighthouse` is never buildable and has no `BuildingDB` entry.** It is only *detected* — `VisionSystem::tileHasLighthouse` reads it for explorer pathing scoring, and `LighthouseSystem` works purely off map-corner coordinates. So corner lighthouses are positional, not building-typed. Inconsistent but harmless.

**Absent entirely: Temples.** No `Temple`, `ForestTemple`, `MountainTemple`, `WaterTemple`, `IceTemple` in the enum, `BuildingDB`, or `BuildingSystem`. Yet `ScoreSystem::templeLevel(int)` exists at `ScoreSystem.cpp:173-176` and is **never called**. Temples are a real scoring and Perfection-mode component of the live game; their absence is a significant completeness gap.

Also absent: Embassies / diplomacy state (grep for `Embassy`, `Peace`, `Diplomacy` as a mechanic: zero hits outside the `TechId::Diplomacy` unlock for Cloak).

### Tribes — 16 named, 12 playable, and the tribe-bonus layer barely exists

`Tribe.cpp:43-61` names all 16. `defaultStartTech` returns `TechId::Count` (none) for Aquarion, Elyrion, Polaris, Cymanti — consistent with "12 regular tribes only."

Critically, **`Tribe` carries only four fields**: name, start stars, start score, start tech (+ a start unit type). There is **no representation of tribe passives at all** — no Luxidoor "capital starts at level 2 with no upgrade choice", no Vengir free Swordsman semantics, no Kickoo/Bardur resource guarantees outside map-gen. The only two special-cases in the entire engine:

- `Game.cpp:366-371` — Luxidoor capital forced to level 2 (but `starsPerRound` and `population` are explicitly set to 0, so the level-2 bonus income is *not* applied and no reward is granted).
- `MapGenerator.cpp:107-125, 523-537` — per-tribe terrain/resource probability multipliers and guaranteed capital resources for Bardur/Imperius/Kickoo/Zebasi/Aquarion.

So "12 tribes supported" overstates it: **tribes differ only in starting stars, starting tech, starting unit, and map biome generation.** Everything else is identical.

### Game modes — one

`Game::updateWinnerByCapitals()` (`Game.cpp:1048-1076`) is the sole terminal condition: a player wins when **every capital city is owned by that one player**. That is Domination.

**Absent:** Perfection (30-turn score race), Glory, Might, Creative/sandbox, and any turn limit. `ScoreSystem::getScore` is fully implemented and exposed but never drives termination. There is no `maxTurns` in `NewGameConfig` (`Game.h:46-52`). Games only end by elimination.

Also absent: multiplayer-ish concerns — no simultaneous turns, no diplomacy/peace treaties. Turn order is a fixed round-robin (`Game.cpp:464-467`).

---

## 5. The critical formulas — verbatim

### 5.1 Combat damage and retaliation

`src/systems/CombatSystem.cpp:256-277` — the core exchange:

```cpp
    // --- Polytopia damage formula ---
    const double aAtk = static_cast<double>(UnitSystem::getAttack(game, attackerId));
    const double dDef = static_cast<double>(UnitSystem::getDefense(game, defenderId));

    const double aHp = static_cast<double>(UnitSystem::getHealth(game, attackerId));
    const double aMaxHp = static_cast<double>(UnitSystem::getMaxHealth(game, attackerId));
    const double dHp = static_cast<double>(UnitSystem::getHealth(game, defenderId));
    const double dMaxHp = static_cast<double>(UnitSystem::getMaxHealth(game, defenderId));

    if (aMaxHp <= 0.0 || dMaxHp <= 0.0) return false;

    const double attackForce = aAtk * (aHp / aMaxHp);
    const double defenceBonus = computeDefenceBonus(game, defenderId);
    const double defenceForce = dDef * (dHp / dMaxHp) * defenceBonus;

    const double total = std::max(1e-9, attackForce + defenceForce);

    const int attackResult = std::max(0, iround((attackForce / total) * aAtk * 4.5));
    const int defenceResult = std::max(0, iround((defenceForce / total) * dDef * 4.5));

    // Apply damage to defender
    UnitSystem::setHealth(game, defenderId, std::max(0, UnitSystem::getHealth(game, defenderId) - attackResult));
```

Rounding helper, `CombatSystem.cpp:24-26`:

```cpp
static inline int iround(double v) {
    return static_cast<int>(std::llround(v));
}
```

Retaliation, `CombatSystem.cpp:364-376`:

```cpp
    // Retaliation only if defender can reach attacker
    const int defenderRange = UnitSystem::getRange(game, defenderId);
    const int backDist = chebyshevDistance(UnitSystem::getPos(game, defenderId), attackerPos);

    // Retaliation is blocked if defender has the Stiff skill
    if (defenceResult > 0 && backDist <= defenderRange && !UnitSystem::hasSkill(game, defenderId, UnitSkill::Stiff)) {
        UnitSystem::setHealth(game, attackerId, std::max(0, UnitSystem::getHealth(game, attackerId) - defenceResult));
        if (UnitSystem::getHealth(game, attackerId) <= 0) {
            UnitSystem::addKill(game, defenderId);
            awardKillToPlayer(game, UnitSystem::getOwnerId(game, defenderId), defenderId);
            killUnit(game, attackerId);
        }
    }
```

Kill short-circuits retaliation entirely — `CombatSystem.cpp:317-322` returns before reaching the block above.

Defence bonus, `CombatSystem.cpp:41-106` (elided for brevity, key branches):

```cpp
static double computeDefenceBonus(const Game& game, UnitId defenderId) {
    double bonus = 1.0;
    ...
    if (base == BaseTerrainEnum::Forest) {
        if (PlayerSystem::hasTech(game, owner, TechId::Archery)) bonus = 1.5;
    } else if (base == BaseTerrainEnum::Mountain) {
        if (PlayerSystem::hasTech(game, owner, TechId::Climbing)) bonus = 1.5;
    } else if (base == BaseTerrainEnum::Water) {
        if (PlayerSystem::hasTech(game, owner, TechId::Aquatism)) bonus = 1.5;
    } else if (base == BaseTerrainEnum::Ocean) {
        if (PlayerSystem::hasTech(game, owner, TechId::Aquatism)) bonus = 1.5;
    }

    double cityBonus = 1.0;
    if (t.getSettlementType() == SettlementTypeEnum::City) {
        // Units without Fortify get NO city defence bonus
        if (UnitSystem::hasSkill(game, defenderId, UnitSkill::Fortify)) {
            cityBonus = 1.5;
            ...
                    if (c->hasCityWallEnabled()) {
                        cityBonus += 2.5; // 1.5 + 2.5 = 4.0
                    }
        }
    }

    // City bonus overrides terrain bonus if larger
    if (cityBonus > bonus) {
        bonus = cityBonus;
    }

    // Poison modifiers from the wiki ...
    //   none: 0.5, standard: 0.7, wall: 2
    if (UnitSystem::isPoisoned(game, defenderId)) {
        if (bonus <= 1.0) {
            bonus = 0.5;
        } else if (bonus <= 1.5) {
            bonus = 0.7;
        } else {
            bonus = 2.0;
        }
    }

    return bonus;
}
```

Splash, `CombatSystem.cpp:282-284`:

```cpp
    if (UnitSystem::hasSkill(game, attackerId, UnitSkill::Splash)) {
        const int splashDamage = std::max(0, attackResult / 2); // half, rounded down
```

Stomp, `CombatSystem.cpp:450-453` — reuses the exchange formula, halves it, and never retaliates:

```cpp
            const int mainHit = std::max(0, iround((attackForce / total) * aAtk * 4.5));

            // Stomp damage should match Splash damage amount: half of main hit (rounded down).
            const int stompDamage = std::max(0, mainHit / 2);
```

Heal Others, `CombatSystem.cpp:505-506`:

```cpp
            // Wiki: Heal Others heals by up to 4 HP (not above max HP).
            (void)UnitSystem::heal(game, uid, 4);
```

**Cross-check verdict:** this is the canonical wiki combat formula, faithfully transcribed, including the `4.5` constant, the HP-ratio scaling on both sides, and the no-retaliation-on-kill rule. **Trust it as a cross-check.** The `defenceBonus` table is the part to verify against the wiki most carefully — the Fortify-gating of the city bonus and the exact `+2.5` wall increment are the kind of detail that changes between patches.

### 5.2 Technology cost

`src/content/tech/TechDB.cpp:91-106`, entire function:

```cpp
// Wiki formula:
// cost = tier * cities + 4
// Literacy: -33% (ceil)
int TechDB::calculatePrice(TechId id, int cityCount, bool hasLiteracy) {
    const TechData& t = getTech(id);

    int tier = static_cast<int>(t.tier);
    int baseCost = tier * cityCount + 4;

    if (!hasLiteracy)
        return baseCost;

    // Literacy discount = 33%, rounded UP
    double discounted = baseCost * (2.0 / 3.0);
    return static_cast<int>(std::ceil(discounted));
}
```

`TechTier` values are the literal tier numbers (`TechDB.h:16-20`): `Tier1 = 1, Tier2 = 2, Tier3 = 3`. So the tier multiplier is 1/2/3.

"Literacy" is wired to the **Philosophy** tech, `src/systems/TechSystem.cpp:32-33` (and identically at line 42, and again in `bindings.cpp:826`):

```cpp
    const bool hasLiteracy = PlayerSystem::hasTech(game, pid, TechId::Philosophy);
    const int price = TechDB::calculatePrice(tech, cityCount, hasLiteracy);
```

`cityCount` is `PlayerSystem::getCities(game, pid).size()` — cities *owned*, not cities founded.

**This is the formula most worth cross-checking hard.** `tier * cities + 4` is a specific claim and it is the single highest-leverage number in the game's economy. If the wiki says otherwise (e.g. a different additive constant, or `cities + 1`), everything downstream of tech pacing is wrong. Note also that the discount is applied to the *already-computed* base, ceil'd — the order of rounding matters and is worth confirming.

### 5.3 City population thresholds and level-up rewards

Threshold, `src/content/settlements/City.cpp:26-29`:

```cpp
uint8_t City::populationNeededToLevelUp() const {
    return getLevel()+1;

}
```

Level-up mechanics, `City.cpp:31-43` and `66-75`:

```cpp
bool City::levelUp() {
    uint8_t level = getLevel();
    uint8_t population = getPopulation();
    uint8_t populationNeeded = populationNeededToLevelUp();

    if (population < populationNeeded) {
        return false;
    }

    setPopulation(population-populationNeeded);
    setLevel(level+1);
    return true;
}

bool City::addPopulation(uint16_t v) {
    population += v;

    if (population >= populationNeededToLevelUp()) {
        return levelUp();
    }


    return true;
}
```

So population is *progress toward the next level* (carry-over preserved), and level `n` → `n+1` costs `n+1` population. Unit capacity, `City.cpp:46-48`:

```cpp
uint8_t City::maxUnitCapacity() const {
    return getLevel()+1;
}
```

Reward options by level, `src/systems/CityRewardSystem.cpp:32-38`:

```cpp
static inline bool isValidChoiceForLevel(CityUpgradeChoice c, uint8_t newLevel) {
    if (newLevel == 2) return c == CityUpgradeChoice::Workshop || c == CityUpgradeChoice::Explorer;
    if (newLevel == 3) return c == CityUpgradeChoice::CityWall || c == CityUpgradeChoice::Resources;
    if (newLevel == 4) return c == CityUpgradeChoice::PopulationGrowth || c == CityUpgradeChoice::BorderGrowth;
    if (newLevel >= 5) return c == CityUpgradeChoice::Park || c == CityUpgradeChoice::SuperUnit;
    return false;
}
```

Reward effects, `CityRewardSystem.cpp:150-213` (condensed to the numeric payloads):

```cpp
        case CityUpgradeChoice::Workshop: {
            // Workshop: +1 star/turn w tym mieście.
            CitySystem::setCityWorkshopEnabled(game, cityId, true);
        } break;

        case CityUpgradeChoice::Explorer: {
            VisionSystem::doExplorer(game, pid, pos);
        } break;

        case CityUpgradeChoice::CityWall: {
            CitySystem::setCityWallEnabled(game, cityId, true);
        } break;

        case CityUpgradeChoice::Resources: {
            // Resources: natychmiast +5 stars dla gracza.
            PlayerSystem::addStars(game, owner, 5);
        } break;

        case CityUpgradeChoice::PopulationGrowth: {
            // +3 population natychmiast.
            CitySystem::addPopulation(game, cityId, 3);
        } break;

        case CityUpgradeChoice::BorderGrowth: {
            const SettlementId sid = static_cast<SettlementId>(cityId);

            CitySystem::claimFreeTerritoryRadius(game,cityId,sid,pos,2);
        } break;

        case CityUpgradeChoice::Park: {
            // Park: +1 star/turn i punkty.
            CitySystem::addCityParkCount(game, cityId);
        } break;

        case CityUpgradeChoice::SuperUnit: {
            ... UnitSpawnSystem::spawnUnitForced(game, map, UnitType::Giant, owner, pos,
                                                 /*canActImmediately=*/false, /*makeVeteran=*/false);
```

Monument trigger, `CitySystem.cpp:105-114`:

```cpp
    // Monument unlock tylko przy przejściu na 5+
    if (oldLevel < 5 && newLevel >= 5) {
        MonumentSystem::onCityReachedLevel5(game, c->getPos());
    }

    // If the city reached a new level threshold, force reward selection via the pending queue.
    if (newLevel > oldLevel) {
        const PlayerId owner = static_cast<PlayerId>(c->getOwnerId());
        game.enqueuePendingCityUpgrade(owner, cityId);
    }
```

**⚠ Bug worth noting: `addPopulation` only levels up once per call.** If a Forge grants +6 population to a level-1 city, `levelUp()` fires exactly once (1→2, consuming 2) and the remaining 4 population sits above the level-3 threshold of 3 without triggering. The live game cascades. Also `levelUp()` reads `uint8_t population = getPopulation()` from an `int16_t` field — silently truncates above 255. **Do not trust PolyEnv's city-growth *dynamics*; the *thresholds* are the only reusable part.**

### 5.4 Star income

Per-city production, `src/content/settlements/City.cpp:54-64`, entire function:

```cpp
uint8_t City::getStarsPerRound() const {
    int perRound = static_cast<int>(getLevel()) + static_cast<int>(starsPerRound);
    if (isCapitalCity()) {
        perRound += 1;
    }
    if (hasWorkshopEnabled()) {
        perRound += 1;
    }
    perRound += static_cast<int>(parkCount);
    return static_cast<uint8_t>(std::clamp(perRound, 0, 255));
}
```

So: **city income = level + capital(+1) + workshop(+1) + parkCount** (`starsPerRound` is a manual override field, 0 by default).

Empire total and collection, `src/systems/StarsSystem.cpp:93-129`:

```cpp
int StarsSystem::calcIncomeForPlayer(const Game& game, PlayerId pid) {
    int income = 0;

    for (CityId cid : PlayerSystem::getCities(game, pid)) {
        if (!CitySystem::cityExists(game, cid)) continue;
        income += static_cast<int>(CitySystem::getCityStarsPerRound(game, cid));
        income += marketIncomeForCity(game, pid, cid);
    }

    return std::max(0, income);
}

void StarsSystem::applyIncomeForPlayer(Game& game, PlayerId pid) {
    int income = 0;

    for (CityId cid : PlayerSystem::getCities(game, pid)) {
        if (!CitySystem::cityExists(game, cid)) continue;

        // No income if city is under siege (enemy unit inside the city)
        if (CitySystem::isCityUnderSiege(game, cid)) {
            continue;
        }

        // No income if city was infiltrated; clear infiltration after income collection attempt.
        if (CitySystem::getCityIsInfiltrated(game, cid)) {
            (void)CitySystem::setCityIsInfiltrated(game, cid, false);
            continue;
        }

        income += static_cast<int>(CitySystem::getCityStarsPerRound(game, cid));
        income += marketIncomeForCity(game, pid, cid);
    }

    if (income > 0) {
        PlayerSystem::addStars(game, pid, income);
    }
}
```

Market income, `StarsSystem.cpp:54-66`:

```cpp
    int unique = 0;
    if (hasForge) ++unique;
    if (hasSawmill) ++unique;
    if (hasWindmill) ++unique;

    // Current Polytopia (Path of the Ocean):
    // 2 stars per UNIQUE adjacent production building type (Forge/Sawmill/Windmill).
    // Doubled if adjacent to a Port.
    int spt = 2 * unique;
    if (hasPort && spt > 0) spt *= 2;

    // Max possible is Forge+Sawmill+Windmill (=6), doubled by Port (=12).
    return std::clamp(spt, 0, 12);
```

Income is applied at **start of turn**, skipped on turn 0 — `src/systems/TurnSystem.cpp:24-32`:

```cpp
void TurnSystem::startTurn(Game& game) {
    const PlayerId pid = game.getCurrentPlayerId();


    if (game.getTurnNumber()!=0) {
        applyIncomeForCurrentPlayer(game,pid);
        MonumentSystem::onStarsUpdated(game,pid);

    }
```

**⚠ Contradiction inside PolyEnv itself.** `StarsSystem::marketIncomeAt` computes market income as `2 × unique_adjacent_types`, capped 12, explicitly citing "Path of the Ocean." But `BuildingSystem::getBuildingLevelForTypeAt` (`BuildingSystem.cpp:364-387`) computes a Market *level* as the **sum of adjacent booster levels**, citing the same wiki page, and that level is used nowhere for income. Two mutually inconsistent Market models coexist. **Do not trust PolyEnv's Market numbers — resolve them from the wiki directly.**

---

## 6. Action space representation

**Flat enum over a fixed `W×H` board. Not a SELECT/VERB/TARGET phase machine.**

`src/ai/ActionSpace.cpp:15-26` lays out contiguous blocks:

```cpp
    offEndTurn_ = 0;
    offBuyTech_ = offEndTurn_ + 1;
    offMove_ = offBuyTech_ + static_cast<size_t>(kTechCount);
    offAttack_ = offMove_ + static_cast<size_t>(cellCount_) * static_cast<size_t>(cellCount_);
    offHeal_ = offAttack_ + static_cast<size_t>(cellCount_) * static_cast<size_t>(cellCount_);
    offUnitUpgrade_ = offHeal_ + static_cast<size_t>(cellCount_);
    offTileAction_ = offUnitUpgrade_ + static_cast<size_t>(kUnitUpgradeCount) * static_cast<size_t>(cellCount_);
    offBuild_ = offTileAction_ + static_cast<size_t>(kTileActionCount) * static_cast<size_t>(cellCount_);
    offSpawn_ = offBuild_ + static_cast<size_t>(kBuildingCount) * static_cast<size_t>(cellCount_);
    offUpgradeCity_ = offSpawn_ + static_cast<size_t>(kSpawnUnitCount) * static_cast<size_t>(cellCount_);
    totalSize_ = offUpgradeCity_ + static_cast<size_t>(kCityUpgradeCount) * static_cast<size_t>(cellCount_);
```

Block sizes (`ActionSpace.h:64-69`):

| Block | Size |
|---|---|
| EndTurn | 1 |
| BuyTech | 25 |
| Move | `cells²` |
| Attack | `cells²` |
| Heal | `cells` |
| UnitUpgrade | 5 × `cells` |
| TileAction | 14 × `cells` |
| Build | 16 × `cells` |
| SpawnUnit | **90 × `cells`** |
| UpgradeCity | 8 × `cells` |

`kSpawnUnitCount = static_cast<int>(UnitType::GiantSuper)` — i.e. **90**, because `UnitType` enum values are sparse (`Warrior=1 … GiantSuper=90`). So the spawn block reserves a slot for every integer 1..90 including the ~37 unused gaps.

**Concrete size:** an 11×11 map (`cells = 121`) yields `1 + 25 + 2×14,641 + 121 + 5×121 + 14×121 + 16×121 + 90×121 + 8×121` ≈ **45,000 actions**. A 16×16 map (`cells = 256`) yields ≈ **165,000**. A 30×30 "Massive" map yields **~1.7 million**.

This is a poor design and a direct argument for our phase-machine approach:

- `cells²` for Move and Attack dominates and scales quadratically in map area, i.e. **quartically in map width**.
- 90 spawn slots per cell when only **9 units are ever trainable** wastes 90% of the largest non-quadratic block.
- The policy head must emit a distribution over ~10⁵–10⁶ logits, of which typically a few dozen are legal.

### Legality masking

Masking is **generate-then-encode**, `ActionSpace.cpp:219-228`, entire function:

```cpp
std::vector<uint8_t> ActionSpace::legalMask(const Game& game, const std::vector<Action>& legalActions) const {
    std::vector<uint8_t> mask(totalSize_, 0);
    for (const Action& a : legalActions) {
        const std::optional<size_t> id = encode(game, a);
        if (id && *id < mask.size()) {
            mask[*id] = 1;
        }
    }
    return mask;
}
```

The legal set is produced elsewhere (`GameStateAdapter` / `bindings.cpp`) by calling every `can*` predicate on `Game`, then each legal action is encoded into its flat index. There is **no incremental mask maintenance and no structural mask** — the dense `vector<uint8_t>` of size `totalSize_` is allocated fresh on every call. On a 16×16 map that is a 165 KB zeroed allocation per masked step.

In practice the Python API sidesteps this: `env.legal_action_ids_fast()` and `legal_param_actions()` return the **sparse legal list** directly, and `VectorGameEnv` uses fixed padded rows (`max_actions=512`) with a separate `action_mask`. So the flat `ActionSpace` is largely vestigial — the real interface is a variable-length list of legal actions with rich metadata (`cost_stars`, `affordable`, `damage_dealt`, `damage_received`, `source_index`, `target_index`).

**That richer per-action metadata is the genuinely good idea here** and worth stealing: exposing predicted damage and affordability alongside each legal action makes both bot authoring and model input far easier.

### Decode is lossy

`ActionSpace::decode` (lines 129-166) recovers the acting unit by looking up whatever unit currently stands on the source cell:

```cpp
        a.unit = game.getMap().unitOn(fromPos);
```

So an action id is only meaningful against the exact state it was encoded from. Encode/decode is not a bijection, and a decoded action can silently refer to a different unit. For a phase machine this class of bug disappears by construction.

---

## 7. Fog of war / partial observability

### Representation

Per-tile **reveal bitmask**, one bit per player, in `VisibilityEnum` (`src/world/terrain/VisibilityEnum.h:12-39`):

```cpp
enum class VisibilityEnum : uint16_t {
    None = 0
};
...
inline void reveal(VisibilityEnum& v, PlayerIndex player) {
    v |= static_cast<VisibilityEnum>(uint16_t(1) << player);
}

inline bool isRevealed(VisibilityEnum v, PlayerIndex player) {
    return (static_cast<uint16_t>(v) & (uint16_t(1) << player)) != 0;
}
```

**Critical semantic: this is "explored," not "currently visible." Bits are only ever set, never cleared.** There is no code path anywhere that un-reveals a tile. So PolyEnv models Polytopia's *permanent* fog lift correctly (matching the real game, where explored terrain stays visible) — but it means the tile bitmask alone cannot express "I explored this tile but no longer see what's on it."

`Game` maintains a redundant second structure, `visibleTilesByPlayer` (a `Pos` list) plus `visibleLookupByPlayer` (a dense `uint8_t` lookup), populated by `noteTileVisible` (`Game.cpp:281-294`). These are also append-only and duplicate the tile bitmask — pure denormalization for fast iteration.

### Reveal sources

`VisionSystem::revealArea(game, pid, center, range, source)` is the single choke point (`VisionSystem.cpp:62-128`). `RevealSource` is one of `Initial`, `Unit`, `Explorer`. Square (Chebyshev) reveal, not circular.

Unit vision, `VisionSystem.cpp:35-48`:

```cpp
void VisionSystem::revealFromUnit(Game& game, UnitId uid) {
    if (!UnitSystem::unitExists(game, uid)) return;

    const PlayerId pid = UnitSystem::getOwnerId(game, uid);
    const Pos p = UnitSystem::getPos(game, uid);

    int r = UnitSystem::getVisionRange(game, uid);
    const Tile& t = game.getMap().at(p);
    if (t.getBaseTerrain() == BaseTerrainEnum::Mountain) {
        r = std::max(r, 2);
    }

    revealArea(game, pid, p, r, RevealSource::Unit);
}
```

Base vision 1, Scout-skill units 2 (enforced in `GameDataSystem.cpp:163-165`: `Scout` skill implies `visionRange >= 2`), mountain standing raises to 2.

Initial reveal is radius 2 around the capital (`Game.cpp:426-431`), and **explicitly skips map corners** (`VisionSystem.cpp:78-81`) so lighthouses can't be discovered on turn 0.

### First-contact reward

Meeting an opponent grants stars, `VisionSystem.cpp:370-374`:

```cpp
int VisionSystem::meetingReward(int enemyScore) {
    const int thousands = (enemyScore + 999) / 1000; // ceil
    int stars = 1 + 2 * thousands;
    return std::min(stars, 11);
}
```

Triggered on newly revealing a tile containing an enemy unit or enemy city (`VisionSystem.cpp:101-125`).

### Hidden units

`Hide` (Cloak) is modelled: `unitHasActivatedHide` (`CombatSystem.cpp:28-33`) treats a Cloak as hidden if its last move direction is nonzero, and hidden units are excluded from `attackable()` and from being attacked. They also do not project zone-of-control (`MovementSystem.cpp:141`). Crude — hiding is inferred from movement rather than being explicit state — but present.

### Tech-gated resource visibility

A genuinely nice touch, `Game.cpp:264-279`, entire function:

```cpp
ResourcesEnum Game::getVisibleResourceForPlayer(PlayerId pid, Pos p) const {
    if (pid == kNoPlayer || !map.inBounds(p) || static_cast<size_t>(pid) >= players.size()) {
        return ResourcesEnum::None;
    }

    const ResourcesEnum resource = map.at(p).getResource();
    const Player& player = players[static_cast<size_t>(pid)];
    if (resource == ResourcesEnum::Metal && !player.hasTech(TechId::Climbing)) {
        return ResourcesEnum::None;
    }
    if (resource == ResourcesEnum::Crops &&
        !player.hasTech(TechId::Organization) && !player.hasTech(TechId::Farming)) {
        return ResourcesEnum::None;
    }
    return resource;
}
```

Metal is hidden without Climbing; Crops without Organization or Farming. This matches the live game and is exactly the kind of detail that is easy to miss. **Worth cross-checking whether the full live-game list is longer** (e.g. does Fish require Fishing?).

### Observation API

`env.observation(visible_only=True, hidden_value=-1)` masks unseen tiles to a sentinel; `env.full_map_numpy()` returns ground truth for labelling/debugging. `BeliefWorldBuilder` (`src/runtime/BeliefWorldBuilder.cpp`, 331 lines) constructs determinized worlds from a player's belief for ISMCTS, and `SelfPlayPool` accepts `[B, P, K, tiles, 23]` belief particles. `ObservedEventJournal` records fog-safe visible events.

This is the most seriously engineered part of the repo and the partial-observability plumbing is well beyond what the rules layer deserves.

---

## 8. Map generation

`src/world/MapGenerator.cpp`, 593 lines. Attributed in the README to `QuasiStellar/Polytopia-Map-Generator` — so it is a reimplementation of a *third party's reverse-engineering* of Polytopia map gen, which is two derivations away from ground truth.

Two map types only: `Lakes` and `Drylands` (`MapType` enum). Square maps, `size × size`.

### Pipeline

1. **Capitals first, before terrain** (lines 156-204). The map is partitioned into a `domainsPerAxis²` grid — `2×2` for ≤4 players, `3×3` for ≤9, `4×4` otherwise — domains shuffled, one capital per domain. For ≤4 players capitals are corner-biased into a 3×3 window with the corner-diagonal tile excluded so the corner lighthouse isn't revealed on turn 0.
2. **Suburbs and pre-terrain villages** (206-252). `kMinSuburbs=1`, `kMaxSuburbs=2` per capital, placed in ring radius 4, at Chebyshev distance >2 from other settlements. Village budget from `gridSlots = (size/3)²`, with `kPreTerrainVillageDensity = 0.30f`.
3. **Water via smoothed noise** (254-311). Uniform random field, `kLakesSmoothingPasses = 2` box blurs with dry map edges, then the lowest-valued cells become water. Target water fraction sampled in `[0.25, 0.28]` of tiles. Capital tiles and their 4-neighbours are locked to land.
4. **Post-terrain villages** (332-346).
5. **Biome assignment by alternating flood fill** (348-412). Each player expands one tile per round from its border set, random pick, shuffled turn order — so seams are irregular. Disconnected islands go to the nearest capital by Chebyshev distance.
6. **Terrain type roll per land tile** (414-436):

```cpp
        constexpr float kBaseMountain = 0.14f;
        constexpr float kBaseForest = 0.38f;
        constexpr float kBaseField = 0.48f;
        const float mountain = std::clamp(kBaseMountain * probabilities.mountain, 0.0f, 1.0f);
        const float forestBeforeModifier = kBaseForest + (kBaseMountain - mountain) *
            (kBaseForest / (kBaseForest + kBaseField));
        const float forest = std::clamp(forestBeforeModifier * probabilities.forest, 0.0f, 1.0f - mountain);
```

7. **Shallow vs. deep water** (438-449): a water tile is `Water` (shallow) if 4-adjacent to land, else `Ocean`.
8. **Resources within radius 2 of a settlement** (451-505), with an inner ring (radius 1) at higher rates. Rates listed in §10.9.
9. **Guaranteed capital resources** (507-538): Bardur 2 Animal/Forest, Imperius 2 Fruit/Land, Kickoo 2 Fish/Water, Zebasi 1 Crops/Land, Aquarion 2 Fish/Water on Drylands.
10. **Ruins and starfish** (541-576). Ruin count by map size (§10.9); starfish `= waterTileCount / 25`.
11. **Corners forced to empty land** (578-587).

### Determinism

Single `std::mt19937` seeded from `Game::worldSeed`, threaded through the whole pipeline. `MapGenerator::generate` returns the capital positions rather than mutating shared state — an explicit fix for concurrent map generation across `Game` instances (comment at `Game.cpp:340-341`).

### Assessment

Structurally plausible and clearly the product of someone reading the wiki's map-generation page carefully. But: **square maps only, two map types, Chebyshev-square geometry**, and the tuning constants are the author's own admission of guesswork — see the comment at lines 26-31:

```cpp
// The documented 50% fish rate applies only to shallow water.  Ocean fish
// are allowed near a city's territory (and its immediate outer ring), but at
// a deliberately lower density.  The public documentation does not define a
// numeric ocean rate, so keep this separate and easy to tune.
```

**Treat map-gen numbers as inspiration, not as spec.** They are third-hand and partly invented. The *pipeline order* (capitals → villages → water → biomes → terrain → resources → ruins) is the transferable insight.

---

## 9. Tests — what they assert (read, not run)

**No C++ tests exist.** `grep` for `test|Catch|gtest|doctest` in `CMakeLists.txt` returns nothing. All testing is Python-level integration testing through the pybind11 layer: 17 files, 4,284 lines, in `python/tests/`.

Of these, only two are rules-correctness tests. The rest cover API plumbing.

| File | Lines | What it covers |
|---|---|---|
| `test_known_values.py` | 423 | **Rules**: damage formula, unit costs, building costs |
| `test_polytopia_mechanics.py` | 721 | **Rules-adjacent**: damage preview vs. actual, income accounting, cost deduction, turn alternation |
| `test_self_play_pool.py` | 813 | RL pool API |
| `test_vector_env.py` | 475 | batched env tensors |
| `test_mcts_pool.py` | 399 | MCTS pool API |
| `test_tile_predictions.py` | 317 | belief/hidden-tile prediction API |
| `test_golden_trajectory.py` | 259 | replays `golden_trajectory_seed42.json` |
| others | ~880 | tribes/replay/player-view/model-request/visible-events APIs |

### `test_known_values.py` — the only genuine rules oracle

Its stated strategy (docstring, lines 1-25) is three-pronged: hardcoded wiki values, an **independent Python reimplementation of the damage formula**, and concrete known cases.

The independent reimplementation (lines 42-76) is the strongest artifact in the repo for our purposes — it restates the formula without reference to the C++:

```python
    atk_force = atk * (atk_hp / atk_max_hp)
    def_force = def_ * (def_hp / def_max_hp) * terrain_bonus
    total = max(1e-9, atk_force + def_force)

    dmg_to_def = max(0, _llround((atk_force / total) * atk * 4.5))
    dmg_to_atk = max(0, _llround((def_force / total) * def_ * 4.5))
```

with `_llround(x) = math.floor(x + 0.5)` to match C++ `std::llround` rather than Python's banker's rounding — a subtlety the author correctly identified and documented.

**Six hand-computed damage cases** are asserted (lines 104-134). These are directly reusable as our own test vectors:

| Attacker (atk, hp/max) | Defender (def, hp/max) | Bonus | Dealt | Retaliation |
|---|---|---|---|---|
| 2.0, 10/10 | 2.0, 10/10 | 1.0 | 5 | 5 |
| 2.0, 10/10 | 2.0, 5/10 | 1.0 | 6 | 0 (kill) |
| 2.0, 5/10 | 2.0, 10/10 | 1.0 | 3 | 6 |
| 2.0, 10/10 | 1.0, 10/10 | 1.0 | 6 | 2 |
| 2.0, 10/10 | 3.0, 10/10 | 1.0 | 4 | 8 |
| 3.0, 10/10 | 2.0, 10/10 | 1.0 | 8 | 4 |

### But the tests are far weaker than they look

Three structural problems:

**(a) The formula tests never actually test the engine.** `test_formula_python_implementation_correct` asserts the *Python* implementation against the *Python*-derived expectations. It is a tautology. The one test that touches C++, `test_engine_matches_python_formula_for_attacks` (lines 170-228), degrades to:

```python
                    assert engine_dealt >= 0, f"Ujemne damage_dealt: {engine_dealt}"
                    assert engine_dealt <= 23, (
                        f"damage_dealt={engine_dealt} przekracza teoretyczne maximum"
                        "(atak=5 * 4.5 = 22.5 → 23)"
                    )
```

That is a range check, not an equality check. The comment admits why: *"Nie znamy atk/def z legal_param_actions, tylko HP"* — the API doesn't expose attack/defense, so it cannot compute the expected value. **The engine's damage formula is not verified against the independent implementation anywhere.**

**(b) Pervasive `pytest.skip` on missing data.** Every cost test follows this shape:

```python
        costs = self._collect_spawn_costs(max_seeds=50)
        if "catapult" not in costs:
            pytest.skip("Catapult nie pojawił się w testowych seedach.")
```

Costs are harvested by enumerating legal actions across seeds at turn 0–3. A Catapult requires Mathematics, which no tribe starts with, so `catapult` **can never appear** — that test is a permanent skip. Likewise Knight, Swordsman (except Vengir), MindBender (except Ai-Mo). A silently-skipping test suite reports green while asserting nothing.

**(c) The building-cost table in the test contradicts the engine.** `test_known_values.py:385-395` declares, sourced to the wiki:

```python
KNOWN_BUILDING_COSTS: dict[str, int] = {
    # Źródło: https://polytopia.fandom.com/wiki/Buildings
    "farm":        5,
    "mine":        5,
    "lumber_hut":  2,
    "forge":       5,  # Forge (produkcja)
    "windmill":    5,
    "sawmill":     5,
    "port":        10,
    "market":      6,
}
```

The engine's `BuildingDB.cpp` says **LumberHut 3, Port 7, Market 5**. Three of eight disagree. The tests do not fail because those buildings never become legal within three turns of a fresh game, so every one of those parametrized cases skips.

This is the most important single finding in §9: **PolyEnv contains two mutually contradictory sets of "wiki" building costs and its own test suite cannot tell.** Port 10 / Market 6 are the *pre-Path-of-the-Ocean* values; Port 7 / Market 5 are post-rework. The test table is stale relative to the engine, and neither has been validated. See §10.4.

### `test_polytopia_mechanics.py`

Better, because it asserts engine-internal consistency rather than external truth:

- `test_damage_preview_matches_actual_dealt` / `_received` — the `legal_param_actions()` damage preview must equal the damage actually applied by `step_fast`. Genuine and useful.
- `test_killing_blow_produces_zero_retaliation` — asserts the no-retaliation-on-kill rule.
- `test_end_turn_income_equals_next_turn_star_income` — `stars_after == stars_at_end + predicted_income`. This is the strongest economic test in the repo.
- `test_buy_tech_deducts_exact_cost`, `test_spawn_unit_deducts_exact_cost` — `delta_stars == -cost`.
- `test_unit_cannot_attack_twice_in_same_turn`, `test_two_player_strict_alternation`, `test_end_turn_always_legal`.
- Determinism: `test_same_seed_same_trajectory`, `test_clone_identical_to_original`, `test_clone_independent_from_original`.

All of these are **self-consistency** tests. None validates a number against the live game. `test_golden_trajectory.py` replaying `golden_trajectory_seed42.json` is a pure regression lock — it pins current behavior, correct or not.

**Net assessment:** the test suite is a competent *API and regression* suite and a **near-useless rules-correctness suite**. It cannot detect that the engine's tech-cost formula or building costs are wrong, and by construction it never has.

---

## 10. Every concrete rule number PolyEnv encodes

This is the cross-check table. Column **Trust** is my read on whether the number is worth using as corroboration for wiki research:
**A** = independently corroborated inside the repo or structurally load-bearing, treat as strong corroboration ·
**B** = plausible, single-sourced, verify against wiki ·
**C** = contradicted internally, stale, or invented — do not trust.

### 10.1 Unit stats — regular land units

Source: `data/Units.json`. Defaults if a field is omitted: `maxHealth 10, attack 1.0, defense 1.0, movePoints 1, range 1, cost 0, visionRange 1, veteranEligible true`.

| Unit | Cost | HP | Atk | Def | Move | Rng | Vis | Tech | Skills | Trust |
|---|---|---|---|---|---|---|---|---|---|---|
| Warrior | 2 | 10 | 2.0 | 2.0 | 1 | 1 | 1 | — | Dash, Fortify | **A** |
| Archer | 3 | 10 | 2.0 | 1.0 | 1 | 2 | 1 | Archery | Dash, Fortify | **A** |
| Defender | 3 | 15 | 1.0 | 3.0 | 1 | 1 | 1 | Strategy | Fortify | **A** |
| Rider | 3 | 10 | 2.0 | 1.0 | 2 | 1 | 1 | Riding | Dash, Escape, Fortify | **A** |
| Swordsman | 5 | 15 | 3.0 | 3.0 | 1 | 1 | 1 | Smithery | Dash | **A** |
| MindBender | 5 | 10 | 0.0 | 1.0 | 1 | 1 | 1 | Philosophy | Heal, Convert, Stiff | **A** |
| Catapult | 8 | 10 | 4.0 | 0.0 | 1 | 3 | 1 | Mathematics | Stiff | **A** |
| Knight | 8 | 10 | 3.5 | 1.0 | 3 | 1 | 1 | Chivalry | Dash, Persist, Fortify | **A** |
| Cloak | 8 | 5 | 0.0 | 0.5 | 2 | 1 | 2 | Diplomacy | Hide, Creep, Infiltrate, Dash, Stiff, Scout, StaticSkill | **B** |

All nine are cross-verified twice inside the repo: `data/Units.json` ↔ `test_known_values.py:84-93` (`KNOWN_UNIT_STATS`) ↔ `ScoreSystem::costStarsByUnitType`. All three agree on cost for these nine. **This block is the most trustworthy data in PolyEnv.** Cloak drops to B only because its 5 HP / 0.5 defense are unusual enough to be worth an explicit wiki check.

### 10.2 Unit stats — naval line (the modern rework)

| Unit | Cost | HP | Atk | Def | Move | Rng | Vis | Tech | Skills | Trust |
|---|---|---|---|---|---|---|---|---|---|---|
| Raft | 0 | **0** | 0.0 | 1.0 | 2 | 1 | 1 | Fishing | WaterOnly, Carry, StaticSkill, Stiff | **A** |
| Scout | 5 | **0** | 2.0 | 1.0 | 3 | 2 | 2 | Sailing | WaterOnly, Dash, Carry, Scout, StaticSkill | **A** |
| Rammer | 5 | **0** | 3.0 | 3.0 | 3 | 1 | 1 | Ramming | WaterOnly, Dash, Carry, StaticSkill | **A** |
| Bomber | 15 | **0** | 3.0 | 2.0 | 2 | 3 | 1 | Navigation | WaterOnly, Carry, Splash, Stiff, StaticSkill | **A** |

`maxHealth: 0` is deliberate: naval units inherit the carried land unit's HP. `UnitUpgradeSystem::upgradeRaftTo*` (lines 187-192, 243-247, 295-300) explicitly preserves `oldMaxHp`/`oldHp` across the conversion and only falls back to the template value if the preserved HP is invalid.

**This is the current live-game naval model** — Raft → Scout / Rammer / Bomber, HP carried from the embarked unit, upgrade in own territory paying the target unit's cost. It is *not* the legacy Boat/Ship/Battleship line. See the verdict.

Upgrade gates (`UnitUpgradeSystem.cpp:63-149`): Scout needs Sailing + 5★, Rammer needs Ramming + 5★, Bomber needs Navigation + 15★; all three require the raft to stand in the player's own city territory.

### 10.3 Unit stats — non-trainable / spawned

| Unit | Cost | HP | Atk | Def | Move | Rng | Vis | Skills | Trust |
|---|---|---|---|---|---|---|---|---|---|
| Dagger | 0 | 10 | 2.0 | 2.0 | 1 | 1 | 1 | Dash, Surprise, Independent, StaticSkill | **B** |
| Pirate | 0 | 10 | 2.0 | 2.0 | 1 | 1 | 1 | WaterOnly, Carry, Dash, Surprise, Independent, StaticSkill | **B** |
| Giant | 0 | 40 | 5.0 | 4.0 | 1 | 1 | 1 | StaticSkill | **A** |
| GiantSuper | 0 | 40 | 5.0 | 4.0 | 1 | 1 | 1 | StaticSkill | **C** — duplicate of Giant, unused |
| Juggernaut | 0 | 40 | 4.0 | 4.0 | 2 | 1 | 1 | WaterOnly, Carry, Stiff, Stomp, StaticSkill | **B** — never instantiated |
| Dinghy | 0 | 5 | 0.0 | 0.5 | 2 | 1 | 1 | WaterOnly, Carry, Hide, Creep, Infiltrate, Stiff | **B** — never instantiated |
| Bunny | 0 | 20 | 5.0 | 1.0 | 1 | 1 | 1 | Independent | **C** — never instantiated |
| Bunta | 0 | 20 | 5.0 | 1.0 | 1 | 1 | 1 | Independent | **C** — duplicate of Bunny, never instantiated |

`Giant` HP 40 / atk 5.0 / def 4.0 is corroborated indirectly: `test_known_values.py:216-218` uses "max attack is 5 (Giant)" to bound damage at 23.

### 10.4 Unit stats — special tribes (data only, zero behavior)

Listed for completeness. **All rated C for behavioral purposes** — these stat blocks are never used by any code path since special tribes are unplayable. As raw *numbers* they may still be a useful wiki cross-check, so they are transcribed faithfully.

**Aquarion (12):**

| Unit | Cost | HP | Atk | Def | Move | Rng | Tech |
|---|---|---|---|---|---|---|---|
| Mermaid | 2 | 10 | 2.0 | 2.0 | 1 | 1 | — |
| AquaticAmphibian | 3 | 10 | 2.0 | 1.0 | 2 | 1 | Riding |
| MermaidArcher | 3 | 10 | 2.0 | 1.0 | 1 | 2 | Archery |
| MermaidDefender | 3 | 15 | 1.0 | 3.0 | 1 | 1 | Strategy |
| Swordsmaid | 5 | 15 | 3.0 | 3.0 | 1 | 1 | — |
| Scuba | 8 | 5 | 0.0 | 0.5 | 2 | 1 | Diplomacy |
| Siren | 8 | 10 | 0.0 | 1.0 | 1 | 1 | Philosophy |
| Shark | 8 | 10 | 3.5 | 2.0 | 3 | 1 | — |
| YellyBelly | 8 | 20 | 0.0 | 2.0 | 2 | 1 | Navigation |
| Puffer | 8 | 10 | 4.0 | 0.0 | 2 | 3 | Mathematics |
| TridentionAq | 8 | 10 | 2.5 | 1.0 | 2 | 2 | Chivalry |
| CrabAq | 0 | 40 | 4.0 | 5.0 | 2 | 1 | — |

**Elyrion (4):** Polytaur 3★/15/3.0/1.0/1/1 · DragonEgg 0★/10/0.0/2.0/1/1 · BabyDragon 0★/15/3.0/3.0/2/1 · FireDragon 0★/20/4.0/3.0/3/2

**Polaris (5):** IceArcher 3★/10/1.0/1.0/1/2 (Archery) · BattleSled 5★/15/3.0/2.0/2/1 · Mooni 5★/10/0.0/2.0/1/1 · IceFortress 15★/20/4.0/3.0/1/2 · Gaami 0★/30/4.0/3.0/1/1

**Cymanti (12):** Hexapod 3★/5/3.0/1.0/2/1 (Riding) · Kiton 3★/15/1.0/3.0/1/1 (Strategy) · Phychi 3★/5/**0.7**/1.0/2/2 (Archery) · Shaman 5★/10/1.0/1.0/1/1 (Philosophy) · Raychi 5★/10/3.0/2.0/3/1 (Sailing) · Exida 8★/10/3.0/1.0/2/3 (Mathematics) · Doomux 10★/20/3.5/2.0/3/1 (Chivalry) · MothC 5★/10/0.0/**0.1**/2/1 (Diplomacy) · LarvaC 0★/10/2.0/2.0/1/1 · InsectEgg 0★/10/2.0/3.0/**0**/1 · Boomchi 5★/10/3.0/3.0/1/1 · LivingIsland 20★/20/4.0/4.0/2/1 (Navigation)

Note the two odd fractional values — Phychi `attack 0.7` and MothC `defense 0.1` — which are the only non-half values in the entire dataset and worth a specific wiki check if special tribes ever matter to us.

### 10.5 Technology tree — all 25

Source: `src/content/tech/TechDB.cpp:13-44`. Tier values are literal multipliers (1/2/3).

| Tech | Tier | Prerequisite | Trust |
|---|---|---|---|
| Climbing | 1 | — | **A** |
| Fishing | 1 | — | **A** |
| Hunting | 1 | — | **A** |
| Organization | 1 | — | **A** |
| Riding | 1 | — | **A** |
| Archery | 2 | Hunting | **A** |
| Ramming | 2 | Fishing | **A** |
| Farming | 2 | Organization | **A** |
| Forestry | 2 | Hunting | **A** |
| Free Spirit | 2 | Riding | **A** |
| Meditation | 2 | Climbing | **A** |
| Mining | 2 | Climbing | **A** |
| Roads | 2 | Riding | **A** |
| Sailing | 2 | Fishing | **A** |
| Strategy | 2 | Organization | **A** |
| Aquatism | 3 | Ramming | **B** |
| Chivalry | 3 | Free Spirit | **A** |
| Construction | 3 | Farming | **A** |
| Diplomacy | 3 | Strategy | **A** |
| Mathematics | 3 | Forestry | **A** |
| Navigation | 3 | Sailing | **A** |
| Philosophy | 3 | Meditation | **A** |
| Smithery | 3 | Mining | **A** |
| Spiritualism | 3 | Archery | **B** |
| Trade | 3 | Roads | **A** |

**5 / 10 / 10 across tiers.** The structure — including `Ramming` at tier 2 under Fishing and `Aquatism` at tier 3 under Ramming — is the **post-Path-of-the-Ocean tree**, not the legacy one. That is a strong positive signal about the repo's vintage.

Aquatism←Ramming and Spiritualism←Archery are marked B: they are the two branches most likely to have moved between patches, and unlike the others they aren't corroborated by a unit or building unlock elsewhere in the code.

**Tech cost:** `cost = tier × cityCount + 4`; with Philosophy ("Literacy"), `ceil(cost × 2/3)`. **Trust: B — this is the highest-leverage single formula in the file and it is single-sourced from one comment.** Verify against wiki before adopting.

Derived cost table (no Philosophy):

| Cities | Tier 1 | Tier 2 | Tier 3 |
|---|---|---|---|
| 1 | 5 | 6 | 7 |
| 2 | 6 | 8 | 10 |
| 3 | 7 | 10 | 13 |
| 4 | 8 | 12 | 16 |
| 5 | 9 | 14 | 19 |
| 8 | 12 | 20 | 28 |
| 10 | 14 | 24 | 34 |

With Philosophy, `ceil(× 2/3)`: e.g. 5 cities tier 3 → `ceil(19 × 0.6667)` = `ceil(12.67)` = **13**.

### 10.6 Buildings

Source: `src/content/buildings/BuildingDB.cpp:19-93`. `pop` = flat population granted on build (`BuildingInfo::populationGain`).

| Building | Cost | Tech | Terrain | Resource | Flat pop | Trust |
|---|---|---|---|---|---|---|
| Farm | 5 | Farming | Land | Crops | 2 | **B** |
| Mine | 5 | Mining | Mountain | Metal | 2 | **B** |
| LumberHut | **3** | Forestry | Forest | — | 1 | **C** — test says 2 |
| Port | **7** | Fishing | Water | — | 1 | **C** — test says 10 |
| Forge | 5 | Smithery | Land | (None) | 0 + adjacency | **B** |
| Sawmill | 5 | Mathematics | Land | (None) | 0 + adjacency | **B** |
| Windmill | 5 | Construction | Land | (None) | 0 + adjacency | **B** |
| Market | **5** | Trade | Land | (None) | 0 | **C** — test says 6 |
| 7 Monuments | 0 | — | Land\|Water | — | 3 | **B** |
| Lighthouse | — | — | — | — | — | **C** — declared, no DB entry, unbuildable |

**The three C-rated rows are the internal contradiction documented in §9.** `test_known_values.py:385-395` asserts LumberHut 2, Port 10, Market 6 against the same wiki page. Those tests never execute. Port 10 / Market 6 are legacy pre-rework values; Port 7 / Market 5 look post-rework. **LumberHut 3 vs 2 is unresolved by any evidence in the repo — flag it for the wiki spec as an open question.**

Adjacency population, `src/systems/BuildingSystem.cpp:111-126`:

| Building | Population gain |
|---|---|
| Sawmill | **+1 per adjacent friendly LumberHut** |
| Windmill | **+1 per adjacent friendly Farm** |
| Forge | **+2 per adjacent friendly Mine** |

Applied both ways — building a Mine next to an existing Forge retroactively grants +2 (`BuildingSystem.cpp:258-280`). Building level is clamped `[1, 8]` (`clampBuildingLevel`, line 89-92). Trust: **B**.

Placement constraints (`BuildingSystem::canBuild`, lines 144-204):

- Forge / Sawmill / Windmill / Market are **Field (Land) only**.
- **Only one Forge, one Sawmill, one Windmill per city** (`cityAlreadyHasBuilding`).
- Market requires ≥1 adjacent owned Forge/Sawmill/Windmill.
- Monuments require earned-and-not-yet-placed.
- Building consumes Fruit on the tile; Port consumes Fish; Sawmill consumes Animal (`BuildingSystem.cpp:216-231`).

Market star income (`StarsSystem.cpp:54-66`): **2 stars per unique adjacent production type**, doubled if adjacent to a Port, clamped to **12**. Trust: **C** — contradicted by `BuildingSystem::getBuildingLevelForTypeAt`, which computes Market level as the *sum* of adjacent booster levels and is used nowhere. Two incompatible Market models. Resolve from wiki.

### 10.7 Tile actions

Source: `src/systems/TileActionSystem.cpp` — each cost is a `static constexpr int kCost` duplicated between the `can*` and the executing function.

| Action | Cost | Tech | Terrain / Resource | Effect | Trust |
|---|---|---|---|---|---|
| Hunt | 2 | Hunting | Land or Forest + Animal | +1 pop, consume Animal | **B** |
| Organization (harvest Fruit) | 2 | Organization | Land + Fruit | +1 pop, consume Fruit | **B** |
| Fishing | 2 | Fishing | Water or Ocean + Fish | +1 pop, consume Fish | **B** |
| Clear Forest | **0** | Forestry | Forest, no building | Forest→Land, **+1 star** | **B** |
| Burn Forest | 5 | Construction | Forest, no building | Forest→Land + Crops | **B** |
| Grow Forest | 5 | Spiritualism | Land, no resource | Land→Forest | **B** |
| Destroy | 0 | Chivalry (bridge: Construction) | own territory, non-settlement | clears resource/building/bridge | **C** — see below |
| Build Road | 3 | Roads | Land or Forest, no unit | Road | **B** |
| Build Bridge | 5 | Roads | Water, land on opposite sides | Bridge | **B** |

Clear Forest paying **0 and refunding 1 star** is a net +1 — worth an explicit wiki check, that asymmetry is unusual.

Destroy is rated C: `TileActionSystem.cpp:294-297` gates bridges on Construction and everything else on Chivalry, with the comment *"Roads are intentionally not destroyable; other tile destruction keeps the old Chivalry gate."* "The old gate" is an admission it wasn't verified. Destroy also has **cost 0**, which is almost certainly wrong.

### 10.8 Combat constants

| Constant | Value | Source | Trust |
|---|---|---|---|
| Damage multiplier | **4.5** | `CombatSystem.cpp:273-274` | **A** |
| Attack force | `atk × (hp / maxHp)` | `CombatSystem.cpp:267` | **A** |
| Defence force | `def × (hp / maxHp) × bonus` | `CombatSystem.cpp:269` | **A** |
| Rounding | `std::llround` (half away from zero) | `CombatSystem.cpp:24-26` | **A** |
| Retaliation on defender death | **none** | `CombatSystem.cpp:317-361` | **A** |
| Retaliation range check | Chebyshev ≤ defender range | `CombatSystem.cpp:365-369` | **A** |
| `Stiff` blocks retaliation | yes | `CombatSystem.cpp:369` | **A** |
| Forest defence bonus (needs Archery) | **1.5×** | `CombatSystem.cpp:57-58` | **B** |
| Mountain defence bonus (needs Climbing) | **1.5×** | `CombatSystem.cpp:59-60` | **B** |
| Water/Ocean defence bonus (needs Aquatism) | **1.5×** | `CombatSystem.cpp:61-64` | **B** |
| City defence bonus (requires `Fortify`) | **1.5×** | `CombatSystem.cpp:70-73` | **B** |
| City wall increment | **+2.5 → 4.0× total** | `CombatSystem.cpp:80-81` | **B** |
| Terrain vs. city bonus | **max**, not multiplied | `CombatSystem.cpp:87-90` | **B** |
| Poisoned: replaces bonus ≤1.0 | **0.5** | `CombatSystem.cpp:95-103` | **C** — dead code |
| Poisoned: replaces bonus ≤1.5 | **0.7** | same | **C** |
| Poisoned: replaces wall bonus | **2.0** | same | **C** |
| Splash damage | **main hit ÷ 2**, floor, 8-neighbourhood, enemies only | `CombatSystem.cpp:282-308` | **B** |
| Stomp damage | **main hit ÷ 2**, no retaliation | `CombatSystem.cpp:450-453` | **B** |
| Heal Others | **+4 HP**, 8-neighbourhood friendlies | `CombatSystem.cpp:505-506` | **B** |
| Passive recovery, own territory | **+4 HP** | `TurnSystem.cpp:95-96` | **B** |
| Passive recovery, elsewhere | **+2 HP** | `TurnSystem.cpp:89` | **B** |
| Recovery condition | did not move **and** did not attack | `TurnSystem.cpp:78-80` | **B** |
| Veteran threshold | **3 kills** | `UnitUpgradeSystem.cpp:60` | **B** |
| Veteran bonus | **+5 max HP, healed to full** | `UnitUpgradeSystem.cpp:42-44` | **B** |
| Veteran ineligibility | has `StaticSkill` | `UnitUpgradeSystem.cpp:36` | **B** |
| Attack after move | only with `Dash` | `CombatSystem.cpp:170` | **A** |
| Melee advance after kill | range == 1, or Rammer | `CombatSystem.cpp:331-333` | **B** |
| `Persist` | may attack again after a kill | `CombatSystem.cpp:326` | **B** |
| `Escape` | may move again after attacking | `CombatSystem.cpp:357-359, 389-391` | **B** |
| Distance metric | **Chebyshev** everywhere | `CombatSystem.cpp:35-39` | **A** |

The four **A** rows are the ones I'd stake money on. The rest are single-sourced from one author's wiki reading.

### 10.9 City economy

| Rule | Value | Source | Trust |
|---|---|---|---|
| Population to reach level `n+1` | **`n + 1`** (carry-over preserved) | `City.cpp:26-29, 31-43` | **A** |
| Unit capacity | **`level + 1`** | `City.cpp:46-48` | **B** |
| Base income | **`level`** stars/turn | `City.cpp:55` | **B** |
| Capital bonus | **+1** star/turn | `City.cpp:56-58` | **B** |
| Workshop reward | **+1** star/turn | `City.cpp:59-61` | **B** |
| Park reward | **+1** star/turn each | `City.cpp:62` | **B** |
| Income timing | start of turn, skipped turn 0 | `TurnSystem.cpp:28-32` | **A** |
| Income blocked under siege | enemy unit on city tile | `StarsSystem.cpp:111-114` | **B** |
| Income blocked once after infiltration | flag consumed on attempt | `StarsSystem.cpp:116-120` | **B** |
| New city / captured city territory | radius **1** square | `CitySystem.cpp:374-376` | **A** |

**Level-up reward pairs** (`CityRewardSystem.cpp:32-38, 130-135`):

| Level | Option A | Option B |
|---|---|---|
| 2 | Workshop (+1★/turn) | Explorer |
| 3 | City Wall | Resources (**+5 stars**) |
| 4 | Population Growth (**+3 pop**) | Border Growth (**radius 2** claim) |
| 5+ | Park (+1★/turn) | Super Unit (**Giant**) |

Trust **B** across the board. Note Border Growth calls `claimFreeTerritoryRadius(..., 2)` — a radius-2 *square*, i.e. the 5×5 block, claiming only unowned tiles.

### 10.10 Monuments

Source: `src/systems/MonumentSystem.cpp`. All grant `populationGain = 3` and cost 0 to place (`BuildingDB.cpp:87-93`).

| Monument | Unlock condition | Extra gate | Trust |
|---|---|---|---|
| Grand Bazaar | **≥5** connected non-capital cities | — | **B** |
| Park of Fortune | any city reaches level **5** | — | **B** |
| Eye of God | discover **4** lighthouses (all map corners) | — | **B** |
| Emperor's Tomb | hold **≥100** stars | requires **Trade** | **B** |
| Gate of Power | **≥10** total kills | — | **B** |
| Altar of Peace | **≥5** consecutive no-attack turns | requires **Meditation** | **B** |
| Tower of Wisdom | **all 25** techs researched | — | **B** |

Lighthouse discovery also grants **+1 population** to the capital (or, lacking one, the highest-level then highest-population city) — `LighthouseSystem.cpp:50-101`. Trust **B**.

### 10.11 Score weights

Source: `src/systems/ScoreSystem.cpp:131-180`. Trust **C** overall — the tribe starting-score function is commented out and returns 0, so the totals cannot be right.

| Component | Points | Source |
|---|---|---|
| Per revealed tile | **×5** | `exploredTiles`, line 131-133 |
| Per owned territory tile | **×20** | `territoryTiles`, 135-137 |
| Per star of unit cost | **×5** | `unitCostStars`, 139-141 |
| Per super unit | **+50** | `superUnits`, 143-145 |
| Per park | **+250** | `parks`, 165-167 — **never called** |
| Per monument | **+400** | `monuments`, 169-171 |
| Per tech, by tier | **tier × 100** | `techTier`, 178-180 |
| Temple level | `100 + (lvl−1) × 100` | `templeLevel`, 173-176 — **never called, no Temple exists** |
| City base | **100** | `cityLevel`, 147-162 |
| City per level `k` (2..lvl) | **`(k × 5) + (50 − 5k)`** = constant **50** | `cityLevel`, 154-156 |
| City current population | **×5** | `cityLevel`, 159 |
| Tribe starting score | **0** (all cases commented out) | `startingScore`, 96-129 |

The city-level expression `(k * 5) + (50 - 5 * k)` algebraically collapses to a constant 50 per level regardless of `k` — either the author transcribed two wiki terms that cancel, or it's a transcription error. Either way it is a code smell worth noting.

The commented-out tribe starting scores are still readable and duplicated (correctly) in `Tribe::defaultStartScore` — see 10.12.

### 10.12 Tribes

Source: `src/content/tribes/Tribe.cpp:64-153`.

| Tribe | Start ★ | Start tech | Start unit | Start score | Trust |
|---|---|---|---|---|---|
| Xin-xi | **7** | Climbing | Warrior | 515 | **B** |
| Imperius | 5 | Organization | Warrior | 515 | **B** |
| Bardur | 5 | Hunting | Warrior | 515 | **B** |
| Oumaji | **6** | Riding | **Rider** | 520 | **B** |
| Kickoo | 5 | Fishing | Warrior | 515 | **B** |
| Hoodrick | **7** | Archery | **Archer** | 620 | **B** |
| Luxidoor | **2** | **none** | Warrior | 515 | **C** — see below |
| Vengir | 5 | Smithery | **Swordsman** | 730 | **B** |
| Zebasi | 5 | Farming | Warrior | 615 | **B** |
| Ai-Mo | 5 | Philosophy | **MindBender** | 615 | **B** |
| Quetzali | **7** | Strategy | **Defender** | 620 | **B** |
| Yădakk | **7** | Roads | Warrior | 615 | **B** |
| Aquarion | 5 | none | Mermaid | 415 | **C** |
| Elyrion | 5 | none | Warrior | 515 | **C** |
| Polaris | 5 | none | Mooni | 630 | **C** |
| Cymanti | 5 | none | Shaman | **515** | **C** — author's comment: *"brak na Twoim screenie → ustawiam bezpieczny default"* ("not on your screenshot → setting a safe default"), i.e. **invented** |

The pattern — 5 stars default, 6 for Oumaji, 7 for the four tribes that start with a tier-2 tech, 2 for Luxidoor — is internally coherent and matches the "Luxidoor trades stars for a level-2 capital" design. Trust B.

**Luxidoor is rated C because the implementation is broken**, `Game.cpp:366-371`:

```cpp
        // Special tribe rule: Luxidoor starts with a strong capital
        if (cfg.tribes[i] == TribeType::Luxidoor) {
            (void)CitySystem::setCityLevel(*this, cid, 2);
            (void)CitySystem::setCityStarsPerRound(*this, cid, 0);
            (void)CitySystem::setCityPopulation(*this, cid, 0);
        }
```

The level is set to 2, so income does rise (level + capital = 3/turn), but **no level-2 reward is granted** — Luxidoor gets neither Workshop nor Explorer, and no `PendingCityUpgrade` is enqueued. In the live game Luxidoor's capital comes with a free level-2 reward. Half-implemented.

Also note: **no tribe has any other passive.** No Vengir defence penalty, no Yădakk road discount, no Zebasi/Quetzali specials. See §4.

### 10.13 Map generation constants

Source: `src/world/MapGenerator.cpp`. Trust **C** across the board — third-hand and partly self-admittedly invented.

| Constant | Value | Line |
|---|---|---|
| Base mountain fraction | **0.14** | 426 |
| Base forest fraction | **0.38** | 427 |
| Base field fraction | **0.48** | 428 |
| Water fraction range (Lakes) | **[0.25, 0.28]** | 22-23 |
| Smoothing passes | **2** | 20 |
| Suburbs per capital | **1–2** | 13-14 |
| Pre-terrain village density | **0.30** | 15 |
| Village grid slots | **`(size / 3)²`** (integer div then square) | 235 |
| Shallow-water fish rate | **0.50** | 29 |
| Ocean fish rate | **0.10** (invented — see comment lines 26-31) | 30 |
| Inner-ring (r=1) fruit / crops | **0.18** each | 490-491 |
| Outer-ring (r=2) fruit / crops | **0.06** each | 490-491 |
| Inner-ring animal (Forest) | **0.19** | 495 |
| Outer-ring animal | **0.06** | 495 |
| Inner-ring metal (Mountain) | **0.11** | 498 |
| Outer-ring metal | **0.03** | 498 |
| Resource radius from settlement | **2** | 457 |
| Starfish count | **waterTiles / 25** | 569 |
| Domains per axis | 2 (≤4 players), 3 (≤9), 4 (else) | 151 |

Ruin counts by map size (`ruinTargetForMapSize`, lines 36-46):

| Map | Tiles | Ruins |
|---|---|---|
| Tiny 11×11 | 121 | 4 |
| Small 14×14 | 196 | 5 |
| Normal 16×16 | 256 | 7 |
| Large 18×18 | 324 | 9 |
| Huge 20×20 | 400 | 11 |
| Massive 30×30 | 900 | 23 |
| other | — | `round(tiles × 23/900)` |

Per-tribe terrain multipliers (`probsForTribe`, lines 107-125) — multipliers on the base fractions above, default 1.0:

| Tribe | forest | mountain | fruit | crops | animals | metal | fish |
|---|---|---|---|---|---|---|---|
| Kickoo | — | 0.5 | — | — | — | — | **1.5** |
| Aquarion | 0.5 | — | — | — | — | — | — |
| Bardur | **0.8** | — | — | **0.0** | — | — | — |
| Xin-xi | — | **1.5** | — | — | — | **1.5** | — |
| Ai-Mo | — | **1.5** | — | 0.1 | — | — | — |
| Oumaji | 0.2 | 0.5 | — | — | 0.2 | — | — |
| Vengir | — | — | 0.1 | — | 0.1 | **2.0** | 0.1 |
| Imperius | — | — | **2.0** | — | 0.5 | — | — |
| Hoodrick | **1.5** | 0.5 | — | — | — | — | — |
| Zebasi | 0.5 | 0.5 | 0.5 | — | — | — | — |
| Quetzali | — | — | **2.0** | 0.1 | — | — | — |
| Yădakk | 0.5 | 0.5 | **1.5** | — | — | — | — |

Guaranteed capital-adjacent resources (lines 523-537): Bardur 2× Animal/Forest · Imperius 2× Fruit/Land · Kickoo 2× Fish/Water · Zebasi 1× Crops/Land · Aquarion 2× Fish/Water (Drylands only).

### 10.14 Miscellaneous

| Rule | Value | Source | Trust |
|---|---|---|---|
| Ruin: stars reward | **10** | `InteractionSystem.cpp:49` | **B** |
| Ruin: population reward | **+3** to capital | `InteractionSystem.cpp:140` | **B** |
| Ruin: veteran unit (land) | veteran **Swordsman**, maxHP +5 | `InteractionSystem.cpp:193-206` | **B** |
| Ruin: veteran unit (water) | veteran **Rammer** at **15 HP** | `InteractionSystem.cpp:52, 177-190` | **C** — 15 is arbitrary |
| Ruin: reward selection | **uniform** over a dynamic pool of {Stars, Tech, Population, Explorer, VeteranUnit} | `InteractionSystem.cpp:221-262` | **C** — real game is weighted |
| Starfish reward | **8** stars, requires **Navigation** + a `WaterOnly` unit | `InteractionSystem.cpp:48, 69-89` | **B** |
| Explorer: moves | **15** steps, 8-neighbourhood, reveals a plus-shape each step | `VisionSystem.cpp:228, 171-183` | **B** |
| Explorer: scoring | `183 − 10 × fogCleared`, lighthouse **−33**, `+100` per BFS layer, 3 layers, fog count capped at 4 | `VisionSystem.cpp:277-310` | **C** — reverse-engineered from "wiki examples" |
| First contact reward | `min(1 + 2 × ceil(score / 1000), 11)` | `VisionSystem.cpp:370-374` | **B** |
| Disband refund | **cost ÷ 2** (floor); super units flat **5** | `DisbandSystem.cpp:29-43` | **B** |
| Disband gate | requires **Free Spirit**, unit must not have acted | `DisbandSystem.cpp:53-55` | **B** |
| Infiltration: cloak damage to city unit | flat **2** | `InfiltrationSystem.cpp:175` | **C** — hardcoded, ignores cloak attack |
| Infiltration: daggers spawned | **city level**, capped **5** | `InfiltrationSystem.cpp:190-192` | **B** |
| Infiltration: income stolen | city's full `starsPerRound`, then blocked next turn | `InfiltrationSystem.cpp:161-166` | **B** |
| Road/bridge move cost | **0.5** move point (1 of 2 half-points) | `MovementSystem.cpp:294-306` | **B** |
| Road bonus voided in enemy territory | yes | `MovementSystem.cpp:302-306` | **B** |
| Forest without road | **terminal** tile (movement ends) | `MovementSystem.cpp:275-282` | **B** |
| Zone of control | enemy 8-neighbourhood ends movement; `Creep` ignores it; `Hide` units don't project | `MovementSystem.cpp:121-141, 283-287` | **B** |
| Mountain entry | requires **Climbing** | `MovementSystem.cpp:447-448` | **A** |
| Ocean entry | requires **Sailing** | `MovementSystem.cpp:458-459` | **B** |
| Vision range on mountain | raised to **2** | `VisionSystem.cpp:43-45` | **B** |
| Initial capital reveal | radius **2**, corners excluded | `Game.cpp:426-431`, `VisionSystem.cpp:78-81` | **B** |
| Win condition | own **all** capitals | `Game.cpp:1048-1076` | **A** |
| Turn order | fixed round-robin, `turnNumber++` when wrapping to player 0 | `Game.cpp:464-467` | **A** |

Note the Ocean/Sailing row: PolyEnv gates **Ocean on Sailing**, but `VisionSystem::doExplorer` (`VisionSystem.cpp:160-167`) gates **Water on Sailing and Ocean on Navigation**. Two different rules in the same codebase. In the live game Sailing unlocks shallow water and Navigation unlocks ocean, so the *explorer* version looks right and the *movement* version looks wrong. **Flag as a specific wiki cross-check.**

---

## 11. Code quality — blunt assessment

The project's own note flagged this as "1 star, single author, ~7 months old, code quality unverified." That framing is fair, and having read it: **the architecture is better than expected, the rules implementation is worse.**

### What is actually good

- **System decomposition.** Stateless namespaced systems over dumb data structs is the right call and consistently applied across 22 systems. `Game` as a thin turn-ownership façade is clean.
- **The `unitAt` occupancy sidecar** in `Map` — O(1) lookups without scanning.
- **Determinism discipline in map gen.** A single seeded `mt19937`, threaded explicitly, with `generate()` returning capitals instead of mutating shared state — with a comment explaining that concurrent `Game` instances forced it. Someone thought about this.
- **The partial-observability layer.** `BeliefWorldBuilder`, `ObservedEventJournal`, belief-particle ingestion for ISMCTS, fog-safe event history. This is real engineering and well beyond hobby quality.
- **Tech-gated resource visibility** (`getVisibleResourceForPlayer`) — a genuinely subtle rule, correctly isolated in one function.
- **Rich per-action metadata** in the Python legal-action list (`damage_dealt`, `damage_received`, `cost_stars`, `affordable`). Steal this idea.
- **`data/Units.json`** as a schema-versioned data file with `_defaults`.

### What is bad

**1. Constants are scattered and duplicated.** Tile-action costs appear twice per action — once in `canX`, once in `X` — as separate `static constexpr int kCost` declarations. `TileActionSystem.cpp` has ten such pairs. Nothing prevents them diverging. Unit costs live in `Units.json` *and* `ScoreSystem::costStarsByUnitType`, and they already disagree (`Raft: 0` vs `Raft: 2`).

**2. Known-wrong code shipped with comments admitting it.** A partial list:

- `TurnSystem.cpp:128-136`: movement-point refresh is `std::max(1, currentMovePoints)` preceded by ~10 lines of commentary explaining the author doesn't have a `maxMovePoints` field. A Knight that spends its 3 points refreshes to 1.
- `ScoreSystem::startingScore` (96-129): the entire body is commented out; returns 0.
- `ScoreSystem::costStarsByUnitType`: *"Replace with UnitDB lookup when you have it."* The UnitDB exists.
- `MovementSystem.cpp:170-172`: *"NOTE: This file currently doesn't have access to terrain/road APIs, so the rule hooks below default to 'no roads'..."* — stale, since roads *are* implemented below it.
- `InfiltrationSystem.cpp:37-38`: *"This is a simplified approximation of the wiki behavior."*
- `Tribe.cpp:103`: *"brak na Twoim screenie → ustawiam bezpieczny default."* A rule number invented because a screenshot didn't show it.

**3. Real logic bugs in the rules layer.**

- `City::addPopulation` levels up **at most once**, dropping cascaded level-ups (§5.3).
- `City::levelUp` reads `uint8_t population = getPopulation()` from an `int16_t` field — truncation above 255.
- `TurnSystem::refreshUnitsForCurrentPlayer` never restores movement points properly.
- `CityRewardSystem::getUpgradeOptions` uses the *current* level while `isValidChoiceForLevel` is documented in terms of `newLevel` — the two are reconciled only by the enqueue ordering, and the parameter is literally named `newLevel` while receiving `level`.
- The poison defence-penalty branch is unreachable (nothing sets `poisoned`).
- Luxidoor's level-2 capital grants no level-2 reward.
- `Ocean` requires Sailing in movement but Navigation in the explorer.

**4. Mixed-language comments.** Polish and English interleave freely, sometimes within a function — `// Monument unlock tylko przy przejściu na 5+`, `// NIE constexpr — bo C++/clang często wywala...`, `// jeżeli miasto było infiltrowane → blokuj income tylko raz`. Roughly a third of comments are Polish. For a repo aiming at English-speaking RL users this is a real adoption barrier, and it makes the "wiki-sourced" claims harder to audit.

**5. Decorative emoji in production comments.** `// ✅ podczas Initial nie odsłaniamy rogów`, `// 🔒 BLOKADA`, `// ✅ Bieżąca populacja`. Cosmetic, but it signals the review bar.

**6. AI-generated-code smell.** `DisbandSystem.cpp` is authored `// Created by Codex on 09/07/2026.` `VisionSystem::doExplorer` is preceded by a block of instructions addressed to a code-generation tool: *"Replace the stub or body of doExplorer with this implementation... adapt signature to match project, do NOT change existing signature!"* — left in the shipped source. This is not disqualifying, but it means chunks of the rules layer were never read by a human, which is consistent with the bug pattern above.

**7. Gratuitous O(n) and O(n²) scans.** `StarsSystem::marketIncomeForCity` scans the entire map per city per turn. `BuildingSystem::cityAlreadyHasBuilding` scans the entire map per placement check. `CitySystem::removeUnitFromAnyCity` iterates every city. `InfiltrationSystem` does a full map scan. For a self-play RL engine this is the wrong constant factor and there is no territory index anywhere.

**8. Dead and duplicate content.** `GiantSuper` duplicates `Giant`. `Bunta` duplicates `Bunny`. `ResourcesEnum::Whale` is unused. `ScoreSystem::parks` and `::templeLevel` are never called. `Lighthouse` is a building type with no building. `BuildingDB::resourceBit` is documented as producing a bit flag but returns the raw enum value, and `resourceAllowed` compares by equality — so the "mask" is not a mask.

**9. The test suite cannot catch any of this** (§9): tautological formula tests, permanent skips, and a wiki cost table that contradicts the engine without failing.

### Bottom line on quality

Treat PolyEnv as **a well-organized skeleton wrapped around an unverified rules body**. The structure is worth studying; roughly half the numbers are worth cross-checking; the control flow inside the systems should not be imitated.

---

## 12. Recommended use of this document

1. **Use §10.1, §10.2, §10.5 (structure) and §10.8 (A-rows) as corroboration** when the wiki is ambiguous. These are the parts with independent internal agreement.
2. **Treat §10.6 (buildings), §10.7 (tile actions), §10.11 (score), §10.13 (map gen) as open questions**, not answers. Every C-rating in those sections is a specific item to resolve from the wiki.
3. **Lift the six damage test vectors** from §9 into our own test suite immediately — they are hand-computed, checkable by inspection, and independent of PolyEnv's C++.
4. **Do not lift code.** The MIT license permits it; the bug density argues against it.
5. **Never vendor `assets/`.**

### Specific wiki questions this audit generated

- Tech cost: is it `tier × cities + 4`? Is the Philosophy/Literacy discount 33% ceil'd on the base?
- Lumber Hut: 2 or 3 stars?
- Port: 7 or 10 stars? Market: 5 or 6?
- Market income: `2 × unique adjacent types` (doubled by Port, cap 12), or sum-of-levels?
- Does Sailing unlock shallow water and Navigation unlock ocean? (PolyEnv's movement and explorer disagree.)
- City defence bonus: is it really gated on `Fortify`, and is the wall total 4.0× or additive-something-else?
- Clear Forest: really 0 cost and +1 star refund?
- Destroy: what is the real cost and tech gate?
- Passive recovery: +2 / +4, or different?
- Veteran: 3 kills, +5 max HP?
- Capital: +1 star/turn on top of level?
- Ruin reward distribution: uniform or weighted?
- Which resources are tech-gated in the fog beyond Metal (Climbing) and Crops (Organization/Farming)?
