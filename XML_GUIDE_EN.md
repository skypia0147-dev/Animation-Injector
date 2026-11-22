# Writing Animation Injector XML

[한국어](XML_GUIDE.md)

Making action mods with Animation Injector Editor is recommended. This guide is for those who want to write the XML by hand. It covers what is used most. For anything not here, build it in the editor and look at how it is written with **XML preview (Ctrl+`)**. A hand-written XML also opens in the editor with its cards laid out automatically, so you can move over to the editor at any time.

Every example in this guide is a file Animation Injector reads as it is.

## Where files go

| What | Where |
|---|---|
| XML | `Data\SKSE\Plugins\AnimationInjector\<mod name>\<any name>.xml` |
| HKX | Under `Data\meshes\actors\character\`. `clip` is the path from that folder |
| Carry ragdoll values | `Ragdoll\<lift action id>.xml` next to the XML ([Carry ragdoll](#carry-ragdoll)) |

- Put the XML directly in the mod-name folder (folders below it are not read).
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
    <Check kind="actorStatus" option="weaponDrawn"/>
  </Start>
  <Action id="Slash1" clip="Animations\MyMod\Slash1.hkx" enter="0.1">
    <IFrame from="0.1" until="0.4"/>
    <HyperArmor until="0.6"/>
    <Cost stamina="20"/>
    <Transition to="Slash2" key="G" gamepad="RB" from="0.45" until="end-0.1"/>
    <MoveCancel at="0.8" blend="0.25"/>
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
| `<Transition to key from until>` | A key press between `from` and `until` moves on to the `to` action. `event="Name"` works in place of a key. A key transition can tell how its key is pressed with `press="tap\|double\|hold"` (Press kinds below, 1.0.8 or later) |
| `<Transition to on="end" blend>` | When this action ends (its Exit point) it moves on to `to` instead of ending. One per action, no key, `from` or `until`. Only in an action that ends at a time (not with `<Exit at="event:…">`) |
| `<Transition to on="release" from until>` | Charge end: moves on while the key is up inside the window. A state, not a moment: let go before the window opens, it moves on as it opens. `key`, `gamepad` or `control` name the key; without them, the key that started this action (a start's or a key transition's, kept through on="end" and on="release"). An NPC counts as releasing only at a loop's `maxHold` |
| `<Transition to on="move" from until buffer>` | Move transition: moves on when a movement key is pressed **in a charge** (while the key that started this action is down). With the charge key up a movement key does nothing and a buffered one is dropped (`on="release"` goes then). An action on a tapped key, such as a dodge, chains by a key transition on its key with a direction card (`<Transition condition="…" key=…>`). As a key transition: a press inside the window goes at once, one made before it opens is buffered and goes as it opens (`buffer="false"`: no buffer). A movement key held as the window opens counts as buffered too — held, the action plays once after another; let go, the one playing plays to its end. The card picks by the direction held (or the one pressed if it is already up), read as a movement condition reads it, TDM included. No key or event, the player only. Into the same action it plays that action again from the start; a loop choosing itself plays on (a charge keeps going). For directional dashes picked by a `movementDirection` card |
| `<Transition end="true" …>` | Ending transition (1.0.8 or later): instead of going to another action it ends this one at once (leaving with `<Exit blend>`). Its trigger is any other transition's: a key (`key`, `gamepad`, `control`, `input`, `press`), an `event`, `on="release"`, `on="move"`, with a window (`from`, `until`, `buffer`). No `to`, condition card, `blend`, `partnerTo` or `on="end"`. Ordinary actions only. Example, a loop started by an event and ended by another: `<Action id="Loop" clip="…" loop="true" maxHold="600"><Transition end="true" event="StopLoop"/></Action>` |
| `<MoveCancel at blend>` | After `at`, a movement key cancels the action back to walking or running. An NPC in combat gets movement back from `at` and leaves as soon as its AI moves or attacks (or plays on if it stays) |
| `<IFrame from until>` | Invulnerable in that window |
| `<HyperArmor from until>` | Stagger resistance in that window |
| `<TimeScale factor from until ease>` | The whole world's time scale in that window (only when the player plays it). `factor` 0.05~5 (0.5 is half speed); `ease` is the real seconds it takes to reach the factor, and back to normal speed at the window's end (0~2; left out, at once). Normal speed also comes back when the action is cut short |
| `<Rotate from until>` | Can turn in that window. True Directional Movement takes no movement input during an action, so while a movement key is held there the runtime turns the player as TDM's setting would (vanilla style: the camera's front; directional: the keys' way from the camera) |
| `<Cost stamina magicka health measure at>` | Resource cost: stamina, magicka, health (health never its last point). `measure="percent"`: percent of each maximum (0~100); without it, the values as written |
| `<Exit at blend>` | When it ends and how many seconds it blends. Without `at`, 0.05 seconds before the clip's end |

`<Action>` attributes: `enter` (blend seconds going in), `air="both"` (ground and air) / `"only"` (air only; without it, ground only), `view="first"` or `"both"` (a first-person action; first-person HKX files go at the same path under `_1stPerson`), `otherView="switch"` (the view switch, 1.0.8 or later: a third-person action started in first person, or a first-person action started in third person, switches the camera to the action's view, plays, and returns to the view it started from when it ends. Without it the action does not start in the other view. Not with `view="both"`, nor on a first-person pair. The `firstPerson` of 1.0.7 and earlier is not read), `loop="true" maxHold="3"` (a loop: the clip plays over and over, and after `maxHold` seconds (default 3) the key counts as released, counted from the press of the key that started the charge while it is still down, so directional loops share one clock; without an on="release" transition it ends then. Ordinary actions only; no `<Exit at>` or on="end" transition. Feature times (i-frames, rotation…) count from the action's start and take no `end-…`; transition windows (`from`, `until`) open and close again **every cycle**, `until="end"` and `end-…` meaning that cycle's end; HKX annotation events come again every cycle).

**Charge example**: `<Start control="rightAttack" press="hold" to="ChargeStart"/>` → ChargeStart's `<Transition to="ChargeLoop" on="end"/>` and `<Transition condition="ChargeLevel" on="release" from="0.2"/>` → `<Action id="ChargeLoop" loop="true" maxHold="3">` with `<Transition condition="ChargeLevel" on="release"/>` → the ChargeLevel card picks the strong or weak release by `<Check kind="heldTime" compare="ge" value="1.5"/>`.

**Writing times**: `0.4` (seconds from the clip's start), `end` (the clip's end), `end-0.1` (0.1 seconds before the end), `event:Name` (the moment that HKX annotation event comes).

## Choosing by conditions

The sprint key does a different dodge for each movement direction, only with a weapon drawn and at least 10 stamina.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyDodge">
  <Start control="sprint" cooldown="0.3">
    <Check kind="actorStatus" option="weaponDrawn"/>
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
| `actorStatus` | `option="weaponDrawn\|combat\|sneaking\|blocking\|sprinting\|swimming\|mounted\|staggered\|ragdoll\|recovering\|bleedingOut\|killmove\|paired\|attacking"` (several, comma separated: any of them) | in that state. `attacking` is any attack stage (a swing, a bow draw, a bash and so on) or the graph's `IsAttacking` (attacks a behavior mod plays, such as BFCO) |
| `air` | `option="any\|falling\|rising"`; `falling` and `rising` also take `compare` `value` (the height fallen) | in the air (at all / falling / rising) |
| `protection` | `option="essential\|protected"` | essential or protected |
| `female` `interior` `child` | nothing | when so |
| `resource` | `option="health\|stamina\|magicka"` `measure="current\|percent"` `compare` `value` | the value compares so |
| `equippedType` | `hand="left\|right"` `type` | that kind in that hand: `sword` `dagger` `axe` `mace` `greatsword` `battleaxe` `warhammer` `bow` `crossbow` `staff` `shield` `spell` `torch` `unarmed` |
| `attackState` | `state` | the attack stage: `none` `draw` `swing` `hit` `bowDraw` `bowDrawn` `bowReleased` and more |
| `movementDirection` | `option="forward\|back\|left\|right\|none"` | the movement key direction. In True Directional Movement's directional mode the character turns toward the input and runs forward, so any held movement key reads `forward` (without TDM or in its other modes, the keys as pressed) |
| `chance` | `value` (0~100) | with that chance |
| `magicEffect` `perk` `spell` `race` `faction` `keyword` | `plugin="Mod.esp"` `form="hex ID"` | it has that |
| `keyword` `wornKeyword` `equippedKeyword` | `keyword="WeapTypeClaw"` (editor ID) instead of `plugin` `form`. `equippedKeyword` takes `option="left\|right\|either"` | it has that keyword (itself, the gear it wears, the gear in that hand). Names match without case; when two plugins use one name, the later loaded wins. An unknown name logs a warning and the check is unavailable |
| `global` | `plugin` `form` `equals` | the global equals the value |
| `previousAction` | `actions="A,B"` (this file's actions) `within="seconds"` | it did that action just before |
| `weather` | `option="pleasant\|cloudy\|rainy\|snow"` | that weather |
| `hitReceived` | `option="normal\|power\|bash\|sneak\|blocked"` `within="seconds"` | the last weapon hit it took was normal (no power attack, no bash), a power attack, a bash, a sneak attack or blocked. Spells do not count. On a block, use it in `<Start event="blockHitStart">` with `within="0.5"` |
| `recentEvent` | `events="event,event"` `within="seconds"` | one of those animation events reached it within that time (events sent to the graph and events the graph sends). To skip the `sprintStop` right after a sprint attack: `<Check kind="recentEvent" events="attackStop" within="0.5" not="true"/>` |
| `foot` | `option="left\|right"` | the foot that last touched the ground is the left or the right one (of the locomotion's `FootLeft` and `FootRight`, the one that came last, at any speed). A stop for each foot: `<Start event="moveStop" actor="player"><Row to="StopLeft"><Check kind="foot" option="left"/></Row><False to="StopRight"/></Start>` |
| `gameHour` | `compare` `value` | the game hour compares so |
| `heldTime` | `compare` `value` | how long the key that started this action was held, in seconds; when an on="release" transition chooses, from the press to the release. The player only (unknown for an NPC) |
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
| `magic` | Magic effect: `action="cast"` casts it once at the actor, `action="add"` keeps it on (a spell that is not an ability or a disease is cast instead), `action="remove"` takes it off (one the actor had too) | `action` `plugin` `form` |
| `iframeOn` / `iframeOff` | Invulnerability on / off | |
| `armorOn` / `armorOff` | Stagger resistance on / off | |
| `rotateOn` / `rotateOff` | Allow turning / stop it | |
| `cost` | Resource cost (health never its last point) | `stamina` `magicka` `health`; the registry's `measure="percent"`: percent of the maximum |
| `shake` | Camera shake (player) | `strength` `duration` |
| `timeOn` / `timeOff` | Change the time scale / set it back (player). Not a clip-time feature of an ordinary action: use `<TimeScale>` | `factor` (0.05~5; 0.5 is half speed), `ease` (real seconds to reach it and to set it back, 0~2) |
| `stopVelocity` | Stop sliding momentum | |
| `strike` | Hit with weapon damage | target `pick="combatTarget\|partner\|crosshair\|area"`, reaction `reaction="none\|stagger\|push"`; values `ratio` (weapon damage multiplier) or `amount` (fixed damage), `strength` (stagger), `force` (push) |
| `ragdoll` | Knock into ragdoll | `direction="down\|up\|forward\|backward\|left\|right"`; value `force` (0~20) |
| `fireArrow` | Shoot the equipped arrow at the crosshair (player) | value `damage` (% of a full draw's damage) |
| `sound` | Play a sound | `file="sound\…\a.wav"` or `plugin` `form`; value `volume` (0~1) |
| `globalSet` | Change a global's value | `plugin` `form` `restore="end"` (set back when it ends); value `value` |
| `modEvent` | Send a mod event to Papyrus | `name` `text`; value `number` |
| `cancel` | End the pair | |
| `gripHold` | Grip hold: keep the hand's weapon bone (right: Weapon, left: Shield) at its value at this moment, past the end of the action ([Grip hold](#grip-hold)) | `hand="right\|left\|both"`, `view="third\|first\|both"` (default third) |
| `gripRelease` | End a grip hold (at once) | `hand`, `view` |

A feature that stays on (hiding, invulnerability and such) ends when its pair (`show`, `iframeOff`, ...) comes or when the actions that follow have all ended. But `iframeOn`, `armorOn`, `rotateOn` and `timeOn` turned on by a registered event, while some event is registered with their pair, stay on until that event comes, even after the animation ends (they end on death, ragdoll or a load). But when the HKX that sent the on event also sends the off event later, it works as a range: it ends when the off event comes or when that HKX ends or is cut short. This holds for our actions and for any other mod's or vanilla HKX alike. A registered on function with a `duration` (seconds, 0~60) ends when that time is up; an off event before then ends it then, and it lasts its time even when the HKX ends or is cut short (real seconds for a time scale, game time for the others).

A feature run by an event can be held back with a `delay` (seconds) value (not firing an arrow). In a pair's `<Do at>`, `target="initiator|receiver|both|player"` says whom it applies to.

## HKX annotation commands (`AI.`)

An HKX annotation `AI.Command|value|value` runs a feature at its time with no registration. The values are the ones written in the annotation, so it works the same in any other mod's or vanilla HKX. The command name ignores case, and every value must be written. The feature applies to the actor that sent the annotation.

| Command | What it does |
|---|---|
| `AI.IFrameOn[\|span]` / `AI.IFrameOff` | Invulnerability on / off |
| `AI.ArmorOn[\|span]` / `AI.ArmorOff` | Stagger resistance on / off |
| `AI.RotateOn[\|span]` / `AI.RotateOff` | Allow turning on / off |
| `AI.TimeOn\|factor\|ease[\|span]` / `AI.TimeOff` | Time scale (player). Factor 0.05~5, ease 0~2 real seconds |
| `AI.Hide` / `AI.Show` | Hide / show the weapons |
| `AI.MagicAdd\|plugin\|form` · `AI.MagicRemove\|…` · `AI.MagicCast\|…` | Add · remove · cast a magic effect |
| `AI.Cost\|stamina\|magicka\|health` · `AI.CostPercent\|…` | Spend resources (amounts / percent of each maximum) |
| `AI.CamShake\|strength\|duration` | Shake the camera (player) |
| `AI.StopVelocity` | Stop the sliding speed |
| `AI.Ragdoll\|direction\|force` | Ragdoll (direction down·up·forward·backward·left·right, force 0~20) |
| `AI.FireArrow\|damage` | Fire an arrow (percent of a full draw's damage, player) |
| `AI.Sound\|file\|bone\|volume` · `AI.SoundForm\|plugin\|form\|bone\|volume` | Play a sound (bone spine·head·rightHand·leftHand·root) |
| `AI.GlobalSet\|plugin\|form\|value` | Change a global's value (not set back) |
| `AI.ModEvent\|name\|text\|number` | Send a mod event to Papyrus |
| `AI.CancelPair` | End the pair |
| `AI.GripHold\|hand\|view` | Grip hold (hand right, left, both; view third, first, both) |
| `AI.GripRelease\|hand\|view` | End a grip hold |

A strike has no command for its many settings; register it on an event. An on command ends by the rules above; an off command can always be sent, so without one it stays on until death, ragdoll or a load. Commands and registered events are one feature (`AI.IFrameOff` also ends invulnerability a registered event turned on).

## Grip hold

For an action that changes how the weapon is held, such as switching to a reverse grip, and the grip should stay after the action ends (1.0.9 and later). H switches to a reverse grip and keeps it; J goes back to the normal grip.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyGrip">
  <Start key="H" to="Reverse">
    <Check kind="actorStatus" option="weaponDrawn"/>
  </Start>
  <Start key="J" to="Normal">
    <Check kind="actorStatus" option="weaponDrawn"/>
  </Start>
  <Action id="Reverse" clip="Animations\MyGrip\ReverseGrip.hkx">
    <Do at="0.6" op="gripHold" hand="right"/>
    <Exit blend="0.2"/>
  </Action>
  <Action id="Normal" clip="Animations\MyGrip\NormalGrip.hkx">
    <Do at="0" op="gripRelease" hand="right"/>
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- `gripHold` remembers the weapon bone's position and rotation relative to the hand at that moment (right hand `Weapon`, left hand `Shield`) and keeps that value in every animation that plays afterwards, attacks and part actions included. The body and arms move as the animations say; only the way the weapon is held stays.
- `gripRelease` lets go at once: from that frame the animation's value shows. Start the release action's first frame in the reverse grip and no seam shows.
- Released on its own when: the weapon is sheathed, that hand's equipment changes, death, ragdoll, loading a save.
- `view`: the views it holds in (`third`, `first`, `both`). The value taken in the view the action played in is carried over to the other view, so the hold works in first person even when the action has no first-person HKX.
- The left hand needs Left Hand Equipment Overhaul 2.0 or later: vanilla's adjustment (`DW_ShieldAdjustment`) overrides the Shield bone. Make left-hand reverse-grip HKX from the reference pose, with an `Adjustment_Additive` annotation at 0 seconds, and an `Adjustment_Off` annotation at the end of the release action. For a left-hand hold carried over to the other view, Animation Injector sends that view `Adjustment_Additive` itself, and `Adjustment_Off` when the hold ends.
- Also usable as a registered event: `<Event name="…"><Do op="gripHold" hand="right" view="both"/></Event>`. Any HKX that sends that event name runs it, ours or another mod's, vanilla or OAR. The HKX annotation commands are `AI.GripHold|right|third` and `AI.GripRelease|right|third`.

## Upper body or one arm while moving (body)

With `body` an `<Action>` plays on the upper body or one arm only; the legs keep the locomotion, so **the player walks and runs freely while it plays** (drinking a potion, crossed arms, a wave). Third person only.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WalkingIdles">
  <Start key="Num1" to="CrossArms"/>
  <Start key="Num2" to="Wave"/>
  <Action id="CrossArms" clip="Animations\MyMod\CrossArms.hkx" body="upper" loop="true">
    <Transition key="Num1" end="true"/>
  </Action>
  <Action id="Wave" clip="Animations\MyMod\Wave.hkx" body="rightArm" enter="0.2">
    <Exit blend="0.4"/>
  </Action>
</AnimationSet>
```

- `body`: `upper` (the spine and everything above it), `rightArm` (the right clavicle to the fingers, the right hand's weapon and AnimObjectR), `leftArm` (the left one, the shield and AnimObjectL). Left out, the action is a full-body action as before.
- With `upper`, **standing plays the whole HKX** (legs and hips too: the HKX's legs carry its leaning upper body), and **moving, the legs are the locomotion's** while the upper body keeps the HKX's pose. Starting to move, the legs go over to the locomotion as the speed rises; stopping, they come back. Sneaking always keeps the locomotion's legs.
  - While moving the upper body takes none of the locomotion's sway, only its turn toward the way it goes (side steps). A lean the HKX makes by tilting COM was held up by its own legs, so it is left out while moving; the spine's own bend stays.
  - An arm (`rightArm`, `leftArm`) follows the locomotion's torso: a hand at the mouth moves with the head while walking.
- The upper body and each arm can play at once (crossed arms with `upper` and a wave with `rightArm`: the right arm is the wave's). A new action on a busy part (a start or a transition) crosses over from the running one at once, over the new action's `enter`.
- End: a clip played once ends at its end (`<Exit at>`); with `loop="true"`, by an end transition (`<Transition key end="true">`), another transition or `maxHold`. A part loop has a `maxHold` only when written, or its default 3 s when it has a charge release (`on="release"`). Blend in and out with `enter` and `<Exit blend>` (0.3 s by default).
- It also ends on: an attack, a block, drawing or sheathing, casting, a jump or the air, a sprint start, a full-body action, ragdoll, knockdown, furniture, a mount, swimming, a switch to first person, death.
- Started during a sprint, it stops the sprint (the player runs) and plays; the sprint key held through it does not sprint again until it is pressed again. An NPC does not start one while sprinting.
- Allowed: `loop` and `maxHold`, `enter`, `<Exit at blend>`, `<Do at>` (clip-time functions), starts (`<Start>` keys, events, condition cards), and `<Transition>`s as a full-body action's: keys, events, press kinds (`press`), on its end (`on="end"`), charge release (`on="release"`), ending (`end="true"`), condition cards. Part and full-body actions transition to each other (charge, below). HKX annotations become events as in other actions (AnimObjects too).
- Not allowed: `<Pair>`, `rig` (creatures), `view="first|both"`, `otherView`, `air`, `attack`, `block`, `<Rotate>`, `<IFrame>`, `<HyperArmor>`, `<TimeScale>`, `<Cost>`, `<MoveCancel>`, `<QTE>`, `<Camera>`, movement transitions (`on="move"`: it plays while moving), a `blend` on a transition into or out of a part action (the destination's `enter` crosses), transitions between a pair and a part action, and a QTE result or `partnerTo` naming a part action.
- Leave `FootLeft` and `FootRight` annotations out of an upper-body HKX: the locomotion makes the footsteps.

### Charging with part actions

Each of a charge's three actions (start, hold, release) has its own part. With the start and the hold on the upper body the player **charges while walking**; with the release on the whole body they stop and let it go.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WalkingCharge">
  <Start key="G" press="hold" to="ChargeStart"/>
  <Action id="ChargeStart" clip="Animations\MyMod\ChargeStart.hkx" body="upper">
    <Transition to="ChargeHold" on="end"/>
  </Action>
  <Action id="ChargeHold" clip="Animations\MyMod\ChargeHold.hkx" body="upper" loop="true" maxHold="4">
    <Transition to="ChargeRelease" on="release"/>
  </Action>
  <Action id="ChargeRelease" clip="Animations\MyMod\ChargeRelease.hkx"/>
</AnimationSet>
```

- On the same part (start -> hold) the upper body crosses over without a break, over the destination's `enter` (0.3 s by default).
- Part -> whole body (hold -> release): the full-body action starts and the upper body blends out over the release's `enter`.
- Whole body -> part: the part action blends in while the full-body action leaves through its own `<Exit blend>`.
- To another part (upper -> right arm): the running part blends out and the new one blends in.
- `maxHold` counts from the press of the start key (as a full-body charge's); past it the key counts as released and the release goes.
- An attack, a block, a jump and the like during the charge end the part action there, without a release.

## Creature actions

An `<Action>` with `rig` is played by that creature on its own. Example: `<Action id="Howl" rig="wolf" clip="animations\MyMod\howl.hkx">`.

- `rig`: a creature name such as `wolf` `bear` `sabrecat` `troll` `draugr` `werewolf` `vampirelord` (the same list as a pair's `FindPartner rig`). Without it the action is the character's. Creatures sharing a folder and skeleton play it together (`wolf` and `dog`, `draugr` and `skeleton`): filter with a race condition for one of them. The editor writes this value itself from the HKX.
- `clip`: a creature action's clip is relative to **that creature's folder** (`meshes\actors\canine\` for wolves, `meshes\actors\bear\` for bears). A character action's stays relative to `meshes\actors\character\`.
- Starting: on an event the creature sends, `<Start event="…" actor="npc" to="…">`. Key and game-control starts play a creature action when the player is a werewolf or vampire lord. A start never plays an action whose `rig` is not the actor's, so one file can hold character and creature actions together.
- Available: `loop`, `enter`, `<Exit>`, `<Transition>` to an action of the same `rig`, `<Rotate>` `<IFrame>` `<HyperArmor>` `<MoveCancel>` `<Cost>`, clip-time features (`<Do at>`) and event features, the conditions every actor has, `block`.
- Character actions only: `view` `otherView` `air` `attack`, `<QTE>` `<Camera>` `<CarryRelease>`, `<Do op="hide|show|fireArrow">`. A `<Transition>` to an action of another skeleton family is refused too.

### Pairs a creature starts

A `<Pair>` with `rig` is started by that creature (a creature kill move). Its partner is a character only (no `FindPartner rig`, or `character`).

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WolfKill">
  <Start event="NPCWolfAttackA" block="NPCWolfAttackA" actor="npc" target="combatTarget" to="WolfPounce"/>
  <Pair id="WolfPounce" rig="wolf" clip="animations\MyMod\WolfPounce.hkx">
    <FindPartner range="200" angle="90"/>
    <Exit blend="0.3"/>
  </Pair>
</AnimationSet>
```

- `clip`: relative to **the character's folder** (`meshes\actors\character\`), as for a pair a character starts. Both read the one HKX holding both roles, with the same roles: the creature starting it is the `NPC` side, the character partner the `2_` side.
- Creatures sharing its folder and skeleton start it too (a dog for `wolf`).
- Not available: everything a creature action may not have, `<Carry>`, a pair between creatures. A `<Transition to>` goes to an action of the same skeleton family, a `partnerTo` to a character action.

## Pairs

An action two people do together is a `<Pair>`. Its HKX is one paired HKX holding both roles (the one who starts it and the partner). In combat, F starts the pair with the actor in front.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyGrab">
  <Start key="F" gamepad="LB" cooldown="1" to="Throw">
    <Check kind="actorStatus" option="combat"/>
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
- `subject="partner"` on a condition checks the partner. Conditions that are the same whoever is asked (`chance` `gameHour` `weather` `global` `movementDirection` `space` `ground` `heldTime`) do not take it.
- Pairs play in third person by default (started in first person, the view switches to third person and back when the pair ends). With `<Pair view="both">` and a first-person HKX at the same path under `_1stPerson`, the pair plays in first person while the player is in first person. `view="first"` is a pair that starts in first person only and needs the first-person HKX alone (a paired HKX holds both roles, so the partner plays that file too; without it the pair does not start). `both` needs both the third-person and the first-person HKX. Carries are third person only. A pair ends when either actor dies or ragdolls while it plays. The exception is a partner killed by the paired HKX's own `2_KillActor` annotation: as in a vanilla kill move, only the partner leaves the animation (it dies and ragdolls) and the starting actor plays its clip to the end (1.0.8 or later), so later annotations such as `NPCEndAnimatedCamera` and `NPCPairEnd` still arrive. Invulnerability, stagger resistance and such in a pair are clip-time features, like `<Do at op="iframeOn">`.

### Carry ragdoll

How a target lifted by a carry (`<Carry target="physics">`) sways under physics is set by one file per lift action: `Ragdoll\<lift action id>.xml` next to that action's XML (e.g. `GrappleCarryLift` in `Grapple\Grapple.xml` → `Grapple\Ragdoll\GrappleCarryLift.xml`). The game reads it again at every pickup, so an edit made while the game runs applies from the next lift. Without the file, or with an error in it, the game's own ragdoll values (vanilla) are used (the error is logged, and the checker checks this file along with the action XML). The editor's carry item makes it.

```xml
<?xml version="1.0" encoding="utf-8"?>
<Ragdoll rig="character" version="1">
  <Motion follow="1" gravity="4" drag="10" spin="10" inertia="10"/>
  <Body bone="NPC COM [COM ]" simulate="false"/>
  <Body bone="NPC Spine2 [Spn2]" simulate="false"/>
  <Body bone="NPC L Thigh [LThg]" simulate="true" cone="40" twist="-15 15"/>
</Ragdoll>
```

| Element | Attributes |
|---|---|
| `<Ragdoll>` | `rig` (the lifted target's rig; another rig's file is not used) `version="1"` |
| `<Motion>` | `follow` (share of the carrier's speed change given to the body at once, 0~1) `gravity` (gravity factor 1~4) `drag` (air drag 0~10) `spin` (spin damping 0~10) `inertia` (rotational inertia factor 1~10) `friction` (joint friction 0~100; left out, vanilla) |
| `<Body bone>` | One per bone carrying a ragdoll body. `simulate` (false: it never sways on its own; below a body that sways, its joint stays at its first angle and it moves along, and below bodies that all stay out of physics it follows the lift pose), that body's own `drag` `spin` `gravity` `inertia` `friction`, the joint to the body above: `cone` (0~180 degrees), `plane` and `twist` ("minimum maximum" degrees), its axis bone `swingFrom`. Joint values left out keep the vanilla joint |

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

Key names are physical keys, whatever the keyboard input language.

### Press kinds (`<Start press>`, `<Transition press>`)

| `press` | Starts |
|---|---|
| `down` (default) | the moment it goes down, without waiting |
| `tap` | the moment a short press comes up; when the same key also has a `double`, once the wait for a second press is over |
| `double` | at the second of two quick presses, the moment it goes down. A held second press is still a `double` |
| `hold` | the moment it has been held past the hold time, without waiting for the release |

- The times are in `AnimationInjector.ini` under `[Input]`: `fPressHoldTime` (shorter is a `tap`, longer a `hold`; default 0.2 s) and `fPressDoubleWindow` (from the first release to the second press; default 0.25 s).
- A key waits only for the kinds it has. The conditions (movement direction and the rest) are read when it goes down; when no kind could start then, it does not wait. The movement direction is the one of the first press.
- The same key's `down` starts are tried first; one that starts takes the press whole.
- A game control's press that no start takes goes back to the game, late by the wait. Example: `<Start control="jump" press="tap" to="Dodge"/>` dodges on a short press and still jumps on a long one.
- Game controls that open a menu (`wait` `favorites` `tweenMenu`) open it the moment they go down, so they take `down` only.

**Press kinds on a transition (1.0.8 or later)**: a key transition (`<Transition key|gamepad|control>`) takes the same `press`. The playing action tells its transition keys apart the way a start's key is.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyDodge">
  <Start key="LAlt" press="double" to="Roll"/>
  <Action id="Roll" clip="Animations\MyDodge\Roll.hkx">
    <Transition to="Roll2" key="LAlt" press="double" from="0.3"/>
    <Transition to="Step" key="LAlt" press="tap" from="0.3"/>
    <Transition to="Step" key="LAlt" press="hold" from="0.3"/>
  </Action>
  <Action id="Roll2" clip="Animations\MyDodge\Roll2.hkx"/>
  <Action id="Step" clip="Animations\MyDodge\Step.hkx"/>
</AnimationSet>
```

- One key can have one transition of each kind. `down` (the default) goes the moment the key goes down, so it cannot share its key with another kind (use `tap`).
- Not for `event` or `on` transitions.
- The waits are a start's: a `tap` on release (after the wait for a second press when the key also has a `double`), a `hold` at the hold time, a `double` at its second press.
- A condition card's movement direction is the one of the first press (even if the movement key is up when the kind is known).
- While the action is on its way out (its exit blend), or when it ends before the kind is known, the press goes to the key's `<Start>` as the kind it turned out to be. A `tap` or `hold` no start takes goes to the `down` starts.

## Separating walk and run (Locomotion)

Vanilla movement blends the walk and run animations by speed. So walking at a steady speed still mixes in some run (about 15% for one-handed forward, about 31% for magic backward), and the playback rate hardly changes when the character's speed does, so the feet slide. With a `<Locomotion>`, each direction plays a walk or a run alone; the two mix only while changing between them and while stopping, and the playback rate follows the character's actual speed.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WalkRunSplit">
  <Locomotion>
    <Tree weapon="2HW" use="own"/>
    <Tree weapon="H2H" use="own"/>
    <Except set="Sneak"/>
  </Locomotion>
</AnimationSet>
```

- `<Locomotion/>` alone applies to every weapon state and direction. There are no numbers to write.
- Playback rate = the character's actual speed / the file's ground speed. A plugin that changes movement speeds, a speed buff or slow, and the weight of what is carried all change the speed, and the feet still keep the ground (within 0.1x to 10x).
- A file's ground speed is found automatically. Vanilla files (and files that replace them through OAR) use the motion data registered in the game. For an own tree's files, see `<Tree>` below.
- Walk or run is chosen from the character's walking/running state and its actual speed. Changing from a run to a walk crosses over as the speed comes down to the walk speed; stopping takes the legs toward the walk while the upper body keeps the run until it blends into the idle. While walk and run mix, the upper body is the run's (Spine2 and above, arms, hands, head).
- `<Except set="…"/>` (a child of `<Locomotion>`, one per set): leaves that weapon state vanilla. `set` is `1HM` (one-handed and unarmed drawn), `2HM` (greatsword), `2HW` (two-handed axes and hammers), `H2H` (unarmed), `Bow`, `CrossBow`, `Magic` (magic readied), `MagicCast` (moving while casting), `Sheathed`, `Sneak`; moving while blocking is `Block1HM` (one-handed, unarmed, magic), `Block2HM`, `Block2HW`, `BlockH2H`, `BlockBow` (bow and crossbow), `BlockSneak`. `2HW`, `Block2HW`, `H2H` and `BlockH2H` exist only with that weapon's `<Tree use="own">`. Moving while blocking always plays the walk alone.
- The original clips are not changed; copies are made, so animations replaced through OAR are followed. NPCs that use the same graphs get it too.
- `<Tree weapon="…" use="…"/>` (a child of `<Locomotion>`, one per weapon): gives a weapon that borrows another weapon's locomotion tree in vanilla one of its own. There are two such weapons.
  - `weapon="2HW"` (two-handed axes and hammers): vanilla plays the 2HM direction ladders on its legs only and a direction-less walk/run blend on its arms, so a change of the legs' rate puts the arms out of step. `use="2HM"`: moving and sneaking both use the greatsword's (2HM) ones; the axe or hammer is held like a greatsword while moving. `use="own"`: builds a 2HW tree laid out like 2HM's. Sneaking is like 2HM's: sneak legs, this tree's upper body.
  - `weapon="H2H"` (unarmed): vanilla plays the one-handed tree whole. `use="own"` makes a copy of that tree for unarmed alone and points the unarmed state at it; one-handed weapons are unchanged. Unarmed sneaking plays the common sneak tree and is not changed.
  - The files of `use="own"`: each slot of the tree plays the original file's name with its prefix replaced, `meshes\actors\character\animations\2HW_<name>.hkx` (from the 2HM files, for example `2HW_RunForward.hkx`) or `H2H_<name>.hkx` (from the 1HM files, for example `H2H_RunForward.hkx`), when that file exists, else the original file. A file's length and stride are free. If the file carries movement annotations (`animmotion x y z`, the notation Animation Motion Revolution uses, for example `animmotion 0 190.16 0` at the end of the cycle), the last annotation's distance over its time is the file's ground speed, so a step made not to slide in the animation tool does not slide in game either. Without them the original file's ground speed is used (the file is played as if its step were the original's). Animation Motion Revolution does not have to be installed. Foot sync and footsteps come from the file's own `FootLeft` and `FootRight` annotations, so every file needs both.
  - `use="own"` for unarmed (`weapon="H2H"`) also makes the arms follow the movement animation while moving. In vanilla, unarmed takes only the legs and torso from the movement animation and holds the arms in one fixed pose (`H2H_LocoMotionPose.hkx`). The unarmed standing pose, sprinting, first person and moving while blocking are unchanged. A direction without an `H2H_` file plays the one-handed animation, arms included, so a mod that uses the unarmed tree should ship the files of every direction it means to use.
  - Moving while blocking gets the same tree, so that weapon walks with these files instead of the original weapon's legs (blocking uses the lower body only; the upper body is the block pose).
- One file in the whole game declares it: `SKSE\Plugins\AnimationInjector\Locomotion\Locomotion.xml` (names ignoring case). That file holds the `<Locomotion>` and nothing else (with an action or an event registration in it the whole file is skipped). A `<Locomotion>` in any other XML is not read, and the log says so.
- When two mods ship one, the files share a path, so the mod manager's (MO2 and others) higher-priority mod's file is used whole. An own tree's HKX files follow the same priority. Put the mod's name in the `AnimationSet`'s `id` (the log names whose file it is).
- Comparing with vanilla: set a key at `[Debug] iLadderToggleKey` in `AnimationInjector.ini` to switch it on and off in game. Off, the own trees (`<Tree use="own">`) go back to the vanilla trees too, so their files do not play.

### Turning to the opposite way

- With the ladders on, a turn to the opposite way (forward and back, left and right) cross-blends the locomotion from the pose at that moment for 0.2 s. Nothing to write for it.
- **Turn actions**: when the player runs one way (run or sprint, not sneaking) and presses the opposite way, the runtime sends an event: `AI_Turn_RightToLeft` from right to left, `AI_Turn_LeftToRight` from left to right, `AI_Turn_ForwardToBack` from forward to back, `AI_Turn_BackToForward` from back to forward. It goes at the key press, before the engine slows down, so the HKX plays everything from the braking to the push-off the other way. Keep the actions it starts in an XML other than the `<Locomotion>` one.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="PivotTurn">
  <Start event="AI_Turn_RightToLeft" actor="player" to="PivotLeft"/>
  <Start event="AI_Turn_LeftToRight" actor="player" to="PivotRight"/>
  <Action id="PivotLeft" clip="Animations\PivotTurn\PivotRightToLeft.hkx" enter="0.12">
    <Exit blend="0.2"/>
  </Action>
  <Action id="PivotRight" clip="Animations\PivotTurn\PivotLeftToRight.hkx" enter="0.12">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

  - Speed: 250 or more forward and sideways, 160 or more running back (running back is about 200, walking back about 80). Never while walking.
  - Left and right only while running sideways, forward and back only while the body moves the way the keys point (running back then forward, running forward then backing off). A character that runs toward where it faces (True Directional Movement's directional setting and the like) turns its body instead, and gets no event.
  - The other key must come within 0.15 s of letting go of the first; none is sent again within 0.4 s.
  - Give it an `enter` (about 0.1 s). Straight in from the running pose it jumps in one frame depending on where the feet are (0.12 s took one frame's movement from 68~135 to 16~21).
  - The HKX: start from the pose of running the first way, end in the pose of running the other way, with the travel in it (root motion). When it ends, the locomotion takes over over its `<Exit blend>`.

## Checking a file

`AnimationInjectorXmlCheck.exe`, in the Animation Injector Editor download, checks a file outside the game with the same rules the game reads by.

```
AnimationInjectorXmlCheck.exe "path\MyMod.xml"
```

`Valid: …` means the game can read the file. With an error, it says which element (for example `/AnimationSet/Action/MoveCancel`) is wrong and how. A file the game did not read leaves the same reason in `AnimationInjector.log`.

Action ids belong to their file (its `AnimationSet id`), so several files may use the same `id` without one replacing another. When two files use the same `AnimationSet id`, the actions of the one whose path sorts later are kept apart and the log warns.
