#!/usr/bin/env python3
"""fetch_firmware.py — download a released Halo firmware binary into a
local cache (ticket 0034).

`halo-emu --fetch <version>` is the whole "download a firmware and use
it" UX; this module is the download half, importable from the launcher
and usable on its own:

    tools/fetch_firmware.py 0.8.8            # -> firmwares/0.8.8/0.8.8.bin
    tools/fetch_firmware.py latest --debug   # the -debug asset
    tools/fetch_firmware.py --list           # what releases exist

Cache layout: `firmwares/<tag>/<asset-name>` under the emulator repo (in
.gitignore).  It lives inside the repo on purpose — per AGENTS.md the
emulator never writes outside its own tree — but $HALO_EMU_CACHE
relocates it (a CI runner points that at a cached directory).

Release artifact names, as published by `brilliantlabsAR/halo-firmware`:

    0.8.9.bin                                 release build   <- the default
    0.8.9-debug.bin                           debug build (--debug); the
                                              only build that prints the
                                              boot banner, which is behind
                                              CONFIG_HALO_LOG_LEVEL_DBG
    halo-firmware-0.8.9-release.signed.bin    imgtool-signed variants, on
    halo-firmware-0.8.9-debug.signed.bin      the pre-release tags
    halo-bootloader-0.8.9-release.bin         mcuboot — never selected
                                              automatically (--asset only)

Selection rule: `.bin` assets that are not a bootloader, preferring the
requested flavour (release unless --debug), then an exact `<tag>.bin`,
then a name containing "firmware", then the shortest name.  Signed and
unsigned images both boot — halo-emu detects the vector table offset.
`--asset NAME` overrides the rule entirely.

Auth: anonymous by default (the repo is public).  $HALO_FW_TOKEN or
$GITHUB_TOKEN, when set, is sent as a bearer token — CI uses that to
stay clear of the unauthenticated GitHub API rate limit.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_FW_REPO = "brilliantlabsAR/halo-firmware"
API = "https://api.github.com"

# Assets that are never the application image.
NOT_APP = ("bootloader", "mcuboot")


class FetchError(Exception):
    """Anything that stops us handing back a firmware path."""


def cache_root():
    return os.environ.get("HALO_EMU_CACHE") or os.path.join(REPO_ROOT,
                                                            "firmwares")


def _repo(explicit=None):
    return explicit or os.environ.get("HALO_FW_REPO") or DEFAULT_FW_REPO


def _request(url, accept="application/vnd.github+json"):
    req = urllib.request.Request(url)
    req.add_header("Accept", accept)
    req.add_header("User-Agent", "halo-emu")
    token = os.environ.get("HALO_FW_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    return req


def _api_json(url):
    try:
        with urllib.request.urlopen(_request(url), timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise FetchError(f"not found: {url}")
        if e.code in (401, 403) and "rate limit" in str(e.reason).lower():
            raise FetchError("GitHub API rate limit reached — set "
                             "$HALO_FW_TOKEN or $GITHUB_TOKEN")
        raise FetchError(f"GitHub API {e.code} {e.reason} for {url}")
    except urllib.error.URLError as e:
        raise FetchError(f"cannot reach GitHub: {e.reason}")


def list_releases(repo=None, per_page=30):
    """[(tag, is_prerelease, [asset names])] newest first."""
    data = _api_json(f"{API}/repos/{_repo(repo)}/releases"
                     f"?per_page={per_page}")
    return [(d["tag_name"], bool(d.get("prerelease")),
             [a["name"] for a in d.get("assets", [])]) for d in data]


def resolve_release(version, repo=None):
    """Release JSON for `version`: 'latest' (newest non-prerelease), a
    tag, or a tag we can reach by adding/removing a leading 'v'."""
    repo = _repo(repo)
    if version in ("latest", "current"):
        return _api_json(f"{API}/repos/{repo}/releases/latest")

    tried = []
    for tag in (version, f"v{version}", version.lstrip("v")):
        if tag in tried:
            continue
        tried.append(tag)
        try:
            return _api_json(f"{API}/repos/{repo}/releases/tags/{tag}")
        except FetchError as e:
            if "not found" not in str(e):
                raise
    known = ", ".join(t for t, _, _ in list_releases(repo)[:10]) or "(none)"
    raise FetchError(f"no release {'/'.join(tried)} in {repo}; "
                     f"recent tags: {known}")


def pick_asset(release, want_debug=False, name=None):
    """Choose the application image among a release's assets."""
    assets = release.get("assets", [])
    if not assets:
        raise FetchError(f"release {release['tag_name']} has no assets")
    if name:
        for a in assets:
            if a["name"] == name:
                return a
        raise FetchError(f"asset {name!r} not in release "
                         f"{release['tag_name']}: "
                         + ", ".join(a["name"] for a in assets))

    tag = release["tag_name"]
    apps = [a for a in assets
            if a["name"].endswith(".bin")
            and not any(w in a["name"].lower() for w in NOT_APP)]
    if not apps:
        raise FetchError(
            f"release {tag} publishes no application .bin — assets: "
            + ", ".join(a["name"] for a in assets)
            + " (pass --asset/--fetch-asset to pick one anyway)")

    def is_debug(a):
        return "debug" in a["name"].lower()

    wanted = [a for a in apps if is_debug(a) == want_debug]
    if not wanted:
        flavour = "debug" if want_debug else "release"
        print(f"fetch-firmware: warning: release {tag} has no {flavour} "
              f"build; using {apps[0]['name']}", file=sys.stderr)
        wanted = apps

    wanted.sort(key=lambda a: (a["name"] != f"{tag}.bin",
                               "firmware" not in a["name"].lower(),
                               len(a["name"]), a["name"]))
    return wanted[0]


