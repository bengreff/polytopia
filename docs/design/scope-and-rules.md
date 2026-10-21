# Scope and the rules that shape the design

## Target format

From the 2026 World Championship rules (YouTube video descriptions) and stream screenshots:

| Setting | Value |
|---|---|
| Mode | **Might**, 1v1. Best of 3, each game independent (face-offs were best of 1) |
| Map | **Lakes**, **Small = 14×14 = 196 tiles**, fog on |
| Tribes | Any of the **12 regular tribes** in either seat: 144 matchups, mirrors included |
| Clock | Live-game timer (see below); per-player time banks are visible on stream |
| Version | The tournament's current stable build. Ours is APK **2.17.3** (build 16375) |

Out of scope: special tribes, Glory, Perfection, Domination, other map types and sizes, FFA, teams.

## Rules that matter for the design

Sources: **[D]** = game data `game/dump/gamedata/GameLogicData28.txt`. **[C]** = decompiled code
(`game/dump/diffable-cs`, `game/dump/decompiled`). **[W]** = older wiki-derived research for 2.16.3,
lowest trust. **[R]** = adversarial rules review, 2026-10-20 (cites code; spot-checked). polyhost
is the final authority.

**Victory [C].** In Might, a player wins when the number of capitals they hold equals the player
count. A player is also eliminated on losing all cities [R]. In 1v1: take the enemy capital while
holding your own. No turn limit.

**Fog — corrected in v5 [C][R].**
- Tiles carry only an explored flag per player (`TileData.explorers`, `GetExplored`). There is no
  per-turn vision state.
- **An explored tile shows its live contents.** `TileData.GetUnit(gameState, playerId,
  includeHidden=false)` hides only **invisible** units (`UnitState.IsInvisible`: Cloak, Dinghy).
- Hidden information is therefore only:
  1. unexplored tiles (terrain, villages, units on them);
  2. invisible enemy units **anywhere**, including next to or inside your cities;
  3. enemy stars;
  4. possibly enemy techs (measure).
- The wiki's "unit positions only where you currently have vision" (old research §9.6) is wrong.
- **Leaks to be aware of:** legality checks use the true state (`ActionIsBlockedByInvisibleUnit`
  reveals the unit when a move is blocked). Ruin rewards and explorer paths come from the game's
  random-number state (`GameState.Seed` / `RandomHash`).

**Tribes [D].** The 12 regular tribes share one 25-tech tree and one unit roster. They differ only
in:
- starting tech;
- starting unit (warrior, or rider for Oumaji, archer for Hoodrick, defender for Quetzali,
  swordsman for Vengir, mindbender for Ai-Mo);
- starting stars (2–7);
- terrain and resource bias (e.g. Bardur gets no crops; Vengir gets fruit, fish and game at 0.1);
- Luxidoor's level-3 capital.

Tribe is an input feature.

**Commands [C] (`CommandType.cs`).** Relevant in regular-tribe 1v1:

| Category | Commands |
|---|---|
| Units | Move, Attack (includes Mindbender convert), Recover, HealOthers, Promote, Disband, Stay, Disembark, Upgrade (hull) |
| Tiles and cities | Build (every improvement, incl. road, bridge, port, burn/clear/grow forest), Destroy, Harvest (starfish, 8★), Capture, ExamineRuins |
| Economy | Train, Research, CityReward |
| Diplomacy | PeaceTreaty, PeaceRequestResponse, BreakPeace, EstablishEmbassy, DestroyEmbassy, Hide (cloak) |
| Turn and game | EndTurn, Resign |

Special-tribe-only commands (FreezeArea, BreakIce, Explode, Boost, Decompose, Flood, Swarm,
ClearTileEffect) are out of scope. **Diplomacy is live in 1v1.**
- Shields grants peace treaties (Quetzali starts with it).
- Diplomacy grants the Cloak, embassies (5★ for +2★ per turn) and capital vision.
- Bots use diplomacy, so the agent must be able to answer a peace request.

