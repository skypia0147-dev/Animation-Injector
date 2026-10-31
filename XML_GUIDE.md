# Animation Injector XML 작성 안내

[English](XML_GUIDE_EN.md)

동작 모드는 Animation Injector Editor로 만드는 것을 권합니다. 이 문서는 XML을 직접 쓰고 싶은 분을 위한 길잡이입니다. 자주 쓰는 것만 다룹니다. 여기 없는 기능은 에디터로 만든 뒤 **XML 미리보기(Ctrl+`)** 로 어떻게 쓰이는지 보면 됩니다. 직접 쓴 XML도 에디터에서 열면 카드가 자동으로 배치되므로, 언제든 에디터로 옮겨 갈 수 있습니다.

이 문서의 예시는 모두 Animation Injector가 그대로 읽는 파일입니다.

## 파일 위치

| 무엇 | 어디에 |
|---|---|
| XML | `Data\SKSE\Plugins\AnimationInjector\<모드 이름>\<아무 이름>.xml` |
| HKX | `Data\meshes\actors\character\` 아래. `clip`에는 이 폴더 기준 경로를 씁니다 |

- HKX 경로의 파일·폴더 이름에는 영문·숫자·일반 기호만 씁니다.
- XML이나 HKX를 바꾼 뒤에는 게임을 완전히 다시 시작해야 반영됩니다.
- 파일 하나에 오류가 하나라도 있으면 그 파일 전체가 무시됩니다. 게임에 넣기 전에 [검사](#검사하기)하세요.

## 가장 작은 파일

G 키(패드 RB)를 누르면 구르기 HKX를 재생합니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyMod">
  <Start key="G" gamepad="RB" cooldown="0.5" to="Roll"/>
  <Action id="Roll" clip="Animations\MyMod\Roll.hkx">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- `<?xml … ?>`: XML 표준 선언(이 파일은 UTF-8로 저장된 XML이라는 표시). 빼도 되지만, 쓴다면 파일 맨 첫 줄에 두고 UTF-8로 저장합니다.
- `<AnimationSet>`: 파일의 맨 바깥. `version="6"`은 그대로 쓰고, `id`는 모드마다 다른 이름(영문·숫자·`_`)으로 정합니다.
- `<Start>`: 동작을 시작하는 방법. `to`가 재생할 동작입니다. `cooldown`(초)이 지나기 전에는 다시 시작하지 않습니다.
- `<Action>`: HKX 하나를 재생하는 동작. `id`는 파일 안에서 겹치지 않게 정합니다. 다른 모드에 같은 `id`가 있어도 서로 섞이지 않습니다.
- `<Exit>`: 끝날 때 원래 동작으로 돌아가는 블렌드 시간(초).

## 동작 이어 하기

무기를 뽑은 상태에서 G를 누르면 베기 1을 하고, 그 도중에 G를 다시 누르면 베기 2로 이어집니다.

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

| 요소 | 뜻 |
|---|---|
| `<Transition to key from until>` | `from`~`until` 사이에 키를 누르면 `to` 동작으로 넘어갑니다. 키 대신 `event="이름"`도 됩니다 |
| `<Transition to on="end" blend>` | 이 동작이 끝나는 순간(종료 지점) 끝내는 대신 `to`로 넘어갑니다. 동작마다 하나, 키·`from`·`until` 없음. 시각으로 끝나는 동작에서만(`<Exit at="event:…">`과 함께 쓸 수 없음) |
| `<Transition to on="release" from until>` | 구간 안에서 키가 떼져 있으면 넘어갑니다. "뗀 순간"이 아니라 상태라 구간 전에 뗐으면 구간이 열리는 순간 넘어갑니다. 키는 `key`·`gamepad`·`control`로 정하고, 쓰지 않으면 이 동작을 시작한 키(시작 키나 키 전환의 키, 끝나면·키를 떼면으로 이어지는 동안 유지). NPC는 반복 동작의 `maxHold`에서만 뗀 것으로 봅니다 |
| `<Transition to on="move" from until buffer>` | **차지 중**(이 동작을 시작한 키를 누르고 있는 동안)에 이동키를 누르면 넘어갑니다. 차지 키를 떼면 이동키는 아무것도 하지 않고 선입력도 버립니다(그때는 `on="release"`). 닷지처럼 키를 짧게 누르는 동작은 이동키+닷지 키를 쓰는 키 전환(`<Transition condition="방향 카드" key=…>`)으로 잇습니다. 키 전환과 같아서 구간 안에서 누르면 바로, 구간이 열리기 전에 누른 것도 선입력으로 받아 열리는 순간 넘어갑니다(`buffer="false"`면 선입력 없음). 구간이 열릴 때 이동키를 누르고 있는 것도 선입력으로 칩니다 — 누르고 있는 동안 한 번씩 이어 재생되고, 떼면 재생 중인 동작은 끝까지 재생하고 끝납니다. 조건 카드는 누르고 있는 방향(이미 뗐으면 누른 방향)으로 고릅니다(이동 방향 조건과 같은 판정, TDM 반영). 키·이벤트 없음, 플레이어만. 같은 동작으로 가면 그 동작을 처음부터 다시 재생하고, 반복 동작이 같은 동작을 고르면 끊지 않고 이어 재생합니다(차지는 그대로 유지). 방향별 대시를 조건 카드(`movementDirection`)로 고르는 데 씁니다 |
| `<MoveCancel at blend>` | `at` 이후 이동 키를 누르면 동작을 끊고 걷기·달리기로 돌아갑니다. 전투 중인 NPC는 `at`부터 AI에게 이동을 돌려주고, AI가 움직이거나 공격하려 하면 끊습니다(가만히 있으면 끝까지 재생) |
| `<Vanilla to from until>` | 그 사이에 게임의 원래 행동으로 끊을 수 있습니다. `to`: `Attack` `PowerAttack` `Block` `Bash` `Sprint` `Jump` `Sneak`. `until`은 끝 기준(`end`, `end-0.2`)으로 씁니다 |
| `<IFrame from until>` | 그 사이 무적 |
| `<HyperArmor from until>` | 그 사이 경직 저항 |
| `<Rotate from until>` | 그 사이 방향을 돌릴 수 있음. True Directional Movement는 동작 중 이동 입력을 받지 않으므로, 그 사이 이동 키를 누르고 있으면 런타임이 TDM 설정대로 돌립니다(바닐라 스타일: 카메라 정면, 방향성: 카메라 기준 누른 키 방향) |
| `<Cost stamina magicka at>` | 스태미나·매지카 소모 |
| `<Exit at blend>` | 언제 끝내고 몇 초 동안 섞을지. `at`을 쓰지 않으면 클립 끝 0.05초 전 |

`<Action>`의 속성: `enter`(들어갈 때 블렌드 초), `air="both"`(땅·공중 모두) / `"only"`(공중에서만, 쓰지 않으면 땅에서만), `view="first"`·`"both"`(1인칭 동작, 1인칭 HKX는 `_1stPerson` 아래 같은 경로), `loop="true" maxHold="3"`(반복 재생: 클립을 끝없이 이어 재생하고, `maxHold`초(기본 3)가 지나면 키를 뗀 것으로 봅니다(차지를 시작한 키를 아직 누르고 있으면 그 키를 누른 순간부터 세어, 방향별 반복 동작을 오가도 시계는 하나). 키를 떼면 전환이 없으면 그때 끝냅니다. 일반 동작만, `<Exit at>`·끝나면 전환은 쓸 수 없습니다. 무적·회전 같은 기능 시각은 동작 시작부터 세고 `end-…`를 쓸 수 없습니다. 전환의 감지 구간(`from`·`until`)은 **바퀴마다** 다시 열리고 닫히며 `until="end"`·`end-…`는 그 바퀴의 끝입니다. HKX 주석 이벤트는 바퀴마다 다시 옵니다).

**차지 예시**: `<Start control="rightAttack" press="hold" to="ChargeStart"/>` → ChargeStart의 `<Transition to="ChargeLoop" on="end"/>`와 `<Transition condition="ChargeLevel" on="release" from="0.2"/>` → `<Action id="ChargeLoop" loop="true" maxHold="3">`의 `<Transition condition="ChargeLevel" on="release"/>` → 조건 카드 ChargeLevel에서 `<Check kind="heldTime" compare="ge" value="1.5"/>`로 강·약 발동.

**시각 쓰는 법**: `0.4`(클립 시작부터 초), `end`(클립 끝), `end-0.1`(끝 0.1초 전), `event:이름`(HKX 주석 이벤트가 오는 순간).

## 조건으로 고르기

질주 키를 누르면 이동 방향에 따라 다른 회피를 합니다. 무기를 뽑았고 스태미나가 10 이상일 때만 합니다.

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

`<Start>` 안에 조건을 쓰는 순서:

1. 맨 위의 `<Check>`는 공통 조건입니다. 모두 맞아야 아래로 갑니다.
2. `<Row to="동작">`은 위에서부터 보고, 안의 조건이 모두 맞는 첫 행의 동작을 재생합니다. `match="any"`를 쓰면 하나만 맞아도 됩니다.
3. `<False>`는 아무것도 맞지 않을 때입니다. 비워 두면 아무것도 하지 않습니다. `<False to="동작"/>`으로 다른 동작을 재생할 수도 있습니다.
4. 행 없이 공통 조건만 쓸 때는 `<True to="동작"/>`을 씁니다.

- 조건을 뒤집으려면 `not="true"`를 붙입니다. 여럿 중 하나만 맞으면 되는 조건은 `<Or>…</Or>`로 묶습니다.
- 여러 곳에서 같은 조건을 쓰려면 루트에 `<Condition id="이름">`으로 한 번 쓰고 `condition="이름"`으로 가리킵니다.
- `<Transition>`과 QTE 결과도 같은 방법으로 조건을 쓸 수 있습니다.

자주 쓰는 조건:

| `kind` | 더 쓰는 속성 | 참일 때 |
|---|---|---|
| `weaponDrawn` `combat` `air` `sneaking` `blocking` `sprinting` `swimming` `mounted` `female` `interior` | 없음 | 그 상태일 때 |
| `resource` | `option="health\|stamina\|magicka"` `measure="current\|percent"` `compare` `value` | 값 비교 |
| `equippedType` | `hand="left\|right"` `type` | 그 손에 그 종류: `sword` `dagger` `axe` `mace` `greatsword` `battleaxe` `warhammer` `bow` `crossbow` `staff` `shield` `spell` `torch` `unarmed` |
| `attackState` | `state` | 공격 단계: `none` `draw` `swing` `hit` `bowDraw` `bowDrawn` `bowReleased` 등 |
| `movementDirection` | `option="forward\|back\|left\|right\|none"` | 이동 키 방향. True Directional Movement의 방향성 모드에서는 캐릭터가 입력 쪽으로 돌아 앞으로 달리므로, 이동 키를 누르고 있으면 항상 `forward`입니다(TDM이 없거나 다른 모드면 키 그대로) |
| `chance` | `value`(0~100) | 그 확률로 |
| `magicEffect` `perk` `spell` `race` `faction` `keyword` | `plugin="모드.esp"` `form="16진수 ID"` | 그것을 가졌을 때 |
| `keyword` `wornKeyword` `equippedKeyword` | `keyword="WeapTypeClaw"`(Editor ID) — `plugin` `form` 대신. `equippedKeyword`는 `option="left\|right\|either"` | 그 키워드를 가졌을 때(본인·착용한 장비·그 손의 장비). 이름은 대소문자를 가리지 않고, 두 플러그인이 같은 이름을 쓰면 나중에 로드된 쪽. 없는 이름이면 로그에 경고하고 검사 불능 |
| `global` | `plugin` `form` `equals` | 글로벌 값이 같을 때 |
| `previousAction` | `actions="A,B"`(이 파일의 동작) `within="초"` | 직전에 그 동작을 했을 때 |
| `weather` | `option="pleasant\|cloudy\|rainy\|snow"` | 그 날씨 |
| `hitReceived` | `option="normal\|power\|bash\|sneak\|blocked"` `within="초"` | 마지막으로 맞은 무기 공격이 일반(파워어택·방패 밀치기 아님)·파워어택·방패 밀치기·기습·막음일 때. 마법은 세지 않음. 막았을 때 `<Start event="blockHitStart">`에 `within="0.5"`로 씁니다 |
| `gameHour` | `compare` `value` | 게임 시각 비교 |
| `heldTime` | `compare` `value` | 이 동작을 시작한 키를 누르고 있던 시간(초). 키를 떼면 전환이 고를 때는 누른 순간부터 뗀 순간까지. 플레이어만(NPC는 판정 불가) |
| `space` | `distance` `direction="selfForward\|selfBack\|selfLeft\|selfRight"` | 그 방향에 그만큼 빈 공간이 있을 때 |

`compare`는 `lt`(작다) `le`(작거나 같다) `eq`(같다) `ge`(크거나 같다) `gt`(크다) `range`(`value`~`upper` 사이)입니다.

## 이벤트로 기능 실행하기

HKX에 넣은 주석 이벤트가 오면 기능을 실행합니다. 이벤트와 기능은 `<EventRegistry>`에 한 번 등록하고, 세기 같은 값은 동작마다 `<EventValues>`에 씁니다. 클립의 정해진 시각에 실행하려면 `<Do at="초">`를 씁니다.

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

기능은 이벤트를 보낸 액터(그 동작을 재생하는 액터)에게 적용됩니다.

| `op` | 하는 일 | 주요 값 |
|---|---|---|
| `hide` / `show` | 무기·방패 숨기기 / 다시 보이기 | |
| `spell` | 주문을 한 번 시전 | `plugin` `form` |
| `stateOn` / `stateOff` | 주문 효과를 켜 두기 / 끄기 | `plugin` `form` |
| `iframeOn` / `iframeOff` | 무적 켜기 / 끄기 | |
| `armorOn` / `armorOff` | 경직 저항 켜기 / 끄기 | |
| `rotateOn` / `rotateOff` | 방향 돌리기 허용 / 끄기 | |
| `cost` | 스태미나·매지카 소모 | `stamina` `magicka` |
| `shake` | 카메라 흔들기(플레이어) | `strength` `duration` |
| `timeOn` / `timeOff` | 시간 배율 바꾸기 / 되돌리기(플레이어) | `factor`(0.05~5, 0.5면 절반 속도) |
| `stopVelocity` | 미끄러지는 속도 멈추기 | |
| `strike` | 무기 피해로 때리기 | 대상 `pick="combatTarget\|partner\|crosshair\|area"`, 반응 `reaction="none\|stagger\|push"`; 값 `ratio`(무기 피해 배율) 또는 `amount`(고정 피해), `strength`(경직)·`force`(밀쳐내기) |
| `ragdoll` | 래그돌로 넘어뜨리기 | `direction="down\|up\|forward\|backward\|left\|right"`; 값 `force`(0~20) |
| `fireArrow` | 장착한 화살을 조준점으로 쏘기(플레이어) | 값 `damage`(풀차징 피해의 %) |
| `sound` | 소리 재생 | `file="sound\…\a.wav"` 또는 `plugin` `form`; 값 `volume`(0~1) |
| `globalSet` | 글로벌 값 바꾸기 | `plugin` `form` `restore="end"`(끝나면 되돌림); 값 `value` |
| `modEvent` | Papyrus에 모드 이벤트 보내기 | `name` `text`; 값 `number` |

이벤트로 실행하는 기능에는 `delay`(초)를 값으로 주어 늦출 수 있습니다(활 발사 제외). 페어의 `<Do at>`에는 `target="initiator\|receiver\|both\|player"`로 누구에게 적용할지 정합니다.

## 페어

두 사람이 함께 하는 동작은 `<Pair>`로 씁니다. HKX는 두 역할(시작한 사람과 상대)이 함께 든 페어 HKX 하나입니다. 전투 중에 F를 누르면 앞에 있는 상대와 페어를 시작합니다.

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

- `<FindPartner range angle>`: 앞쪽 몇 거리·몇 도 안의 상대를 찾습니다(기본 150·60).
- 다른 모드가 보내는 이벤트로 시작하려면 `<Start event="이름" target="combatTarget" to="…">`처럼 씁니다.
- 바닐라 동작을 바꾸려면 그 이벤트로 시작하고 같은 이벤트를 `block`에 씁니다. 예: 막았을 때 `<Start event="blockHitStart" block="blockHitStart" to="…">`. 시작할 수 있을 때만 바닐라 그래프에 그 이벤트가 가지 않고 이 동작이 재생됩니다(`chance` 조건이 있으면 막지 않음). NPC도 `actor`가 NPC를 포함하면 같습니다. `<Action block="…">`은 그 동작이 재생되는 동안 막고, 키·조작 시작의 `block`은 플레이어에게만 적용됩니다.
- 재생 중인 동작에 같은 이벤트의 `<Transition event>`가 있으면 그 이벤트는 전환이 받고, 같은 이벤트의 `<Start>`는 시작하지 않습니다(키와 같은 규칙). 구간이 열리기 전이면 선입력으로 기억했다가 열릴 때 전환하고, 닫힌 뒤에는 아무것도 하지 않습니다. 예: 막기 피격 동작 안의 `<Transition event="blockHitStart" from="0.1">`로 연속 막기.
- 조건에 `subject="partner"`를 붙이면 상대를 검사합니다. 누구를 검사해도 같은 조건(`chance` `gameHour` `weather` `global` `movementDirection` `space` `ground`)에는 붙일 수 없습니다.
- 페어는 3인칭에서만 재생됩니다. 페어 안의 무적·경직 저항 등은 `<Do at op="iframeOn">`처럼 시각 기능으로 씁니다.

## QTE

동작 도중 화면에 QTE를 띄우고, 결과에 따라 다른 동작으로 넘어갑니다.

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

- `type`: `splitArc` `timingBar` `rotatingNeedle`(타이밍 맞추기), `holdRelease`(누르고 있다가 떼기), `rapidPress`(연타, `pressCount`), `keySequence`(키 순서).
- 결과는 `<Success>` `<Failure>` `<Perfect>`이고, 각각 `to`(다음 동작)나 `event`(보낼 이벤트)를 씁니다.
- 같은 순간에 QTE 두 개를 띄우려면 `channel`을 다르게 씁니다. 같은 채널이면 먼저 온 하나만 뜹니다.

## 키 이름

- 키보드(`key`): `A`~`Z`, `0`~`9`, `F1`~`F12`, `Space` `Enter` `Tab` `Esc` `Backspace` `LShift` `RShift` `LCtrl` `RCtrl` `LAlt` `RAlt` `CapsLock` `Up` `Down` `Left` `Right` `Home` `End` `PageUp` `PageDown` `Insert` `Delete` `Num0`~`Num9` `NumEnter` `NumPlus` `NumMinus` `NumMul` `NumDiv` `NumDot` `Minus` `Equals` `LBracket` `RBracket` `Semicolon` `Apostrophe` `Grave` `Backslash` `Comma` `Period` `Slash`, 마우스 `Mouse1`~`Mouse7` `MouseWheelUp` `MouseWheelDown`
- 게임패드(`gamepad`): `A` `B` `X` `Y` `LB` `RB` `LT` `RT` `LS` `RS` `Start` `Back` `Up` `Down` `Left` `Right`
- 게임 동작(`control`, `key`·`gamepad` 대신): `jump` `sprint` `sneak` `activate` `readyWeapon` `shout` `rightAttack` `leftAttack` `togglePOV` `run` `toggleRun` `autoMove` `wait` `favorites` `tweenMenu`. 플레이어의 키 설정을 따라가고, 그 키로 동작이 시작되면 게임의 원래 행동(실제 점프 등)은 일어나지 않습니다.

키 이름은 물리 키 기준이라 한/영 입력 상태와 상관없습니다.

### 누름 종류 (`<Start press>`)

| `press` | 시작하는 순간 |
|---|---|
| `down` (기본) | 누르는 순간. 기다리지 않습니다 |
| `tap` | 짧게 눌렀다 뗀 순간. 같은 키에 `double`도 있으면 두 번째 누름을 기다리는 시간이 지난 뒤 |
| `double` | 빠르게 두 번 누를 때, 두 번째로 누르는 순간. 두 번째를 길게 눌러도 `double`입니다 |
| `hold` | 누른 채 기준 시간을 넘기는 순간. 떼기를 기다리지 않습니다 |

- 기준 시간은 `AnimationInjector.ini`의 `[Input]`: `fPressHoldTime`(이보다 짧으면 `tap`, 길면 `hold`, 기본 0.2초), `fPressDoubleWindow`(첫 누름을 뗀 뒤 두 번째 누름까지, 기본 0.25초).
- 키는 그 키에 걸린 종류만큼만 기다립니다. 누르는 순간 조건(이동 방향 등)을 읽어, 시작할 수 있는 종류가 없으면 기다리지 않습니다. 이동 방향은 처음 누른 순간의 값입니다.
- 같은 키의 `down` 시작을 먼저 시도하고, 그 동작이 시작되면 그 누름은 거기서 끝납니다.
- 게임 동작(`control`)의 누름 가운데 어떤 시작도 가져가지 않은 것은 게임에 돌려줍니다. 기다린 만큼 늦게 일어납니다. 예: `<Start control="jump" press="tap" to="Dodge"/>`는 짧게 누르면 닷지, 길게 누르면 원래 점프.
- 메뉴를 여는 게임 동작(`wait` `favorites` `tweenMenu`)은 누르는 순간 메뉴가 열려 `down`만 씁니다.

## 검사하기

Animation Injector Editor 배포 파일에 든 `AnimationInjectorXmlCheck.exe`로 게임 밖에서 검사할 수 있습니다. 게임과 같은 규칙으로 읽습니다.

```
AnimationInjectorXmlCheck.exe "경로\MyMod.xml"
```

`Valid: …`가 나오면 게임이 읽을 수 있는 파일입니다. 오류가 있으면 어느 요소(예: `/AnimationSet/Action/Vanilla`)의 무엇이 틀렸는지 나옵니다. 게임에서 읽히지 않은 파일은 `AnimationInjector.log`에 같은 이유가 남습니다.

같은 `id`의 동작이 여러 파일에 있으면 파일 이름 순으로 나중 것이 쓰입니다.
