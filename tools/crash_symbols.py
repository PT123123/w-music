#!/usr/bin/env python3
"""Turn the RVAs from a diag.log AV / CRASH line back into function names.

w-music ships no PDB, so the in-process crash logger (wm::app::InstallCrashLogger)
can only record offsets. tools\\dev-build.ps1 links the app with /MAP, so the
matching build\\w-music.map resolves them:

    python tools/crash_symbols.py                  # fault events in diag.log
    python tools/crash_symbols.py +1a2b3 +2b4c5    # explicit RVAs

Only events whose `ts=` matches the map's link timestamp are resolved: RVAs from
an older build would otherwise come back as confident nonsense.
"""

import argparse
import os
import re
import sys

MAP_LINE = re.compile(r"^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{8,16})\b")
TIMESTAMP = re.compile(r"^\s*Timestamp is ([0-9A-Fa-f]{8})")
LOAD_BASE = re.compile(r"^\s*Preferred load address is ([0-9A-Fa-f]{8,16})")
FAULT_HEAD = re.compile(r"\b(AV|CRASH)\b.*?\bts=([0-9a-fA-F]{8})")
RVA = re.compile(r"\+([0-9a-fA-F]{3,8})\b")


def load_map(path):
    symbols = []
    stamp = None
    base = None
    in_publics = False
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not in_publics:
                match = TIMESTAMP.match(line)
                if match:
                    stamp = match.group(1)
                    continue
                match = LOAD_BASE.match(line)
                if match:
                    base = int(match.group(1), 16)
                    continue
                if "Publics by Value" in line:
                    in_publics = True
                continue
            if line.startswith("  Entry point"):
                break
            match = MAP_LINE.match(line)
            if match:
                symbols.append((int(match.group(2), 16) - (base or 0), match.group(1)))
    symbols.sort()
    return stamp, symbols


def nearest(symbols, rva):
    low, high = 0, len(symbols) - 1
    best = None
    while low <= high:
        mid = (low + high) // 2
        if symbols[mid][0] <= rva:
            best = symbols[mid]
            low = mid + 1
        else:
            high = mid - 1
    return best


def fault_events(text, stamp):
    """Pair each '<tag> code=… ts=…' header with the '<tag> stack +a +b …' line.

    Returns (events, skipped): events are (header, [rva]) whose ts matches the
    map, skipped counts the ones from other builds.
    """
    events = []
    skipped = 0
    pending = None
    for line in text.splitlines():
        head = FAULT_HEAD.search(line)
        if head:
            if stamp is None or head.group(2).lower() == stamp.lower():
                pending = (line.strip(), [])
                events.append(pending)
            else:
                if pending is not None and not pending[1]:
                    events.remove(pending)
                pending = None
                skipped += 1
            continue
        if " stack " in line and pending is not None:
            pending = (pending[0], [int(v, 16) for v in RVA.findall(line)])
            events[-1] = pending
            pending = None
    return events, skipped


def main():
    local = os.path.join(os.environ.get("LOCALAPPDATA", ""), "w-music", "diag.log")
    parser = argparse.ArgumentParser()
    parser.add_argument("rvas", nargs="*", help="RVA written as +<hex>, or a diag.log path")
    parser.add_argument("--map", default=os.path.join("build", "w-music.map"))
    parser.add_argument("--diag", default=local if os.path.exists(local) else None)
    args = parser.parse_args()

    if not os.path.exists(args.map):
        sys.exit("no map file at %s -- run 'just build' (link uses /MAP)" % args.map)
    stamp, symbols = load_map(args.map)
    if not symbols:
        sys.exit("no public symbols parsed from %s" % args.map)
    print("map %s: %d symbols, timestamp %s" % (args.map, len(symbols), stamp))

    explicit = [int(value, 16) for value in args.rvas if value.startswith("+")]
    paths = [value for value in args.rvas if not value.startswith("+")]
    diag_path = paths[0] if paths else args.diag

    if explicit:
        for rva in explicit:
            report(rva, symbols)
        return

    if not diag_path or not os.path.exists(diag_path):
        sys.exit("nothing to resolve: pass +<rva> values or a diag.log path")
    with open(diag_path, "r", encoding="utf-8", errors="replace") as handle:
        events, skipped = fault_events(handle.read(), stamp)
    if not events:
        sys.exit("no fault lines for this build in %s%s"
                 % (diag_path,
                    " (%d from other builds skipped)" % skipped if skipped else ""))
    for header, rvas in events:
        print("\n%s" % header)
        if not rvas:
            print("  (no stack line recorded)")
        for rva in rvas:
            report(rva, symbols)
    if skipped:
        print("\n%d fault event(s) from other builds skipped (map ts=%s)" % (skipped, stamp))


def report(rva, symbols):
    found = nearest(symbols, rva)
    if found is None:
        print("+%x  <below the first symbol>" % rva)
    else:
        print("+%x  %s+0x%x" % (rva, found[1], rva - found[0]))


if __name__ == "__main__":
    main()
