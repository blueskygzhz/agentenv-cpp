#!/usr/bin/env python3
"""Checks `docs/RUST_TO_CPP_MAPPING.md` against both trees it claims to relate.

That table is the port's coverage evidence -- it is what a reader consults to
answer "was this module ported?". A wrong row is worse than a missing one. A
missing row prompts a search; a row pointing at `src/snapshot/manager.cc`
asserts the module was ported to a specific place, and the natural next step --
open it -- is the only thing that disproves it. Reviewing coverage from this
table meant building on claims that had quietly stopped being true as the trees
moved.

Three checks, because each caught a different class of rot:

1. Every concrete C++ path exists. Twelve of 131 did not: `src/snapshot/
   manager.cc`, `include/agentenv/image/image.h`, `src/p2p/all.cc` and others --
   modules deliberately inlined elsewhere, split into directories, or missing a
   path prefix.

2. Every Rust path resolves in the upstream tree. Checking only the C++ column
   left the left-hand side unguarded, and it had drifted too:
   `src/snapshot/repository/*.rs` named a directory upstream does not have (the
   repository layer lives at `src/repository/`). A row whose *source* is fiction
   cannot be evidence of anything.

3. A row annotated `(N files)` expands to exactly N. The same row claimed 22
   files where the glob matches 18 -- the kind of error that makes a reader
   believe four modules were accounted for when nothing names them.

The reverse direction -- every upstream `.rs` is covered by some row -- is
reported but not enforced; see `UNCOVERED_IS_FATAL`.

Run from CI or by hand:

    python3 tests/check_mapping_doc.py

The upstream tree is found at `../AgentENV` or `$AGENTENV_RUST_ROOT`. Without
it, checks 2 and 3 are skipped rather than silently passed.
"""

import itertools
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOC = ROOT / "docs/RUST_TO_CPP_MAPPING.md"

RUST_ROOT = pathlib.Path(
    os.environ.get("AGENTENV_RUST_ROOT", ROOT.parent / "AgentENV")
)

# Prefixes that denote a path in *this* repository. Anything else in backticks
# is Rust source, a type name or prose.
CPP_PREFIXES = ("include/", "src/", "storage/", "tests/", "crates/")

# A Rust claim is only checkable if it is rooted in the upstream crate. Prose
# rows refer to neighbours in shorthand ("1:1 from `orchestrator/types.rs` +
# `mod.rs`"), which names no single file and is not a coverage claim.
RUST_PREFIXES = ("src/", "crates/")

# Upstream files no row is expected to name: the crate's own entry points and
# build plumbing, which the port restructures rather than mirrors.
UNCOVERED_EXEMPT = ("build.rs",)

# The reverse check is advisory. Enforcing it would require a row for every
# upstream test module and helper, which the table does not try to be.
UNCOVERED_IS_FATAL = False


def rows():
    """The table's body lines, with line numbers."""
    for number, line in enumerate(DOC.read_text().splitlines(), 1):
        if line.strip().startswith("|"):
            yield number, line


def backticked(line):
    return re.findall(r"`([^`]+)`", line)


def claimed_cpp_paths():
    """Every concrete C++ path the table asserts, with its line number.

    Skipped, deliberately:

    - globs and brace expansions (`src/p2p/*.cc`): the table uses them to mean
      "a set of files", and checking them would need a glob semantics this does
      not have. The rows that matter most are the specific ones anyway.
    - `...` elisions, which are the document's own shorthand for a long path.
    - `path.h:12-34` line references, which point *into* a file rather than at
      one; the file part is checked by whatever row names it plainly.
    - `.rs` paths, which live in the upstream tree and are checked against it.
    """
    for number, line in rows():
        for path in backticked(line):
            if not path.startswith(CPP_PREFIXES):
                continue
            if any(ch in path for ch in "*{}") or "..." in path:
                continue
            if ":" in path or path.endswith(".rs"):
                continue
            yield number, path


def expand_braces(pattern):
    """`a/{x,y}.rs` -> `a/x.rs`, `a/y.rs`, for any number of groups."""
    groups = re.findall(r"\{([^{}]*)\}", pattern)
    if not groups:
        return [pattern]
    skeleton = re.sub(r"\{[^{}]*\}", "{}", pattern)
    return [
        skeleton.format(*choice)
        for choice in itertools.product(*(g.split(",") for g in groups))
    ]


