# Polytopia UI Reference

**Purpose.** Describe how the *real* game's human interface behaves, concretely enough to implement
against. This is a behavioural spec, not a style guide.

**Scope.** Live game, mobile and Steam. **The current shipping version is 2.17.2**, not 2.16 as your
brief assumed — 2.17.x notes cover a Unity bump, *"Improved map zoom with + and - keys"*, and a
multiplayer tribe-disabling fix
([App Store](https://apps.apple.com/us/app/the-battle-of-polytopia/id1006393168)). Nothing in this
document is invalidated by that, but if you diff against release notes, start from 2.17.2.

**One correction is flagged in red below** (§5.7, enemy city visibility) where I initially reasoned
to the wrong conclusion and a screenshot overturned it. It is the kind of error a clone would
inherit, so it is called out rather than quietly fixed.

---

## 0. Sources and how much to trust them

| Source | What it is | Confidence |
|---|---|---|
| **`caderek/polytopia-languages/original.json`** ([raw](https://raw.githubusercontent.com/caderek/polytopia-languages/master/original.json), [repo](https://github.com/caderek/polytopia-languages)) | **The game's own English localisation file**, 1404 keys, dumped from the shipped app. Every UI label, button, tooltip and error string, with its namespace (`topbar.*`, `bottommenu.*`, `actionbox.*`, `techview.*`, `playerinfo.*`, `tooltip.*`). Last updated 2022-08-15. | **A — primary.** This is the game telling us what its own widgets are called. Version caveat below. |
| **`Citillan/Polytopia-Language-Improvements/English+.json`** ([raw](https://raw.githubusercontent.com/Citillan/Polytopia-Language-Improvements/main/English%2B.json), [repo](https://github.com/Citillan/Polytopia-Language-Improvements)) | A 2024-02 community re-translation built against a **2024 key set**. | **A for key-set verification, B for wording** — the author edited the English text, so use caderek for verbatim strings and Citillan only to prove a key still exists. |
| Polytopia wiki, via `https://polytopia.fandom.com/api.php?action=parse&page=<Title>&prop=text&format=json` | Community wiki. Rendered HTML is Cloudflare-gated; the API is not. `WebFetch` gets HTTP 402 on it — use `curl`. | B. Occasionally stale; flagged inline. |
| [polytopia.io](https://polytopia.io/) blog + [Steam announcements](https://store.steampowered.com/news/?appids=874390) | Official dev posts. | A. |
| **Live-game mod source (2026)** — [PolyModdingTeam/PolyMod](https://github.com/PolyModdingTeam/PolyMod), [Polibrary-Team/Polibrary](https://github.com/Polibrary-Team/Polibrary), [johnklipi/PolytopiaMapMaker](https://github.com/johnklipi/PolytopiaMapMaker), [OutlastOutworkOutthink/Better-BoP-Mod](https://github.com/OutlastOutworkOutthink/Better-BoP-Mod) | Harmony patches against the **shipped IL2CPP assemblies of Polytopia build 122 (= v2.16.x)**. Class names, method signatures and field names are the real ones, as of 2026. | **A+ — this is the actual current UI, not a description of it.** |
| **Frame-by-frame video analysis** — [Beginners Guide to Polytopia Ep 1](https://www.youtube.com/watch?v=hI8e-Lw75MI) (**uploaded 2026-03-11, i.e. the current build**), [official tutorial](https://www.youtube.com/watch?v=G7zMMEKFb0g), [gameplay clip](https://www.youtube.com/watch?v=fuCH3xsXav4); plus full-resolution [iOS store screenshots](https://apps.apple.com/app/id6463000253) (**v2.17.1**) and [Steam screenshots](https://store.steampowered.com/app/874390/) | Actual rendered pixels of the shipped game, with on-screen text transcribed verbatim. | **A+ — direct observation.** Cited below as `[V]` (2026 video), `[iOS]`, `[STEAM]`. |
| [Steam patch notes](https://store.steampowered.com/news/app/874390) | Official per-build changelogs. | A. |
| Reviews, guides, Steam/Fandom threads | Player-written. | C — used only for interaction details nothing else covers, always attributed. |

### Version caveat on the localisation dump — read this

The caderek dump is from **August 2022** (~v2.1–2.4). I verified the schema is stable by diffing it
against the February 2024 Citillan file: **every load-bearing UI key still exists in 2024** —
`topbar.stars`, `topbar.turn`, `topbar.score`, all four `bottommenu.*`, `actionbox.insufficientfunds`,
`actionbox.building.requiredtech`, `actionbox.unit.toomany`, `actionbox.unit.kills`, `techview.locked`,
`techview.locked.info`, `techview.expensive.info`, `tooltip.unit.actions.move`,
`tooltip.unit.actions.attack`, `playerinfo.locked.researchdiplomacy`, `playerinfo.stats.income`,
`gamestatus.capitals`, `gamestatus.unknown.tribe`, `world.unit.veteran.progress`,
`settings.suggestions`, `settings.confirm`.

Only two of the keys I checked disappeared: `actionbox.cantperformaction.description` and
`settings.compactui`. Between 2022 and 2024 the file gained 135 keys (`building.names.market`,
`building.names.aquafarm`, `unit.names.rammership` = "Ironclad", `settings.replay.enablefog`,
`settings.replay.sharedfog`, `tooltip.abilities.stiff`, `tooltip.unit.city.capture.flying`, …) and
lost 237 — consistent with content churn, **not** a UI redesign.

**Conclusion: the UI architecture described here has been stable since at least 2022.** Where I quote a
2022 string I say so; treat exact wording as ~2022 and the *structure* as current.

---

## 1. HUD / permanent chrome

### 1.0 The real widget tree (from build 122 assemblies)

Mods patch these classes by name, so these are the actual components of the live in-game HUD:

```
HudScreen                      // root of the in-match UI; OnMatchStart, OnNextTurn(bool forceConfirmation)
├── ResourceBar                // TOP BAR. Exactly three children:
│     ├── currencyContainer    //   stars  (has headerLabel)
│     ├── scoreContainer       //   score
│     └── turnsContainer       //   turn
├── HudButtonBar               // BOTTOM BAR of persistent buttons; Init()
└── InteractionBar             // THE CONTEXTUAL ACTION MENU
      ├── tile                 //   the Tile it is bound to
      ├── description          //   a text line inside the bar
      ├── buttons              //   List<UIRoundButton>
      ├── Show(bool instant, bool force)
      ├── AddAbilityButtons(Tile tile)
      ├── AddUnitActionButtons(List<CommandBase> availableActions)
      └── CreateRoundBottomBarButton(string label, bool) -> UIRoundButton
```

Sources: [Polibrary `src/UI.cs`](https://github.com/Polibrary-Team/Polibrary/blob/main/src/UI.cs),
[MapMaker `src/UI/Editor.cs`](https://github.com/johnklipi/PolytopiaMapMaker/blob/main/src/UI/Editor.cs),
[PolyMod `src/Managers/Visual.cs`](https://github.com/PolyModdingTeam/PolyMod/blob/main/src/Managers/Visual.cs).

`UIRoundButton` fields used by mods: `.text`, `.sprite`, `.Cost`, `.ShowLabel`,
`.m_showLabelBackground`, `.UpdateLabelVisibility()`, `.OnClicked`. **Cost is a per-button property
with its own visibility flag** — see §3.2, this matters.

Note `ResourceBar` having exactly `currencyContainer` / `scoreContainer` / `turnsContainer` — this
independently confirms the three-readout top bar below, from the shipped build rather than from a
2022 string table.

### 1.1 Top bar — three readouts

Namespace `topbar.*` ([original.json](https://raw.githubusercontent.com/caderek/polytopia-languages/master/original.json)):

```
topbar.stars = 'Stars (+{0})'
topbar.score = 'Score'
topbar.turn  = 'Turn'
```

- **Stars**: current balance with **stars-per-turn in parentheses**, e.g. `12 (+5)`. Confirmed
  independently by the wiki [Star](https://polytopia.fandom.com/wiki/Star) page: *"This income is
  measured in stars per turn (SPT) and is shown in brackets next to the current amount of stars."*
- **Score**: **your own live score, always on screen.** Not end-of-game only.
- **Turn**: current turn number.

There is no opponent information in the top bar.

### 1.2 Bottom bar — exactly four buttons

Namespace `bottommenu.*` — the complete set is four keys, nothing else:

```
bottommenu.menu      = 'Menu'
bottommenu.gamestats = 'Game Stats'
bottommenu.techtree  = 'Tech Tree'
bottommenu.nextturn  = 'Next Turn'
```

Corroborated by the Steam keyboard shortcuts (community-compiled,
[thread](https://steamcommunity.com/app/874390/discussions/0/2797250897506712887/)): `1` = Menu,
`2` = Scoreboard, `3` = Tech, `4` = End turn — the same four, in the same order.

**As rendered** `[V, iOS, STEAM]`: four large **round** buttons with labels beneath, positioned
**bottom centre-right** — Settings (hamburger) · Game Stats (tribe head) · Tech Tree (flask) ·
End Turn (checkmark). The top readouts sit **top centre**, as three labelled columns.

Three behaviours a clone will miss:

- **The Game Stats button carries a live rank badge** — `1st`, `2nd`, … on the button itself. **Your
  current placing is permanently on the HUD**, not buried in a screen.
- **End Turn turns solid blue when no unit has actions left** `[V 682s+]` — *"once there's nothing
  left to move the end turn button will turn blue."* This is the visual counterpart of
  `world.turn.nomoves`.
- **Press-and-hold End Turn cycles the camera to units that still have actions.** A "next idle unit"
  affordance hidden on the end-turn button.

**Next Turn is a state machine, not a static label.** Relevant `world.turn.*` strings:

```
world.turn.next     = 'Next Turn'
world.turn.end      = 'End Turn'
world.turn.your     = 'Your turn'
world.turn.start    = 'START'
world.turn.nomoves  = 'No more moves available, end turn'
world.turn.last     = 'Last turn!'
world.turn.remaining     = '{0} turns left'
world.turn.waiting       = 'Waiting for {0} to play...'
world.turn.waiting.unknown = 'Waiting for an unknown tribe to play...'
world.turn.end.question  = 'End your turn?'
world.turn.end.confirm   = 'Confirm'
```

Implement at minimum: idle "Next Turn" → **"No more moves available, end turn"** once every unit has
acted and nothing is affordable. This nudge is a real feature and a clone will omit it.

`world.turn.end.question` / `.confirm` fire only when the **`settings.confirm = 'Confirm turn'`**
option is on — so end-turn confirmation is opt-in, off by default.

### 1.3 Full keyboard map (Steam)

Community-compiled, no official list exists
([Steam](https://steamcommunity.com/app/874390/discussions/0/2797250897506712887/)):

| Key | Action |
|---|---|
| `WASD` | pan map; also navigates the tech tree |
| Arrow keys | move the map tile *selection* |
| `Space` | **open the tile interaction menu** — "eg. open menu to build roads, harvest resources, train troops on a city etc." |
| `1` | Menu |
| `2` | Scoreboard (Game Stats) |
| `3` | Tech tree |
| `4` | End turn |
| `Enter` | End turn / OK |
| `Esc` | Menu / Start Turn / Cancel |
| `+` / `-` | zoom the map (added in **2.17.2**) |

The Steam store page lists "Keyboard controls and shortcuts" as a feature
([Steam](https://store.steampowered.com/app/874390/The_Battle_of_Polytopia/)), but keybinds are not
rebindable in options.

### 1.4 Settings that change interaction

From `settings.*`:

```
settings.suggestions = 'Suggestions'
settings.confirm     = 'Confirm turn'
settings.info        = 'Info on Build'
settings.scale       = 'UI Scale'   (Auto / Small / Medium / Large / Very Large)
settings.compactui   = 'Compact UI'   [2022 only; absent from the 2024 key set]
```

Plus replay-only toggles added by 2024: `settings.replay.autofocus`, `settings.replay.enablefog`,
`settings.replay.sharedfog`.

**`Info on Build` inserts a confirmation popup before every build/train — see §2.1b.**
**`UI Scale` also changes map zoom**, not just widget size
([speedrun.com guide](https://www.speedrun.com/the_battle_of_polytopia/guides/hx5jb)).

**Suggestions** is the in-game hint system:

```
world.suggestion.title   = "Here's a Tip!"
world.suggestion.message = 'You should {0}'
world.suggestion.disable = '(These suggestions can be switched off in the menu)'
```

The `action.info.*` namespace supplies the `{0}` fragments, which is why those strings are written as
sentence continuations:

```
action.info.move    = 'move this unit. Select the unit and tap any of the BLUE targets'
action.info.attack  = 'make an attack with this unit. Select the unit and tap any of the RED targets if you want to attack'
action.info.train   = 'train a unit in this city. ...'
action.info.capture2= 'capture this city. ...'
action.info.endturn = "end this turn to get more resources and unit moves. Press the 'Next Turn' button"
action.info.build   = 'build a {0} here'
action.info.research= 'research {0}.'
```

With Suggestions on, unmoved units are also flagged: *"If you have hints turned on, they will mark
unmoved units with a pink heart to help you out"*
([Level Winner](https://www.levelwinner.com/the-battle-of-polytopia-beginners-guide-tips-tricks-strategies-to-rule-the-world/)) — C-grade, single source, but consistent with the Suggestions toggle existing.

`v2.16.3` added *"Always focus on player capital when opening a game, even if auto focus is off"*
([polytopia.io](https://polytopia.io/bug-fixes-in-version-2-16-3/)) — so an **auto-focus** camera
setting exists that pans to whatever needs attention.

### 1.5 Undo — there is none

No `undo` key exists anywhere in the 1404-key localisation file. Community feature requests confirm
the absence and describe the consequence:

> "I am a bit clunky and sometimes select a unit, after which I press some nearby tile (e.g. to
> upgrade it) and the unit gets accidentally moved."
> — [Steam beta discussion](https://steamcommunity.com/app/874390/discussions/2/603034577716472879/)

Also [Undo Button](https://polytopia.fandom.com/f/p/3133400722440360810) and
[CONDITIONAL Undo Button](https://polytopia.fandom.com/f/p/3512704744554664589/r/3768372613349544477)
on the wiki forums. **Actions commit immediately and irreversibly.**

That quote also documents a real behaviour worth copying deliberately or rejecting deliberately:
**while a unit is selected, tapping a nearby reachable tile moves the unit there** rather than
selecting that tile. Selection state changes what a tap means.

### 1.6 Turn timer

**Single-player has no clock.** Perfection instead shows a turn counter (e.g. `16/30`) via
`world.turn.remaining = '{0} turns left'`.

Multiplayer: the host picks **Live / 24 hours / 7 days**
([Level Winner](https://www.levelwinner.com/the-battle-of-polytopia-beginners-guide-tips-tricks-strategies-to-rule-the-world/)).
**Live games use a dynamic time bank**: the per-turn allowance *increases with how many units and
cities you have*, and unspent time carries over — *"Any time left at the end of your turn will be
added to your next turn."* It cannot be disabled in Live games.
The `gameitem.*` strings show the enforcement UI:

```
gameitem.timelimit          = 'Time limit'
gameitem.timelimit.info     = 'You have {0} to make your move, after that you will be removed from the game.'
gameitem.timelimit.timeleft = 'Time Left'
gameitem.timelimit.timeleft.info = 'This is the time you have left to make your move. When the time runs out you will be automatically skipped.\n\nYou have been skipped {0} times in this game.\n\nAfter {1} skips you will be kicked from the game.'
gameitem.timeup      = 'Time Up!'
gameitem.timeup.info = 'Your time to make a move is up, do you want to resign?'
gameitem.slow.action = 'REMIND'
gameitem.skipturn.action = 'SKIP TURN'
gameitem.kick.action = 'KICK'
```

Timeout → auto-skip, N skips → kick. Not a hard turn clock inside the map view.

---

## 2. Selection and tile interaction

### 2.1 The core model

One tap on a tile opens a contextual action menu for that tile. It has two internal names: the
localisation namespace is **`actionbox.*`** and the runtime class is **`InteractionBar`**. I use
"action bar" throughout; when I quote a string key it will say `actionbox`.

The tutorial phrases every interaction as "click X and select Y":

```
tutorial.task.trainunit.description   = 'Click on your city and select the unit you want to train. ...'
tutorial.task.harvestfruit.description= "Click a fruit tile within the borders of your city and select the 'Harvest Fruit' option. ..."
tutorial.task.capturevillage.description = 'Click your unit in the city. The blue markers that appear on the map are places the unit can move to this turn. ...'
tutorial.task.unlockTech.description  = "Click the 'Tech Tree' button and the pick the 'Hunting' technology. ..."
```

### 2.1a Geometry — three cooperating surfaces, not one menu

This was the hardest thing to pin down and the answer is **not** "one menu". Selecting a tile lights
up **three distinct surfaces at once**, confirmed by frame-by-frame reading of a March 2026 gameplay
video and full-resolution v2.17.1 store screenshots.

**① Description panel — bottom-LEFT corner, screen-anchored.**
A dark rounded card with an icon, the tile/unit name, a one-line description, and a chevron to
expand. Transcribed verbatim from `[V]`:

```
Field, Fruit   / Extract this resource to upgrade your city
Field, Fruit   / This resource is outside of your empire
Field, Crop    / Research Farming to extract this resource
Village        / Move a unit here to capture this city!
City ▪Imperius / Build a road to connect this city to your capital.
```

This is `InteractionBar.description`. Note it is where **"why you can't"** messages surface.

**② Tile action button — anchored in WORLD SPACE, floating above the selected tile.**
A **round** button on a small grey fan/shelf that points down at the tile. Inside: the action's
isometric icon. Top-right of the circle: **gold star + price**. Below the circle: the action name.
Captured: a circle with two apples, `★2`, labelled **"Harvest Fruit"** `[V ~513.5s]`; same pattern
with an animal icon, `★2`, **"Hunting"** in the official tutorial.

**③ Unit ability buttons — round buttons in the bottom-left tray**, stacked under the description
panel. Captured: **"Recover"** (blue circle, green plus glyph) and **"Capture"** (blue circle, flag
glyph) `[V 760s, 794–804s]`.

**So: tile build/harvest actions hover over the tile; unit abilities sit bottom-left.**

This reconciles with the shipped assemblies rather than contradicting them. The MapMaker mod adds a
button via `InteractionBar.AddAbilityButtons` using
`CreateRound**BottomBar**Button() -> UIRoundButton`
([`src/UI/Editor.cs`](https://github.com/johnklipi/PolytopiaMapMaker/blob/main/src/UI/Editor.cs)) —
i.e. **the "bottom bar" naming belongs to surface ③, the unit-ability tray**, which is exactly where
that mod's button appears. `InteractionBar` owns all three surfaces: `.tile`, `.description`,
`.buttons`, `AddAbilityButtons(Tile)`, `AddUnitActionButtons(List<CommandBase>)`, and
`Show(bool instant, bool force)` to animate them in and out as selection changes.

**The 2021 "radial menu" review was not wrong either** — see §6.2: the *city unit-training* menu
genuinely is a radial circle menu, confirmed by an official patch note.

> "if you click on a piece of terrain you get a radial menu with all the things you can build, click
> on a city and you get a similar menu with all the units you can build and if you click on a unit
> you'll see **markers on the ground** showing where you can move."
> — [Save or Quit, 2021-03-02](https://saveorquit.com/2021/03/02/review-the-battle-of-polytopia/)

Two structural consequences that matter more than the shape:

1. **One controller serves every tile type.** Build menu, harvest menu, city training menu and unit
   ability tray are all `InteractionBar` in different modes. There is no separate "city screen" (§6).
2. **`AddUnitActionButtons` takes a `List<CommandBase> availableActions`** — populated from *command
   objects*, each button carrying a `Cost`. See §3.

Build options are **sorted by price** (Steam patch note, Beta 2.0.12.2930).

### 2.1b Two taps or one — the "Info on Build" setting

Tapping the tile action button opens a **black rounded-rect action box near the tile**. Transcribed
verbatim from `[V ~514.5s]`:

```
┌──────────────────────────────────┐
│ Harvest Fruit                 ★2 │
│ Produces 1 population for Ipres. │
│ Must be on a tile with Fruit.    │
│                                  │
│      [ BACK ]      [ DO IT ]     │   BACK = grey,  DO IT = blue
└──────────────────────────────────┘
```

Note the structure: **title + price, effect line, requirement line, then a grey cancel and a coloured
commit.** The requirement line is present even when the requirement is satisfied.

A confirm step exists:

```
actionbox.building.doit    = 'DO IT'      [2024 file: 'CONFIRM']
actionbox.confirm          = 'Confirm {0}'
actionbox.confirm.button   = 'YES'
actionbox.confirm.info     = 'Are you sure you want to do this?'
actionbox.confirm.info.breakpeace = 'Are you sure you want to break the peace treaty? Doing so will disable all your units for the rest of this turn.'
actionbox.confirm.info.breakpeace.disband = 'It will also disband the {0} units you have in their territory.'
```

**`settings.info = 'Info on Build'` is exactly this toggle**, and a speedrunning guide states its
effect verbatim:

> "Turn **Info on Build OFF**, the only thing it's good for is give you **one more popup animation and
> one more click** to do any unit training or resource gathering."
> — [Basic guide (Moonrise update), speedrun.com](https://www.speedrun.com/the_battle_of_polytopia/guides/hx5jb)

So:

- **Info on Build ON (default):** tap an action → an info popup appears describing what you are about
  to build/train → tap `DO IT` / `CONFIRM` to commit. **Two taps.**
- **Info on Build OFF:** tap an action → it executes immediately. **One tap.**

This reconciles `actionbox.building.doit = 'DO IT'` with the review's "it happens instantly" — both
are true, under different settings.

The same guide gives two more settings facts:

- **Confirm turn** — *"Turn Confirm turn OFF, as it doesn't save you from accidentally ending a turn,
  it's just pure time loss to wait for that extra popup and do an extra click."*
- **UI Scale** — *"Turn UI Scale to Very Large so buttons will be larger and easier to hit, though it
  also zooms in on the map more so you might need to zoom out in the middle of animations."* Note this
  means **UI scale and map zoom are coupled**.

**Implication for a clone: the info-popup step is the default experience.** A player's first
encounter with any build or train action is a description of what it does, not a silent commit — and
experienced players turn that off. Ship both paths.

### 2.2 Tap-by-tap behaviour

**Honesty note.** *Every* tile tap opens the same `InteractionBar`, bound to that tile
(`InteractionBar.tile`), with a `description` line and a row of round buttons. The table below says
what *populates* it in each case. The population rules are reconstructed from the game's string table
plus `InteractionBar.AddAbilityButtons(Tile)` / `AddUnitActionButtons(List<CommandBase>)`; I did not
observe the running game. Rows sourced only to "localisation" are strong inference from a string that
must be displayed somewhere, not direct observation.

| You tap | What happens | Source |
|---|---|---|
| **Empty tile you own** | Action box: buildings you can build here, resources you can harvest here, roads. `action.info.build = 'build a {0} here'`, `actionbox.tile.roads = 'roads'` | localisation; Save or Quit |
| **Tile you don't own** | Still selectable and inspectable, but improvement actions are refused: `tooltip.tile.outside = 'This resource is outside of your empire'` | localisation |
| **Your own unit** | Unit info box opens **and** move/attack markers paint on the map — blue for move, red for attack. | `tooltip.unit.actions.move`, `.attack`; tutorial |
| **Enemy unit** | Inspect only: `tooltip.unit.enemy = 'This is the enemy!'`, `tooltip.unit.enemy.territory = 'This enemy is in your territory!'`, `tooltip.unit.enemy.city = 'This enemy is capturing your city!'`, `tooltip.unit.ally = 'This is your ally.'` | localisation |
| **Your own city** | Action box in "city" mode: the unit-training list. `actionbox.city = 'City'`, `actionbox.city.level = 'lvl {0}'`, `actionbox.unit.train = 'TRAIN'` | localisation; Save or Quit |
| **Enemy city** | Inspect: `building.city.owner = '{0} is a city in the {1} empire'`, `building.capital.owner = '{0} is the {1} capital'`, `building.capital.owner.former = '{0} is the former capital of the {1} empire, currently occupied by {2} forces'`. Capture is an action **on your unit**, not on the city (§2.4). | localisation |
| **Neutral village** | `actionbox.village = 'Village'`, `world.building.village = 'Village'`, `tooltip.tile.capture.tip = 'Move a unit here to capture this city!'` | localisation |
| **Cloud (undiscovered)** | **Nothing actionable.** `tooltip.tile.explore = 'Explore this area to see what it holds!'`. It is not inert though: *"When tapped on, a cloud tile plays one of four notes: G, B♭, C♯, or D. These four notes make up the melody of the main theme."* ([Terrain](https://polytopia.fandom.com/wiki/Terrain)) | localisation; wiki |
| **Ruin** | `tooltip.tile.ruin = 'Move a unit here and examine these ancient ruins.'` | localisation |

Other contextual tile tooltips — each one is a case a clone will silently drop:

```
tooltip.tile.choose_unit   = 'Choose a unit to train.'
tooltip.tile.limit         = 'This city cannot support any more units.'
tooltip.tile.blocked       = 'This resource is blocked by an enemy unit'
tooltip.tile.riot          = 'There are riots in this city! It produces no income this turn.'
tooltip.tile.road          = 'Build a road to connect this city to your capital.'
tooltip.tile.sailing       = 'Move a unit here to start sailing!'
tooltip.tile.monuments     = 'Monuments give your empire extra score!'
tooltip.tile.level.max     = 'Has reached maximum level'
tooltip.tile.level.next    = 'Next level in {0} turns'
tooltip.tile.capture       = 'This city is being captured.'
tooltip.tile.capture.enemy = 'Your city is being captured by the enemy!'
tooltip.tile.extract.stars   = 'Extract this resource to earn stars instantly'
tooltip.tile.extract.upgrade = 'Extract this resource to upgrade your city'
tooltip.tile.extract.convert = 'This resource can be transformed into a unit'
tooltip.tile.extract.research= 'You need to research {0} to extract this resource'
```

### 2.3 A tile with both a unit and a city

**Tapping selects the unit.** The evidence is the tutorial string, which is specifically about the
starting warrior standing in the starting capital:

> `tutorial.task.capturevillage.description` = **"Click your unit in the city.** The blue markers that
> appear on the map are places the unit can move to this turn."

I could **not** find a documented toggle, long-press, or cycle to reach the city underneath while a
unit occupies it. The community answer to "how do I train while a unit is in the city" is
*move the unit out first* — and that is a hard rule anyway, because a city tile is a tile and only one
unit may occupy a tile:

> "each tile can only be occupied by one unit at a time" — [Movement](https://polytopia.fandom.com/wiki/Movement)

So the ambiguity mostly dissolves: if a unit is standing in your city you **cannot** train there
regardless of what you select, because the newly trained unit would have nowhere to spawn. See
UNRESOLVED #2 — I have not confirmed whether the city's *other* actions (inspecting level/population)
are reachable without moving the unit.

### 2.4 Capture is a unit action

```
tooltip.unit.city.capture      = "Tap 'Capture' to add this city to your empire"
tooltip.unit.city.capture.next = 'This city will be ready to capture next turn'
tooltip.unit.city.capture.flying = 'Flying units cannot capture cities'   [2024]
action.info.capture  = 'Capture'
action.info.capture2 = 'capture this city. Cities generate stars every turn that you can use to develop your empire'
world.building.capture.ready = 'Will be ready to capture next turn'
world.building.capture.ready.title = 'Entering {0}!'
world.building.capture.warning = "Capturing will be completed next turn if you don't stop them."
world.building.capture.warning.title = '{0} is under siege!'
```

So: move a unit onto a village/city → it is *not* captured this turn → next turn, select the unit and
a **Capture** button appears in its action box. Matches the wiki: *"Any unit can capture a village if
it started the turn on the village, and capturing a village ends the unit's turn."*
([City](https://polytopia.fandom.com/wiki/City))

### 2.5 Deselection

No documented dedicated deselect affordance. On Steam, `Esc` is "Menu/Start Turn/**Cancel** in some
cases" ([Steam](https://steamcommunity.com/app/874390/discussions/0/2797250897506712887/)). The
community workaround is **tap the selected unit's own tile again** to dismiss the move overlay; a
"right click deselects" option is a standing feature request
([Steam beta thread](https://steamcommunity.com/app/874390/discussions/2/603034577716472879/)).
C-grade. See UNRESOLVED #3.

---

## 3. The action bar — and how unavailable actions are shown

**This is the section that matters most, and the answer is unambiguous.**

### 3.0 The three refusal states, as actually rendered

Confirmed by pixel-level reading of `[V]` (2026 build). **There is no padlock icon anywhere in this
game** — refusal is communicated by button colour, button absence, and explanatory text.

| Case | What you see |
|---|---|
| **Available & affordable** | Popup with `BACK` (grey) + **blue** commit button (`DO IT` / `RESEARCH`). Price in gold at top-right. |
| **Cannot afford** | Option still shown, popup still opens, but the commit button is **dark red with dim red text**; `BACK` stays grey. Body text: *"You don't have enough stars to buy this. Tap 'next turn' to get more stars."* |
| **Prerequisite not researched** | Popup opens, title suffixed **`(locked)`**, and the **commit button is absent entirely** — only a blue `BACK`. Body text: *"You must research X before you can learn Y."* |
| **Already done** | Title suffixed **`(completed)`**, body *"You have already researched this technology."*, `BACK` only. |

Verbatim capture, unaffordable tech `[V ~610.5s]`:

```
Farming                                          ★6
You don't have enough stars to buy this.
Tap 'next turn' to get more stars.
This tech will enable the following:  [Farm]

        [ BACK ]  (grey)      [ RESEARCH ]  (DARK RED)
```

Verbatim capture, prerequisite-locked tech `[V ~602s]`:

```
Forestry (locked)
You must research Hunting before you can learn Forestry.
This tech will enable the following:  Lumber Hut, Clear Forest

        [ BACK ]  (blue)          — no RESEARCH button at all
```

**The distinction to implement:** *can't afford* = red commit button (the action is legal, you just
lack stars, and the button still teaches you what it costs). *Prerequisite missing* = no commit
button (the action isn't reachable yet, so there is nothing to press). Both are still fully
inspectable, and both still tell you exactly what the tech would unlock.

On tiles rather than the tech tree, the missing-tech case surfaces in the description panel instead:
`Field, Crop / Research Farming to extract this resource` `[V 608s]`, and
`actionbox.building.requiredtech` swaps the commit button for a **`TECH TREE`** button that jumps you
there.

### 3.1 Unavailable actions are SHOWN, not hidden

Every "you can't do this" case in Polytopia has a **dedicated explanatory string**. A string like
that only exists because the option is rendered and the player taps it and gets told why. If these
options were filtered out of the menu, the strings would be dead code.

**(a) Cannot afford it — shown, with an explanation:**

```
actionbox.insufficientfunds = "You don't have enough stars to buy this. Tap 'next turn' to get more stars."
```

**(b) Requires un-researched technology — shown, with a shortcut to fix it:**

```
actionbox.building.requiredtech = "You need to research {0} to do this, tap the 'Tech Tree' button."
actionbox.building.techtree     = 'TECH TREE'
```

Note `actionbox.building.techtree = 'TECH TREE'` is a **button inside the action box**. The game
renders the locked building, tells you which tech gates it, *and gives you a button that takes you
straight to the tech tree*. This is the single biggest gap between a legality-mask UI and this one.

Same pattern for tile resources and for movement:

```
tooltip.tile.extract.research = 'You need to research {0} to extract this resource'
tooltip.tile.research         = 'You need to research {0} to be able to move here'
```

**(c) Blocked for another reason — shown, with the reason:**

```
actionbox.unit.toomany      = '(too many)'
actionbox.unit.toomany.info = "This city can't support any more units. Upgrade the city to get more space for units."
tooltip.tile.limit          = 'This city cannot support any more units.'
tooltip.tile.blocked        = 'This resource is blocked by an enemy unit'
tooltip.tile.outside        = 'This resource is outside of your empire'
tooltip.unit.actions.none   = "No actions left. Press 'Next Turn' to move this unit again."
tooltip.unit.city.capture.flying = 'Flying units cannot capture cities'
actionbox.embassyestablished.description  = 'The embassy is already established.'
actionbox.establishembassynocapital.description = 'You cannot establish an embassy if the tribe has no capital.'
actionbox.pendingpeacetreaty.title       = 'Pending Peace Treaty'
actionbox.pendingpeacetreaty.description = 'Peace treaty pending. Waiting for {0} to respond'
actionbox.cantperformaction.description  = 'You cannot perform this action right now.'   [2022 only]
```

`actionbox.unit.toomany = '(too many)'` is especially telling: it is a **suffix appended to a unit
entry that is still listed**. The unit stays in the training list, annotated `(too many)`.

**Rule to implement: never remove an action from the menu. Render it in a disabled state and attach
the specific reason.** The only things genuinely absent are actions that make no categorical sense
for that tile (there is no "Harvest Fish" on a mountain).

### 3.1a Engine-level proof: the UI asks for unavailable options

The game's own API for populating a city's training list is:

```csharp
CommandUtils.GetTrainableUnits(GameState gameState, PlayerState player, TileData tile,
                               bool includeUnavailable) -> List<TrainCommand>
```
— signature reproduced in [Polibrary `src/UI.cs`](https://github.com/Polibrary-Team/Polibrary/blob/main/src/UI.cs).

**There is a `bool includeUnavailable` parameter.** That parameter exists precisely because the UI
passes `true` — the menu is built from a list that deliberately includes the options you cannot take.
The legality check is a separate predicate (`GameLogicData.CanBuild(gameState, tile, playerState,
improvement)`), used to *annotate* buttons, not to filter the list.

**This is the architectural difference in one line.** A legality-mask UI computes the legal move set
and renders it. Polytopia computes the *full* option set, renders it, and decorates each entry with
why it is or isn't available.

Popup buttons carry an explicit state enum too — `PopupBase.PopupButtonData(labelKey,
PopupBase.PopupButtonData.States.None, action, …)` (from
[MapMaker `Editor.cs`](https://github.com/johnklipi/PolytopiaMapMaker/blob/main/src/UI/Editor.cs)) —
so "rendered but in a non-default state" is a first-class concept in this UI.

**And a disabled button is still clickable.** From the build-122 notes:

> "`PlayerInfoPopup.CreateDiplomacyActionButton` may retain **both action and unavailable callbacks**.
> `ClearCallbacks`, register one correct peace action, and intercept both
> `UIButtonBase.OnPointerClick` and `OnButtonClicked` … Keep the **unavailable-popup suppression** as a
> final guard."
> — [Better-BoP-Mod `MODDING_NOTES.md`](https://github.com/OutlastOutworkOutthink/Better-BoP-Mod/blob/main/MODDING_NOTES.md)

So each action button carries **two** callbacks: an `action` callback for when it is available, and an
**`unavailable` callback that fires a popup explaining why not** when it isn't. That is the delivery
mechanism for `actionbox.insufficientfunds`, `actionbox.building.requiredtech`,
`actionbox.unit.toomany.info` and friends.

**Implement it this way.** A greyed-out, inert button is *not* the same interaction: in Polytopia you
tap the thing you can't do and the game tells you why, and often hands you the fix.

### 3.2 Prices — shown on most buttons, but it is a per-button flag

`price.*` is a formatting namespace used across the UI:

```
price.stars = 'star' / price.stars.plural = 'stars'
price.population = 'population' / .plural
price.points = 'point' / .plural
```

Costs live on the button object itself, with **their own visibility toggle**:

```csharp
button.Cost = improvementData.cost;
button.ShowLabel = true;
button.m_showLabelBackground = true;
button.UpdateLabelVisibility();
```

**Do not over-generalise to "prices are always shown."** That snippet is from Polibrary, under the
comment *"Tries showing the cost of manual improvements in the unit interactionbar"*
([Polibrary `src/UI.cs`](https://github.com/Polibrary-Team/Polibrary/blob/main/src/UI.cs)) — i.e. it
is a **mod fixing a case where vanilla leaves the cost label off**: build actions offered from a
*unit's* interaction bar.

So the accurate statement is:

- Costs are shown on city unit-training buttons, on tile build/harvest buttons, and on tech nodes.
- At least one vanilla case — improvements built via a **unit's** action bar — ships **without** a
  visible cost label, which a modder considered worth patching.
- Mechanically, `ShowLabel` is per-button, so treat cost display as a property you set, not an
  invariant.

`actionbox.insufficientfunds` is what you get when you tap something you can't pay for. The wiki
[Buildings](https://polytopia.fandom.com/wiki/Buildings) page tabulates "Cost (stars)" per building,
matching what the menu shows.

### 3.3 Unit actions

From `action.info.*` — this is the full verb list the game ships:

| String | Label |
|---|---|
| `action.info.move` / `.attack` | move / attack (via blue/red markers, not buttons) |
| `action.info.capture` | `Capture` |
| `action.info.recover` | `Recover` (heal) |
| `action.info.healothers` | `Heal Others` (Mind Bender) |
| `action.info.healarea` | `Heal Area` |
| `action.info.disband` | `Disband {0}` |
| `action.info.promote` | `Promote` |
| `action.info.upgrade` | `Upgrade to {0}` |
| `action.info.examineruins` / `action.info.examine` [2024] | `Examine` |
| `action.info.explode` | `Explode` (Cymanti) |
| `action.info.freezearea` | `Freeze Area` (Polaris) |
| `action.info.breakice` | `Break Ice` |
| `action.info.boost` | `Boost` |
| `action.info.decompose` | `Decompose` |
| `action.info.destroy` | `Destroy` |
| `action.info.remove` | `Remove` |
| `action.info.stay` | `Stay` |
| `action.info.trip` | `Trip` |
| `action.info.establishembassy` | `Establish Embassy` |
| `action.info.peacetreaty` | `Offer Peace` |
| `action.info.breakpeace` | `Break Peace` |
| `action.info.meet` | `Meet` |
| `action.info.cityreward` | `City Reward` |
| `action.info.train` / `.build` / `.research` | train / build / research |

`Destroy` and `Decompose` also appear as `actionbtn.remove.building = 'Building'` /
`actionbtn.remove.roads = 'Roads'` — i.e. removal is split into two targets.

⚠️ **`Recover` is mostly a convenience button, because healing is automatic.** From
[Units](https://polytopia.fandom.com/wiki/Units): *"If a unit does not take any action in a given
turn, it will automatically heal if it can do so."* A clone that requires an explicit Recover click
to heal an idle damaged unit will diverge from the real game on every single turn. The explicit
button exists to heal *instead of* moving/attacking when the unit could otherwise act.

Two ability tooltips explain the removal verbs
(`tooltip.ability.*`):

```
tooltip.ability.destroy   = 'Remove any building within your borders, great for reconstructing your empire.'
tooltip.ability.decompose = 'Decompose any building within your borders and get its full cost in stars back at the end of the turn.'
tooltip.ability.disband   = 'Remove any of your own units and get half of its cost in return.'
```

### 3.4 Unit info box

Fields (`unit.info.*` and `world.unit.*`):

```
unit.info.health / .attack / .defence / .movement / .range / .skills
                = Health / Attack / Defense / Movement / Range / Skills
world.unit.info.from        = 'From the city of {0}.'
world.unit.veteran          = 'This unit is a veteran.'
world.unit.veteran.progress = '{0}/{1} to become a veteran.'
actionbox.unit.kills        = '{0}/{1} kills'
actionbox.unit.veteran      = 'Veteran'
actionbox.unit.new          = 'Train a new {0} {1}'
actionbox.unit.upgrade      = 'UPGRADE'
world.unit.ability          = 'Unit Ability'
```

Status badges are separate keys, so they render as chips on the unit:

```
actionbox.unit.Boosted / .Frozen / .Poisoned / .Invisible
```

Skills render as ALL-CAPS tags (`unit.abilities.dash = 'DASH'`, `FORTIFY`, `SCOUT`, `SPLASH`, …),
each with a plain-English tooltip (`tooltip.ability.dash = 'This unit can attack after moving if
there is an enemy in range.'`). The wiki confirms placement: *"They are always listed on the title
beside the name in unit info box."* ([Movement](https://polytopia.fandom.com/wiki/Movement))

**A unit's kill count and veteran progress are visible in its own info box.** This matters for §5.

### 3.5 On-map unit decoration

From the wiki [Units](https://polytopia.fandom.com/wiki/Units) page:

> "Every unit type has an unique icon shown on the map at the unit's **upper right**. For example, the
> icon for a Rider is shaped like the head of a horse. These icons can be used to determine at a
> glance which unit a naval vessel carries. **The white outline of a unit's icon disappears when it
> cannot make any attacks**, which is useful for identifying if a unit with the escape skill can
> attack after moving."

From [Combat](https://polytopia.fandom.com/wiki/Combat):

> "A **single shield** around the unit's HP indicates that the unit has a defence bonus. […] A
> **double shield** around a unit's HP indicates that the unit is standing in a city with a city wall."

So per-unit map decoration:

- **HP** — a **shield-shaped badge with a number, at the unit's upper-LEFT** `[STEAM]`.
- **Type icon** — at the unit's **upper-RIGHT**. Also reveals what a boat is carrying.
- **White outline on the type icon** = the unit can still attack; it **disappears** when it cannot
  (*"After you move a unit the white border disappears"*).
- **Cyan glow** around units that have not yet acted this turn `[V, all frames]` — a second,
  more visible cue than the outline.
- **Shields around the HP badge**: 1 = standard defence bonus, 2 = city wall.
- Status effect visuals for poisoned / frozen / boosted / invisible (v2.16.3: *"New visuals for unit
  effects (poisoned, frozen etc)"*, [polytopia.io](https://polytopia.io/bug-fixes-in-version-2-16-3/)).

---

## 4. Movement and targeting

### 4.1 Blue = move, red = attack

Unambiguous and stated four separate ways in the game's own strings:

```
tooltip.unit.actions.move   = 'Click a blue mark to move.'
tooltip.unit.actions.attack = 'Click a red mark to attack!'
action.info.move   = 'move this unit. Select the unit and tap any of the BLUE targets'
action.info.attack = 'make an attack with this unit. Select the unit and tap any of the RED targets if you want to attack'
tutorial.task.capturevillage.description = '... The blue markers that appear on the map are places the unit can move to this turn.'
```

Corroborated: *"When your unit is within the attacking range of an enemy unit, those units will have
red circles underneath them"*
([Level Winner](https://www.levelwinner.com/the-battle-of-polytopia-beginners-guide-tips-tricks-strategies-to-rule-the-world/)).

**As rendered** `[V 632–642s vs 802–816s]`: the markers are **concentric ring / donut decals laid
flat on the tiles** — **pale cyan-teal rings** on move targets, **orange-red rings underneath enemy
units** for attack targets. They differ in both hue and placement (move rings sit on empty ground;
attack rings sit under an occupied tile), so they remain distinguishable for colour-blind players.

⚠️ One sourcing trap: a plausible-looking quote — *"Click on a blue tile to move your unit… Click on
a red tile to select your attack target"* — circulates in search results but traces to a 2004 student
project for an unrelated game. The blue/red conclusion is correct, but source it to the game's own
strings above, not to that page.

**Yes, you see the full reachable set before committing.** Selecting a unit paints every legal
destination. Move and attack targets are visually distinct (blue vs red) and both are shown at the
same time.

`tooltip.unit.actions.none = "No actions left. Press 'Next Turn' to move this unit again."` — a spent
unit still selects and still shows its info box, it just paints no markers.

### 4.2 Battle preview

From [Combat](https://polytopia.fandom.com/wiki/Combat):

> "The battle preview shows the result of an attack. To see how much damage will be dealt to either
> unit, **hold on (mobile) or hover over (Steam)** an enemy unit within range. **Sweating** indicates
> that the enemy unit will be killed by the attack, while a **black and red ring** indicates that the
> attacking unit will die from retaliation."

A second source describes a skull instead of sweating: *"Having a skull on top of a unit would mean
that the unit would die after the attack"*
([Level Winner](https://www.levelwinner.com/the-battle-of-polytopia-beginners-guide-tips-tricks-strategies-to-rule-the-world/)).
Sources conflict on the icon — see UNRESOLVED #4. **They agree on the interaction**: press-and-hold /
hover an in-range enemy → exact damage both ways, with a death warning for each side.

This is a **non-committing preview**. A clone that only tells you the outcome after you attack has
removed a core planning tool.

v2.16.3 fixed *"Battle preview accuracy for ice archer"*
([polytopia.io](https://polytopia.io/bug-fixes-in-version-2-16-3/)) — confirming the preview is live
and maintained.

### 4.3 Movement rules that the markers must encode

The blue set is not "tiles within `movement` steps". From
[Movement](https://polytopia.fandom.com/wiki/Movement):

- Chebyshev distance, modified by terrain.
- Mountains and forests **block** movement — you may move *onto* them but not *through* them.
- Roads cost 0.5, and **round up**: *"even if you have 0.5 movement points remaining, you can still
  move onto a tile costing 1 or even 3 movement points."*
- **Zone of Control**: moving adjacent to an enemy unit ends movement immediately, even with points
  left. Bypassed only by `Creep`.
- **ZoC is projected by enemy units you cannot currently see** — the wiki's own example caption is
  *"An enemy hidden by the fog is blocking movement."* So the blue set is computed against ground
  truth ZoC, and the player will sometimes be stopped by something invisible. v2.16.3 fixed the
  *"Hinting interface for invisible nearby units"*, so the game does surface *some* signal here.
- "Units cannot move into clouds (the fog of war)."
- Most land units entering a Port become Rafts and are done for the turn.

---

## 5. Information visibility / fog

### 5.1 What you can see about opponents — summary

| About an opponent | Visible? | Where | Source |
|---|---|---|---|
| **Stars (balance)** | **Never.** No key anywhere exposes another player's star count. | — | absence in localisation |
| **Income (SPT)** | **Yes — but gated.** `playerinfo.stats.income = 'Income'` sits next to `playerinfo.locked.researchdiplomacy = 'Research Diplomacy to gain more insight.'` and `playerinfo.locked.upgradeembassy = 'Upgrade your embassy to gain more insight.'` | Tribe Info screen | localisation; see UNRESOLVED #5 |
| **Score** | **Yes, live, for every met tribe.** `gamestatus.score = 'score: {0} points'`, `gamestatus.scores = 'Scores'` | Game Stats | localisation |
| **City count** | **Yes.** `gamestatus.cities = '{0} cities'` / `gamestatus.city = '{0} city'` | Game Stats | localisation |
| **Capitals owned** | **Yes, in Might mode only** — it replaces city count. `gamestatus.capitals = 'Capitals Owned'` | Game Stats | localisation; wiki |
| **Researched tech** | **No.** No key exposes another player's tech. | — | absence in localisation |
| **Tribe identity** | Only after you meet them. Until then `gamestatus.unknown.tribe = 'Unknown tribe'`, `gamestatus.unknown.ruler = 'Unknown ruler'`, `world.turn.waiting.unknown = 'Waiting for an unknown tribe to play...'` | Game Stats | localisation |
| **Their relations with each other** | Only with Diplomacy researched. | Tribe Info | official + wiki |
| **Their opinion of you** | Yes (bots). `playerinfo.summary.reasons = 'They think you are {0}.'`, colour-coded. | Tribe Info | localisation; wiki |

The wiki's own gloss on the turn-0 score table makes the leak explicit: *"These scores can be used to
guess which tribe opponent players are using."* ([Score](https://polytopia.fandom.com/wiki/Score)) —
i.e. score is visible early enough that people fingerprint tribes from it.

### 5.2 Game Stats — a live in-game scoreboard

Bound to `2` on Steam and to the `Game Stats` bottom-bar button. `gamestatus.*` / `gamestats.*`:

```
gamestats.title      = 'GAME STATS'
gamestats.gamemode   = 'Game mode: {0}'
gamestats.bonus      = 'Difficulty bonus: {0}'
gamestats.battle     = 'Battle skills'      gamestats.battle.info = '{0} won, {1} lost'
gamestats.speed      = 'Speed skills'       gamestats.speed.info  = '{0}/{1} turns'
gamestatus.ruler       = 'Ruler of the {0}'
gamestatus.ruled       = 'Ruled by {0}'      gamestatus.ruled.bot = 'bot'   gamestatus.ruled.you = 'Ruled by you'
gamestatus.score       = 'score: {0} points'
gamestatus.scores      = 'Scores'
gamestatus.cities      = '{0} cities'
gamestatus.capitals    = 'Capitals Owned'
gamestatus.destroyed   = 'destroyed'
gamestatus.killedonturn= 'killed on turn {0}'
gamestatus.tribes      = 'Tribes destroyed'
gamestatus.tasks       = 'Tasks {0}/{1}'
gamestatus.tasks.complete = 'Complete!'
gamestatus.difficulty  = 'Difficulty rating'
gamestatus.unknown.tribe / .unknown.ruler = 'Unknown tribe' / 'Unknown ruler'
```

**It is live, not end-of-game.** It is one of only four bottom-bar buttons, and it lists every tribe
with a live score, a live city count, and whether they are dead and on which turn. Unmet tribes
appear as placeholder rows ("Unknown tribe"), which is itself information — **you always know how
many players are in the game.**

**Scores are exact integers, not bands.** A wiki screenshot of the Game Stats screen renders
per-tribe values Xin-xi 2,810 / Bardur 2,385 / Imperius 2,165 / Oumaji 1,075
([image](https://static.wikia.nocookie.net/supertribes/images/b/b1/Bot_Opponent_Relationships_Example.png)).
Do **not** confuse this with the *Tribe Info* screen's relative adjectives (Divine / Faithful /
Terrible / Pathetic, per [Tribe Relations](https://polytopia.fandom.com/wiki/Tribe_Relations)) — those
are flavour text on a different screen. Game Stats gives you the raw number.

This is what makes turn-0 tribe fingerprinting possible (§5.1, §5.8) and it is an established
competitive technique, not a curiosity.

Mode difference, from [Game Modes](https://polytopia.fandom.com/wiki/Game_Modes):

> "The Might game mode is unique in that in the Game Stats menu, **the number of capitals each tribe
> controls is shown instead of the number of cities** each tribe has, and tribes are **first ordered
> by the number of capitals** they control instead of the number of points they have."

So the default sort is **by score, descending**. `world.ranks = '1st,2nd,...,16th'` supplies the
ordinal labels.

The end-of-game screen (`endscreen.*`) is a *different*, richer screen: `Army & Territory`,
`Cities`, `Monuments & Temples`, `Science`, `{0} units, {1} tiles empire`, `{0} monuments, {1}
temples`, `{0}/{1} technologies researched`, `Difficulty Bonus`, `FINAL SCORE`. Do not conflate the
two — the score *breakdown* is end-of-game only.

### 5.3 Tribe Info screen

Reached from Game Stats — `tooltip.ability.peacetreaty` says: *"You find this function in the 'Tribe
Info Screen', **click the name of a tribe** to get there."* Introduced by the Diplomacy update:
*"New Tribe Info screen with more detailed info about your opponents"*
([Steam announcement](https://store.steampowered.com/news/posts/?feed=steam_community_announcements&appids=874390)).

```
playerinfo.title            = 'Tribe Info'
playerinfo.summary.ruledby  = 'The {0} tribe is ruled by {1}.'
playerinfo.summary.reasons  = 'They think you are {0}.'
playerinfo.summary.dead     = '{0} was destroyed by {1} on turn {2}.'
playerinfo.stats.income     = 'Income'
playerinfo.triberelations.title   = 'Tribe Relations:'
playerinfo.triberelations.unknown = 'You have not met this tribe yet.'
playerinfo.locked.researchdiplomacy = 'Research Diplomacy to gain more insight.'
playerinfo.locked.upgradeembassy    = 'Upgrade your embassy to gain more insight.'
playerinfopopup.bot.description = 'This is a bot. Bzzt!'
playerinfopopup.you.description = 'This is you. Go you!'
```

Relation pairs are rendered as explicit statements:

```
playerinfo.triberelations.popup.title = 'Tribe Relation'
... .neutral.description    = '{0} has a neutral relation to {1}'
... .peaceneutral.description = '{0} has a peace treaty with {1}'
... .peacepeace.description   = 'You are both at peace with {1}'
... .peacewar.description     = '{0} has a peace treaty with your enemy {1}'
... .warneutral.description   = '{0} is at war with {1}'
... .warpeace.description     = '{0} is at war with your ally {1}'
... .warwar.description       = 'You are both at war with {1}'
```

Opinion chips (`opinion.*`, 27 keys) are *"labelled in green for positive opinions and red for
negative ones"* ([Tribe Relations](https://polytopia.fandom.com/wiki/Tribe_Relations)); official
wording: *"Colored bubbles offer detailed explanations of what the tribes think of them"*
([polytopia.io/diplomacy](https://polytopia.io/diplomacy/)).

Relation state is also shown on the tribe head itself: at war = *"flames around their tribe head and
2 swords"*, at peace = *"golden shapes below the tribe head"*, horrible bot relation = *"red flames"*,
great relation = *"a halo"* ([Tribe Relations](https://polytopia.fandom.com/wiki/Tribe_Relations)).

### 5.4 When do you learn an opponent's tribe

On **meeting** them. Meeting is an event with its own popup and a **star reward**:

```
world.meet.tribe = 'You meet {0}'
wcontroller.meet.tribe.smaller.friendly = 'bows in awe of your mighty civilization.'
wcontroller.meet.tribe.bigger.friendly  = 'salutes you friendly but pays no attention to your tiny realm.'
wcontroller.meet.tribe.bigger.hostile   = 'laughs at your puny excuse of a Tribe.'
wcontroller.meet.tribe.smaller.hostile  = 'seems a bit hostile and greet you suspiciously.'
wcontroller.meet.tribe.distant = 'A delegation from a distant land arrives in your capital!'
wcontroller.meet.tribe.resource.boost = 'The discovery of this {0} tribe gives you a one time trade boost of {1} stars!'
```

[Star](https://polytopia.fandom.com/wiki/Star) quantifies it: *"Meeting an enemy player: 1 star plus 2
stars per 1000 points of the enemy's score rounded up to the next thousand; max. 11 stars."*
**The greeting text itself leaks whether they out-score you** — see [Tribe Relations](https://polytopia.fandom.com/wiki/Tribe_Relations)
for the mapping.

You can also be met *without seeing them*: `wcontroller.meet.tribe.distant` fires *"when a tribe that
you cannot see sends a peace treaty or puts an embassy in your capital"* or moves a unit into your
capital ([Tribe Relations](https://polytopia.fandom.com/wiki/Tribe_Relations)).

Before meeting: "Unknown tribe" / "Unknown ruler" / "Waiting for an unknown tribe to play...".

### 5.5 What Diplomacy reveals — precisely

[Diplomacy](https://polytopia.fandom.com/wiki/Diplomacy) (T3, after Strategy). Unlocks: Cloak,
Embassy, **Capital Vision**.

1. **Capital Vision.** The tightest official wording, from the Diplomacy FAQ:

   > "Capital Vision will reveal **the tile of the capital city of each tribe you've met** (and **not**
   > the capitals of any tribes you haven't met)."
   > — [Diplomacy FAQ, Steam](https://steamstore-a.akamaihd.net/news/externalpost/steam_community_announcements/44692728)

   Note the precision: **one tile**, not an area, and strictly scoped to tribes you have already met.
   Supporting: `tooltip.ability.capitalvision = 'Reveals the capital of all tribes you encounter.'`;
   wiki *"including capitals that have been captured"*; and
   [polytopia.io/diplomacy](https://polytopia.io/diplomacy/) *"Capital Vision will also take effect
   when players meet someone new"* — so it is both **retroactive** (capitals of tribes already met)
   and **prospective** (applies automatically on each new meeting).
2. **Relation graph.** *"Researching Diplomacy additionally allows you to see which players are at
   peace or at war with each other."* (wiki). Official: *"Researching Diplomacy will also improve the
   insight you have on other tribes, showing their relationships with their friends and enemies."*
   ([Steam announcement](https://store.steampowered.com/news/posts/?feed=steam_community_announcements&appids=874390))
3. **Opinion "reason pills" on the Tribe Info popup.** This one is confirmed from the shipped build,
   not inferred:

   > "Without Diplomacy, `PlayerInfoPopup.Refresh` does **not** call `GetLocalizedTopReasons`, so a
   > label postfix alone cannot display reason pills. For Oblivion, hide only **the locked-info
   > panel** and call the popup's native `SwapButtons` with the rebuilt three-reason dictionary."
   > — [Better-BoP-Mod `MODDING_NOTES.md`](https://github.com/OutlastOutworkOutthink/Better-BoP-Mod/blob/main/MODDING_NOTES.md) (Polytopia 122 / PolyMod 1.2.17)

   So: **without Diplomacy the Tribe Info popup shows a "locked-info panel" instead of the opponent's
   top three opinion reasons.** That locked panel is what renders
   `playerinfo.locked.researchdiplomacy = 'Research Diplomacy to gain more insight.'`, and the
   payload it unlocks is `playerinfo.summary.reasons = 'They think you are {0}.'` — the green/red
   opinion chips. There are **exactly three** reasons.

4. **What "more insight" means, officially.** The Diplomacy beta announcement pins the Diplomacy half
   of the gate to the **relations matrix**, not to any numeric stat:

   > "Researching Diplomacy will also improve the **insight** you have on other tribes, showing their
   > **relationships with their friends and enemies**."
   > — [Diplomacy! beta announcement, Steam](https://steamstore-a.akamaihd.net/news/externalpost/steam_community_announcements/42373261)

   The word "insight" is the same word used in both locked strings, and this sentence attaches it to
   relationships. So `playerinfo.locked.researchdiplomacy` gates the **Tribe Relations matrix + the
   three opinion reason pills**.

5. **Opponent income (`playerinfo.stats.income = 'Income'`) — most likely gated behind the EMBASSY,
   not Diplomacy.** By elimination: Diplomacy's "insight" is officially the relations matrix, and the
   only other locked string on the screen is `playerinfo.locked.upgradeembassy`. A player screenshot
   with Diplomacy researched but no embassy shows **no Income row**, which is consistent.
   **This is inference, not confirmation** — see UNRESOLVED #5. It is the most consequential open
   question in this document.

**Diplomacy does not reveal stars, score, or tech** — score is already public via Game Stats, and no
tech-reveal key exists.

> ⚠️ **Do not implement "opponent score is gated behind Strategy."** A widely-circulated reddit post
> presenting itself as the 2.8.6 changelog contains the line *"Seeing opponent's score now only
> available with Strategy."* **It is an April Fools' joke** (posted 2024-04-01, flaired "Meme").
> The real 2.8.6 notes are one line about a Unity version bump, and across the entire Steam news feed
> there is **no** official statement that opponent score was ever gated behind anything.
> [The post](https://www.reddit.com/r/Polytopia/comments/1bss1is/). It reads exactly like a real
> rule, which is why it is worth naming here.

**Embassy** ([Embassy](https://polytopia.fandom.com/wiki/Embassy), 5★): *"Building an Embassy will
additionally reveal a 3x3 area centered on the host capital city."* Official: *"Embassies reveal the
surrounding eight tiles"* ([polytopia.io/diplomacy](https://polytopia.io/diplomacy/)). Also
`playerinfo.locked.upgradeembassy = 'Upgrade your embassy to gain more insight.'` — see UNRESOLVED #6,
because embassies have **no documented upgrade mechanic** in the live game.

The embassy is visible to *everyone*: *"The dome on the embassy will have the colour of the sender,
allowing other players to see who has sent an embassy to that tribe."* So embassies leak diplomatic
information to third parties.

### 5.6 What you see about a visible enemy unit

Everything you see about your own, as far as I can determine: type, HP, defence shields, status
effects, and the type icon. The `world.unit.*` / `unit.info.*` info-box fields are not player-scoped
in the string table.

`actionbox.unit.kills = '{0}/{1} kills'` and `world.unit.veteran.progress = '{0}/{1} to become a
veteran.'` exist, and `world.unit.veteran = 'This unit is a veteran.'` — and a veteran is visually
distinct anyway (higher max HP). Whether the *kill counter* renders for enemy units is not
established — UNRESOLVED #7.

Hidden units are a real mechanic: `actionbox.unit.Invisible`, `tooltip.ability.hide = 'This unit hides
when it moves, becoming invisible to the enemy.'`, `tooltip.ability.detect = 'This unit can detect
nearby invisible units.'`, `world.reveal.header = 'Aha!'` / `world.reveal.text = 'A {0} was revealed!'`,
and `tutorial.suggestion.description = 'Someone is hiding here in the clouds...'`.

### 5.7 What you see about a visible enemy city

**Confirmed shown:** owner and capital status —

```
building.city.owner           = '{0} is a city in the {1} empire'
building.capital.owner        = '{0} is the {1} capital'
building.capital.owner.former = '{0} is the former capital of the {1} empire, currently occupied by {2} forces'
```

### 🔴 Enemy cities show a NAME PLATE ONLY — no income, no population bar

**I initially got this wrong and am correcting it.** The natural inference is that because level,
population and income are drawn as map decoration on the city tile, they must be visible on any enemy
city you can see. **They are not.** The game suppresses them for cities you do not own.

Verified by reading an official gameplay screenshot at full resolution
([File:Gameplay-sample.png](https://polytopia.fandom.com/wiki/File:Gameplay-sample.png)):

| City | Owner | Rendering |
|---|---|---|
| `Kolkolkol ★8` | mine | badge + name + **★income** + **segmented population bar** |
| `Ipmo ★4`, `Tenurolu ★4` | mine | badge + name + **★income** + **population bar** (dots visible) |
| `Leywawa`, `Camado`, `Ickber`, `Zanyaza` | enemy | **coloured name plate and nothing else** |

So for an enemy city you can see, you get: **its name, its owner (by plate colour), its position, its
level implicitly from the city sprite's size/buildings, and capital status.** You do **not** get its
income, its population, or its progress to the next level.

This substantially **vindicates the 2020 paper** on population and production, which I had wrongly
refuted. See UNRESOLVED #8.

What *is* still readable on an enemy city, because it is rendered as world geometry rather than as a
label: **Workshops and Parks appear as actual structures in the city sprite**, and city size grows
with level ([City](https://polytopia.fandom.com/wiki/City)). Capitals keep their crown/underline, and
a captured capital reads *"former capital of the {1} empire, currently occupied by {2} forces"*.
So a clone should render enemy cities' *buildings* but suppress their *numbers*.

### 5.8 Fog, vision, and the start of the game

**Two-layer model.** Clouds (undiscovered) vs. explored-but-not-currently-observed. Explored terrain
**stays revealed permanently.** Three independent lines of evidence:

- No key or wiki statement anywhere describes fog returning.
- The PolyEnv audit in `docs/rules-research/polyenv-audit.md` §7 notes the reference
  implementation's explored bitmask is append-only, *"matching the real game, where explored terrain
  stays visible."*
- Stated bluntly by a player: *"This game doesn't have fog of war besides the initial unexplored
  tiles."* ([Steam discussion](https://steamcommunity.com/app/874390/discussions/0/4768721849833022036))

**So "fog of war" in Polytopia means one-way cloud removal, not a re-fogging visibility layer.** What
you *do* lose when you walk away is live knowledge of mobile things — enemy units move, and you see
them only where you currently have vision. Terrain, cities and buildings, once seen, stay drawn.

**Vision radii** ([Combat § Vision](https://polytopia.fandom.com/wiki/Combat)):

- Normal unit: **1 tile** radius.
- On a mountain: **2 tiles** — *"Mountains provide extra sight (a two tile radius instead of the normal one)."*
- `Scout` skill: **2 tiles** — *"Units with the scout skill can see two tiles in every direction."*
  (`tooltip.ability.scout = 'This unit has double vision range.'`)
- Lighthouse: revealed by exploration, and grants map reveal
  ([Lighthouse](https://polytopia.fandom.com/wiki/Lighthouse)).

**Starting reveal: 5×5 = 25 tiles (radius 2), with 3×3 = 9 tiles of owned territory.**

Not stated in prose anywhere I could find — but it is **provable from the published turn-0 scores.**
[Score](https://polytopia.fandom.com/wiki/Score) gives: 20 pts/territory tile, 5 pts/explored tile,
100 pts/city, 5 pts per star of unit cost, 100 pts/tech tier. The wiki also states directly
*"level 1 city + 9 tiles territory = 280 points"* → 100 + 9×20 = 280, **confirming 9-tile (3×3)
territory.** Every tribe's turn-0 score then fits:

```
score = 100 (city) + 180 (9 territory tiles) + 125 (25 explored tiles)
      + 5 × (starting unit cost) + 100 × (starting tech tier) + 50 × (capital level − 1)
```

Imperius = 100+180+125 + 5×2 + 100×1 = **515** ✓. This reproduces all 16 published values (415, 515,
520, 530, 615, 620, 630, 730). The exploration term is **125 = 25 × 5**, so **25 explored tiles = 5×5,
radius 2**. Your notes' "disputed" reading is settled: **radius 2, and only radius 1 is owned.**

A corner spawn scores 5 less: *"If a tribe spawns in a corner, due to the update where lighthouses have
to be revealed by exploration, that corner tile will be obscured by the clouds. This will result in 5
points less than usual."* ([Score](https://polytopia.fandom.com/wiki/Score))

**Use this as an engine self-test.** If your turn-0 scores don't match exactly, your starting vision,
territory, unit costs, tech tiers or capital levels are wrong.

**Resources are hidden behind technology.** This is a *visibility* rule, not just a legality rule —
the sprite is not drawn ([Technology](https://polytopia.fandom.com/wiki/Technology)):

- Climbing — *"allows players to see metal"*
- Fishing — *"allows players to see starfish"*
- Organization — *"allows players to see crop"* (*"Zebasi can see crop tiles without needing
  Organization, since they start with Farming"*; Cymanti never see crop)

`tooltip.tile.extract.research = 'You need to research {0} to extract this resource'` covers the
already-visible-but-unextractable case.

**Fogged enemies still project Zone of Control** — see §4.3. The blue movement set is computed against
truth, so a player can be stopped by a unit they cannot see.

---

## 6. City UI

### 6.1 There is no city screen

**A city is a tile.** You tap it and get the same action box, in "city" mode, showing the list of
trainable units. `tutorial.task.trainunit.description`: *"Click on your city and select the unit you
want to train."* Save or Quit: *"click on a city and you get a similar menu with all the units you can
build."*

```
actionbox.city       = 'City'
actionbox.city.level = 'lvl {0}'
actionbox.village    = 'Village'
actionbox.unit.train = 'TRAIN'
actionbox.unit.new   = 'Train a new {0} {1}'
tooltip.tile.choose_unit = 'Choose a unit to train.'
world.building.info  = 'The city of {0}'
```

**The training menu is a radial circle menu, and it resizes itself.** Official patch note (Beta
2.0.9.2698): *"The circle menu for training units now scales with the amount of options to choose
from."* This is the one place the 2021 review's "radial menu" description is literally correct.

### 6.1a The city label, as rendered

Above each city tile, left to right `[STEAM screenshot 7, magnified]`:

```
(badge)  CityName  ★N
         ▁▁ ▁▁ ▁▁ ▁▁     ← population bar segments
```

- **Round badge**: **crown** = capital; **triangle-network** glyph = connected to the capital by road.
- **City name**, **underlined if it is a capital**.
- **Gold star + income per turn**, to the right of the name.

Confirmed values: `Kăugăn ★8` with crown badge; `Kitjasam ★4` / `Gyümkol ★4` / `Argy ★4` with network
badges.

### 6.2 Unit training list — what unavailable entries look like

- **Cannot afford**: entry shown with price; tapping yields
  `actionbox.insufficientfunds = "You don't have enough stars to buy this. Tap 'next turn' to get more stars."`
- **Not unlocked**: entry shown; `actionbox.building.requiredtech = "You need to research {0} to do
  this, tap the 'Tech Tree' button."` plus a `TECH TREE` button.
- **City at unit capacity**: entry shown, annotated **`(too many)`**, with
  `actionbox.unit.toomany.info = "This city can't support any more units. Upgrade the city to get more
  space for units."` and `tooltip.tile.limit = 'This city cannot support any more units.'`

Nothing is hidden. Every "no" comes with a reason and, where applicable, a route to fixing it.

### 6.3 Population bars — including negative

All on the **map tile**, under the city name ([City](https://polytopia.fandom.com/wiki/City),
[Population](https://polytopia.fandom.com/wiki/Population)):

- *"Every city has a set of population bars under its name representing how much population the city
  needs to upgrade."* Upgrading to level *n* requires *n* population, so a level-2 city shows **3**
  bars.
- Filled bars are **blue**. *"How much population a city currently has is represented by the number of
  filled-in (blue) population bars."*
- **Unit capacity is overlaid on the same bars as dots**: *"The maximum number of units a city can
  support is equal to the number of population bars (shown below the city's name) it has, which is one
  more than the city's level. **Dots in population bars represent the units supported by the city;
  if every population bar has a dot in it, no more units can be trained there.**"*
- **Negative population is drawn as red bars**: *"A city may have negative population, shown as **red
  population bars**. (This typically occurs to a former capital that has lost the population produced
  from city connections.) Each negative population decreases the income the city produces by one star
  per turn up to the point where the city no longer produces any income. A city cannot produce
  negative income. **However, negative population does not impact the number of units a city can
  support.**"*

**As rendered** `[STEAM shot 7, G7 01:01]`: unfilled segments are **grey/white**, filled are **blue**;
dots render **white-on-blue** and **black-on-grey** for contrast. A worked example from a real
screenshot — Kitjasam shows 2 blue-with-white-dot, 1 grey-with-black-dot, 1 grey-empty → **population
2, three units alive, capacity 4** (so it is level 3).

So a single bar strip encodes **three** things simultaneously: progress to next level (blue fill),
unit slots used (dots), and debt (red). Clone this exactly — it is the densest widget in the game.

- Income is a number **to the right of the city's name**: *"The income each city produces is shown to
  the right of the city's name."* ([Star](https://polytopia.fandom.com/wiki/Star))
- Capital: crown icon + underlined name. Connected: road symbol next to name.
- Workshops and Parks are **drawn into the city sprite**: *"Cities are visually comprised of city
  buildings. Any Workshops and/or Parks are also shown in the city buildings."*

### 6.4 Level-up reward choice

Levelling opens a **modal two-option choice**:

```
wcontroller.building.upgrade.reward = '{0} upgraded to level {1} and its production has increased +1. You also get to pick an additional reward:'
wcontroller.reward.workshop / .explorer / .citywall / .resources / .bordergrowth / .populationgrowth / .park / .superunit / .rebellion
```

Options by level ([City Upgrades](https://polytopia.fandom.com/wiki/City)): L2 Workshop | Explorer ·
L3 City Wall | Resources (5★) · L4 Population Growth | Border Growth · L5+ Park | Super Unit.
*"Upgrading a city offers a choice of one of two rewards."*

**As rendered** `[G7 76s]`: a black popup reading *"Urbu level up! Urbu upgraded to level 2 and its
production has increased +1. You also get to pick an additional reward:"* with **two large round
buttons side by side** — e.g. `+1★ Workshop` and `Explorer`.

Players report the dialogue **obscures the map** and want it collapsible
([Steam](https://steamcommunity.com/app/874390/discussions/2/603034577716472879/)) — it is a genuine
modal, not a sidebar.

---

## 7. Tech tree

### 7.1 It is part of the map

> "The tech tree is actually **not on a different screen. It occupies the same space as the map, just
> placed very far away in the top-right.** This means it is technically possible to access the tech
> tree by scrolling from the map, without pressing the tech tree button."
> — [Technology § Trivia](https://polytopia.fandom.com/wiki/Technology)

The `Tech Tree` bottom-bar button is a camera jump. On Steam, `WASD` pans it and `3` jumps to it. It
is zoomable and pannable, and renders on the same starfield background as the map (Steam patch notes:
*"Tech tree is displayed with the same background as the game-world"*, *"Added zoom in tech tree"*).
The top bar (Score / Stars / Turn) stays visible over it.

**Layout is radial.** The tribe's capital head sits at the **centre**, with **five branches radiating
outward** — one per T1 root, each with 2 T2 branches, each with 1 T3 leaf. **Tapping the centre head
exits the tree** and returns you to the map. Confirmed `[V 508.5s]`, `[iOS]`, `[STEAM]`.

### 7.1a Node states — three-way, visually unmistakable

| State | Rendering `[iOS, V]` |
|---|---|
| **Researched** | Solid **green** circle, icon + name in white, **green** connector lines. **No price.** |
| **Available** | **Blue** circle with a bright **red/orange outline ring**, red connector line, icon + name, and **gold star + cost at the top of the node** (`★6` Farming, `★10` Archery, `★7` Fishing). |
| **Locked by prerequisite** | Empty **dark circle, thin grey ring**, name in **grey italics**, **no icon and no price**. |

That third style *is* the lock indicator — again, **no padlock icon exists**. Note that a locked node
withholds its price and icon, so the tree does communicate "you cannot cost this out yet" while still
showing the node's name and position.

A permanent footer caption reads *"Tech costs increase for each city in your empire."*, with a second
line about Literacy once you own it.

### 7.2 State display — nothing is hidden

`techview.*` — the whole namespace is only 8 keys, and **three of them are states**:

```
techview.research       = 'RESEARCH'
techview.completed      = '(completed)'    [2024 file: '(Researched)']
techview.completed.info = 'You have already researched this technology.'
techview.locked         = '(locked)'
techview.locked.info    = 'You must research {0} before you can learn {1}.'
techview.expensive.info = "You don't have enough stars to buy this. Tap 'next turn' to get more stars."
techview.info           = 'Tech costs increase for each city in your empire.'
techview.info.literacy  = 'Literacy reduces the price of all technologies by {0}%!'
```

So every tech is one of: **researched `(completed)`**, **available (`RESEARCH` button + price)**,
**too expensive (price shown, tap → "You don't have enough stars")**, or **`(locked)` (tap → "You must
research X before you can learn Y")**.

**The entire tree is always visible with all prices.** Locked techs are labelled `(locked)`, not
removed, and tell you the prerequisite by name. `techview.info` explains the scaling cost inline.

### 7.3 Tech nodes show what they unlock

From the shipped build: `TechItem.GetUnlockItems(TechData techData, PlayerState playerState, bool
onlyPickFirstItem)` and `TechPopup.SetTechData(TechData data)`, where `TechData` carries
`improvementUnlocks` (a `List<ImprovementData>`) and `unitUnlocks` (a `List<UnitData>`)
([Polibrary `src/UI.cs`](https://github.com/Polibrary-Team/Polibrary/blob/main/src/UI.cs)).

So:

- **Each node in the tree renders unlock icons** — `TechItem.GetUnlockItems`, with an
  `onlyPickFirstItem` flag for the compact case where the node shows just one representative icon.
- **Tapping a node opens a `TechPopup`** listing every unit and every improvement that tech grants,
  before you buy it.

A clone that shows tech names and prices but not *what you get* has removed the main thing players
read the tree for. Related classes: `TechView.CreateNode`, `TechItem.SetupComplete`,
`TechPopupContent.CreateTechDataContent`.

Cost formula ([Technology](https://polytopia.fandom.com/wiki/Technology),
[City](https://polytopia.fandom.com/wiki/City)): base by tier, **+1/+2/+3 stars per extra city** for
T1/T2/T3, then Literacy applies a percentage discount. Prices on buttons are therefore dynamic — they
change when you take a city.

Since the 2025 balance pass, techs are researchable "backwards"
([polytopia.io/2025-balance-pass](https://polytopia.io/2025-balance-pass/)), so `(locked)` applies to
fewer nodes than it used to.

---

## 8. Starting stars per tribe

Straight from the official dev blog, [polytopia.io/2025-balance-pass](https://polytopia.io/2025-balance-pass/):

> "Xin-xi begins with 7 Stars / Oumaji begins with 6 Stars (that's not a typo, they begin with 6) /
> Hoodrick begins with 7 Stars / Quetzali begins with 7 Stars / Yădakk begins with 7 Stars / Luxidoor
> begins with 2 Stars in addition to their upgraded Capital / All other tribes begin with 5 Stars."

| Tribe | Starting ★ | Starting tech | Tier | Starting unit |
|---|---|---|---|---|
| Xin-xi | **7** | Climbing | T1 | Warrior |
| Imperius | **5** | Organization | T1 | Warrior |
| Bardur | **5** | Hunting | T1 | Warrior |
| Oumaji | **6** | Riding | T1 | Rider |
| Kickoo | **5** | Fishing | T1 | Warrior |
| Hoodrick | **7** | Archery | T2 | Archer |
| Luxidoor | **2** | *(none)* | — | Warrior |
| Vengir | **5** | Smithery | T3 | Swordsman |
| Zebasi | **5** | Farming | T2 | Warrior |
| Ai-Mo | **5** | Philosophy | T3 | Mind Bender |
| Quetzali | **7** | Strategy | T2 | Defender |
| Yădakk | **7** | Roads | T2 | Warrior |

**Warnings.**

- **Pre-2025 sources all say a flat 5 and are wrong.** The wiki is not reliable here; the dev blog is.
- **Luxidoor gets 2 stars *and* a level-3 capital** (4 SPT without a Workshop) and **no starting tech**
  ([Tribes](https://polytopia.fandom.com/wiki/Tribes)).
- **Ai-Mo starts with Philosophy + a Mind Bender**, not Meditation. The
  [Technology](https://polytopia.fandom.com/wiki/Technology) and
  [Meditation](https://polytopia.fandom.com/wiki/Meditation) wiki pages are **stale**; the
  [Ai-Mo](https://polytopia.fandom.com/wiki/Ai-Mo) and [Tribes](https://polytopia.fandom.com/wiki/Tribes)
  pages and the 2025 patch note agree on Philosophy. The 730 turn-0 score confirms it (T3 tier).
  Consequence: **Ai-Mo has Literacy active from turn 0.**
- Special tribes (Aquarion, ∑∫ỹriȱŋ, Polaris, Cymanti) are out of scope for this table.

Cross-check against turn-0 scores ([Score](https://polytopia.fandom.com/wiki/Score)): Aquarion 415 ·
Xin-xi/Imperius/Bardur/Kickoo/Luxidoor/Elyrion 515 · Oumaji 520 · Polaris 530 · Zebasi/Yădakk 615 ·
Quetzali/Hoodrick 620 · Cymanti 630 · Vengir/Ai-Mo 730.

---

## 9. Things a clone will obviously get wrong

1. **Filtering the menu to legal actions.** The real game shows illegal options with a reason string
   and often a fix button (`TECH TREE`). Its own API takes `includeUnavailable` and the UI passes
   `true`. §3.1, §3.1a.
2. **No battle preview.** Press-and-hold / hover is how players plan every fight. §4.2.
3. **Move and attack markers not visually distinct.** Blue vs red, both painted at once. §4.1.
4. **Treating the city as a separate screen.** It is a tile with an action box. §6.1.
5. **Population bar as a simple progress bar.** It carries level progress (blue), unit slots (dots),
   and negative population (red) at once. §6.3.
6. **No live scoreboard.** Game Stats is one of four permanent buttons and shows live scores and city
   counts for every tribe, plus placeholder rows for unmet ones. §5.2.
7. **Leaking opponent stars.** Never shown. Score and city count *are*. §5.1.
8. **Hiding the tech tree's locked half.** The whole tree is always visible with live prices. §7.2.
9. **Static tech prices.** They scale with your city count and are discounted by Literacy. §7.2.
10. **Drawing resources the player can't see yet.** Metal/starfish/crop sprites are hidden until the
    gating tech. §5.8.
11. **Flat 5 starting stars for everyone.** §8.
12. **Starting vision of 3×3.** It is 5×5 explored, 3×3 owned. §5.8.
13. **Adding an undo.** There isn't one, and the whole interaction model assumes commitment. §1.5.
14. **A static "Next Turn" label.** It becomes "No more moves available, end turn". §1.2.
15. **Ignoring the meeting event.** Meeting a tribe is a popup, a star payout scaled to their score,
    and the moment their identity becomes known. §5.4.
16. **Computing move markers from visible state.** ZoC from fog-hidden enemies truncates movement.
    §4.3.
17. **Forgetting the unit-can-still-attack cue.** The white outline on the type icon. §3.5.
18. **Forgetting siege.** *"Cities that are under siege by an enemy unit do not produce any income"*
    ([City](https://polytopia.fandom.com/wiki/City)) — and `tooltip.tile.riot` for infiltrated cities.
19. **Selection state changing tap semantics.** With a unit selected, tapping a reachable tile *moves*
    rather than *selects*. This is the #1 source of misclicks in the real game. §1.5.
20. **No Tribe Info screen.** Peace treaties are offered from it, not from the map. §5.3.
21. **Tech nodes that don't show what they unlock.** Each node renders unlock icons, and tapping opens
    a popup listing every unit and improvement granted. §7.3.
22. **A modal city-level-up dialogue that can't be dismissed to see the map.** It is a real modal and
    players complain it blocks the board — but it is genuinely modal, so replicate the two-option
    choice, and expect the same complaint. §6.4.
23. **Requiring an explicit heal click.** Idle units auto-heal at end of turn. §3.3.
24. **Showing enemy cities' income and population.** Enemy cities are a bare name plate. §5.7.
25. **Forgetting your rank is on the HUD.** The Game Stats button carries a live `1st`/`2nd` badge.
    §1.2.
26. **A static End Turn button.** It turns solid blue when every unit is out of actions
    (changelog 2.8.2.11621: *"End Turn now properly turns blue when all units are out of actions"*),
    and press-and-hold cycles the camera to units that can still act. §1.2.
27. **Adding a padlock icon.** There isn't one anywhere. Locked = grey empty ring + grey italic name +
    no price + no commit button. §3.0, §7.1a.
28. **Making the action bar a fixed size.** At Medium UI Scale in landscape it can occupy roughly half
    the screen — a repeatedly-cited complaint and the reason `Compact UI` and a five-step `UI Scale`
    exist. Budget for it.
29. **Dismissing menus by tapping outside.** Menus close only via the back chevron; tap-anywhere
    dismissal is a standing feature request.
30. **Terrain-level affordances.** The game hints at the map, not just in menus — e.g. *"Added
    shorelines to hint if a bridge can be placed on a water tile that leads into the fog."*

---

## UNRESOLVED

Things I could not settle. **Do not implement these from guesswork.**

1. ~~**Does a build action fire immediately or via a `DO IT` confirm?**~~ **RESOLVED** — the
   `settings.info = 'Info on Build'` toggle switches between a two-tap info-popup flow (default, on)
   and one-tap immediate execution (off). See §2.1b. Remaining minor unknown: whether the default is
   ON out of the box (the guide's phrasing *"turn it OFF"* implies yes, but does not say so outright).
2. **Reaching a city's UI while your own unit stands on it.** No documented toggle/long-press/cycle.
   Training is impossible anyway (one unit per tile), but whether you can *inspect* the city is
   unconfirmed.
3. **The canonical deselect gesture.** Community reports say tapping the unit's own tile again
   dismisses the overlay, and `Esc` cancels "in some cases". No authoritative statement.
4. **Battle-preview death icon: sweating or skull?** [Combat](https://polytopia.fandom.com/wiki/Combat)
   says sweating + black-and-red ring; [Level Winner](https://www.levelwinner.com/the-battle-of-polytopia-beginners-guide-tips-tricks-strategies-to-rule-the-world/)
   says skull. Possibly different game eras, possibly two different indicators. The *interaction* is
   agreed.
5. **The Income gate.** *Narrowed, still not confirmed.* Established: Diplomacy's "insight" is
   officially the **relations matrix**, and build-122 source confirms the **three opinion reason
   pills** are Diplomacy-gated behind a "locked-info panel" (§5.5). By elimination, opponent
   **Income** is therefore most likely gated behind the **Embassy**
   (`playerinfo.locked.upgradeembassy`) — supported by a screenshot showing Diplomacy researched, no
   embassy, and no Income row. **But no source states this directly.** Still the single most
   consequential open question here: always-visible opponent income is a large intel leak that a
   clone would either wrongly grant or wrongly withhold.
6. **`playerinfo.locked.upgradeembassy = 'Upgrade your embassy to gain more insight.'`** implies
   **upgradeable embassies**, which have no documented mechanic in the live game. Either an unshipped
   feature, or an undocumented one. Present in both the 2022 and 2024 files.
7. **Does the kill counter (`actionbox.unit.kills`) render for enemy units, or only your own?**
8. **The 2020 paper's enemy-city hidden fields.** *Largely CONFIRMED — I initially refuted this and
   was wrong.* An official gameplay screenshot shows enemy cities rendering a **bare name plate**:
   no ★income, no population bar (§5.7). So **population and production are genuinely hidden** for
   enemy cities, as the paper says. Still unverified individually: kill count, original/founding
   city, and units created — though all three are label-type data of exactly the kind the game
   suppresses, so the paper is now the more likely reading. The paper is **not on arXiv** (the arXiv
   API returns zero results for Polytopia); AIIDE 2020 appears to be the only venue, so I could not
   read the full text. Note its five fields may split across *units* and *cities* rather than all
   applying to cities, so check the original before encoding it literally.
9. **Action bar geometry.** *Largely resolved* — `InteractionBar.CreateRoundBottomBarButton` returning
   `UIRoundButton` settles it as a **bottom bar of round buttons** in build 122 (§2.1a). The remaining
   uncertainty: the 2021 review says "radial menu", so either it was radial then and was changed, or
   the reviewer was being loose. Also unverified whether mobile and desktop lay it out identically —
   the codebase has both `BasicPopup`/`BasicPopupLegacy` and `*_UI2` screen variants
   (`GameSetupScreen_UI2`, `StartScreen_UI2`, `UIRoundButton_UI2`, `UIButtonBase_UI2`), so a UI
   migration is in progress and two generations coexist.
10. **Exact wording as of 2.16.** All quoted strings are from the 2022 dump (schema verified stable to
    2024). Wording may have been polished since; structure almost certainly has not.
11. **Whether Score in the top bar is shown in all game modes** or only Perfection/Glory.
13. **Does Game Stats show a score for a tribe you have NOT met?** The row exists ("Unknown tribe"),
    but whether it carries a number is unconfirmed. If it does, turn-0 tribe fingerprinting works on
    players you've never seen, which would be a significant design fact.
14. **Reveal radius when you capture a city.** Newly captured cities grant vision/territory, but the
    exact live-game radius is not documented.
15. **Visual layout of negative (red) population bars.** The wiki states the rule unambiguously, but
    no screenshot of red bars was found. Unknown whether they extend the bar leftward, replace blue
    segments, or render as a separate run. **Do not guess the layout.**
16. **Whether the cyan halo on your own units means "mine" or "still has actions".** It appears on the
    player's units in every screenshot. The *sourced* action-availability cue is the white outline on
    the unit-type icon; the halo's semantics are unconfirmed.
17. **Whether selecting an enemy unit or city opens an action bar at all**, versus only a description
    panel and tooltips.
18. **Bottom-bar label pair.** Current screenshots read `Settings` / `End Turn`; the string table has
    `bottommenu.menu` = "Menu" and `bottommenu.nextturn` = "Next Turn". Both label pairs are attested.
    What switches between them — version, platform, or UI generation — is unknown.
19. **Do unit-ability buttons carry star prices?** String evidence says no (the ability verbs have no
    price placeholder, unlike `actionbox.unit.new`), and a mod exists to add cost labels to that bar.
    Consistent, but not visually confirmed. See §3.2.
12. **Mobile vs desktop divergence generally.** Everything here is drawn from a codebase shared across
    platforms, plus a PC review and PC keyboard shortcuts. The `tooltip.*` strings say "Tap" in some
    places and "Click" in others, which suggests one string table serves both and the wording is not
    reliably platform-specific.

### How to close these gaps cheaply

Most of the remaining unknowns are a 20-minute empirical question, and there is a purpose-built tool
for it. [PolyMod](https://github.com/PolyModdingTeam/PolyMod) is a BepInEx/Harmony mod loader for the
Steam build; a throwaway patch on `InteractionBar.Show`, `InteractionBar.AddUnitActionButtons` and
`PlayerInfoPopup.Refresh` that logs its arguments would settle #2, #5, #7 and #9 definitively. That is
a far better use of effort than more web searching — I have exhausted what the public written record
contains.
