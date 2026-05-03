"""Fix UTF-8 / Windows-1251 mojibake in project source (comments and strings)."""
import sys
from pathlib import Path

try:
    import ftfy
except ImportError:
    print("Run: py -3 -m pip install ftfy", file=sys.stderr)
    raise

# UTF-8 for "И" was split into broken sequence (see main.cpp line 2).
BROKEN_I = b"\xd0\xa0\xc2\x98"
FIXED_I = b"\xd0\x98"

# "Р" + "ё" mojibake for "и" (after partial ftfy)
RYO = "\u0420\u0451"


def fix_text_content(text: str) -> str:
    lines_out = []
    for line in text.split("\n"):
        pure = line.rstrip("\r")
        trailing_r = "\r" if line.endswith("\r") else ""
        try:
            fixed = pure.encode("cp1251").decode("utf-8")
        except Exception:
            fixed = ftfy.fix_text(pure)
        while RYO in fixed:
            fixed = fixed.replace(
                RYO, RYO.encode("cp1251").decode("utf-8")
            )
        lines_out.append(fixed + trailing_r)
    return "\n".join(lines_out)


def fix_file_bytes(raw: bytes) -> bytes:
    bom = raw.startswith(b"\xef\xbb\xbf")
    b = raw[3:] if bom else raw
    b = b.replace(BROKEN_I, FIXED_I)
    text = b.decode("utf-8")
    out = fix_text_content(text)
    out_bytes = out.encode("utf-8")
    return (b"\xef\xbb\xbf" + out_bytes) if bom else out_bytes


def main() -> int:
    root = Path(__file__).resolve().parents[1] / "source"
    files = sorted(set(root.glob("*.cpp")) | set(root.glob("*.h")))
    if not files:
        print("No files matched.", file=sys.stderr)
        return 1
    for path in files:
        raw = path.read_bytes()
        new_raw = fix_file_bytes(raw)
        if new_raw != raw:
            path.write_bytes(new_raw)
            print("fixed", path.relative_to(root.parent))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