def _download(url, dest, size=None):
    tmp = dest + ".part"
    try:
        with urllib.request.urlopen(
                _request(url, accept="application/octet-stream"),
                timeout=300) as r, open(tmp, "wb") as f:
            got = 0
            while True:
                chunk = r.read(256 * 1024)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if size:
                    print(f"\rfetch-firmware: {got * 100 // size:3d}% "
                          f"({got}/{size} bytes)", end="", file=sys.stderr)
        if size:
            print(file=sys.stderr)
    except (urllib.error.URLError, OSError) as e:
        if os.path.exists(tmp):
            os.unlink(tmp)
        reason = f"HTTP {e.code} {e.reason}" \
            if isinstance(e, urllib.error.HTTPError) else e
        raise FetchError(f"download failed: {reason}")
    if size and os.path.getsize(tmp) != size:
        os.unlink(tmp)
        raise FetchError(f"download truncated: expected {size} bytes")
    os.replace(tmp, dest)


def fetch(version, repo=None, want_debug=False, asset_name=None,
          refresh=False, quiet=False):
    """Download (or reuse) the firmware for `version`; return its path."""
    release = resolve_release(version, repo)
    asset = pick_asset(release, want_debug=want_debug, name=asset_name)
    tag = release["tag_name"]

    outdir = os.path.join(cache_root(), tag)
    dest = os.path.join(outdir, asset["name"])
    size = asset.get("size") or 0

    if not refresh and os.path.exists(dest) and \
            (not size or os.path.getsize(dest) == size):
        if not quiet:
            print(f"fetch-firmware: cached {dest}", file=sys.stderr)
        return dest

    os.makedirs(outdir, exist_ok=True)
    if not quiet:
        print(f"fetch-firmware: {_repo(repo)} {tag} -> {asset['name']} "
              f"({size} bytes)", file=sys.stderr)
    _download(asset["browser_download_url"], dest, size if not quiet else None)
    if not quiet:
        print(f"fetch-firmware: cached {dest}", file=sys.stderr)
    return dest


def main():
    p = argparse.ArgumentParser(
        prog="fetch_firmware.py",
        description="Download a released Halo firmware into "
                    f"{os.path.relpath(cache_root(), os.getcwd())}/.")
    p.add_argument("version", nargs="?",
                   help="release tag (e.g. 0.8.8) or 'latest'")
    p.add_argument("--list", action="store_true",
                   help="list recent releases and their assets, then exit")
    p.add_argument("--debug", action="store_true",
                   help="take the -debug build (the one that prints the "
                        "boot banner) instead of the release build")
    p.add_argument("--asset", metavar="NAME",
                   help="exact asset name, bypassing the selection rule")
    p.add_argument("--repo", metavar="OWNER/NAME",
                   help=f"firmware repository (default {DEFAULT_FW_REPO}, "
                        "or $HALO_FW_REPO)")
    p.add_argument("--refresh", action="store_true",
                   help="re-download even if the cached copy looks good")
    args = p.parse_args()

    try:
        if args.list:
            for tag, pre, assets in list_releases(args.repo):
                mark = " (pre-release)" if pre else ""
                print(f"{tag}{mark}: {', '.join(assets) or '(no assets)'}")
            return
        if not args.version:
            p.error("give a version (or --list)")
        print(fetch(args.version, repo=args.repo, want_debug=args.debug,
                    asset_name=args.asset, refresh=args.refresh))
    except FetchError as e:
        sys.exit(f"fetch-firmware: {e}")


if __name__ == "__main__":
    main()
