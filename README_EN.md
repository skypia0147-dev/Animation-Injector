# Animation Injector

**Version 1.0.0** · [한국어](README.md)

An SKSE framework that adds new actions to Skyrim: actions done alone and paired actions done by two people together. Adding or changing actions does not require re-running Nemesis or Pandora. Keep the behavior patches your other mods require.

This mod does nothing by itself. Install it together with action mods made for Animation Injector.

## Requirements

- SKSE64
- Address Library for SKSE Plugins (the one for your game version)
- Skyrim SE/AE. Tested on 1.6.1170. VR is not supported.

## Installation

Install with a mod manager (MO2, Vortex, ...). If you edited the INI, note your values before overwriting it with an update.

## What it can do

Action mods combine these features into actions.

| Area | What |
|---|---|
| Starting | Keyboard and gamepad keys, game controls (Jump, Sprint, ...; they follow your key bindings), animation events. For the player and NPCs |
| Chaining | Move on to the next action on a key press or an event; cancel by moving or by another action |
| Conditions | Play a different action by weapon, movement direction, being in the air, stamina, equipment, race, the previous action, weather and more |
| During an action | Invulnerability, stagger resistance, turning, stamina and magicka costs, strikes, firing arrows, spells, ragdoll, sounds, camera shake and more |
| Pairs | Actions two people do together. Both are lined up in position and facing when it starts, and their positions are corrected when it ends |
| Carrying | Lift someone, carry them and put them down |
| QTE | Six kinds: timing, rapid presses, hold and release, key sequences and more. Each result can lead to a different action |
| View | Third- and first-person actions; switching from first to third person to play one |

Pairs and carrying work in third person only.

## Making an action mod

Use Animation Injector Editor. Place HKX files as cards and connect them; it writes the XML the game reads and the mod folder.

- Use only English letters, digits and common symbols in the file and folder names of HKX paths. The game cannot find files under non-English paths.
- Restart the game fully after changing an XML or HKX file.
- If an XML file has any error, none of its actions are added. The reason is in the log.

To write the XML by hand, see `XML_GUIDE_EN.md`.

## Settings

The main entries of `SKSE/Plugins/AnimationInjector.ini`. See the INI's notes for the others.

| Entry | Default | Meaning |
|---|---|---|
| `iNativePairAlignment` | 2 | Lining up position and facing when a pair starts. 0 off / 1 this mod's pairs only / 2 all pairs (killmoves included). 1 is recommended with Paired Animation Improvements |
| `iPairEndPosition` | 2 | Position correction when a pair ends. 0 off / 1 this mod's pairs only / 2 all pairs |
| `bOrdinary` | true | Preload the animations of single-actor actions too (pairs are always preloaded) |
| `bLoadingProgress` | true | Show preparation progress when the game starts and a save loads |
| `bDebug` | false | Detailed log. Turn it on only to report a problem |

## Compatibility

- Tested with OAR, AMR, Precision and MCO/BFCO. Not every combination of mods is guaranteed.
- A pair ends if the partner dies or falls during it.
- If two action mods add an action with the same name, the one in the file that sorts later by name is used.

## Reporting a problem

1. Set `bDebug = true` in the INI and restart the game.
2. Reproduce the problem.
3. Send `Documents\My Games\Skyrim Special Edition\SKSE\AnimationInjector.log` (`AnimationInjector.previous.log` for the run before). For a crash, also send your crash logger's log. Say which action mods you use and which action you did last.

## License

GNU General Public License 3.0 (`LICENSE`). The licenses of the libraries it uses are in `licenses/`. Where to find the source code is in `SOURCE.txt`.
