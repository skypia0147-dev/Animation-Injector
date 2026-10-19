"""Rewrite an AnimationSet declaration from format 2/3 to format 4 (docs/계획-문법개편-v4.md).

Format 4 keeps what a file did, so values whose omitted default changed are written out:
  - version="2"/"3" -> "4"
  - every Action/Pair without Require cooldown gets cooldown="0.2" (the old omitted value; format 4 omits 0)
  - every Action/Pair without Advanced safety gets safety="0.7" (old; format 4 omits 3, a pair 30)
  - Require stamina="X" checked and spent; format 4 only checks, so a <Cost stamina="X"/> (at the start) is added
And drops what format 4 no longer reads:
  - Advanced graph (every action plays on the full-body route)
  - <Window always="true"/> (an omitted Window is the whole clip); a Cancel/Connect left empty closes itself
  - Carry alignStart, Hold pose
  - gamepad="<number>" -> the button's name (gamepad="4096" -> "A"), else code:<number>; gamepad="0" is dropped
Combos (Combo, Step, Transition, External type="combo", QTE step) have no mechanical translation: such a
file is refused and listed, to be rewritten with the editor (one Action per HKX, joined with Connect).
So are the features 0.12.238 removed (Carry weapon="keep" and weaponShow, the Lock window and the
lock/unlock event functions, External): the file is refused and listed, to be edited by hand.

Bytes outside the rewritten text are kept as they were (line endings, spacing, attribute order, comments).

Usage: python tools/convert_v4.py [--backup DIR] [--dry-run] FILE...
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

# The engine's gamepad codes by name (XmlV2.cpp kGamepadButtons).
GAMEPAD = {0x1: 'Up', 0x2: 'Down', 0x4: 'Left', 0x8: 'Right', 0x10: 'Start', 0x20: 'Back', 0x40: 'LS', 0x80: 'RS',
           0x100: 'LB', 0x200: 'RB', 0x1000: 'A', 0x2000: 'B', 0x4000: 'X', 0x8000: 'Y', 9: 'LT', 10: 'RT'}
OLD_COOLDOWN = '0.2'
OLD_SAFETY = '0.7'

COMMENT = re.compile(r'<!--.*?-->', re.S)
ATTR = r'\s+{}\s*=\s*("[^"]*"|\'[^\']*\')'


def masked(text: str) -> str:
    """The text with comments blanked (same length), so element searches skip commented-out markup."""
    return COMMENT.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text)


def attr(tag: str, name: str):
    m = re.search(ATTR.format(name), tag)
    return m.group(1)[1:-1] if m else None


def drop_attr(tag: str, name: str) -> str:
    return re.sub(ATTR.format(name), '', tag)


def set_attr(tag: str, name: str, value: str) -> str:
    """Replace the attribute's value, or append it before the tag's end."""
    if attr(tag, name) is not None:
        return re.sub(ATTR.format(name), lambda m: m.group(0)[:m.group(0).index('=') + 1] + f'"{value}"', tag)
    end = len(tag) - (2 if tag.endswith('/>') else 1)
    return tag[:end].rstrip() + f' {name}="{value}"' + tag[end:]


def substitute(text: str, pattern: str, fn) -> tuple[str, int]:
    """re.sub over the comment-masked text, applied to the real text."""
    mask = masked(text)
    out, last, n = [], 0, 0
    for m in re.finditer(pattern, mask, re.S):
        real = text[m.start():m.end()]
        new = fn(real)
        if new != real:
            n += 1
        out.append(text[last:m.start()])
        out.append(new)
        last = m.end()
    out.append(text[last:])
    return ''.join(out), n


def convert(text: str):
    mask = masked(text)
    root = re.search(r'<([A-Za-z]+)\b[^>]*>', re.sub(r'<\?.*?\?>', lambda m: ' ' * len(m.group(0)), mask))
    if not root or root.group(1) != 'AnimationSet':
        raise ValueError('not an AnimationSet declaration')
    version = attr(root.group(0), 'version')
    if version == '4':
        raise ValueError('already format 4')
    if version not in ('2', '3'):
        raise ValueError(f'unknown version {version!r}')
    combo = [what for what, pat in (('Combo', r'<Combo\b'), ('Step', r'<Step\b'), ('Transition', r'<Transition\b'),
                                    ('External type="combo"', r'<External\b[^>]*\btype\s*=\s*["\']combo'),
                                    ('QTE step', r'<QTE\b[^>]*\sstep\s*='))
             if re.search(pat, mask)]
    if combo:
        raise ValueError('combo (' + ', '.join(combo) + '): rewrite with the editor as Actions joined by Connect')
    removed = [what for what, pat in (('Carry weapon="keep"', r'<Carry\b[^>]*\bweapon\s*=\s*["\']keep["\']'),
                                      ('Carry weaponShow', r'<Carry\b[^>]*\bweaponShow\s*='),
                                      ('Lock', r'<Lock\b'),
                                      ('Do op="lock"/"unlock"', r'<Do\b[^>]*\bop\s*=\s*["\'](?:lock|unlock)["\']'),
                                      ('External', r'<External\b'))
               if re.search(pat, mask)]
    if removed:
        raise ValueError('removed in 0.12.238 (' + ', '.join(removed) + '): no translation, edit by hand')
    nl = '\r\n' if '\r\n' in text else '\n'
    counts = {}

    def count(key, n):
        if n:
            counts[key] = counts.get(key, 0) + n

    # Root version.
    start = root.start()
    tag = text[start:root.end()]
    text = text[:start] + set_attr(tag, 'version', '4') + text[root.end():]

    # Advanced graph: gone. An Advanced left empty keeps its place: safety is written into it below.
    text, n = substitute(text, r'<Advanced\b[^>]*>', lambda s: drop_attr(s, 'graph'))
    count('Advanced graph', n)

    # Window always="true": the omitted Window. A Cancel/Connect left empty closes itself.
    always = r'<Window\s+always\s*=\s*["\']true["\']\s*/>'

    def unwindow(block):
        kind = re.match(r'<(\w+)', block).group(1)
        open_end = re.match(r'<[^>]*>', block).end()
        close_at = block.rindex(f'</{kind}')
        body = block[open_end:close_at]
        body, k = re.subn(r'(?m)^[ \t]*' + always + r'[ \t]*\r?\n', '', body)
        body, k2 = re.subn(always, '', body)
        if not k + k2:
            return block
        if body.strip() == '':
            return block[:open_end - 1].rstrip() + '/>'
        return block[:open_end] + body + block[close_at:]
    text, n = substitute(text, r'<(Cancel|Connect)\b[^>]*[^/]>.*?</\1\s*>', unwindow)
    count('Window always', n)
    if re.search(r'<Window\b[^>]*\balways\s*=', masked(text)):
        raise ValueError('Window always with other attributes: edit by hand')

    # Carry and Hold.
    text, n = substitute(text, r'<Carry\b[^>]*>', lambda s: drop_attr(s, 'alignStart'))
    count('Carry', n)
    text, n = substitute(text, r'<Hold\b[^>]*>', lambda s: drop_attr(s, 'pose'))
    count('Hold pose', n)

    # Gamepad numbers to names; gamepad="0" ("no button") is the attribute left out.
    def pad(s):
        def one(m):
            code = int(m.group(3))
            if code == 0:
                return ''
            return f'{m.group(1)}{m.group(2)}{GAMEPAD.get(code, f"code:{code}")}{m.group(2)}'
        return re.sub(r'(\sgamepad\s*=\s*)(["\'])(\d+)\2', one, s)
    text, n = substitute(text, r'<[A-Za-z]+\b[^>]*\sgamepad\s*=\s*["\']\d+["\'][^>]*>', pad)
    count('gamepad', n)

    # Per Action/Pair: the old omitted cooldown and safety, Require stamina -> Cost.
    def element(block):
        kind = re.match(r'<(Action|Pair)\b', block).group(1)
        open_end = re.match(r'<[^>]*>', block).end()
        open_tag = block[:open_end]
        if open_tag.endswith('/>'):
            body, close = '', ''
            open_tag = open_tag[:-2].rstrip() + '>'
        else:
            close_at = block.rindex(f'</{kind}')
            body, close = block[open_end:close_at], block[close_at:]
        mbody = masked(body)
        # Child indent: the first child's, else the parent's (read from the text before it) + two spaces.
        child = re.search(r'\n([ \t]*)<[A-Za-z]', mbody)
        parent_indent = element.indent
        indent = child.group(1) if child else parent_indent + '  '
        added = []

        req = re.search(r'<Require\b[^>]*>', mbody)
        stamina = None
        if req:
            tag = body[req.start():req.end()]
            new = tag
            if attr(tag, 'cooldown') is None:
                new = set_attr(new, 'cooldown', OLD_COOLDOWN)
                count('Require cooldown', 1)
            stamina = attr(tag, 'stamina')
            body = body[:req.start()] + new + body[req.end():]
        else:
            added.append(f'<Require cooldown="{OLD_COOLDOWN}"/>')
            count('Require cooldown', 1)
        if stamina is not None and float(stamina) > 0:
            if kind == 'Pair':
                raise ValueError('Pair with Require stamina: a pair spends through Effects, edit by hand')
            if re.search(r'<Cost\b', masked(body)):
                raise ValueError('Require stamina with a Cost already: merge by hand')
            added.append(f'<Cost stamina="{stamina}"/>')
            count('stamina -> Cost', 1)

        mbody = masked(body)
        adv = re.search(r'<Advanced\b[^>]*>', mbody)
        if adv:
            tag = body[adv.start():adv.end()]
            if attr(tag, 'safety') is None:
                body = body[:adv.start()] + set_attr(tag, 'safety', OLD_SAFETY) + body[adv.end():]
                count('Advanced safety', 1)
        else:
            added.append(f'<Advanced safety="{OLD_SAFETY}"/>')
            count('Advanced safety', 1)

        if not added:
            return open_tag + body + close
        lines = ''.join(f'{nl}{indent}{a}' for a in added)
        if not close:  # was self-closing
            return open_tag + lines + f'{nl}{parent_indent}</{kind}>'
        # After a leading self-closing Start, else as the first children.
        st = re.match(r'\s*<Start\b[^>]*/>', masked(body))
        at = st.end() if st else 0
        return open_tag + body[:at] + lines + body[at:] + close

    mask = masked(text)
    out, last = [], 0
    for m in re.finditer(r'<(Action|Pair)\b[^>]*?/>|<(Action|Pair)\b[^>]*>.*?</\2\s*>', mask, re.S):
        line_start = text.rfind('\n', 0, m.start()) + 1
        element.indent = re.match(r'[ \t]*', text[line_start:m.start()]).group(0)
        out.append(text[last:m.start()])
        out.append(element(text[m.start():m.end()]))
        last = m.end()
    out.append(text[last:])
    text = ''.join(out)
    return text, counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+', type=Path)
    ap.add_argument('--backup', type=Path)
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    failed = False
    for path in args.files:
        data = path.read_bytes()
        bom = data.startswith(b'\xef\xbb\xbf')
        try:
            out, counts = convert(data[3 if bom else 0:].decode('utf-8'))
        except (ValueError, UnicodeDecodeError) as e:
            print(f'SKIP {path}: {e}')
            failed = True
            continue
        new = (b'\xef\xbb\xbf' if bom else b'') + out.encode('utf-8')
        summary = ', '.join(f'{k} {v}' for k, v in counts.items()) or 'version only'
        print(f'{"would edit" if args.dry_run else "edit"} {path}: {summary}')
        if args.dry_run:
            continue
        if args.backup:
            anchor = path.resolve().anchor
            target = args.backup / str(path.resolve())[len(anchor):]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        path.write_bytes(new)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
