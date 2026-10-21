# Handoff: Polytopia engine on the real game code (2026-10-20, second session)

Paste this file as the prompt for the next session.

## Goal and standing decisions (from Ben)

- **Goal:** a superhuman Polytopia AI. That needs a training bench running the exact current
  rules at maximum speed, thousands of games in parallel.
- **Exact rules only.** Never write rules from memory or from wikis. We run the shipped game's
  own compiled code (`GameLogicAssembly`) and verify against the real app.
- **The real app is the reference.** Our engine is diffed automatically against the real
  Android app running in a rootable emulator under Frida. Ben does not want to playtest.
- **The browser UI is paused** until the engine is proven. After that, polish it once and Ben
  plays.
- **Scope is the World Championship format and nothing else** ("That is the entire scope").
  - 1v1 Might, best of 3, on a Lakes map, Small (14×14), fog on.
  - Either seat can be any of the 12 regular tribes.
  - The game version is pinned (2.17.3 = game version 123, logic data 28).
  - See memory `championship-scope` and the other agent's design docs in `docs/design/`
    (`README.md` is the decision log; then `scope-and-rules.md`, `architecture.md`,
    `training-and-data.md`). Read them first.
  - The app only has the 4 free tribes (Xin-xi 13, Imperius 7, Bardur 4, Oumaji 10). Verify
    with those. **Do not patch purchase checks.** The paid tribes run on the same real code.
- **Hardware is local only.**
  - Training runs on the PC `ssh backhouse`: Windows 11, i7-14700K 20C/28T, 64 GB, RTX 4070
    Ti SUPER 16 GB, WSL2 Ubuntu 22.04. Its default shell is cmd; use
    `powershell -EncodedCommand` for scripts.
  - Ben owns Polytopia on Steam (app 874390). It isn't installed on the PC yet, and the PC's
    network may not have the Mac's filter.
  - No cloud, and no play on official servers.
- **Repo:** Ben wants it public on GitHub as `bengreff/polytopia`, with commits straight to
  `main`. Not created yet; see "What exists".
- **Working style:** quick, direct work, no subagent swarms. Ask Ben when a decision is his.
  Never time out on a question; wait for the answer.
- **Network:** this Mac's network runs a FortiGate filter that blocks the "Games" category
  (Steam, App Store purchases, APK sites). GitHub, Google's download servers and PyPI work.
  Don't suggest a VPN.

## What exists

- **Repo `~/polytopia`:** everything is **staged but not committed**. Claude Code's
  permission check blocked both `git commit` and `gh repo create bengreff/polytopia --public
  --source . --remote origin --push`. Ben wants a commit to `main` and a push to a new public
  repo `bengreff/polytopia`; run those once permission is granted. The staged change removes
  the old from-memory engine and adds the real-code engine, the tools and the design docs.
  `game/` stays ignored.
- **`game/`** is in .gitignore and contains Midjiwan's files. Never commit or publish it.
  - `game/android/`: APKMirror bundle of **2.17.3.16375**, signed by Midjiwan. Game version
    123, logic data version 28.
  - `game/dump/`: `symbols.tsv` (115,724 method offsets), Cpp2IL `diffable-cs` type dump,
    `gamedata/GameLogicData1-28` and `AvatarData` JSON, and decompiled snippets.
  - `game/tools/`: NDK r30, Cpp2IL, Frida server 17.24.3.
  - `game/ghidra/`: Ghidra project with GameLogicAssembly functions defined.
  - `game/traces/`: real-app recordings. `play1` is Ben's 3 games; `play2`, `mapgen1`,
    `boundary1.bin` and `auto1` came later.
  - `game/.venv/`: Python with frida, UnityPy, androguard and websocket-client.
- **The bot interface (new this session):**
  - **What it is:** `polyhost/env.hpp` (`env::Match`), `polyhost/wire.hpp` (binary protocol,
    `polyhost env <port>`), `py/polyenv.py` (numpy client), `py/smoke.py`.
  - **Setup:** a match is built as the real app builds a pass-and-play game
    (`ClientBase.CreateSession`, GameType 2). Settings and seats were recorded from the real app's
    menus (`game/traces/hotseat1`), and `hotseatcheck` rebuilds that game byte-identical.
  - **Seats:** outside-driven (person/agent, handicap 1) or the game's AI at a difficulty. The
    nature player (id 255) takes turns too, and the AI plays them.
  - **Legal list:** the game's generators plus `IsValid`.
  - **End of game:** once the win is met the game sits in FinalTurn until an `EndMatchCommand`.
    The interface sends it automatically after any pending reward choice is answered.
  - **Bot-only command:** `STEP_BOT_OWN` plays the AI's own command, including its bot-only
    `StayCommand`.
  - **Checks:** `envtest` covers legality, bot coverage, encoding and determinism; `envbench`
    gives timings. See README "Interface checks" and "Measured".
  - `bench` and `serve` now run on `env::Match`. The unfaithful legacy `newMatch`/`Setup` is
    deleted.
