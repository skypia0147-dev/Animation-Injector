"""Scale test declarations: clone real AnimationSet files until there are N actions.

Each clone renames every action id (and every attribute that refers to one), its start events,
its registered events (EventRegistry), its animation set id and its clip paths, so the result behaves like many independent mods:
distinct actions, events and HKX files. Keys (an <Input>, or written where they are used: Start, Connect, Carry)
are moved to ScrollLock so a stress install never answers ordinary keys.

    python tools/scale/generate_stress.py --out <dir> --actions 5000 [--seed file:weight ...]
    python tools/scale/generate_stress.py --out <dir> --hkx-root <mod>/meshes/actors/character --link-from <file.hkx>

--hkx-root with --link-from also creates every referenced HKX as a hard link to copies of one real file,
for an in-game run (hard links take no disk space). Output: <out>/<SetId>/<SetId>.xml, the
folder layout Data/SKSE/Plugins/AnimationInjector expects.
"""
import argparse
import os
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SEEDS = [
    # Weights give a mix close to a large real load order: mostly single actions, some pairs and carries.
    (ROOT / "tests/data/dynamic-dodge-shot.xml", 8),
    (ROOT / "ports/CleanUpOutfit/SKSE/Plugins/AnimationInjector/CleanUpOutfit/CleanUpOutfit.xml", 6),
    (ROOT / "tests/data/grapple.xml", 1),
    (ROOT / "tests/data/carry/putdown.xml", 1),
]
TOP = {"Action", "Pair"}
KEYED = {"Input", "Start", "Connect", "Carry"}
EVENT_ATTRS = {("Start", "event")}


def action_ids(root):
    return {e.get("id") for e in root if e.tag in TOP and e.get("id")}


def clone(root, k, tag):
    root = ET.fromstring(ET.tostring(root))
    ids = action_ids(root)
    suffix = f"_{tag}{k:05d}"
    root.set("id", root.get("id", "Set") + suffix)
    for e in root.iter():
        for name, value in list(e.attrib.items()):
            if value in ids:
                e.set(name, value + suffix)
            elif (e.tag, name) in EVENT_ATTRS:
                e.set(name, value + suffix)
            elif e.tag == "Event" and name == "name":
                # Registered events per clone too: the global registry grows with the load order.
                e.set(name, value + suffix)
            elif value.lower().endswith(".hkx") and value.lower().startswith("animations\\"):
                e.set(name, f"Animations\\{tag}\\{k:05d}\\" + value[len("animations\\"):])
        if e.tag in KEYED and e.get("key"):
            e.set("key", "ScrollLock")
        if e.tag in KEYED and e.get("gamepad"):
            del e.attrib["gamepad"]
    return root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--actions", type=int, default=5000)
    ap.add_argument("--tag", default="Stress")
    ap.add_argument("--seed", action="append", help="file:weight")
    ap.add_argument("--hkx-root")
    ap.add_argument("--link-from")
    args = ap.parse_args()
    seeds = [(Path(s.rsplit(":", 1)[0]), int(s.rsplit(":", 1)[1])) for s in args.seed] if args.seed else DEFAULT_SEEDS
    trees = [(ET.parse(p).getroot(), w) for p, w in seeds]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    total, k, clips = 0, 0, set()
    while total < args.actions:
        for root, weight in trees:
            for _ in range(weight):
                if total >= args.actions:
                    break
                c = clone(root, k, args.tag)
                k += 1
                total += len(action_ids(c))
                folder = out / c.get("id")
                folder.mkdir(exist_ok=True)
                ET.ElementTree(c).write(folder / (c.get("id") + ".xml"), encoding="utf-8", xml_declaration=True)
                for e in c.iter():
                    for v in e.attrib.values():
                        if v.lower().endswith(".hkx"):
                            clips.add(v)
    print(f"sets={k} actions={total} clips={len(clips)}")
    if args.hkx_root and args.link_from:
        # NTFS allows 1023 links per file: link from a fresh copy every 1000 links. The copies live
        # beside the output (<out>/../_hkxsource), outside the folders the game reads.
        base, made = Path(args.hkx_root), 0
        sources = out.parent / "_hkxsource"
        sources.mkdir(parents=True, exist_ok=True)
        # A regenerated set keeps the sources an earlier run filled; new links start a new source.
        first = len(list(sources.glob("source*.hkx")))
        src = None
        for clip in sorted(clips):
            dst = base / clip.replace("\\", "/")
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.exists():
                continue
            if made % 1000 == 0:
                src = sources / f"source{first + made // 1000:03d}.hkx"
                shutil.copyfile(args.link_from, src)
            os.link(src, dst)
            made += 1
        print(f"hkx hard links={made}")


if __name__ == "__main__":
    sys.exit(main())
