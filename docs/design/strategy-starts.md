# Strategy-tagged start positions (2026-11-18)

**Verdict:** 18 kinds of strategy found in the human championship games, each with 107 to 1,618
successful episodes in the training games (measured). A balanced set of 11,490 start positions (10,438 from training games,
1,052 held out) is in `runs/strategy/strategy_positions.tsv`, and its states are built on the PC in
`/home/greff/polydata/strategy_starts` (survivors per kind below). Diplomacy was dropped: only 42
successful training-game episodes (measured). `py/selfplay.py` already draws starts uniformly from
`index.tsv`, so the balanced index needs no change there.

Ben (2026-11-18): knight chains are "ONE strategy of MANY, look in human games". The self-play
agent should meet each kind of situation often, so it learns which strategy fits the position.

## What was built

- `py/strategy_positions.py`:
  - reads the converted human games (`data/human1`, the layout of `py/convert_replays.py`);
  - stage 1 (extract) turns each game into an event table and per-turn summaries, cached in
    `runs/strategy/events.pkl` (162 MB, 22 s with 4 processes on the Mac, measured);
  - stage 2 finds the episodes of each kind, keeps the successful ones, and writes the positions
    TSV, an episodes TSV (every attempt, successful or not) and a summary TSV.
- `py/human_starts.py`: one small change. When the positions TSV has a `strategy` column, the
  `index.tsv` it writes gets a `strategy` column too. Nothing else changed, and old position files
  work as before.
- `tools/strategy_starts_build.sh`: the PC build (6 shards on ports 11000-11005, joins the shard
  indexes, writes `DONE`).

Detection uses the game record only: the commands each player chose (units trained, techs, builds,
boat upgrades, city rewards, diplomacy), and what each chosen attack or capture did, read from that
player's own observation just before and just after the command. Nothing re-implements a rule;
states are built and checked by the game's own code in polyhost.

## Data and split

