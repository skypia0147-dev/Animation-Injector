# Animation Injector

**Version 0.12.266** · [한국어](README.md)

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
| Ordinary actions | Enter/exit blends, Recovery (break out with movement input), Cancel (break out into a vanilla action), Connect (chain into the next action) |
| Slots | Rotation, invulnerability (IFrame), stagger resistance (HyperArmor), stamina/magicka cost, attack data |
| Branching | `Flow` picks the action to play by movement direction, weapon, airborne, lying side, equipment, race, sex, chance and more |
| Movement | Moves as the HKX's AMR annotations (`animmotion`, `animrotation`) say |
| Pairs | Paired playback from one HKX holding both roles, start alignment (the victim turns in place, the attacker moves), end position correction, an exit blend when the partner is lost |
| Event registry | Bind functions to HKX annotation events globally: hide/show weapons, invulnerability on/off, costs, firing arrows |
| QTE | Six types (`splitArc`, `timingBar`, `rotatingNeedle`, `holdRelease`, `rapidPress`, `keySequence`); events, sounds and follow-up actions per result; input-only QTEs that draw nothing (`hidden="true"`) |
| Carry | Lift a target through a pair, carry it and put it down (partial ragdoll) |
| View | Ordinary actions choose third person, first person or both with `view` (first-person HKX under `_1stPerson` at the same path), with an option to switch to third person for the action |

Pairs and carries are third person only.

## Declaration example

XML location: `Data/SKSE/Plugins/AnimationInjector/<mod name>/<file>.xml`
HKX location: under `Data/meshes/actors/character/` (`clip` is a path relative to that folder)

```xml
<AnimationSet version="4" id="MyMod">
  <Action id="Slash1" clip="Animations\MyMod\Slash1.hkx">
    <Start key="G" gamepad="RB"/>
    <Require weapon="drawn"/>
    <Recovery at="event:MyMod_Recovery" fallback="0.9" blend="0.15"/>
    <IFrame from="0.1" until="0.4"/>
    <Connect to="Slash2" key="G" gamepad="RB"><Window from="0.45"/></Connect>
    <Cancel to="vanilla:Block"/>
  </Action>
  <Action id="Slash2" clip="Animations\MyMod\Slash2.hkx">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- Only a `<AnimationSet version="4">` root is read. A file in any other format is skipped whole, with the reason in the log.
- A file loads completely or not at all: a single error keeps every action in it from registering.
- File and folder names in HKX paths must be ASCII letters, digits and common symbols (no non-ASCII characters).
- Key names refer to physical keys and do not depend on the input language.
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
