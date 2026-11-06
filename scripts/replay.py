#!/usr/bin/env python3
"""Plays a recorded run again (DM_RECORD sessions, dm/host/hle_session.c).

A session folder holds the save as the game mounted it, player 1's input in presented frames (input.keys) and the
host's arguments and DM_* settings (meta.tsv). This starts the same host with the same settings on a fresh copy of
that save, with the input as a DM_KEYS script. The keyboard and controllers still work during the replay: press
anything to take over, or stop the script early with --until.

  scripts/replay.py <session or recordings folder>          the newest session when given the folder
  scripts/replay.py <recordings folder> --list              the sessions there
  scripts/replay.py <session> --until 5400                  input up to frame 5400, then the player's

The replay runs on a copy in <session>/replay/, so the session itself stays as it was and can be played again.
Replays are approximate with the real clock: a load that takes a different number of frames than it did in the
recorded run moves the input after it (docs/development.md, Recording and replaying a session).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

STAMP = re.compile(r"^\d{8}-\d{6}(-\d+)?$")


def die(msg):
    print(f"replay: {msg}", file=sys.stderr)
    sys.exit(1)


def read_meta(session):
    meta = {"arg": [], "env": []}
    with open(os.path.join(session, "meta.tsv"), encoding="utf-8") as f:
        for line in f:
            k, _, v = line.rstrip("\n").partition("\t")
            if k in meta and isinstance(meta[k], list):
                meta[k].append(v)
            else:
                meta[k] = v
    return meta


def sessions(folder):
    return sorted(os.path.join(folder, n) for n in os.listdir(folder)
                  if STAMP.match(n) and os.path.isfile(os.path.join(folder, n, "meta.tsv")))


def pick(path):
    if os.path.isfile(os.path.join(path, "meta.tsv")):
        return path
    found = sessions(path) if os.path.isdir(path) else []
    if not found:
        die(f"no recorded session in {path}")
    return found[-1]


def describe(session):
    m = read_meta(session)
    keys = os.path.join(session, "input.keys")
    steps = sum(1 for _ in open(keys, encoding="utf-8")) if os.path.isfile(keys) else 0
    end = f"{m['frames']} frames, {m['ended']}" if "ended" in m else "no clean exit (crash or kill)"
    return f"{os.path.basename(session)}  {m.get('started', '?')}  {steps} input steps  {end}"


def until(src, dst, frame):
    """The steps that begin before frame, the last one cut to end there."""
    step = re.compile(r"^f(\d+):(.*)~f(\d+),?$")
    with open(src, encoding="utf-8") as f, open(dst, "w", encoding="utf-8") as out:
        for line in f:
            m = step.match(line.strip())
            if not m:
                continue
            at, keys, n = int(m[1]), m[2], int(m[3])
            if at >= frame:
                break
            out.write(f"f{at}:{keys}~f{min(n, frame - at)},\n")


def fresh_copy(src, dst):
    if os.path.exists(dst):
        shutil.rmtree(dst)
    if os.path.isdir(src):
        # cp -c clones on APFS (no space used); a copy where it cannot
        if subprocess.run(["cp", "-c", "-R", src, dst], capture_output=True).returncode:
            shutil.copytree(src, dst, symlinks=True)
    else:
        os.makedirs(dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("path", help="a session folder, or a recordings folder (DM_RECORD) for its newest session")
    ap.add_argument("--list", action="store_true", help="list the sessions in the recordings folder")
    ap.add_argument("--until", type=int, metavar="FRAME", help="stop the recorded input at this presented frame")
    ap.add_argument("--no-input", action="store_true", help="only start from the session's save, without its input")
    ap.add_argument("-e", "--env", action="append", default=[], metavar="KEY=VALUE", help="a setting for this replay")
    a = ap.parse_args()

    path = os.path.abspath(os.path.expanduser(a.path))
    if a.list:
        for s in sessions(path):
            print(describe(s))
        return
    session = pick(path)
    m = read_meta(session)
    if not m["arg"]:
        die(f"{session}/meta.tsv names no host")
    print(f"replay: {describe(session)}", flush=True)

    work = os.path.join(session, "replay")
    os.makedirs(work, exist_ok=True)
    fresh_copy(os.path.join(session, "save"), os.path.join(work, "save"))
    if os.path.isdir(os.path.join(session, "save_device")):
        fresh_copy(os.path.join(session, "save_device"), os.path.join(work, "save_device"))
    for leftover in ("save.work", "save.new", "save_device.work", "save_device.new"):
        shutil.rmtree(os.path.join(work, leftover), ignore_errors=True)

    env = {k: v for k, v in os.environ.items() if not k.startswith("DM_")}
    for kv in m["env"]:
        k, _, v = kv.partition("=")
        env[k] = v
    for k in ("DM_REC", "DM_RECORD", "DM_KEYS", "DM_CTL"):
        env.pop(k, None)
    env["DM_SAVE"] = os.path.join(work, "save")
    env["DM_SAVE_SYNC"] = "0"   # the cloud's save would replace the session's
    keys = os.path.join(session, "input.keys")
    if not a.no_input and os.path.isfile(keys):
        if a.until is not None:
            cut = os.path.join(work, "input.keys")
            until(keys, cut, a.until)
            keys = cut
        env["DM_KEYS"] = "@" + keys
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v

    cwd = m.get("cwd") or session
    print(f"replay: {m['arg'][0]} with the save copy in {work}" + (", input " + env["DM_KEYS"][1:] if "DM_KEYS" in env else ", no input"), flush=True)
    sys.exit(subprocess.run(m["arg"], cwd=cwd, env=env).returncode)


if __name__ == "__main__":
    main()
