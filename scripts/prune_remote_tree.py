#!/usr/bin/env python3
"""清理 GitHub 远端 tree 里的历史残留文件，保留全部提交历史。

背景：git-filter-repo 剔除垃圾文件后，push_rest.py 用了 base_tree 增量叠加，
导致已被本地删除的文件仍留在远端 tree 里（如旧包名 com/gloai/mirror/*.kt、
qemu-test/images/*.png 抓帧）。这些文件会被 CI 当成活跃源码编译，导致构建失败。

做法：按本地 HEAD 的 ls-tree 建一棵**不带 base_tree 的全量 tree**，
挂成一个新 commit（parent = 远端当前 tip），再 force 更新分支引用。
历史一条不丢，残留文件全部消失。

用法:
    python3 prune_remote_tree.py [local_commit] [branch]
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
ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
LOCAL_COMMIT = ARGS[0] if ARGS else "HEAD"
BRANCH = ARGS[1] if len(ARGS) > 1 else "main"
API = "https://api.github.com"


def gh(method, path, body=None, retries=4):
    url = path if path.startswith("http") else API + path
    data = json.dumps(body).encode() if body is not None else None
    last = None
    for i in range(retries):
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + os.environ["GH_TOKEN"])
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("User-Agent", "prune-remote-tree")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read().decode() or "{}")
        except Exception as e:  # noqa: BLE001
            last = e
            detail = ""
            if isinstance(e, urllib.error.HTTPError):
                detail = " " + e.read().decode()[:300]
            if isinstance(e, urllib.error.HTTPError) and e.code not in (429, 500, 502, 503, 504):
                raise RuntimeError(f"{method} {path} -> {e.code}{detail}") from e
            sys.stderr.write(f"  retry {i+1}/{retries} ({e}){detail}\n")
            time.sleep(2 ** i)
    raise RuntimeError(f"{method} {path} failed: {last}")


def git_text(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def git_bytes(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def remote_blobs(tree_sha):
    out = {}
    stack = [(tree_sha, "")]
    while stack:
        sha, prefix = stack.pop()
        for node in gh("GET", f"/repos/{REPO}/git/trees/{sha}").get("tree", []):
            path = prefix + node["path"]
            if node["type"] == "blob":
                out[path] = node["sha"]
            elif node["type"] == "tree":
                stack.append((node["sha"], path + "/"))
    return out


def main():
    sha = git_text("rev-parse", LOCAL_COMMIT).strip()
    msg = git_text("log", "-1", "--format=%s", sha).strip()
    author = git_text("log", "-1", "--format=%an <%ae>", sha).strip()
    when = git_text("log", "-1", "--format=%aI", sha).strip()

    ref = gh("GET", f"/repos/{REPO}/git/refs/heads/{BRANCH}")
    parent = ref["object"]["sha"]
    parent_tree = gh("GET", f"/repos/{REPO}/git/commits/{parent}")["tree"]["sha"]
    print(f"远端 {BRANCH} tip = {parent[:9]}  tree={parent_tree[:9]}")

    remote = remote_blobs(parent_tree)

    entries = []
    local = {}
    for line in git_text("ls-tree", "-r", sha).splitlines():
        if not line.strip():
            continue
        meta, path = line.split("\t", 1)
        mode, otype, osha = meta.split()
        local[path] = osha
        entries.append({"path": path, "mode": mode, "type": otype, "sha": osha})

    print(f"本地 {len(entries)} 个文件 / 远端 {len(remote)} 个文件")
    stale = sorted(set(remote) - set(local))
    for p in stale:
        print(f"  D  {p}")
    if not stale:
        print("无残留，无需处理")
        return

    # 复用远端已有 blob 的 sha（内容相同 sha 就相同），只上传远端没有的
    remote_sha_set = set(remote.values())
    uploaded = 0
    for e in entries:
        if e["type"] != "blob" or e["sha"] in remote_sha_set:
            continue
        raw = git_bytes("cat-file", "blob", e["sha"])
        e["sha"] = gh("POST", f"/repos/{REPO}/git/blobs", {
            "content": base64.b64encode(raw).decode(),
            "encoding": "base64",
        })["sha"]
        uploaded += 1
        if uploaded % 20 == 0:
            print(f"  上传 blob {uploaded}")
    print(f"  新上传 blob {uploaded} 个（其余复用远端已有）")

    # 关键：不传 base_tree，这棵树就是完整清单，残留文件自然不存在
    tree = gh("POST", f"/repos/{REPO}/git/trees", {"tree": entries})["sha"]
    print(f"新 tree = {tree}")

    name, _, mail = author.partition(" <")
    who = {"name": name, "email": mail.rstrip(">"), "date": when}
    commit = gh("POST", f"/repos/{REPO}/git/commits", {
        "message": f"chore: 清理远端 tree 历史残留（{len(stale)} 个已删除文件）\n\n"
                   f"filter-repo 后增量推送的 base_tree 残留导致 CI 把旧包名源码\n"
                   f"com/gloai/mirror/*.kt 当活跃代码编译，Android 构建失败。\n"
                   f"本次用不带 base_tree 的全量 tree 覆盖，历史不变。\n\n{msg}",
        "tree": tree, "parents": [parent],
        "author": who, "committer": who,
    })["sha"]
    gh("PATCH", f"/repos/{REPO}/git/refs/heads/{BRANCH}", {"sha": commit, "force": True})
    print(f"\n✓ {BRANCH} -> {commit}  清理 {len(stale)} 个残留文件")


if __name__ == "__main__":
    main()
