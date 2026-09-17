#!/usr/bin/env python3
"""Convert calls of gnuworld's network functions from printf style to std::format style.

    Notice(theClient, "%s is not on %s (%d users)", nick.c_str(), chan.c_str(), n);
becomes
    Notice(theClient, "{} is not on {} ({} users)", nick, chan, n);

The network functions (Notice, Message, Write, ...) took "const char*, ..." and
now take a gnuworld::CheckedFormat. This rewrites the format string and drops
the .c_str() that printf needed. A call it leaves behind does not compile, so
nothing is converted silently wrong for lack of being found.

What it cannot know is the type of an argument, and std::format prints two
types differently from printf's %d: a bool comes out as "true"/"false" and a
char as the character. Every %d-like conversion is therefore listed for
review; cast such an argument to int if the number is what is wanted.

Usage: printf2format.py [--check] FILE...      (--check reports, changes nothing)
"""

import re
import sys

FUNCS = (
    "Write|WriteDuringBurst|WriteWithTime|Notice|Message|NoticeChannelOps|"
    "Wallops|WallopsAsServer|serverNotice"
)
CALL = re.compile(r"\b(" + FUNCS + r")\s*\(")
CONVERSION = re.compile(r"%(?P<flags>[-+ #0]*)(?P<width>\d*)(?:\.(?P<prec>\d+))?"
                        r"(?:hh|h|ll|l|z|j|t)?(?P<conv>[sdiuxXcfg%])")
INTEGER = "diu"


def convert_format(literal: str) -> tuple[str, list[str]]:
    """The body of a string literal, printf to std::format. Returns (text, conversions)."""
    seen: list[str] = []
    out: list[str] = []
    pos = 0
    for m in CONVERSION.finditer(literal):
        out.append(literal[pos:m.start()].replace("{", "{{").replace("}", "}}"))
        pos = m.end()
        conv = m.group("conv")
        if conv == "%":
            out.append("%")
            continue
        seen.append(m.group(0))
        flags, width, prec = m.group("flags"), m.group("width"), m.group("prec")

        spec = ""
        if "-" in flags:
            spec += "<"
        elif width and conv in "sc":
            spec += ">"  # printf right-aligns a padded string; std::format left-aligns it
        if "+" in flags:
            spec += "+"
        if "#" in flags:
            spec += "#"
        if "0" in flags and "-" not in flags:
            spec += "0"
        spec += width
        if prec:
            spec += "." + prec
        if conv in "xXfg":
            spec += conv
        out.append("{" + (":" + spec if spec else "") + "}")
    out.append(literal[pos:].replace("{", "{{").replace("}", "}}"))
    return "".join(out), seen


def split_call(text: str, open_paren: int) -> tuple[int, list[tuple[int, int]]]:
    """(index past the closing paren, [(start, end) of each top-level argument])."""
    depth, i, in_str, in_chr, esc = 0, open_paren, False, False, False
    args, start = [], open_paren + 1
    while i < len(text):
        ch = text[i]
        if in_str or in_chr:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif (in_str and ch == '"') or (in_chr and ch == "'"):
                in_str = in_chr = False
        elif ch == '"':
            in_str = True
        elif ch == "'":
            in_chr = True
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append((start, i))
                return i + 1, args
        elif ch == "," and depth == 1:
            args.append((start, i))
            start = i + 1
        i += 1
    raise ValueError("unbalanced call")


LITERALS_ONLY = re.compile(r'\s*(?:"(?:[^"\\]|\\.)*"\s*)+')
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
C_STR = re.compile(r"\s*\.\s*c_str\s*\(\s*\)\s*$")


def convert(text: str, path: str) -> tuple[str, int, list[str]]:
    edits: list[tuple[int, int, str]] = []
    review: list[str] = []
    converted = 0
    for m in CALL.finditer(text):
        try:
            _end, args = split_call(text, m.end() - 1)
        except ValueError:
            continue
        # The format is the first argument that is nothing but string literals
        fmt_index = next((k for k, (a, b) in enumerate(args)
                          if LITERALS_ONLY.fullmatch(text[a:b])), None)
        if fmt_index is None:
            continue
        a, b = args[fmt_index]
        pieces = LITERAL.findall(text[a:b])
        if not CONVERSION.search("".join(pieces)):
            continue
        has_args = fmt_index + 1 < len(args)
        if not has_args and not any("%%" in p for p in pieces):
            continue

        seen_all: list[str] = []

        def repl(lm: re.Match) -> str:
            new, seen = convert_format(lm.group(1))
            seen_all.extend(seen)
            return '"' + new + '"'

        edits.append((a, b, LITERAL.sub(repl, text[a:b])))
        for (x, y) in args[fmt_index + 1:]:
            stripped = C_STR.sub("", text[x:y])
            if stripped != text[x:y]:
                edits.append((x, y, stripped))
        converted += 1
        line = text.count("\n", 0, m.start()) + 1
        for k, conv in enumerate(seen_all):
            if conv[-1] in INTEGER and fmt_index + 1 + k < len(args):
                x, y = args[fmt_index + 1 + k]
                review.append(f"{path}:{line}: {conv} <- {' '.join(text[x:y].split())}")

    for a, b, new in sorted(edits, reverse=True):
        text = text[:a] + new + text[b:]
    return text, converted, review


def main(argv: list[str]) -> int:
    check = "--check" in argv
    paths = [p for p in argv if not p.startswith("--")]
    if not paths:
        print(__doc__)
        return 2
    total = 0
    for path in paths:
        original = open(path, encoding="utf-8").read()
        new, n, review = convert(original, path)
        total += n
        if n:
            print(f"{n:4d}  {path}")
        for note in review:
            print("      review " + note)
        if n and not check:
            open(path, "w", encoding="utf-8").write(new)
    print(f"{total:4d}  calls {'would be ' if check else ''}converted")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
