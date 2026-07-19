#!/usr/bin/env python3
import pathlib
import sys


DANGEROUS = {
    "\ufeff": "BOM",
    "\u200e": "LEFT-TO-RIGHT MARK",
    "\u200f": "RIGHT-TO-LEFT MARK",
    "\u202a": "LEFT-TO-RIGHT EMBEDDING",
    "\u202b": "RIGHT-TO-LEFT EMBEDDING",
    "\u202c": "POP DIRECTIONAL FORMATTING",
    "\u202d": "LEFT-TO-RIGHT OVERRIDE",
    "\u202e": "RIGHT-TO-LEFT OVERRIDE",
    "\u2066": "LEFT-TO-RIGHT ISOLATE",
    "\u2067": "RIGHT-TO-LEFT ISOLATE",
    "\u2068": "FIRST STRONG ISOLATE",
    "\u2069": "POP DIRECTIONAL ISOLATE",
}


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check-dangerous-unicode.py FILE...", file=sys.stderr)
        return 2
    failures = 0
    for name in sys.argv[1:]:
        path = pathlib.Path(name)
        text = path.read_text(encoding="utf-8")
        for line_no, line in enumerate(text.splitlines(), 1):
            for col_no, ch in enumerate(line, 1):
                if ch in DANGEROUS:
                    failures += 1
                    print(
                        f"{path}:{line_no}:{col_no}: U+{ord(ch):04X} {DANGEROUS[ch]}",
                        file=sys.stderr,
                    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
