"""The phone's Python (res/zydek/python/zydek_web.py) against cases.json: the same answers as the C++.

    python3 -I test_python.py [cases.json]

Only the functions under test are taken from zydek_web.py (it starts yt-dlp and the rest on import)."""
import ast
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "res", "zydek", "python", "zydek_web.py")


def load():
    ns = {"re": re}
    tree = ast.parse(open(SOURCE, encoding="utf-8").read())
    for node in tree.body:
        names = [getattr(t, "id", "") for t in getattr(node, "targets", [])]
        if (isinstance(node, ast.Assign) and {"_JUNK", "_RESERVED"} & set(names)) or (
                isinstance(node, ast.FunctionDef) and node.name in ("safe_filename", "guess_artist_title")):
            exec(compile(ast.Module([node], []), SOURCE, "exec"), ns)
    return ns


def main():
    cases = json.load(open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "cases.json"), encoding="utf-8"))
    ns = load()
    wrong = 0
    for given, want in cases["fileNames"]:
        got = ns["safe_filename"](given)
        if got != want:
            wrong += 1
            print(f"Python WRONG: safe_filename({given!r}) = {got!r}, not {want!r}")
    for info, want in cases["artistTitle"]:
        got = list(ns["guess_artist_title"](info))
        if got != want:
            wrong += 1
            print(f"Python WRONG: guess_artist_title({info!r}) = {got!r}, not {want!r}")
    print("Python:", "WRONG" if wrong else "all right")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
