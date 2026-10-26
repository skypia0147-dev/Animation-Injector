# Animation Injector

**Version 0.12.285** · [한국어](README.md)

An SKSE framework that adds ordinary and two-person paired animations to Skyrim through XML declarations and HKX files alone. Adding actions does not require re-running Nemesis or Pandora. Keep the behavior patches your other mods require.

This package is the framework itself. It adds no hotkeys or actions on its own; it takes effect together with action mods that ship Animation Injector XML.

## Requirements

- SKSE64
- Address Library for SKSE Plugins (matching your game version)
- Tested in game on **Skyrim SE/AE 1.6.1170**. Built with CommonLibSSE-NG and Address Library for SE 1.5.x through AE 1.6.x, but not every runtime has been tested. VR is not supported.

## Installation

Install with a mod manager (MO2 or similar). Contents:

| File | Purpose |
|---|---|
| `SKSE/Plugins/AnimationInjector.dll` | The plugin |
| `SKSE/Plugins/AnimationInjector.ini` | Settings (comments in English and Korean) |
| `Interface/AnimationInjectorLoading.swf` | Empty host the loading indicator and QTEs draw into. Without it no QTE is shown |
| `licenses/` | License texts of the bundled libraries |

If you edited the INI, move your values over before overwriting it with an update.

## Features

| Area | What it does |
|---|---|
| Start | Keyboard key names or scan codes, gamepad button names, animation events. Player and NPCs |
| Ordinary actions | Enter/exit blends, MoveCancel (break out with movement input), Vanilla (break out into a vanilla action), Transition (chain into the next action), start limits (ground/air, cooldown), clip-time functions (hide/show weapons, spells, states, ragdoll, camera shake, stop velocity, time scale) |
| Slots | Rotation, invulnerability (IFrame), stagger resistance (HyperArmor), stamina/magicka cost, attack data |
| Branching | A condition card inside a start or transition (`Check`, `Row`, `True`, `False`) picks the action to play by movement direction, weapon, attack state, stamina, airborne, lying side, equipment, race, sex, chance and more |
| Movement | Moves as the HKX's AMR annotations (`animmotion`, `animrotation`) say |
| Pairs | Paired playback from one HKX holding both roles, start alignment (the victim turns in place, the attacker moves), end position correction, an exit blend when the partner is lost |
| Event registry | Bind functions to HKX annotation events globally: hide/show weapons, invulnerability on/off, costs, firing arrows (as drawn or direct). A function acts on the actor that sent the event |
| Annotation commands | The HKX annotation `AI.CamShake\|strength\|duration` shakes the camera at that moment. No registration; every number must be written |
| QTE | Six types (`splitArc`, `timingBar`, `rotatingNeedle`, `holdRelease`, `rapidPress`, `keySequence`); events, sounds and follow-up actions per result; input-only QTEs that draw nothing (`hidden="true"`) |
| Carry | Lift a target through a pair, carry it and put it down (partial ragdoll) |
| View | Ordinary actions choose third person, first person or both with `view` (first-person HKX under `_1stPerson` at the same path), with an option to switch to third person for the action |

Pairs and carries are third person only.

## Declaration example

XML location: `Data/SKSE/Plugins/AnimationInjector/<mod name>/<file>.xml`
HKX location: under `Data/meshes/actors/character/` (`clip` is a path relative to that folder)

