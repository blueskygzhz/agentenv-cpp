#!/usr/bin/env python3
"""Mutation harness for the unified `OverlaybdLayerStore` boundary.

The defect being guarded here was structural: the trait was declared twice,
so the image cache's implementation could not be handed to the snapshot-side
`RuntimeImageMaterializer` at all. A type-level fix cannot be mutation-tested
directly -- re-introducing the duplicate declaration makes the code *fail to
compile*, which is the defect, not a surviving mutant. So the mutations below
target the **behaviour that the unification made reachable**: the `file=` vs
`dir=` decision travelling from `ImageCacheService::LayerLocation` into the
overlaybd `image.json` a snapshot launches from.

Status is decided **by exit code only**: the suite calls `exit(1)` on the
first failing assertion, so its summary line is not printed on failure and
scraping stdout for "failed" would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNTIME_SUPPORT = ROOT / "src/snapshot/runtime_support.cc"
CACHE_SERVICE = ROOT / "src/image/cache_service.cc"
TARGET = "test_image_cache_service"

ATTACH = """    if (!lower->file.empty() || !lower->dir.empty() || lower->digest.empty()) return;
    const image::cache::OverlaybdLayerLocation loc =
        store->LayerLocation(lower->digest, lower->size, has_remote);
    if (loc.kind == image::cache::OverlaybdLayerLocation::Kind::LocalFile) {
        lower->file = loc.path;
    } else {
        lower->dir = loc.path;
    }
"""

MUTATIONS = [
    # The store is never consulted, so no lower gets a placement at all. This
    # is what the whole boundary exists to do.
    ("store_never_consulted", RUNTIME_SUPPORT, ATTACH, "    (void)store; (void)has_remote;\n"),
    # The decision is inverted: a remote-recoverable layer gets pinned with
    # `file=` and a local-only one gets `dir=`. The first makes a cached
    # commit un-reclaimable; the second points overlaybd at a directory for a
    # layer nothing else can recover.
    (
        "file_and_dir_swapped",
        RUNTIME_SUPPORT,
        "    if (loc.kind == image::cache::OverlaybdLayerLocation::Kind::LocalFile) {\n"
        "        lower->file = loc.path;\n"
        "    } else {\n"
        "        lower->dir = loc.path;\n"
        "    }\n",
        "    if (loc.kind == image::cache::OverlaybdLayerLocation::Kind::LocalFile) {\n"
        "        lower->dir = loc.path;\n"
        "    } else {\n"
        "        lower->file = loc.path;\n"
        "    }\n",
    ),
    # `has_remote` is dropped, so every layer looks local-only and a
    # registry-recoverable commit is pinned with `file=`.
    (
        "has_remote_ignored",
        RUNTIME_SUPPORT,
        "        store->LayerLocation(lower->digest, lower->size, has_remote);\n",
        "        store->LayerLocation(lower->digest, lower->size, false);\n",
    ),
    # The service stops honouring the registry fallback, which is the same
    # downgrade one layer lower -- proving the test reaches through the
    # adapter in `cache_store.cc` rather than stopping at the materializer.
    (
        "service_ignores_remote_recoverability",
        CACHE_SERVICE,
        "    out.kind = OverlaybdLayerLocation::Kind::CacheDir;\n",
        "    out.kind = OverlaybdLayerLocation::Kind::LocalFile;\n",
    ),
]


def build():
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", TARGET],
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run():
    return subprocess.run([str(ROOT / "build/bin" / TARGET)],
                          cwd=ROOT, capture_output=True, text=True).returncode


def main():
    sources = {RUNTIME_SUPPORT, CACHE_SERVICE}
    originals = {p: p.read_text() for p in sources}
    backups = {p: p.with_suffix(p.suffix + ".mutation-backup") for p in sources}
    for p, b in backups.items():
        shutil.copy(p, b)

    if not build():
        print("BASELINE BUILD FAILED")
        return 2
    if run() != 0:
        print("BASELINE TESTS FAILED")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken = [], [], []
    try:
        for name, path, old, new in MUTATIONS:
            for p in sources:
                p.write_text(originals[p])
            text = path.read_text()
            assert text.count(old) == 1, \
                f"anchor for {name} matched {text.count(old)} times in {path.name}"
            path.write_text(text.replace(old, new, 1))

            if not build():
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue
            if run() != 0:
                caught.append(name)
                print(f"  caught   {name}")
            else:
                survived.append(name)
                print(f"  SURVIVED {name}")
    finally:
        for p, b in backups.items():
            shutil.copy(b, p)
            b.unlink()
        build()

    print(f"\n{len(caught)}/{len(MUTATIONS)} caught, "
          f"{len(survived)} survived, {len(broken)} did not compile")
    for label, group in (("survivors", survived), ("did not compile", broken)):
        if group:
            print(f"{label}:")
            for name in group:
                print(f"  - {name}")
    return 1 if (survived or broken) else 0


if __name__ == "__main__":
    raise SystemExit(main())
