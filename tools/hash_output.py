"""Fingerprints the recompiler output so different machines can confirm they generated identical code.

Usage: python tools/hash_output.py [output-dir]   (default: ./output)
Prints the file count and one SHA-256 over every file name and its contents, in sorted order.
Line endings are normalised so a checkout with different newline settings still matches.
"""
import hashlib
import os
import sys


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else "output"
    if not os.path.isdir(root):
        print(f"no such directory: {root}", file=sys.stderr)
        return 1

    digest = hashlib.sha256()
    names = sorted(os.listdir(root))
    for name in names:
        with open(os.path.join(root, name), "rb") as handle:
            data = handle.read().replace(b"\r\n", b"\n")
        digest.update(name.encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(data).digest())

    print(f"files:  {len(names)}")
    print(f"sha256: {digest.hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