- Games used: 5,431 (measured) of the 7,732 converted games. They replay exactly (final check
  `bytes`/`content`, or differing only in `VillageNameSeed`, as `py/human_positions.py`), are game
  version 121 or later (as `py/human_starts.py`'s default), and have their raw replay on the PC
  (`/home/greff/polydata/replays_raw`, 5,994 files). 73 exact v121+ games (of 5,504 on the Mac) have no raw
  replay on the PC and were left out (derived).
- Held-out split: `train_bc.is_val`, i.e. game id CRC32 % 10 == 0 (`HUMAN_VAL_EVERY = 10`), the
  same split `human_starts_big` and the S-runs' `--starts_split val` evaluations use. 573 of the
  5,431 games are held out (measured). (`docs/design/superhuman-plan.md` says % 20; the code says
  % 10, and the code is what runs.)
- Held-out positions are in the same `index.tsv`, marked `split = val` in the positions TSV, as in
  `human_starts_big`. `py/selfplay.py --starts_split train` (the training default) drops them, and
  `--starts_split val` uses only them. They never enter training starts.

## Unit and tech roster (checked against the game data)

From `game/dump/gamedata/GameLogicData28.txt` and the game's `CityReward` enum
(`GameLogicAssembly/CityReward.cs`), and what the human games actually train (measured over the 5,504
exact v121+ games on the Mac):

- Trained: warrior 91,939; rider 44,647; defender 24,150; archer 16,606; catapult 8,259; swordsman
  7,670; knight 6,105; cloak 3,505; mind bender 1,459.
- Not trained but present: giant (the SuperUnit city reward, 27,761 times), juggernaut (a giant that
  boards at a port), dagger (spawned when a cloak infiltrates a city), and boats: raft
  (`transportship`, a land unit at a port), upgraded to scout 15,593, rammer 9,272, bomber 4,085 times.
- Kills by attacker: warrior 23,304; rider 21,604; scout ship 10,405; giant 9,087; knight 8,078;
  archer 7,752; catapult 6,854; swordsman 5,526; rammer 5,182; bomber 4,744; juggernaut 4,034.
- Enemy cities captured by: giant 2,061; warrior 1,384; rider 809; swordsman 489; defender 277;
  knight 107; dagger 68.
- Diplomacy is rare: 75 peace proposals in 37 games, 22 embassies in 15 games.

## How the record was read (and checked)

- City owner: the tile's territory owner (`owner`); the improvement's `imp_owner` is always 0 for
  cities in these observations (measured), so it can't be used. A village is a city tile with
  territory owner 0.
- A kill: the attacker's `kills` counter rose between the attack and the attacker's next decision.
  Checked over 48,133 attacks in 1,000 games (measured): the target was gone afterwards in 98.8% of
  kills and in 0.9% of non-kills; the exceptions are bomber splash kills (a neighbour died) and
  mind bender or cloak actions (the target changed hands).
- A conversion: a mind bender's attack after which the target is the player's (164 of 164 mind bender
  attacks in the same 1,000 games, measured).
- An infiltration: a cloak's attack on an enemy city tile; daggers stand there afterwards.
- A landing: a boat carrying a unit moves from water onto land; the passenger stood on the landing
  tile afterwards in 99.4% of 3,114 landings (same 1,000 games, measured). An embarkation: a land unit moves onto water.
- Following one knight through a turn: by its tile, through its moves and the kills it moves in after
  (melee kills moved the attacker in about 90% of the time for warriors, riders, knights, swordsmen
  and giants, measured).

## Taxonomy and detection rules

Each kind has an **attempt** (the player tried it), a **payoff** (what the strategy is for) and a
**success** rule. `t0` is the turn execution starts: the first turn of the run of build-up (training
the kind's unit, researching its tech, taking its city reward) that leads without a gap of more than
2 turns into the payoff, at most 8 turns before it. "Net" below means the player's kills minus its
own losses from `t0` to the turn after the payoff (from the game's kill and casualty counters).

| Kind | Payoff (detection) | Success |
|---|---|---|
| `capital_rush` | the enemy capital captured by turn 12; t0 = first attack in the 6 turns before | the capital not lost again within 3 turns |
| `rider_rush` | riders kill >= 3 units within 4 turns, or a rider captures an enemy city, by turn 14 | payoff and net >= 0 |
| `early_rush` | warriors and archers kill >= 3 within 4 turns, or capture an enemy city, by turn 10 | payoff and net >= 0 |
| `knight_chain` | one knight kills >= 2 units in one turn (persist) | payoff and net >= 0 |
| `swordsman_push` | swordsmen kill >= 3 within 4 turns, or a swordsman captures an enemy city | payoff and net >= 0 |
| `giant_push` | giants kill >= 3 within 4 turns, or a giant captures an enemy city; build-up includes the SuperUnit reward | payoff and net >= 0 |
| `juggernaut` | juggernauts kill >= 2 within 4 turns; build-up includes the SuperUnit reward and the giant boarding | payoff and net >= 0 |
| `catapult_siege` | >= 2 catapult attacks on enemy territory within a run of turns, then an enemy city within 3 tiles of a bombarded tile captured within 3 turns | the capture |
| `defensive_line` | enemy units inside the player's territory at the start of >= 2 of its turns; in that turn and the 5 after, >= 2 defenders or archers trained, or >= 2 kills by them | no city lost, >= 3 kills inside own territory, net > 0 |
| `city_wall` | the CityWall reward taken with an enemy unit within 3 tiles | the city not lost in the next 6 turns, >= 1 kill within 2 tiles of it |
| `naval_landing` | an enemy city captured by a unit that came ashore from a boat within the 3 turns before, within 2 tiles; t0 = first embarkation in the 3 turns before the landing | the capture |
| `naval_expansion` | >= 2 villages within 5 turns captured by units that came ashore within 2 turns, within 1 tile | the villages |
| `naval_battle` | scout ships, rammers or bombers sink >= 2 enemy boats within 4 turns | payoff, net >= 0, more boats sunk than lost |
| `naval_bombard` | the same warships kill >= 2 land units on land within 4 turns (water control) | payoff, net >= 0, more kills than boats lost |
| `economic_boom` | from turn 4, 5 turns with >= 4 techs and >= 1 new city (or >= 5 techs), <= 2 units trained and no attacks | won the game, and 8 turns later income >= the opponent's and >= 1.5x its own at t0 |
| `mind_bender` | >= 2 conversions, 3 turns apart at most | the conversions |
| `cloak_dagger` | a cloak infiltrates an enemy city | within 3 turns the daggers kill >= 1 or a city within 2 tiles is captured |
| `recapture` | a city lost and taken back within 5 turns | held for 2 more turns |
| `diplomacy` (dropped) | a peace proposal, a peace answer or an embassy | won the game |

**The success definition in one line:** the episode reached its payoff, read from the record (kills
by the strategy's units, a capture, conversions, a city held), and where it is a trade, the player
did not lose more units than it killed. Winning the game is not required (it is for
`economic_boom` only, which has no other outcome to read); the share of episodes by the eventual
winner is reported instead.

Thresholds (3 kills, 4-turn windows, turn 10/12/14 for rushes, 8 turns of build-up) are my choices
(guessed), set after looking at the distributions over the 5,504 exact v121+ games (measured): an
enemy capital falls at turn 16 median (5-95%: 8-26, over 689 capital captures), and a player's first
enemy city at turn 13 median (7-22, over 3,367 player-games that took one).

## Counts (measured, 5,431 games)

| Kind | Attempts | Successful | Rate | By the winner | Successful with a start position: train / held out | Positions: train / held out | Games (train) |
|---|---|---|---|---|---|---|---|
| capital_rush | 4,741 | 201 | 4% | 100% | 182 / 19 | 532 / 57 | 181 |
| rider_rush | 6,595 | 1,370 | 21% | 75% | 1,218 / 140 | 600 / 60 | 600 |
| early_rush | 7,853 | 1,137 | 14% | 74% | 746 / 85 | 600 / 60 | 600 |
| knight_chain | 1,225 | 649 | 53% | 62% | 582 / 66 | 600 / 60 | 427 |
| swordsman_push | 3,760 | 609 | 16% | 76% | 546 / 61 | 600 / 60 | 452 |
| giant_push | 6,974 | 1,798 | 26% | 81% | 1,618 / 176 | 600 / 60 | 600 |
| juggernaut | 2,666 | 680 | 26% | 80% | 613 / 67 | 600 / 60 | 534 |
| catapult_siege | 1,237 | 408 | 33% | 84% | 369 / 38 | 600 / 60 | 332 |
| defensive_line | 2,197 | 472 | 21% | 73% | 428 / 44 | 600 / 60 | 389 |
| city_wall | 1,073 | 580 | 54% | 52% | 523 / 57 | 600 / 60 | 467 |
| naval_landing | 3,409 | 1,036 | 30% | 85% | 926 / 110 | 600 / 60 | 600 |
| naval_expansion | 396 | 396 | 100% | 72% | 349 / 46 | 600 / 60 | 346 |
| naval_battle | 4,263 | 727 | 17% | 78% | 663 / 64 | 600 / 60 | 559 |
| naval_bombard | 5,296 | 1,299 | 25% | 81% | 1,178 / 119 | 600 / 60 | 600 |
| economic_boom | 621 | 271 | 44% | 100% | 246 / 25 | 600 / 60 | 244 |
| mind_bender | 493 | 119 | 24% | 53% | 107 / 12 | 306 / 35 | 82 |
| cloak_dagger | 652 | 322 | 49% | 67% | 283 / 39 | 600 / 60 | 184 |
| recapture | 2,461 | 410 | 17% | 59% | 365 / 44 | 600 / 60 | 309 |
| diplomacy | 86 | 43 | 50% | 100% | 42 / 0 | dropped | |

Notes:
- "Attempts" for `naval_expansion` are only the successes (an attempt without the villages isn't
  defined), so its rate says nothing.
- `early_rush` loses 306 of its successes for a start position (measured): the rush began at turn 0
  or 1, and positions start at turn 1.
- The kinds are tribe-flavoured (measured, executing player's tribe): mind bender 68% Ai-Mo, cloak and
  dagger 53% Quetzali, swordsman push 37% Vengir, defensive line 27% Quetzali, economic boom 22% Ai-Mo.
  91-97% of positions in every kind are mirror matches, like the human games.
- The executing player is player 1 in 47-91% of a kind's positions (capital rush 91%).

## Start positions and balance

- One position per episode: the executing player's own turn `t0 - lead`, lead 1, 2 or 3 drawn at
  random, at that player's first recorded decision of the turn (where `py/human_starts.py` saves
  the state), from turn 1 on.
- Left out, as in `py/human_positions.py`: decided positions (a player without a city, or a score
  below 40% of the other's). A recapture's positions must come after the city was lost.
- Cap: 600 positions per kind from training games, 60 from held-out games (my choice, inside the
  300-800 asked for). When a kind has fewer episodes than the cap, the episodes' other leads fill
  it (so `capital_rush`, `mind_bender`, `economic_boom`, `cloak_dagger` hold 2-3 positions per
  episode).
- Spread: first one episode per game, then the rest. A position (game, turn, player) is used by one
  kind only; kinds with fewer episodes choose first.
- Result (measured): 11,490 positions from 3,798 games; leads 1/2/3: 4,109 / 3,735 / 3,646.
  Start turn medians per kind run from 3 (`early_rush`, `economic_boom`) to 17 (`knight_chain`).
- Each training generation draws a start uniformly from the index (`py/selfplay.py` line 368), so
  every kind comes up about equally often (derived). Mixing with `human_starts_big` would need a merged
  index; not done.

## The PC build

`cd ~/poly/py_strat && setsid nohup bash strategy_starts_build.sh > ~/polydata/strategy_starts.log 2>&1 < /dev/null &`
(6 shards of `py/human_starts.py`, `POLYHOST=linux`, `--raw /home/greff/polydata/replays_raw
--data /home/greff/polydata/human_full`; the PC's `human_full` npz files are byte-identical to the
Mac's `data/human1`, checked by md5 on 2 games). The built-in checks stay: loaded as the current
version, and again after a plain reload, the state must show the recorded observation and legal list.

A 30-position test first: 30 saved, 0 failed (measured).

**Result (measured, 2026-11-18, 17:43-18:01 on the PC, 6 shards):** every one of the 11,490
positions survived the checks: 0 failed, 0 engine errors, 174 MB in
`/home/greff/polydata/strategy_starts` (11,490 `.state` files, `index.tsv` with the `strategy`
column, `DONE`). Per kind, states saved (training / held out): 600 / 60 for 16 kinds,
`capital_rush` 532 / 57, `mind_bender` 306 / 35. No old-version games are in the set (all v121+),
which is probably why nothing failed, unlike S8's 53,272 of ~70k.

Checked with `selfplay.load_starts` on the PC (measured): `train` gives 10,438 starts, `val` 1,052,
and `match_starts(val, 100)` finds 100 held-out games. The positions TSV is also at
`/home/greff/polydata/strategy_positions.tsv`.

Use: `sp_loop.py ... --starts /home/greff/polydata/strategy_starts --start_share 0.3` (or
`selfplay.py --starts`), and `--eval_starts /home/greff/polydata/strategy_starts` for held-out
matches from these positions. Each training start is then one of 18 kinds with probability
about 1/18 (derived; `capital_rush` and `mind_bender` a bit less).

## Example positions to watch (game id, executing player, start turn)

All from successful episodes the executing player went on to win, at lead 1 (the turn before the
episode starts). "Episode a-b" is t0 to the payoff turn.

- **capital_rush:** `2ab55abd-38cf-4ec6-06b7-08df027310ef` P1 turn 9 (episode 10-12: capital at x9 y11); `c05ab435-a57f-48dc-1d25-08dec5ac0f6d` P1 turn 4 (episode 5-10: capital at x2 y4); `fcb36704-c1b0-43b7-cb6b-08df050ed1a2` P1 turn 5 (episode 6-12: capital at x2 y9)
- **rider_rush:** `10b509c0-d6a9-4a78-d274-08deb521355e` P1 turn 7 (episode 8-10: kills 3); `5035b84b-145d-407d-9b9b-08debf3a5110` P1 turn 5 (episode 6-9: kills 3); `2651eaf2-86f6-478e-2027-08df0e75dec1` P1 turn 13 (episode 14-14: kills 0 capture)
- **early_rush:** `972e90fd-59a9-426c-2e36-08ded25fe76a` P1 turn 7 (episode 8-10: kills 2 capture); `8b83e6e0-8e07-4af4-541a-08deac2a763d` P1 turn 1 (episode 2-10: kills 3); `929b2ff2-298b-408e-ab89-08dec5ac0f6c` P2 turn 4 (episode 5-8: kills 3)
- **knight_chain:** `ab78112f-e19e-4e52-d047-08debd63eb23` P1 turn 14 (episode 15-20: chain 8); `65862490-03fc-4616-9147-08df1f15178e` P2 turn 13 (episode 14-17: chain 2); `d6210bca-fb76-449d-133b-08deb641ba2a` P2 turn 22 (episode 23-24: chain 4)
- **swordsman_push:** `53688245-b23f-4921-0f56-08debd63eb24` P1 turn 14 (episode 15-20: kills 1 capture); `37f14dc2-a7e0-4923-0360-08deaef13dcc` P2 turn 13 (episode 14-15: kills 1 capture); `d45e717b-c3db-4ab9-0c50-08debd63eb24` P1 turn 9 (episode 10-11: kills 1 capture)
- **giant_push:** `0644d676-74a8-40dc-5f9e-08ded25fcc30` P1 turn 12 (episode 13-16: kills 3); `819abe16-34e6-4f5d-66eb-08ded25fe769` P1 turn 26 (episode 27-28: kills 1 capture); `8d8f30ec-4f29-4be8-aa18-08dec5ac0f69` P1 turn 11 (episode 12-14: kills 3)
- **juggernaut:** `dc915a74-e497-4a9a-2eff-08dec0e6f21d` P1 turn 11 (episode 12-17: kills 2); `012d41c6-e823-481a-22c2-08defdd3b7c4` P1 turn 14 (episode 15-23: kills 2); `fe1d35d5-bab6-4f1d-7c1a-08ded25fcc2f` P2 turn 7 (episode 8-11: kills 2)
- **catapult_siege:** `9ee06806-f412-4323-1be6-08deb21bbcb3` P2 turn 11 (episode 12-20: 8 catapult attacks, capture); `617846ba-93fa-4e9c-48e8-08dec5ac0f6a` P1 turn 14 (episode 15-24: 9 catapult attacks, capture); `ffa53f64-f711-4ef4-7509-08deb21c031a` P1 turn 12 (episode 13-22: 7 catapult attacks, capture)
- **defensive_line:** `65e61e75-f7c4-42fb-0b8c-08dec5ac0f6a` P1 turn 9 (episode 10-15: 7 line units, 2 line kills, 5 home kills); `e060d739-1f7e-4d85-1a3b-08dec5ab769e` P1 turn 20 (episode 21-26: 4 line units, 1 line kills, 4 home kills); `23f2102f-4bff-4a4a-9bbf-08deb21bbcb3` P1 turn 9 (episode 10-15: 9 line units, 0 line kills, 3 home kills)
- **city_wall:** `5bf610e0-4c3f-41ce-0ea1-08ded25fcc2f` P1 turn 5 (episode 6-6: wall at x6 y3, 6 kills near); `02e6ef03-7d85-4392-347a-08ded25fcc2f` P1 turn 16 (episode 17-17: wall at x7 y12, 1 kill near); `02b9a7af-3b4d-4e16-6027-08ded25fcc30` P2 turn 17 (episode 18-18: wall at x3 y11, 3 kills near)
- **naval_landing:** `101eac3c-0d71-462a-1192-08dec5ab76a0` P1 turn 16 (episode 17-21: landed 20, captured x11 y1); `dd23f3df-d9f3-44e2-201c-08dec5ac0f6d` P2 turn 11 (episode 12-16: landed 14, captured x7 y6); `c2c5d33a-a304-4ca1-f14e-08df0e75dec0` P1 turn 13 (episode 14-17: landed 16, captured x12 y7)
- **naval_expansion:** `02def5dd-2c4a-4f62-3c1d-08df02731023` P1 turn 6 (episode 7-13: 2 villages by boat); `91653b30-4758-4faa-1e00-08deac5daf50` P1 turn 6 (episode 7-14: 2 villages by boat); `52472e10-da29-43c8-ffa0-08deb521355f` P1 turn 3 (episode 4-10: 2 villages by boat)
- **naval_battle:** `9be8ae8a-2165-4b5e-cb2d-08deac2a763d` P1 turn 16 (episode 17-17: kills 3); `0ba5091d-85f4-4571-9b6c-08deff9bf6af` P1 turn 13 (episode 14-16: kills 3); `ca8802fe-ca6e-4d16-cbca-08ded25fe76a` P1 turn 10 (episode 11-12: kills 2)
- **naval_bombard:** `656eb156-48df-4d25-ec46-08deaf667d40` P1 turn 17 (episode 18-20: kills 2); `b2fae964-b557-4c64-d976-08debd63eb23` P1 turn 11 (episode 12-15: kills 2); `c578ba7c-30de-4e49-7c15-08ded25fcc2f` P1 turn 19 (episode 20-21: kills 2)
- **economic_boom:** `a62dcdb7-cbcf-4de3-d241-08dec6369863` P1 turn 8 (episode 9-13: 5 techs, +1 cities, 1 units); `41e4479d-3e4d-4050-287b-08debf7f9a6d` P2 turn 8 (episode 9-13: 7 techs, +1 cities, 2 units); `65e61e75-f7c4-42fb-0b8c-08dec5ac0f6a` P1 turn 4 (episode 5-9: 4 techs, +1 cities, 1 units)
- **mind_bender:** `395239fa-ec72-4f6d-789a-08df0e75dec3` P2 turn 11 (episode 12-16: 2 conversions); `a346415e-be52-45f2-48eb-08dec5ac0f6a` P2 turn 18 (episode 19-21: 2 conversions); `c858a642-fbe7-4276-cb24-08deac2a763d` P1 turn 11 (episode 12-15: 2 conversions)
- **cloak_dagger:** `d1a04875-7f90-4a0f-e7ce-08deac2a763c` P1 turn 13 (episode 14-14: dagger kills 0 capture); `8119f3c4-3dff-41aa-5110-08dec5ab769e` P1 turn 6 (episode 7-9: dagger kills 3 capture); `045e051b-1cf4-4bed-7b91-08dec5ab769d` P1 turn 15 (episode 16-19: dagger kills 1)
- **recapture:** `6e5ef258-81c7-4746-e700-08deb641ba28` P2 turn 10 (episode 11-11: lost 10 at x2 y5); `fb305d93-23fd-48fa-ad52-08ded25fe769` P1 turn 12 (episode 13-13: lost 11 at x12 y7); `93d4c136-5192-4f35-f079-08ded25fcc2f` P2 turn 14 (episode 15-15: lost 14 at x5 y7)

Tiles are the game's x and y coordinates (polyenv tile index = y * 14 + x).

## What this does not do

- It doesn't judge positions or label moves; human moves are never used, only the states.
- `naval_expansion` counts only boat-borne village grabs that succeeded; a failed attempt isn't defined.
- Episodes overlap: a giant landing that takes a city is both `giant_push` and `naval_landing`; a
  position goes to one kind only.
- The thresholds are hand-set (guessed); a different cut changes counts, not the kinds.
- No training run uses this set yet. Using it is a pre-registration decision (Ben's call), e.g.
  `sp_loop.py --starts /home/greff/polydata/strategy_starts --start_share 0.3`.

## Reproduce

    .venv/bin/python py/strategy_positions.py --data data/human1 --only runs/strategy/pc_raw_ids.txt --procs 4
    # pc_raw_ids.txt: `ls /home/greff/polydata/replays_raw` without .json
    # copy py/ (with runs/strategy/strategy_positions.tsv as strategy_positions.tsv) to ~/poly/py_strat, then the PC build above
