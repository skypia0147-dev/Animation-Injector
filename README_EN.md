# Animation Injector

**Version 1.0.4** · [한국어](README.md)

Adding a new animation usually means patching the behavior graph with a tool such as Nemesis or Pandora. Animation Injector is an SKSE framework that skips that step.

You describe in an XML file which HKX animation plays, when, and under what conditions. The game reads the XML at startup and plays your animations. Adding or changing animations never requires running a behavior tool again.

Animation Injector does not replace or change mods made with Nemesis, Pandora or OAR. It only runs mods written for Animation Injector, and your existing behavior mods keep working alongside it as they are.

**Apart from the paired animation fixes below, this mod does nothing by itself.** Install it together with animation mods made for Animation Injector.

- **Users**: animation mods made for Animation Injector work as soon as you install them, with no Nemesis or Pandora run.
- **Modders**: bring new animations into the game by writing XML, without patching the behavior graph.

## How it works

Animation Injector leaves the vanilla behavior graph as it is and plays new animations on top of it, for example when an animation event fires or a hotkey is pressed. It can also take over a vanilla event and play your animation instead of the vanilla reaction (for example on a blocked hit).

To play a stop animation after sprinting, for instance, use the `sprintStop` event, which fires when a sprint ends, as the trigger. [This video](https://www.youtube.com/watch?v=H0bTLvZqs0Q) shows how to build that XML in Animation Injector Editor.

## Requirements

- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (the one for your game version)
- Skyrim SE/AE. Tested on 1.6.1170. VR is not supported.

## Installation

Install with a mod manager (MO2, Vortex, ...). If you edited the INI, note your values before overwriting it with an update.

## What it can do

Action mods combine these features into actions.

| Area | What |
|---|---|
| Starting | Keyboard and gamepad keys, game controls (Jump, Sprint, ...; they follow your key bindings), animation events. For the player and NPCs. A key can tell the moment it goes down, a tap, a double press and a hold; a game control press no start takes goes back to the game (e.g. tap Jump to dodge, hold it to jump). Transitions on an action's end, on a key's release and on a movement change, looping clips and a held-time condition make charge attacks (moving ones too, by directional dash loops) |
| Chaining | Move on to the next action on a key press or an event; cancel by moving or by another action |
| Conditions | Play a different action by weapon, movement direction, being in the air, stamina, equipment, race, the previous action, the hit just taken (normal, power attack, blocked and more), weather and more |
| During an action | Invulnerability, stagger resistance, turning, time scale, stamina, magicka and health costs, strikes, firing arrows, spells, ragdoll, sounds, camera shake and more |
| HKX events | Register features on the animation events any HKX sends (other mods' and vanilla ones too). An HKX annotation `AI.Command\|values` runs at its time with no registration |
| Pairs | Actions two people do together. Both are lined up in position and facing when it starts, and their positions are corrected when it ends |
| Carrying | Lift someone, carry them and put them down. How a target swinging under physics moves is set per lift action by a ragdoll file beside the XML (vanilla without one) |
| QTE | Six kinds: timing, rapid presses, hold and release, key sequences and more. Each result can lead to a different action |
| View | Third- and first-person actions; switching from first to third person to play one |

Pairs and carrying work in third person only.

## Paired animation fixes

These two fixes apply to vanilla kill moves and other mods' paired animations too, not only to this mod's pairs.

- **Start alignment**: the victim stays where it is and only turns toward the attacker; the attacker moves onto its mark.
- **End position**: when an animation's root bone moves away from the actor, for example one that ends walking off, the actor normally snaps back to where it started as the animation ends. This mod leaves the actor where the animation ended instead.

To use them for this mod's pairs only, set `iNativePairAlignment` and `iPairEndPosition` in the INI to 1; set them to 0 to turn them off.

## Making an action mod

Actions are defined in XML files, written by hand or made in Animation Injector Editor, a tool for mod authors. In the editor, place HKX files as cards and connect them; it writes the XML the game reads and the mod folder. Players don't need the editor, only Animation Injector and the animation mods that use it.

- Use only English letters, digits and common symbols in the file and folder names of HKX paths. The game cannot find files under non-English paths.
- Restart the game fully after changing an XML or HKX file.
- If an XML file has any error, none of its actions are added. The reason is in the log.

To write the XML by hand, see `XML_GUIDE_EN.md`.

## Settings

The main entries of `SKSE/Plugins/AnimationInjector.ini`. See the INI's notes for the others.

| Entry | Default | Meaning |
|---|---|---|
| `iNativePairAlignment` | 2 | Lining up position and facing when a pair starts. 0 off / 1 this mod's pairs only / 2 all pairs (killmoves included). |
| `iPairEndPosition` | 2 | Position correction when a pair ends. 0 off / 1 this mod's pairs only / 2 all pairs |
| `bOrdinary` | true | Preload the animations of single-actor actions too (pairs are always preloaded) |
| `bLoadingProgress` | true | Show preparation progress and the engine limits (graph nodes, animations) when the game starts and a save loads. Amber from 90%, red when something was left out |
| `bDebug` | false | Detailed log. Turn it on only to report a problem |

## Compatibility

- Tested with OAR, AMR, Precision and MCO/BFCO. Not every combination of mods is guaranteed.
- With True Directional Movement installed, movement direction conditions and an action's rotation window follow TDM's settings. To turn this off, set `bFollowTDM = false` in the INI.
- A pair ends if the partner dies or falls during it.
- Action names belong to their XML file (its AnimationSet ID), so two action mods using the same name do not overwrite each other. If two XML files use the same AnimationSet ID, the later file is loaded on its own and the log warns.

## Reporting a problem

1. Set `bDebug = true` in the INI and restart the game.
2. Reproduce the problem.
3. Send `Documents\My Games\Skyrim Special Edition\SKSE\AnimationInjector.log` (`AnimationInjector.previous.log` for the run before). For a crash, also send your crash logger's log. Say which action mods you use and which action you did last.

If the game closes before the main menu with no crash log, launch it once more and send `AnimationInjector.log`. The second launch writes `[Startup]` lines saying where the first one stopped and what Windows recorded about it (the faulting module and exception code).

## Credits

- **SKSE Team**: [Skyrim Script Extender](https://skse.silverlock.org/)
- **meh321**: [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- **Ryan McKenzie**: [CommonLibSSE](https://github.com/Ryan-rsm-McKenzie/CommonLibSSE)
- **CharmedBaryon**: [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)
- **alandtse** and contributors: [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR/tree/ng) (the version this mod is built with)
- **SARDONYX**: [serde-hkx](https://github.com/SARDONYX-sard/serde-hkx) (HKX reading in the editor)
- **Lukas Cone (PredatorCZ)**: [HavokLib](https://github.com/PredatorCZ/HavokLib) (spline animation decompression in the editor, ported)
- Libraries: pugixml, spdlog, fmt, rapidcsv, DirectXTK, DirectXMath, xbyak, hde64 (runtime) · Tauri, React, React Flow, three.js (editor)

## License

GNU General Public License 3.0 (`LICENSE`). Where to find the source code, and the commit this download was built from, is in `SOURCE.txt`. The licenses of the libraries it uses are in `licenses/`.