- **`polyhost/`:** C++ built with the NDK. It loads `libil2cpp.so` with no Unity and calls the
  game code through il2cpp exports.
  - `il2cpp.hpp`: bindings, `call`/`tryCall`, `Pin` (GC handles are pointer-sized),
    `method(…, firstParamType)`, `methodWith(…, substring)`.
  - `game.hpp`: `loadGameData`, `Game` handles, `botCommand` (TryGetTriggerCommand, then
    AI.GetMove), plus the verified builders:
    - `SeedSource`: splitmix64 that can replay recorded seeds.
    - `generateMap`: MapGenerator.Generate step for step.
    - `createLocalGame`: GameStateUtils.CreateLocalGame step for step, with seeds supplied.
    - `localPlayer`: as ClientBase.CreateSession builds the human seat.
    - `menuSettings`/`MenuChoice`.
  - `dump.hpp`: full JSON `Dumper` and `StateIO` (GameState save/load through the game's
    BinaryWriter/Reader).
  - `serve.hpp` + `web/`: HTTP server and browser UI (paused).
  - `main.cpp` commands: `bench`, `serve`, `state`, `symbols`, `handicaps`, `dump`, `regen`,
    `fresh`, `replay` (env `POLY_CHECK_AI=1` checks bot choices), `scores`, `newplayer`. Env
    `POLY_WAIT=N` pauses after boot so Frida can attach.
  - `run.sh`: `./polyhost/run.sh push` once, then `./polyhost/run.sh <cmd args>` builds,
    pushes and runs it on the emulator.
- **`tools/ghidra/decompile.sh 'Class$$Method/argc' | 0xRVA`:** headless decompile with
  callee names. Big functions spill into neighbouring code; the time and payload limits are
  already raised.
- **`tools/frida/`:**
  - `agent.js`: generic il2cpp hooks with argument decoding, call chains and GameState
    snapshots (`snapshots: [{arg|ret|field}]`), plus the autoplay driver.
  - `trace.py hooks.json outdir [--attach] [--program polyhost] [--autoplay games.json]`.
  - `boundary.js`/`boundary.py`/`boundary_report.py`: record every client→rules call. It uses
    a CModule; keep it referenced, and read the caller from `cpu_context->lr`.
  - `emu.sh` (screenshot/tap) and the hook lists `hooks-play.json`, `hooks-play2.json`,
    `hooks-mapgen.json`, `hooks-score.json`.
- **`tools/statediff.py`, `tools/statecompare.py`, `tools/mapascii.py`:** state diffs.
  `statecompare` compares game content and ignores dictionary layout, default diplomacy
  relations, aiState/opinions and caches.
- **Emulators:**
  - `PolyRef` (Android 14 google_apis arm64, rootable) is the reference and runs polyhost.
    Start it with
    `emulator -avd PolyRef -no-window -no-snapshot -cores 6 -memory 4096 -gpu swiftshader_indirect`,
    then `adb root`, `adb shell setenforce 0`, and
    `adb shell "(nohup /data/local/tmp/frida-server >/data/local/tmp/frida.log 2>&1 &)"`.
    Never `pkill -f frida-server` in the same shell command that restarts it; it matches its
    own command line.
  - The old `Medium_Phone_API_36` AVD has been deleted, with Ben's OK.
  - The real app is installed on PolyRef.
  - The Mac is low on memory (swap nearly full) and disk (about 11 GB free after the cleanup).
    Android freezes ("system not responding") when starved.

## Verified against the real app

