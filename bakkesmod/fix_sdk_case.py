"""The BakkesMod SDK is written on Windows, where file names ignore case. Add symlinks so every
#include "..." spelling in the SDK and the plugin resolves on Linux.  Usage: fix_sdk_case.py SDK_INCLUDE_DIR [SRC_DIR...]"""
import os
import re
import sys

INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def find_ci(base, rel):
    """Real path of base/rel matched case-insensitively, or None."""
    cur = base
    for part in rel.replace("\\", "/").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            cur = os.path.dirname(cur)
            continue
        if not os.path.isdir(cur):
            return None
        hit = next((e for e in os.listdir(cur) if e.lower() == part.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def main():
    root = os.path.abspath(sys.argv[1])
    sources = [root] + [os.path.abspath(p) for p in sys.argv[2:]]
    made = 0
    for _ in range(5):                      # repeat: a new link can expose more nested includes
        new = 0
        for src in sources:
            for dirpath, _, files in os.walk(src):
                for f in files:
                    if not f.lower().endswith((".h", ".hpp", ".cpp", ".inl")):
                        continue
                    text = open(os.path.join(dirpath, f), errors="ignore").read()
                    for inc in INCLUDE.findall(text):
                        for base in (dirpath, root):
                            want = os.path.join(base, inc)
                            if os.path.exists(want):
                                break
                            real = find_ci(base, inc)
                            if real and os.path.normcase(real) != os.path.normcase(want):
                                os.makedirs(os.path.dirname(want), exist_ok=True)
                                os.symlink(os.path.relpath(real, os.path.dirname(want)), want)
                                new += 1
                                break
        made += new
        if not new:
            break
    print(f"{made} liens de casse ajoutés")


if __name__ == "__main__":
    main()
