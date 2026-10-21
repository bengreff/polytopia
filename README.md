# Polytopia on its real rules

This project runs The Battle of Polytopia's own compiled rules code, with no Unity and no
reimplementation. The rules live in `GameLogicAssembly`, which ships inside the game as native
code (`libil2cpp.so`). `polyhost` loads that library, starts the il2cpp runtime, and calls the
game's own functions directly:

- `ClientBase.CreateSession`'s pass-and-play branch, rebuilt step for step, creates a match,
  including map generation (`MapGenerator`).
- `ActionManager.ExecuteCommand` applies moves.
- `CommandUtils`, `UnitDataExtensions.GetMovementOptions`/`GetAttackOptions` and each command's
  `IsValid` say which moves are legal.
- `AI.GetMove` is the built-in bot.

Nothing in the rules is written by us.

Source build: Android 2.17.3.16375, which reports game version 123 and logic data version 28.

## The bot interface

`polyhost env` serves matches in the championship format (1v1 Might, Lakes, Small 14×14) to
Python (`py/polyenv.py`):

- **Create:** `new(slot, Spec(tribes, bots, seed))`. Each seat is either driven from outside
  (the agent) or by the game's AI at a difficulty. Built exactly as the real app builds a
  pass-and-play game: checked byte for byte against a game the real app created.
- **Legal commands:** a list of `(type, a, b, arg)` rows: the game's `CommandType`, source tile,
  target tile, and argument (unit, improvement or tech type, city reward, opponent).
- **Step:** `step([(slot, index)])` plays a legal command, then lets bot seats move. Index
  `STEP_BOT_OWN` plays the built-in AI's own command, for generating data from bot games.
- **Observe:** what that player's client shows (fog-correct):
  - per tile, 32 features;
  - 20 global features;
  - the viewer's techs.
- **Bot:** `bot([slot])` gives where the AI's choice sits in the legal list.
- **Save/load and dump:** save and load use the game's own serializer; dump writes the full state
  as JSON.

One process hosts many matches; run several processes for parallelism.

## Layout

| Path | What |
|---|---|
| `polyhost/il2cpp.hpp` | Binding to the il2cpp runtime exports |
| `polyhost/game.hpp` | Loads the shipped rules data, verified single-player builders (`createLocalGame`, `generateMap`) |
| `polyhost/env.hpp` | The bot interface: `env::Match` (pass-and-play construction, legal list, observation, save/load) |
| `polyhost/wire.hpp` | Binary protocol server for the interface |
| `polyhost/envtest.hpp` | Checks the interface against the game (legality, bot coverage, encoding, determinism) |
| `polyhost/json.hpp`, `polyhost/dump.hpp` | JSON of any game object; GameState save/load |
| `polyhost/serve.hpp`, `polyhost/web/` | HTTP server and browser UI on the same interface (paused) |
| `polyhost/main.cpp` | Commands (below) |
| `py/polyenv.py`, `py/smoke.py` | Python client (numpy) and its smoke test |
| `tools/frida/` | Real-app tracing and recording (`trace.py`, hook lists, boundary recorder) |
| `tools/ghidra/` | Decompile any game method by name |
| `tools/statediff.py`, `tools/statecompare.py` | Diff two dumped GameStates |
| `docs/design/` | Design of the AI: decision log, architecture, training plan |
| `game/` | Game files, extracted data, tools, traces. **Not committed**: it contains Midjiwan's code |

## Setup

1. Get the Android build (free). The APKMirror bundle `air.com.midjiwan.polytopia_2.17.3...apkm`
   is signed by Midjiwan. Unzip it into `game/android/splits/`, then unzip `base.apk` into
   `game/android/base/` and `split_config.arm64_v8a.apk` into `game/android/arm64/`.
2. Extract the rules data: load `game/android/base/assets/bin/Data` with UnityPy and save the
   `GameLogicData1`–`28` and `AvatarData` TextAssets to `game/dump/gamedata/<name>.txt`.
3. Install the NDK into `game/tools/android-ndk-r30`. Boot an arm64 Android emulator:
   `emulator -avd PolyRef -no-window -no-snapshot -cores 6 -memory 4096 -gpu swiftshader_indirect`.
4. `./polyhost/run.sh push` copies the game binary, metadata and data to the emulator.

## Use

- `./polyhost/run.sh build`, then `python3 py/smoke.py`: the Python interface end to end.
- `./polyhost/run.sh envtest <games> [seed] [every] [turn cap]`: checks the interface against the
  game.
- `./polyhost/run.sh envbench <games>`: times each part of the interface.
- `./polyhost/run.sh bench <games>`: Crazy vs Crazy championship games.
- `./polyhost/run.sh hotseatcheck <real.bin> <seeds>`: rebuilds a recorded real-app pass-and-play
  game and compares bytes.
- `./polyhost/run.sh serve`: then open http://localhost:8123 to play the built-in bot (paused).
- `./polyhost/run.sh symbols /data/local/tmp/poly/symbols.tsv`: method addresses for
  `tools/ghidra/decompile.sh 'GameRules$$LoadPreset/1'`.

## Verified against the real app

- **Pass-and-play Might, Lakes, Small:**
  - The real app's menus (Multiplayer → New Game → Pass & Play) were recorded with Frida.
  - `env::Match::build` rebuilds that game **byte-identical** (8,244 bytes) from the recorded
    seeds and tribes.
  - Only the profile ids and the random game name are copied, since neither affects the rules.
- **Single player:** game creation and map generation are byte-identical for 5 recorded games.
- **Rules:** replaying Ben's 313-command Domination game matches the real snapshots after every
  command.
- **Bots:** 212 of 212 bot moves are identical to ours.

## Interface checks (`envtest`, 12 games, 10,430 decisions)

- **Legality:**
  - Moves, attacks, research and end-turn match a brute-force sweep of every command the rules
    accept (`IsValid` over every class, tile and argument), on 869 sampled states.
  - `IsValid` alone is laxer than the client: it accepts captures, recovers and abilities that the
    game's own `Can*` checks (used by the generators and the UI) reject. So the generators are
    the authority, and the extra commands are reported, not offered.
- **Bot coverage:** every command the built-in AI chose is in the legal list.
  - The one exception is its `StayCommand`, which marks a unit done; the client never offers it
    to a person.
- **Encoding:** no two legal commands encode the same.
- **Determinism:**
  - save/load is idempotent after one round trip;
  - a restored copy has the same legal list and observation;
  - stepping it gives the same bytes.

## Measured (M2 Pro, emulator with 6 cores, championship format)

- **Per decision, one thread:**
  - legal list 1.2–1.6 ms (136–152 commands on average);
  - built-in AI 1.2–1.4 ms;
  - execute 0.09 ms;
  - observation 0.01 ms;
  - save 0.14 ms; load about 0.4 ms.
- **Crazy vs Crazy:** 1.65 s per game on one process (31 turns, about 1,000 commands); **2.4
  games/s** on 6 processes.
- **Through Python:** about 760 random-play decisions/s with observations, 16 games batched on one
  process.

## Not yet done

- **Real-app self-play oracle:** whole games of pass-and-play Might, diffed command by command
  (`tools/frida/agent.js` autoplay; see HANDOFF.md).
- **Speed:** direct method-pointer calls instead of `runtime_invoke`, a cheaper legal list, and
  threads in one process.
- **The native Windows build** on the PC.
