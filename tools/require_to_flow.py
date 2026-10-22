"""Move the state checks of <Require> (weapon, blockWhileAttacking, stamina) into condition branch tests
(runtime 0.12.276).

Usage: python require_to_flow.py <xml> [<xml> ...]          (shows what would change)
       python require_to_flow.py --write <xml> [<xml> ...]  (rewrites the files, originals kept as <xml>.before-0.12.276)

From 0.12.276 <Require> takes air and cooldown only; a state check belongs to a condition branch on the way into
the action.
- Values that did nothing (weapon="any", blockWhileAttacking="false", stamina="0") are dropped.
- weapon="drawn" -> <Test kind="weaponDrawn">, blockWhileAttacking="true" -> <Test kind="attackState" state="none">,
  stamina="n" -> <Test kind="stamina" minimum="n"> (all subject="self"; passing goes on).
- An action with a Flow gets the checks at the front of it.
- An action another action's Flow picks: when that Flow's owner has the same checks (they go to its front) they are
  dropped; otherwise they go just before that destination inside that Flow.
- Otherwise an action that starts by itself (a key or an event) gets a Flow of its own that plays itself.
- An action reached only by transitions (Connect, a QTE result) loses the checks: the action before it checked. One
  that a Connect with its own blend leads to cannot hold a Flow (runtime), so its checks are dropped and reported.
A file with an Animation Injector Editor block is opened and saved in the editor instead (this tool leaves blocks).
"""
import io
import re
import shutil
import sys

sys.stdout.reconfigure(encoding="utf-8")

ELEMENT = re.compile(r'(?ms)^([ \t]*)<(Action|Pair)\b([^>]*?)(/?)>(?:(.*?)^\1</\2>)?')
REQUIRE = re.compile(r'<Require\b([^>]*?)/>')
ATTR = re.compile(r'\s(\w+)="([^"]*)"')


def attrs(text):
    return dict(ATTR.findall(text))


def needs_of(req):
    out = []
    if req.get("weapon") == "drawn":
        out.append(("RequireWeapon", 'kind="weaponDrawn" subject="self"'))
    if req.get("blockWhileAttacking") == "true":
        out.append(("RequireNotAttacking", 'kind="attackState" subject="self" state="none"'))
    stamina = float(req.get("stamina", "0") or 0)
    if stamina > 0:
        out.append(("RequireStamina", 'kind="stamina" subject="self" minimum="%s"' % req["stamina"]))
    return out


def unique(name, used):
    i, out = 1, name
    while out in used:
        i += 1
        out = "%s%d" % (name, i)
    used.add(out)
    return out