**Combat [W] — deterministic, integer rounding [C: `BattleHelpers.Round/Floor`].**
- Damage = the side's share of the total force × its own stat × 4.5.
- Defence bonus: 1.5 on forest, mountain or water with the right tech, and in a city; 4 with a
  city wall.
- City bonuses apply only to units with Fortify. Swordsman, Giant, Catapult and Mindbender lack it.
- No retaliation if the defender dies or can't reach or see the attacker.
- Melee units advance after a kill. Dash, Escape and Persist ordering apply.

**Movement [W].**
- Distance counts diagonals (Chebyshev); roads cost 0.5.
- Forest and mountain end movement. Moving next to an enemy ends movement (zone of control).
- Embark, disembark, capture, ruins and recover end the unit's turn.
- **A capture needs the unit to start its turn on the city**, so every capital threat unfolds
  over at least 2 turns.

**Economy [C][W][R].**
- Income = city levels + workshops and parks + markets + embassies.
- **The capital bonus is `PlayerState.handicap`**, added at the player's original start tile
  (`income.c`).
- **A city with an enemy unit on it (besieged) produces 0.**
- Negative population reduces income.
- **City connections give +1 population to the capital and to the connected city.** Roads, ports
  and bridges create them; captures cut them.
- Tech cost = tier × number of cities + 4, cut by a third with Literacy. An embassy may halve tech
  prices further (`embassyTechPriceMultiplier` = 0.5; verify).
- One-time stars: meeting the enemy, ruins, +5★ city reward, starfish, clearing forest (+1★).
  Burning forest costs 3★.
- Level-up rewards: workshop/explorer, wall/+5★, +3 pop/border growth, park/Giant.
  - A Giant spawning pushes nearby units and can delete them or lift a siege.
- Windmill, forge and sawmill need an adjacent farm, mine or lumberhut; markets need one of those
  three. Buildings level up, adding population, which can level up the city mid-turn.
- Lighthouses and task monuments (e.g. the 10th kill: +3 population) also fire mid-turn.

**Naval [W][R].**
- A Port turns a land unit into a Raft (a Giant into a Juggernaut, a Cloak into a Dinghy, a Dagger
  into a Pirate).
- Hull upgrades in own territory: Scout or Rammer for 5★, Bomber for 15★. Disembarking loses the
  upgrade.
- Lakes maps are 25–30% water: naval play is mandatory.

**Randomness.** Map generation, ruins (5 on Small; reward chosen by the game's random-number
state), explorer paths, and the bots' deliberate mistakes (`AI.StupidFactor`). Combat is
deterministic.

**Clock [R] (`GameSettings.SetLiveModePreset`).** Live games give 8 s per turn + 12 s per city +
1 s per population, with time banks, dynamic timers and **auto-skip**. Human replays will contain
rushed and skipped turns.

**Bots [C][R].**
- Handicap table: Easy 0, Normal 1, Hard 2, Crazy 4, Frozen −1, added at the capital. Humans are 1,
  so **Crazy has a +3★ per turn edge**.
- Difficulty also sets aggression and `GetStupidity` (deliberate mistakes driven by the game's
  random-number generator).
- The handicap is a separate argument when adding a bot, so a "Crazy-brain, Normal-income" bot
  should be possible. Verify that aggression doesn't read the handicap.
- **Bots are probably omniscient:** `AI.PlayerMapData` and `CollectMilitaryStatsFromTile` take the
  full state. Since explored tiles are visible anyway, any cheating can only involve unexplored
  tiles, cloaks and stars.

**Strategic shape (competitive play) [R].**
- Starting vision is 25 of 196 tiles, so fog matters most on turns 0–6, and the opening decides
  games:
  - the village race;
  - research before capturing;
  - the level-2 and level-3 rewards.
- Rushes end games early: Oumaji riders, Vengir swordsman, Ai-Mo Mindbender with Literacy,
  Luxidoor's level-3 capital, Kickoo ports, Giant timings.