```xml
<AnimationSet version="6" id="MyMod">
  <Start key="G" gamepad="RB" cooldown="0.3" to="Slash1">
    <Check kind="weaponDrawn"/>
  </Start>
  <Action id="Slash1" clip="Animations\MyMod\Slash1.hkx">
    <MoveCancel at="event:MyMod_Recovery" blend="0.15"/>
    <IFrame from="0.1" until="0.4"/>
    <Transition to="Slash2" key="G" gamepad="RB" from="0.45"/>
    <Vanilla to="Block"/>
  </Action>
  <Action id="Slash2" clip="Animations\MyMod\Slash2.hkx">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- A start (`<Start>`) stands at the root (format 6, 0.12.291). Its `to` names an action, or it holds a condition card: common conditions (`<Check>`, `<And>`, `<Or>`; `not="true"` negates), rows tried from the top whose conditions all hold (`<Row to="action">conditions</Row>`), `<True>` for a card of common conditions only, and `<False>` when nothing holds. A card several places share is a root `<Condition id="name">`, named by `condition="name"`. Transitions (`<Transition>`) and QTE results choose their destination the same way. An action can have several starts.
- A cooldown belongs to the start (`<Start cooldown="0.3">`): after what that start started ends, only that start waits. An action's air rule is its `air` attribute: `both` (ground and air) or `only` (air only); left out, ground only.
- A check such as "only with a drawn weapon" is a `<Check>` inside the start, as above.
- A moment set to an event (`event:Name`) acts only when that event comes. The "if it does not come by n seconds" `fallback` is gone (0.12.278).
- Room and ground checks (`kind="space"`, `"ground"`) take a `direction` of `selfForward|selfBack|selfLeft|selfRight`, turned from the heading the actor will have on its start mark in that pair (0.12.280). The older `partner*` directions are still read.
- An event registry `<Do>` names the function and what it acts with only: `op`, a spell's `plugin` and `form`, a ragdoll's `direction`. The numbers (`delay`, `mode`/`damage`, `strength`/`duration`, `stamina`/`magicka`, `factor`, `force`) go in `<EventValues><Event name="Name"><Do op="…" …/></Event></EventValues>` of each action or pair whose clip sends the event. When the event comes, its functions run with the numbers of the action playing on the sender, else the defaults; `cost` and `timeOn` run only with an action's numbers (0.12.281). Values pair with the event's functions of the same op, in order.
- A strike (0.12.293) is `<Do op="strike"/>`. The changed actor attacks: the damage is what the engine works out for a hit with its weapon (weapon, skill, perks, the target's armour and block; the difficulty applies), without the HitFrame cone. The registry (or a clip-time function) names whom and how: `pick="combatTarget|partner|crosshair|area"` (default the combat target; the player's crosshair actor), `hit="hostile|all"` (the area's targets), `hand="right|left|both"`, `block="vanilla|ignore"`; an action's `<EventValues>` (or the clip-time function) the numbers: `ratio` (times the weapon's damage, default 1) or `amount` (fixed damage), the area's `radius`, `angle` (whole width, 360 = a circle), `up`/`down` (from the feet), `edge` (the share at the radius), `duration` (seconds; each actor that comes in is hit once). Ragdolled actors and actors behind a wall are not hit. After the damage (0.12.294) the registry names a reaction, `reaction="none|stagger|push"`, and the values its strength: a stagger's `strength="0~1"` or a push's `force="0~20"` (scaled by the falloff; not for a target the hit killed). As a real hit, it brings the weapon's enchantment (the player's charge spent), its poison (a dose spent), the "Apply Combat Hit Spell" perks, the impact (hit sound, blood) and a hit event (TESHitEvent); the registry's `without="enchantment,poison,perks,impact,hitEvent"` leaves any out.
- Conditions (0.12.296): `<Check kind="previousAction" actions="A,B" within="0.8" ended="any|completed|cancelled"/>` (the last of our actions this actor finished; recorded only while a declaration uses it), `kind="falling" option="rising|falling"` (in the air; optionally the fallen distance by `compare`/`value`), `kind="slope" option="downhill|uphill|any" compare value` (degrees under the feet), `kind="actorValue" option="OneHanded|…|level" measure="current|base" compare value`, `kind="gameHour" compare value [upper]` (a range may wrap past midnight, 20 to 5), `kind="weather" option="pleasant|cloudy|rainy|snow"`. Each is worked out only when a condition card is checked.
- Functions (0.12.296): `<Do op="sound" file="sound\fx\…\a.wav"|plugin form bone="spine|head|rightHand|leftHand|root"/>` (values: `volume` 0~1), `op="globalSet" plugin form restore="end"` (values: `value`; with restore the old value comes back when the run ends, and after loading a save made meanwhile), `op="modEvent" name text` (values: `number`; Papyrus `RegisterForModEvent` hears it, sent as the actor's).
- A ragdoll is `<Do op="ragdoll" direction="down|up|forward|backward|left|right"/>` in the registry, its force `force="0~20"` in an action's `<EventValues>`. During a pair it first frees that actor from the pair's physics hold and watches its get-up after the ragdoll (0.12.278; the old `releasePhysics` is refused).
- Only a `<AnimationSet version="6">` root is read. A file in any other format is skipped whole, with the reason in the log. Convert a format 4 or 5 file with `python tools/convert_format6.py --backup <folder> <xml>...` (`AnimationInjectorXmlCheck --compare-starts <old.xml> <new.xml>` confirms the result behaves the same).
- A file loads completely or not at all: a single error keeps every action in it from registering.
- File and folder names in HKX paths must be ASCII letters, digits and common symbols (no non-ASCII characters).
- Key names refer to physical keys and do not depend on the input language.
- A game control can stand for a key (0.12.292): write `control="jump"` where `key`/`gamepad` go. It works with every keyboard, mouse and gamepad key the player has bound to that control and follows remapping in the game settings. It is accepted on starts, transitions, QTEs, the carry put-down and `<Inputs><Input>`; an element takes one of `input`, `key`/`gamepad` or `control`. When such a press starts one of our actions, or a transition or QTE takes it, the game does not get it (a start on Jump does not also jump); when nothing starts (a condition says no) the game acts on it as usual. With `onKeyUp="true"` the game has already acted on the press. Names: `jump` `sprint` `sneak` `activate` `readyWeapon` `shout` `rightAttack` `leftAttack` `togglePOV` `run` `toggleRun` `autoMove` `wait` `favorites` `tweenMenu`. A QTE `keySequence` takes keys only.
- Restart the game completely after changing XML or HKX files.

For more of the syntax, see the XML of published action mods that use Animation Injector. Links to the reference mods are on the mod page.

## Settings

Main entries of `SKSE/Plugins/AnimationInjector.ini`. Each is described in the INI comments.

| Entry | Default | Meaning |
|---|---|---|
| `[Compatibility] iNativePairAlignment` | 2 | Paired start alignment. 0 off / 1 Animation Injector pairs only / 2 every pair (kill moves included). Use 1 with Paired Animation Improvements |
| `[Compatibility] iPairEndPosition` | 2 | Paired end position correction. 0 off / 1 Animation Injector pairs only / 2 every pair |
| `[Compatibility] bFireArrow` | true | Bow release integration during actions |
| `[Preload] bOrdinary` | true | Preload ordinary action animations too |
| `[Display] bLoadingProgress` | true | Show preparation progress at game start and load |
| `[Debug] bDebug` | false | Verbose log. Turn on only when reporting a problem |

## Compatibility and limits

- Confirmed working alongside OAR, AMR, Precision and MCO/BFCO in the tested setups. Not every combination or every race's graph is guaranteed.
- With Precision installed, a carried target is taken out of Precision's hit handling from the lift until the carry ends (public API `ToggleDisableActor`).
- Invulnerability (IFrame) makes only that actor a ghost. Other NPCs sharing its base are not affected.
- Pairs use an HKX holding the character skeleton's `NPC`/`2_` roles. Two separate HKX files are not combined into a pair automatically.
- When the partner dies or ragdolls, the remaining actor ends the pair (it does not keep playing).
- When the same action ID (case-insensitive) appears in several files, the later file in name order replaces the earlier one.

## Reporting problems

1. Set `bDebug = true` in the INI and restart the game.
2. Reproduce the problem.
3. Send `Documents\My Games\Skyrim Special Edition\SKSE\AnimationInjector.log` (the run before is `AnimationInjector.previous.log`), plus your crash logger's log for a crash. Mention the action mod and the last action you ran.

The plugin version and the game runtime are logged at the top. When an XML file is not read, lines starting with `[Decl]` or `[Decl:v2]` name the file and the reason.

## License

Animation Injector is released under the GNU General Public License 3.0 (GPL-3.0). The full text is in `LICENSE`; the bundled libraries' licenses are in `licenses/`. It uses CommonLibSSE-NG (the alandtse fork, pinned to a commit). The source is provided as a separate source package.