def resolve_rust(pattern):
    """The upstream files a Rust-column pattern names.

    `*` is matched recursively: the table writes `src/repository/*.rs` for a
    subtree, and annotates it with the count it expects to cover.
    """
    found = []
    for alternative in expand_braces(pattern):
        if "*" not in alternative:
            if (RUST_ROOT / alternative).exists():
                found.append(alternative)
            continue
        head, _, tail = alternative.partition("*")
        base = RUST_ROOT / head
        if not base.is_dir():
            continue
        found.extend(
            str(p.relative_to(RUST_ROOT))
            for p in base.rglob("*" + tail)
            if p.is_file()
        )
    return sorted(set(found))


def claimed_rust_patterns():
    """Rust-column patterns, with line number and any `(N files)` annotation."""
    for number, line in rows():
        count = re.search(r"\((\d+)\s+files?\)", line)
        expected = int(count.group(1)) if count else None
        patterns = [
            p for p in backticked(line)
            if p.endswith(".rs") and p.startswith(RUST_PREFIXES)
        ]
        # The annotation belongs to the row's source column, so it only means
        # something when exactly one pattern could be the thing being counted.
        for pattern in patterns:
            if "..." in pattern or ":" in pattern:
                continue
            yield number, pattern, expected if len(patterns) == 1 else None


def check_cpp():
    claims = list(claimed_cpp_paths())
    missing = [(n, p) for n, p in claims if not (ROOT / p).exists()]

    print(f"checked {len(claims)} concrete C++ paths in {DOC.name}")
    if not missing:
        return 0

    print(f"\n{len(missing)} claimed C++ path(s) do not exist:")
    for number, path in missing:
        print(f"  {DOC.name}:{number}  {path}")
    print("\nFix the row rather than creating the file: the usual cause is a "
          "module that was inlined, split into a directory, or renamed.")
    return 1


def check_rust():
    patterns = list(claimed_rust_patterns())
    unresolved = []
    miscounted = []
    covered = set()

    for number, pattern, expected in patterns:
        found = resolve_rust(pattern)
        if not found:
            unresolved.append((number, pattern))
            continue
        covered.update(found)
        if expected is not None and len(found) != expected:
            miscounted.append((number, pattern, expected, len(found)))

    print(f"checked {len(patterns)} Rust patterns against {RUST_ROOT}")

    status = 0
    if unresolved:
        print(f"\n{len(unresolved)} Rust pattern(s) match nothing upstream:")
        for number, pattern in unresolved:
            print(f"  {DOC.name}:{number}  {pattern}")
        print("\nThe source column is the claim's premise; a path that does "
              "not exist upstream makes the row unverifiable.")
        status = 1

    if miscounted:
        print(f"\n{len(miscounted)} row(s) annotate a count that does not match:")
        for number, pattern, expected, actual in miscounted:
            print(f"  {DOC.name}:{number}  {pattern}"
                  f"  claims {expected} files, matches {actual}")
        print("\nAn inflated count reads as coverage for modules nothing names.")
        status = 1

    all_rs = {
        str(p.relative_to(RUST_ROOT))
        for p in (RUST_ROOT / "src").rglob("*.rs")
        if p.is_file()
    }
    uncovered = sorted(all_rs - covered - set(UNCOVERED_EXEMPT))
    if uncovered:
        label = "not covered by any row" if UNCOVERED_IS_FATAL else \
                "not covered by any row (advisory)"
        print(f"\n{len(uncovered)} upstream file(s) {label}:")
        for path in uncovered:
            print(f"  {path}")
        if UNCOVERED_IS_FATAL:
            status = 1

    return status


def main():
    status = check_cpp()

    if not RUST_ROOT.is_dir():
        print(f"\nskipped Rust-side checks: {RUST_ROOT} is not a directory")
        print("set AGENTENV_RUST_ROOT to the upstream tree to enable them")
        return status

    return check_rust() | status


if __name__ == "__main__":
    sys.exit(main())
