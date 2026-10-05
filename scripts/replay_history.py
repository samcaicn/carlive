#!/usr/bin/env python3
"""逐个 commit 重建到 GitHub，保留完整提交历史（filter-repo 改写历史后用）。

用法: replay_history.py <remote_ref_prefix> [dry_run]

前置：本地已完成 filter-repo，HEAD 是改写后的 tip。
对每个 commit 按拓扑序（旧→新）重建 blob/tree，parent 指向上一条重建出的 commit。
"""
import base64
import json
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error

REPO = os.environ.get("GH_PUSH_REPO", "samcaicn/carlive")
BRANCH = os.environ.get("GH_PUSH_BRANCH", "main")
API = "https://api.github.com"


def gh(method, path, body=None, retries=4):
    url = path if path.startswith("http") else API + path
    data = json.dumps(body).encode() if body is not None else None
    last = None
    for i in range(retries):
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + os.environ["GH_TOKEN"])
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("User-Agent", "replay-history")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read().decode() or "{}")
        except Exception as e:  # noqa: BLE001
            last = e
            detail = ""
            if isinstance(e, urllib.error.HTTPError):
                detail = " " + e.read().decode()[:300]
            # 4xx（非 429）立即失败
            if isinstance(e, urllib.error.HTTPError) and e.code not in (429, 500, 502, 503, 504):
                raise RuntimeError(f"{method} {path} -> {e.code}{detail}") from e
            sys.stderr.write(f"  retry {i+1}/{retries} ({e}){detail}\n")
            time.sleep(2 ** i)
    raise RuntimeError(f"{method} {path} failed: {last}")


def git(*args, binary=False):
    return subprocess.run(
        ["git", "-C", os.environ["REPO_DIR"], *args],
        check=True, capture_output=True,
        text=not binary,
    ).stdout


def main():
    repo_dir = sys.argv[1] if len(sys.argv) > 1 else "/Users/k/carlive"
    os.environ["REPO_DIR"] = repo_dir
    dry = "--dry-run" in sys.argv

    # 旧→新的拓扑序（filter-repo 后 HEAD 的第一父链）
    shas = git("rev-list", "--reverse", "HEAD").split()
    print(f"共 {len(shas)} 个 commit 待重建")

    tree_cache = {}
    blob_cache = {}
    parent = None

    for i, sha in enumerate(shas, 1):
        msg = git("log", "-1", "--format=%s", sha).strip()
        author = git("log", "-1", "--format=%an <%ae>", sha).strip()
        when = git("log", "-1", "--format=%aI", sha).strip()

        entries = []
        for line in git("ls-tree", "-r", sha).splitlines():
            meta, path = line.split("\t", 1)
            mode, otype, osha = meta.split()
            if otype != "blob":
                entries.append({"path": path, "mode": mode, "type": otype, "sha": osha})
                continue
            key = osha
            if key not in blob_cache:
                content = git("cat-file", "blob", osha, binary=True)
                blob_cache[key] = gh("POST", f"/repos/{REPO}/git/blobs", {
                    "content": base64.b64encode(content).decode(),
                    "encoding": "base64",
                })["sha"]
            entries.append({"path": path, "mode": mode, "type": "blob", "sha": blob_cache[key]})

        body = {"tree": entries}
        if parent and parent != "DRYRUN":
            body["base_tree"] = parent
        new_tree = gh("POST", f"/repos/{REPO}/git/trees", body)["sha"]
        if new_tree == tree_cache.get(sha):
            new_sha = parent
        else:
            tree_cache[sha] = new_tree
            cbody = {"message": msg, "tree": new_tree, "parents": [parent] if parent else []}
            name, _, mail = author.partition(" <")
            cbody["author"] = {"name": name, "email": mail.rstrip(">"), "date": when}
            cbody["committer"] = cbody["author"]
            if dry:
                print(f"[dry] {sha[:8]} {msg}")
                parent = "DRYRUN"
                continue
            new_sha = gh("POST", f"/repos/{REPO}/git/commits", cbody)["sha"]
        parent = new_sha
        if not dry and i % 5 == 0:
            print(f"  {i}/{len(shas)} {sha[:8]} -> {new_sha[:8]}  {msg[:40]}")

    if dry:
        print("[dry] 未创建任何 commit/ref")
        return

    gh("PATCH", f"/repos/{REPO}/git/refs/heads/{BRANCH}", {"sha": parent, "force": True})
    print(f"\n✓ {BRANCH} -> {parent}  ({len(shas)} commits, blobs={len(blob_cache)})")


if __name__ == "__main__":
    main()
