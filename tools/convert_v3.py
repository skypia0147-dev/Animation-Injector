"""Convert an Animation Injector declaration from format 2 to format 3 (global EventRegistry).

    python tools/convert_v3.py <in.xml> [out.xml]     (no out: print the result)

What changes (docs/계획-전역이벤트등록.md §3.3):
  - <Effect name="E" op=.. role=.. source=.. .../>  -> <EventRegistry><Event name="E"><Do op=.. target=.. from=../>
    (equal functions of one name are written once; role/source values: other -> partner, target -> receiver)
  - <Effect at="s" .../>                            -> <Do at="s" .../> in place
  - <Events extra="a,b"/>                            -> <Event name="a"/> <Event name="b"/> (names only)
  - <FireArrow at="event:E" [fallback=".."]/>        -> <FireArrow/> + <Event name="E"><Do op="fireArrow" from="player"/>
  - version="2"                                      -> version="3"
Event moments of windows (IFrame, HyperArmor, Lock, Rotate) and Cost stop the conversion: format 3 has
them only as registry functions, and which actor and scope they meant needs a person's decision.
The rest of the file (comments, layout) is left as it is.
"""
import re
import sys

EFFECT = re.compile(r'^([ \t]*)<Effect\b([^>]*?)/>[ \t]*\r?\n?', re.M)
EVENTS = re.compile(r'^[ \t]*<Events\b([^>]*?)/>[ \t]*\r?\n?', re.M)
FIRE = re.compile(r'<FireArrow\b([^>]*?)/>')
ATTR = re.compile(r'([A-Za-z_][\w.-]*)\s*=\s*"([^"]*)"')
ROLE = {'sender': 'sender', 'other': 'partner', 'partner': 'partner', 'both': 'both', 'player': 'player',
        'initiator': 'initiator', 'target': 'receiver'}
# Format 2 sources: sender and both (the default) accepted either pair actor, which is "any" now.
SOURCE = {'any': 'any', 'sender': 'any', 'both': 'any', 'player': 'player', 'npc': 'npc', 'initiator': 'initiator',
          'target': 'receiver'}
WINDOW = re.compile(r'<(IFrame|HyperArmor|Lock|Rotate|Cost)\b[^>]*"event:')


def attrs(text):
    return [(k, v) for k, v in ATTR.findall(text)]


def function(pairs, where):
    """<Do> attributes (without at) from an old Effect's attributes."""
    out, rest = [], []
    op = target = source = None
    for k, v in pairs:
        if k == 'op':
            op = v
        elif k == 'role':
            if v not in ROLE:
                raise SystemExit(f'{where}: unknown role "{v}"')
            target = ROLE[v]
        elif k == 'source':
            if v not in SOURCE:
                raise SystemExit(f'{where}: unknown source "{v}"')
            source = SOURCE[v]
        elif k not in ('name', 'at'):
            rest.append((k, v))
    if not op:
        raise SystemExit(f'{where}: Effect without op')
    out.append(('op', op))
    if target and target != 'sender':
        out.append(('target', target))
    if source and source != 'any':
        out.append(('from', source))
    return tuple(out + rest)


def text(pairs):
    return ' '.join(f'{k}="{v}"' for k, v in pairs)


def convert(xml):
    notes = []
    if WINDOW.search(xml):
        raise SystemExit('an IFrame/HyperArmor/Lock/Rotate/Cost uses an event moment: convert it by hand')
    if not re.search(r'<AnimationSet\b[^>]*version="2"', xml):
        raise SystemExit('not a format 2 AnimationSet')
    registry = {}  # name -> list of functions (first-seen order)
    order = []

    def register(name, fn=None):
        key = name.lower()
        if key not in registry:
            registry[key] = (name, [])
            order.append(key)
        functions = registry[key][1]
        if fn is not None and fn not in functions:
            if any(f[0] == fn[0] for f in functions):
                notes.append(f'"{name}": {fn[0][1]} with other settings kept as a second function')
            functions.append(fn)

    def effect(m):
        indent, body = m.group(1), m.group(2)
        pairs = attrs(body)
        values = dict(pairs)
        where = f'line {xml.count(chr(10), 0, m.start()) + 1}'
        if 'name' in values and 'at' in values:
            raise SystemExit(f'{where}: Effect with both name and at')
        fn = function(pairs, where)
        if 'name' in values:
            register(values['name'], fn)
            return ''
        if 'at' not in values:
            raise SystemExit(f'{where}: Effect without name or at')
        newline = '\n' if m.group(0).endswith('\n') else ''
        return f'{indent}<Do at="{values["at"]}" {text(fn)}/>{newline}'

    xml = EFFECT.sub(effect, xml)

    def events(m):
        values = dict(attrs(m.group(1)))
        for name in values.get('extra', '').split(','):
            if name.strip():
                register(name.strip())
        return ''

    xml = EVENTS.sub(events, xml)

    def fire(m):
        values = dict(attrs(m.group(1)))
        at = values.get('at', '')
        if not at.startswith('event:'):
            return m.group(0)
        register(at[6:], (('op', 'fireArrow'), ('from', 'player')))
        if 'fallback' in values:
            notes.append(f'FireArrow "{at}": fallback {values["fallback"]} dropped (no clip-time shot with an event)')
        return '<FireArrow/>'

    xml = FIRE.sub(fire, xml)
    # Effects left with nothing inside and no attributes do nothing.
    xml = re.sub(r'^[ \t]*<Effects>\s*</Effects>[ \t]*\r?\n?', '', xml, flags=re.M)
    xml = re.sub(r'(<AnimationSet\b[^>]*version=")2(")', r'\g<1>3\2', xml, count=1)

    newline = '\r\n' if '\r\n' in xml else '\n'
    lines = ['  <EventRegistry>']
    for key in order:
        name, functions = registry[key]
        if not functions:
            lines.append(f'    <Event name="{name}"/>')
            continue
        lines.append(f'    <Event name="{name}">')
        lines += [f'      <Do {text(fn)}/>' for fn in functions]
        lines.append('    </Event>')
    lines.append('  </EventRegistry>')
    block = newline.join(lines) + newline
    if not order:
        return xml, notes
    inputs = re.search(r'</Inputs>[ \t]*\r?\n', xml) or re.search(r'<Inputs\b[^>]*/>[ \t]*\r?\n', xml)
    anchor = inputs or re.search(r'<AnimationSet\b[^>]*>[ \t]*\r?\n', xml)
    xml = xml[:anchor.end()] + block + xml[anchor.end():]
    return xml, notes


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    with open(sys.argv[1], encoding='utf-8', newline='') as f:
        source = f.read()
    result, notes = convert(source)
    for note in notes:
        print('note:', note, file=sys.stderr)
    if len(sys.argv) == 3:
        with open(sys.argv[2], 'w', encoding='utf-8', newline='') as f:
            f.write(result)
    else:
        sys.stdout.write(result)


if __name__ == '__main__':
    main()