def convert(text):
    blocks = []
    for m in ELEMENT.finditer(text):
        a = attrs(m.group(3))
        body = m.group(5) or ""
        req_m = REQUIRE.search(body)
        req = attrs(req_m.group(1)) if req_m else {}
        flow_m = re.search(r'<Flow entry="([^"]*)">', body)
        plays = re.findall(r'<Play id="([^"]*)" action="([^"]*)"\s*/>', body)
        blocks.append({"m": m, "id": a.get("id"), "req": req, "needs": needs_of(req), "flow": flow_m, "plays": plays})
    by_id = {b["id"]: b for b in blocks}
    owners = {}
    for b in blocks:
        if b["flow"]:
            for _, dest in b["plays"]:
                if dest != b["id"]:
                    owners.setdefault(dest, []).append(b["id"])
    edits = {}  # block id -> new body
    report = []

    def body_of(bid):
        b = by_id[bid]
        return edits.get(bid, b["m"].group(5) or "")

    for b in blocks:
        bid, req = b["id"], b["req"]
        if not req:
            continue
        body = body_of(bid)
        # <Require> keeps air and cooldown only.
        kept = {k: v for k, v in req.items() if k in ("air", "cooldown")}
        old = REQUIRE.search(body).group(0)
        new = "<Require%s/>" % "".join(' %s="%s"' % kv for kv in kept.items()) if kept else ""
        if new:
            body = body.replace(old, new, 1)
        else:
            body = re.sub(r'(?m)^[ \t]*' + re.escape(old) + r'[ \t]*\r?\n', "", body, count=1)
        edits[bid] = body
        if not b["needs"]:
            if old != new:
                report.append("%s: dropped values that did nothing" % bid)
            continue
    # Move the checks.
    for b in blocks:
        bid, needs = b["id"], b["needs"]
        if not needs:
            continue
        body = body_of(bid)
        used = set(re.findall(r'\sid="([^"]*)"', body))
        indent = re.search(r'(?m)^([ \t]*)<', body)
        inner = indent.group(1) if indent else "    "
        if b["flow"]:
            entry = b["flow"].group(1)
            ids = [unique(n, used) for n, _ in needs]
            lines = []
            for i, (name, kind) in enumerate(needs):
                nxt = ids[i + 1] if i + 1 < len(ids) else entry
                lines.append('%s  <Test id="%s" %s pass="%s"/>' % (inner, ids[i], kind, nxt))
            body = body.replace('<Flow entry="%s">' % entry, '<Flow entry="%s">\n%s' % (ids[0], "\n".join(lines)), 1)
            edits[bid] = body
            report.append("%s: checks %s at the front of its Flow" % (bid, ", ".join(ids)))
        elif bid in owners:
            for owner in owners[bid]:
                if set(n for n, _ in needs) <= set(n for n, _ in by_id[owner]["needs"]):
                    report.append("%s: same as the front checks of %s's Flow, dropped" % (bid, owner))
                    continue
                obody = body_of(owner)
                ouse = set(re.findall(r'\sid="([^"]*)"', obody))
                for play_id, dest in by_id[owner]["plays"]:
                    if dest != bid:
                        continue
                    ids = [unique(n + "_" + play_id, ouse) for n, _ in needs]
                    # Every route that went to this Play now goes through the checks first.
                    obody = re.sub(r'(pass|fail|entry)="%s"' % re.escape(play_id), r'\1="%s"' % ids[0], obody)
                    pindent = re.search(r'(?m)^([ \t]*)<Play id="%s"' % re.escape(play_id), obody).group(1)
                    lines = [
                        '%s<Test id="%s" %s pass="%s"/>' % (pindent, ids[i], kind, ids[i + 1] if i + 1 < len(ids) else play_id)
                        for i, (_, kind) in enumerate(needs)
                    ]
                    obody = re.sub(r'(?m)^(%s<Play id="%s")' % (re.escape(pindent), re.escape(play_id)),
                                   "\n".join(lines).replace("\\", "\\\\") + r"\n\1", obody, count=1)
                    report.append("%s: checks before it inside %s's Flow" % (bid, owner))
                edits[owner] = obody
        elif "<Start" not in body:
            report.append("%s: reached by transitions only, checks dropped (the action before it checked)" % bid)
        elif re.search(r'<Connect\b[^>]*\bto="%s"[^>]*\bblend=' % re.escape(bid), text):
            report.append("%s: a Connect with its own blend leads to it and it cannot hold a Flow: checks dropped, review it" % bid)
        else:
            ids = [unique(n, used) for n, _ in needs]
            self_id = unique("Self", used)
            lines = ['%s<Flow entry="%s">' % (inner, ids[0])]
            for i, (name, kind) in enumerate(needs):
                nxt = ids[i + 1] if i + 1 < len(ids) else self_id
                lines.append('%s  <Test id="%s" %s pass="%s"/>' % (inner, ids[i], kind, nxt))
            lines.append('%s  <Play id="%s" action="%s"/>' % (inner, self_id, bid))
            lines.append("%s</Flow>" % inner)
            body = body.rstrip(" \t") + "\n".join(lines) + "\n" + re.match(r"[ \t]*", b["m"].group(1)).group(0)
            body = re.sub(r"\n([ \t]*)$", "\n", body)
            edits[bid] = body.rstrip("\n") + "\n"
            report.append("%s: a Flow of its own added with the checks" % bid)
    out, last = [], 0
    for b in blocks:
        m = b["m"]
        if b["id"] not in edits:
            continue
        start, end = m.span(5)
        out.append(text[last:start])
        out.append(edits[b["id"]])
        last = end
    out.append(text[last:])
    return "".join(out), report


def main(args):
    write = "--write" in args
    files = [a for a in args if a != "--write"]
    if not files:
        sys.exit(__doc__)
    for path in files:
        text = io.open(path, encoding="utf-8", newline="").read()
        if "AnimationInjectorEditor" in text and "<!--" in text[text.rfind("</AnimationSet>"):]:
            print("%s: has an editor block: open and save it in Animation Injector Editor" % path)
            continue
        new, report = convert(text)
        print("%s: %s" % (path, "; ".join(report) if report else "nothing to change"))
        if write and new != text:
            shutil.copy2(path, path + ".before-0.12.276")
            io.open(path, "w", encoding="utf-8", newline="").write(new)


if __name__ == "__main__":
    main(sys.argv[1:])