- **Pass-and-play Might, Lakes, Small (this session):** the real app's menus offer Might under
  Multiplayer → New Game → Pass & Play. Both players are local profiles, and tribes are picked
  after Start Game.
  - Recorded settings:
    - GameType 2;
    - base and rules mode 4, `GameRules.LoadPreset(4)` (WinByCapital, WinByExtermination, no
      limits);
    - mapPreset 2, MapSize 14;
    - OpponentCount 0, Difficulty 0;
    - unlockedTribes [0,1,4,7,10,13].
  - Each seat is a `PlayerData` (LocalUser, state 2, knownTribe, **climate 0**).
  - `CreateSession` builds each person as `new PlayerState`:
    - Id i+1, AccountId = profile id, AutoPlay off, name from the profile;
    - `_climate` = PlayerData.climate, the tribe, hasChosenTribe;
    - handicap default 1.
  - Then SetPlayerColors, AddNaturePlayer, `MapGenerator.Generate`, starting stars and units,
    and `StartMatchCommand(1)`.
  - `hotseatcheck` is byte-identical given the recorded seeds.

- **Game creation and map generation:** byte-identical for 5 recorded real games, given the
  recorded seeds. `fresh` from menu choices is byte-identical for Ben's Domination game.
- **Rules:** replaying Ben's Domination game (313 commands) through ActionManager matches the
  real snapshot content after every command, and his second game (2 commands) also matches.
- **Bots:** 212 of 212 bot moves are identical to our `botCommand`.
- **What the client does, from the decompile and the boundary trace:**
  - The human seat is `new PlayerState` with handicap defaulting to 1 (the +1 capital star),
    Id 1, an empty AccountId with hasValue set, AutoPlay off, and tribe, skin, climate and
    hasChosenTribe from GameManager statics. The name "Anonymous" is set after
    CreateLocalGame.
  - Bots come from `AddAIOpponent(GetRandomPickableTribe, HandicapFromDifficulty)`. The
    handicap table is Easy 0, Normal 1, Hard 2, Crazy 4. The tribe pick uses a process-global
    System.Random and only unlocked tribes.
  - Seed, VillageNameSeed and Generate's attempt seeds all come from
    `RandomNumberGenerator.GetInt32`. Generate makes up to `equalityIterations` (50) attempts
    and keeps the lowest `GetInequality`. An attempt counts if its inequality is at least
    -999; invalid attempts get up to 15 retries. It stops early below `equalityLimit`,
    regenerates the best seed if it wasn't the last, then sets every score from
    `PlayerExtensions.GetScore`.
  - The game ends when no AutoPlay=false player is alive.
  - The UI (HasEmbassyWith and similar) lazily creates default diplomacy relations, and the
    advisor runs AI.GetMove for the human seat. These are UI side effects only.
- **Two corrections to earlier findings:**
  - The population "bug" was my UI. `population` is cumulative and `xp` is progress to the
    next level.
  - Map types are official (`GameSettings.mapPreset`).

## Where it stopped

**The interface is ready to start building the bot (M1).** Everything in "Next steps" 1–2 below
is optional hardening that can run alongside.

## Next steps, in order

1. **M1, the smallest learning loop** (docs/design/training-and-data.md):
   - Generate bot-game data with `polyenv`: both seats as bots (`bots=(d, d)`), or one outside
     seat stepping `STEP_BOT_OWN`, and record per decision:
     - the observation;
     - the legal list;
     - `bot()` (the label; -1 for its StayCommand);
     - the outcome.

     Speed: 2.4 Crazy-vs-Crazy games/s on 6 emulator processes. The 4 free tribes run in the app;
     all 12 run in polyhost.
   - Model: the pointer policy over legal rows plus a value head. Install PyTorch (Mac MPS, or the
     PC's GPU).
   - Ben checks in after M1 (plays 2 games). The browser UI runs on `env::Match` now, so it can
     be used for that once polished.
2. **Interface hardening:**
   - the perturbation fog-leak test;
   - canonical hash and determinized-state builder (M3);
   - a dummy-network end-to-end benchmark.
   - **Speed:**
     - direct method-pointer calls instead of `runtime_invoke`;
     - a cheaper legal list: tile generators run on all 196 tiles, and research checks all 63
       techs;
     - threads in one process.
   - **Real-app self-play oracle** (the previous session's step 1):
     - `Interceptor.replace` `ClientActionManager.Pause`/`HoldExecution` with no-ops (approved by
       Ben);
     - make simulation mode opt-in in `agent.js`;
     - then pass-and-play Might games played to the end and diffed with `replay`.
   - **Open check:** Crazy vs Crazy won 6 of 6 for the first seat. Measure first-move advantage
     over more games.
3. **The native Windows build on `backhouse`** (needs Ben to install Polytopia from Steam there;
   still not installed as of this session). Check that its game version is 123, then port
   polyhost (`LoadLibrary("GameAssembly.dll")`).
4. **Keep committing** to `main` on the public repo. Never commit `game/`, which is Midjiwan's
   code and data.
