# Writing Animation Injector XML

[한국어](XML_GUIDE.md)

Making action mods with Animation Injector Editor is recommended. This guide is for those who want to write the XML by hand. It covers what is used most. For anything not here, build it in the editor and look at how it is written with **XML preview (Ctrl+`)**. A hand-written XML also opens in the editor with its cards laid out automatically, so you can move over to the editor at any time.

Every example in this guide is a file Animation Injector reads as it is.

## Where files go

| What | Where |
|---|---|
| XML | `Data\SKSE\Plugins\AnimationInjector\<mod name>\<any name>.xml` |
| HKX | Under `Data\meshes\actors\character\`. `clip` is the path from that folder |

- Use only English letters, digits and common symbols in the file and folder names of HKX paths.
- Restart the game fully after changing an XML or HKX file.
- If a file has any error, the whole file is ignored. [Check](#checking-a-file) it before putting it in the game.

## The smallest file

Pressing G (RB on a pad) plays a roll HKX.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyMod">
  <Start key="G" gamepad="RB" cooldown="0.5" to="Roll"/>
  <Action id="Roll" clip="Animations\MyMod\Roll.hkx">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- `<?xml … ?>`: the standard XML declaration (says the file is XML saved as UTF-8). It may be left out; if you keep it, it must be the very first line and the file must be saved as UTF-8.
- `<AnimationSet>`: the outside of the file. Keep `version="6"` as it is; give `id` a name of your own for each mod (English letters, digits, `_`).
- `<Start>`: how the action starts. `to` is the action to play. It does not start again until `cooldown` (seconds) has passed.
- `<Action>`: an action that plays one HKX. Its `id` must not repeat within the file; another mod may use the same `id` without the two mixing.
- `<Exit>`: the blend time (seconds) back to normal movement when it ends.

## Chaining actions

With a weapon drawn, G does slash 1; pressing G again during it leads on to slash 2.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyMod">
  <Start key="G" gamepad="RB" cooldown="0.3" to="Slash1">
    <Check kind="weaponDrawn"/>
  </Start>
  <Action id="Slash1" clip="Animations\MyMod\Slash1.hkx" enter="0.1">
    <IFrame from="0.1" until="0.4"/>
    <HyperArmor until="0.6"/>
    <Cost stamina="20"/>
    <Transition to="Slash2" key="G" gamepad="RB" from="0.45" until="end-0.1"/>
    <MoveCancel at="0.8" blend="0.25"/>
    <Vanilla to="Block" from="0.5"/>
    <Exit blend="0.2"/>
  </Action>
  <Action id="Slash2" clip="Animations\MyMod\Slash2.hkx">
    <MoveCancel at="event:MyMod_Recovery" blend="0.2"/>
    <Exit at="end-0.1" blend="0.2"/>
  </Action>
</AnimationSet>
```

| Element | Meaning |
|---|---|
| `<Transition to key from until>` | A key press between `from` and `until` moves on to the `to` action. `event="Name"` works in place of a key |
| `<MoveCancel at blend>` | After `at`, a movement key cancels the action back to walking or running |
| `<Vanilla to from until>` | In that window, the game's own action can cancel it. `to`: `Attack` `PowerAttack` `Block` `Bash` `Sprint` `Jump` `Sneak`. `until` is counted from the end (`end`, `end-0.2`) |
| `<IFrame from until>` | Invulnerable in that window |
| `<HyperArmor from until>` | Stagger resistance in that window |
| `<Rotate from until>` | Can turn in that window |
| `<Cost stamina magicka at>` | Stamina and magicka cost |
| `<Exit at blend>` | When it ends and how many seconds it blends. Without `at`, 0.05 seconds before the clip's end |

`<Action>` attributes: `enter` (blend seconds going in), `air="both"` (ground and air) / `"only"` (air only; without it, ground only), `view="first"` or `"both"` (a first-person action; first-person HKX files go at the same path under `_1stPerson`).

**Writing times**: `0.4` (seconds from the clip's start), `end` (the clip's end), `end-0.1` (0.1 seconds before the end), `event:Name` (the moment that HKX annotation event comes).

## Choosing by conditions

The sprint key does a different dodge for each movement direction, only with a weapon drawn and at least 10 stamina.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyDodge">
  <Start control="sprint" cooldown="0.3">
    <Check kind="weaponDrawn"/>
    <Check kind="resource" option="stamina" measure="current" compare="ge" value="10"/>
    <Row to="DodgeBack"><Check kind="movementDirection" option="back"/></Row>
    <Row to="DodgeLeft"><Check kind="movementDirection" option="left"/></Row>
    <Row to="DodgeRight"><Check kind="movementDirection" option="right"/></Row>
    <False/>
  </Start>
  <Action id="DodgeBack" clip="Animations\MyDodge\Back.hkx">
    <IFrame until="0.4"/>
    <MoveCancel at="0.6" blend="0.3"/>
  </Action>
  <Action id="DodgeLeft" clip="Animations\MyDodge\Left.hkx">
    <IFrame until="0.4"/>
    <MoveCancel at="0.6" blend="0.3"/>
  </Action>
  <Action id="DodgeRight" clip="Animations\MyDodge\Right.hkx">
    <IFrame until="0.4"/>
    <MoveCancel at="0.6" blend="0.3"/>
  </Action>
</AnimationSet>
```

How conditions inside a `<Start>` are read:

1. The `<Check>` elements at the top are common conditions. All must hold before going on.
2. `<Row to="action">` rows are tried from the top; the first row whose conditions all hold plays its action. With `match="any"`, one is enough.
3. `<False>` is for when nothing holds. Left empty, it does nothing; `<False to="action"/>` plays another action.
4. With common conditions only and no rows, write `<True to="action"/>`.

- Add `not="true"` to turn a condition around. Group conditions of which one is enough in `<Or>…</Or>`.
- To use the same conditions in several places, write them once at the root as `<Condition id="Name">` and point to them with `condition="Name"`.
- `<Transition>` and QTE results take conditions the same way.

Common conditions:

| `kind` | Also takes | Holds when |
|---|---|---|
| `weaponDrawn` `combat` `air` `sneaking` `blocking` `sprinting` `swimming` `mounted` `female` `interior` | nothing | in that state |
| `resource` | `option="health\|stamina\|magicka"` `measure="current\|percent"` `compare` `value` | the value compares so |
| `equippedType` | `hand="left\|right"` `type` | that kind in that hand: `sword` `dagger` `axe` `mace` `greatsword` `battleaxe` `warhammer` `bow` `crossbow` `staff` `shield` `spell` `torch` `unarmed` |
| `attackState` | `state` | the attack stage: `none` `draw` `swing` `hit` `bowDraw` `bowDrawn` `bowReleased` and more |
| `movementDirection` | `option="forward\|back\|left\|right\|none"` | the movement key direction |
| `chance` | `value` (0~100) | with that chance |
| `magicEffect` `perk` `spell` `race` `faction` `keyword` | `plugin="Mod.esp"` `form="hex ID"` | it has that |
| `keyword` `wornKeyword` `equippedKeyword` | `keyword="WeapTypeClaw"` (editor ID) instead of `plugin` `form`. `equippedKeyword` takes `option="left\|right\|either"` | it has that keyword (itself, the gear it wears, the gear in that hand). Names match without case; when two plugins use one name, the later loaded wins. An unknown name logs a warning and the check is unavailable |
| `global` | `plugin` `form` `equals` | the global equals the value |
| `previousAction` | `actions="A,B"` (this file's actions) `within="seconds"` | it did that action just before |
| `weather` | `option="pleasant\|cloudy\|rainy\|snow"` | that weather |
| `hitReceived` | `option="normal\|power\|bash\|sneak\|blocked"` `within="seconds"` | the last weapon hit it took was normal (no power attack, no bash), a power attack, a bash, a sneak attack or blocked. Spells do not count. On a block, use it in `<Start event="blockHitStart">` with `within="0.5"` |
| `gameHour` | `compare` `value` | the game hour compares so |
| `space` | `distance` `direction="selfForward\|selfBack\|selfLeft\|selfRight"` | there is that much open space that way |

`compare` is `lt` (less) `le` (less or equal) `eq` (equal) `ge` (greater or equal) `gt` (greater) `range` (between `value` and `upper`).

## Running features on events

When an annotation event placed in the HKX comes, a feature runs. Register the event and its feature once in `<EventRegistry>`; numbers such as strength go in each action's `<EventValues>`. To run one at a set clip time, write `<Do at="seconds">`.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyMod">
  <EventRegistry>
    <Event name="MyMod_Sheathe">
      <Do op="hide"/>
    </Event>
    <Event name="MyMod_Draw">
      <Do op="show"/>
    </Event>
    <Event name="MyMod_Kick">
      <Do op="strike" pick="combatTarget" reaction="stagger"/>
    </Event>
  </EventRegistry>
  <Start key="V" to="Kick"/>
  <Action id="Kick" clip="Animations\MyMod\Kick.hkx">
    <EventValues>
      <Event name="MyMod_Kick">
        <Do op="strike" ratio="1.5" strength="0.5"/>
      </Event>
    </EventValues>
    <Do at="0.35" op="shake" strength="0.4" duration="0.3"/>
    <Do at="0.35" op="sound" file="sound\fx\mymod\kick.wav"/>
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

A feature applies to the actor that sent the event (the one playing the action).

| `op` | Does | Main values |
|---|---|---|
| `hide` / `show` | Hide weapon and shield / show them again | |
| `spell` | Cast a spell once | `plugin` `form` |
| `stateOn` / `stateOff` | Keep a spell's effect on / take it off | `plugin` `form` |
| `iframeOn` / `iframeOff` | Invulnerability on / off | |
| `armorOn` / `armorOff` | Stagger resistance on / off | |
| `rotateOn` / `rotateOff` | Allow turning / stop it | |
| `cost` | Stamina and magicka cost | `stamina` `magicka` |
| `shake` | Camera shake (player) | `strength` `duration` |
| `timeOn` / `timeOff` | Change the time scale / set it back (player) | `factor` (0.05~5; 0.5 is half speed) |
| `stopVelocity` | Stop sliding momentum | |
| `strike` | Hit with weapon damage | target `pick="combatTarget\|partner\|crosshair\|area"`, reaction `reaction="none\|stagger\|push"`; values `ratio` (weapon damage multiplier) or `amount` (fixed damage), `strength` (stagger), `force` (push) |
| `ragdoll` | Knock into ragdoll | `direction="down\|up\|forward\|backward\|left\|right"`; value `force` (0~20) |
| `fireArrow` | Shoot the equipped arrow at the crosshair (player) | value `damage` (% of a full draw's damage) |
| `sound` | Play a sound | `file="sound\…\a.wav"` or `plugin` `form`; value `volume` (0~1) |
| `globalSet` | Change a global's value | `plugin` `form` `restore="end"` (set back when it ends); value `value` |
| `modEvent` | Send a mod event to Papyrus | `name` `text`; value `number` |

A feature run by an event can be held back with a `delay` (seconds) value (not firing an arrow). In a pair's `<Do at>`, `target="initiator|receiver|both|player"` says whom it applies to.

## Pairs

An action two people do together is a `<Pair>`. Its HKX is one paired HKX holding both roles (the one who starts it and the partner). In combat, F starts the pair with the actor in front.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyGrab">
  <Start key="F" gamepad="LB" cooldown="1" to="Throw">
    <Check kind="combat"/>
  </Start>
  <Pair id="Throw" clip="Animations\MyGrab\Throw.hkx">
    <FindPartner range="150" angle="60"/>
    <Exit blend="0.4"/>
  </Pair>
</AnimationSet>
```

- `<FindPartner range angle>`: finds a partner within that distance and angle ahead (default 150 and 60).
- To start on an event another mod sends, write `<Start event="Name" target="combatTarget" to="…">`.
- To replace a vanilla action, start on its event and name the same event in `block`, e.g. on a blocked hit `<Start event="blockHitStart" block="blockHitStart" to="…">`. While the action can start, the vanilla graph does not get the event and this action plays (not with a `chance` condition). An NPC is the same when `actor` includes NPCs. `<Action block="…">` blocks while that action plays; a key or control start's `block` is the player's only.
- While an action plays that has a `<Transition event>` on an event, that event is the transition's and a `<Start>` on the same event does not start (the rule keys follow). Before its window opens a buffered transition keeps it and goes when the window opens; after it closes nothing happens. For example a `<Transition event="blockHitStart" from="0.1">` inside a block hit action for a block hit after block hit.
- `subject="partner"` on a condition checks the partner. Conditions that are the same whoever is asked (`chance` `gameHour` `weather` `global` `movementDirection` `space` `ground`) do not take it.
- Pairs play in third person only. Invulnerability, stagger resistance and such in a pair are clip-time features, like `<Do at op="iframeOn">`.

## QTE

A QTE shows on screen during an action, and the result leads to a different action.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyQte">
  <Start key="H" to="Struggle"/>
  <Action id="Struggle" clip="Animations\MyQte\Struggle.hkx">
    <QTE id="Escape" at="0.5" key="E" gamepad="A" type="rapidPress" pressCount="8">
      <Success to="EscapeWin"/>
      <Failure to="EscapeLose"/>
    </QTE>
  </Action>
  <Action id="EscapeWin" clip="Animations\MyQte\Win.hkx"/>
  <Action id="EscapeLose" clip="Animations\MyQte\Lose.hkx"/>
</AnimationSet>
```

- `type`: `splitArc` `timingBar` `rotatingNeedle` (timing), `holdRelease` (hold, then release), `rapidPress` (rapid presses, `pressCount`), `keySequence` (keys in order).
- Results are `<Success>` `<Failure>` `<Perfect>`, each with `to` (the next action) or `event` (an event to send).
- To show two QTEs at the same moment, give them different `channel` values. On the same channel only the first one shows.

## Key names

- Keyboard (`key`): `A`~`Z`, `0`~`9`, `F1`~`F12`, `Space` `Enter` `Tab` `Esc` `Backspace` `LShift` `RShift` `LCtrl` `RCtrl` `LAlt` `RAlt` `CapsLock` `Up` `Down` `Left` `Right` `Home` `End` `PageUp` `PageDown` `Insert` `Delete` `Num0`~`Num9` `NumEnter` `NumPlus` `NumMinus` `NumMul` `NumDiv` `NumDot` `Minus` `Equals` `LBracket` `RBracket` `Semicolon` `Apostrophe` `Grave` `Backslash` `Comma` `Period` `Slash`, mouse `Mouse1`~`Mouse7` `MouseWheelUp` `MouseWheelDown`
- Gamepad (`gamepad`): `A` `B` `X` `Y` `LB` `RB` `LT` `RT` `LS` `RS` `Start` `Back` `Up` `Down` `Left` `Right`
- Game controls (`control`, in place of `key` and `gamepad`): `jump` `sprint` `sneak` `activate` `readyWeapon` `shout` `rightAttack` `leftAttack` `togglePOV` `run` `toggleRun` `autoMove` `wait` `favorites` `tweenMenu`. They follow the player's key bindings, and when such a press starts an action, the game's own action (the actual jump and so on) does not happen.

Key names are physical keys, whatever the keyboard input language. To start when the key is released, write `<Start onKeyUp="true">`.

## Checking a file

`AnimationInjectorXmlCheck.exe`, in the Animation Injector Editor download, checks a file outside the game with the same rules the game reads by.

```
AnimationInjectorXmlCheck.exe "path\MyMod.xml"
```

`Valid: …` means the game can read the file. With an error, it says which element (for example `/AnimationSet/Action/Vanilla`) is wrong and how. A file the game did not read leaves the same reason in `AnimationInjector.log`.

If several files have an action with the same `id`, the one in the file that sorts later by name is used.
