"""Fetch the Bistro scene into D3D12/Resources/Scenes/Bistro.

The assets are ~2GB, far past what's reasonable to track in git, so they're
downloaded on demand from NVIDIA's RTXDI-Assets repository instead. Only the
lighting rig (bistro-rtxdi.scene.json) lives in this repo; this script fills in
the geometry and textures around it.

Usage:
    python scripts/fetch_bistro.py [--cleanup] [--force]

Requires git and git-lfs (https://git-lfs.com) on PATH.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

ASSETS_URL = "https://github.com/NVIDIA-RTX/RTXDI-Assets.git"
LIGHTS_NAME = "bistro-rtxdi.scene.json"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLONE_DIR = os.path.join(REPO_ROOT, "_Temp", "RTXDI-Assets")
SRC_DIR = os.path.join(CLONE_DIR, "bistro")
DST_DIR = os.path.join(REPO_ROOT, "D3D12", "Resources", "Scenes", "Bistro")


def run(args, cwd=None, env=None):
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    result = subprocess.run(args, cwd=cwd, env=full_env)
    if result.returncode != 0:
        sys.exit("Command failed: %s" % " ".join(args))


def check_prerequisites():
    for tool, args in (("git", ["git", "--version"]), ("git-lfs", ["git", "lfs", "version"])):
        try:
            subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        except (OSError, subprocess.CalledProcessError):
            sys.exit("%s is required but was not found on PATH." % tool)


def download():
    # Blobless + sparse + shallow keeps this to the bistro folder rather than the
    # full multi-scene repository. LFS smudge is skipped during checkout so the
    # pointer files land first and only bistro's binaries get downloaded after.
    lfs_off = {"GIT_LFS_SKIP_SMUDGE": "1"}

    if not os.path.isdir(os.path.join(CLONE_DIR, ".git")):
        os.makedirs(os.path.dirname(CLONE_DIR), exist_ok=True)
        print("Cloning %s ..." % ASSETS_URL)
        run(["git", "clone", "--filter=blob:none", "--no-checkout", "--depth", "1",
             ASSETS_URL, CLONE_DIR], env=lfs_off)

    print("Selecting the bistro folder ...")
    run(["git", "sparse-checkout", "init", "--cone"], cwd=CLONE_DIR, env=lfs_off)
    run(["git", "sparse-checkout", "set", "bistro"], cwd=CLONE_DIR, env=lfs_off)
    run(["git", "checkout"], cwd=CLONE_DIR, env=lfs_off)

    # The lighting rig sits at the repository root rather than inside bistro/, and
    # upstream tracks .json through LFS too, so it needs naming explicitly here or
    # it stays a pointer file.
    print("Downloading LFS objects (~2GB, this takes a while) ...")
    run(["git", "lfs", "pull", "--include=bistro/**,bistro-rtxdi.scene.json"], cwd=CLONE_DIR)


def convert():
    """Rewrite the glTF into the form this engine's loader expects and copy it over."""
    with open(os.path.join(SRC_DIR, "bistro.gltf"), "r", encoding="utf-8") as f:
        gltf = json.load(f)

    images = gltf.get("images", [])
    textures = gltf.get("textures", [])

    # Resolve MSFT_texture_dds: point each texture at the DDS image instead of the
    # PNG fallback, which RTXDI-Assets doesn't actually ship. Matches how Sponza
    # references its textures, so Image::Load takes the DDS path directly.
    used = {}
    for tex in textures:
        ext = tex.get("extensions", {})
        dds = ext.get("MSFT_texture_dds")
        if dds is not None:
            tex["source"] = dds["source"]
            ext.pop("MSFT_texture_dds")
            if not ext:
                tex.pop("extensions")
        src = tex.get("source")
        if src is not None and src not in used:
            used[src] = len(used)

    new_images = [None] * len(used)
    for old, new in used.items():
        img = dict(images[old])
        img["mimeType"] = "image/dds"
        new_images[new] = img
    for tex in textures:
        if "source" in tex:
            tex["source"] = used[tex["source"]]
    gltf["images"] = new_images
    gltf["extensionsUsed"] = [e for e in gltf.get("extensionsUsed", []) if e != "MSFT_texture_dds"]

    bin_uri = gltf["buffers"][0]["uri"]
    gltf["buffers"][0]["uri"] = "Bistro.bin"

    os.makedirs(DST_DIR, exist_ok=True)

    missing = []
    for img in new_images:
        uri = img["uri"]
        src = os.path.join(SRC_DIR, uri.replace("/", os.sep))
        if not os.path.isfile(src):
            missing.append(uri)
            continue
        dst = os.path.join(DST_DIR, uri.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.isfile(dst) or os.path.getsize(dst) != os.path.getsize(src):
            shutil.copyfile(src, dst)

    shutil.copyfile(os.path.join(SRC_DIR, bin_uri), os.path.join(DST_DIR, "Bistro.bin"))
    with open(os.path.join(DST_DIR, "Bistro.gltf"), "w", encoding="utf-8") as f:
        json.dump(gltf, f, indent=4)

    # The lighting rig is tracked in this repository so it can be tuned without
    # re-downloading, which also means a re-run must not overwrite those edits.
    # Only put it back when it's genuinely absent.
    lights = os.path.join(DST_DIR, LIGHTS_NAME)
    if not os.path.isfile(lights):
        shutil.copyfile(os.path.join(CLONE_DIR, LIGHTS_NAME), lights)
        print("Restored %s from upstream." % LIGHTS_NAME)

    print("Textures: %d referenced, %d missing" % (len(new_images), len(missing)))
    for m in missing[:10]:
        print("  MISSING %s" % m)
    print("Nodes: %d, meshes: %d, materials: %d" % (
        len(gltf.get("nodes", [])), len(gltf.get("meshes", [])), len(gltf.get("materials", []))))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cleanup", action="store_true",
                        help="delete the intermediate clone once the scene is in place")
    parser.add_argument("--force", action="store_true",
                        help="re-convert even if the scene is already present")
    args = parser.parse_args()

    if os.path.isfile(os.path.join(DST_DIR, "Bistro.gltf")) and not args.force:
        print("Bistro is already present. Pass --force to rebuild it.")
        return

    check_prerequisites()
    download()
    convert()

    if args.cleanup:
        print("Removing %s ..." % CLONE_DIR)
        shutil.rmtree(CLONE_DIR, ignore_errors=True)

    print("\nDone. Load it in-engine via File > Load Scene > Bistro.")


if __name__ == "__main__":
    main()
