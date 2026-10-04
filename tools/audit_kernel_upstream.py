#!/usr/bin/env python3
"""Read-only audit of a kernel patch range against a pinned target ref.

Examples:
  python3 tools/audit_kernel_upstream.py --repo /home/slwyts/linux-piano \
    --base 500df175a7f9e6bc1a9c328590ca5150f84f9ff0 \
    --ref piano-stable --next-ref piano-next --output build/kernel-audit.json

The tool requires Git's --no-lazy-fetch option and never fetches, checks out,
changes refs, applies or deletes patches.
Missing objects, shallow history and bounded/incomplete searches stay unknown.
An ancestor is proof of ancestry, not of current hardware behavior. A patch-id
match also requires an exact whitespace-preserving change fingerprint; it is
evidence for the same textual change, not proof of semantic/device equivalence.
Squashed source commits are not declared upstream just because one canonical
component is already upstream.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit


DISPLAY_SQUASH = "5c2c95fddbe9c29720b7b4e84119357358e9f0cb"
AUDIO_SQUASH = "2f6c29345edf991b04ce15d2231e6060412335aa"
CANONICAL_COMMITS = [
    ("DSC all slices in one packet", "ffa88de8ddb64067df49e4d9f253d09a9c247059", DISPLAY_SQUASH),
    ("MSM DSI multiple slices per packet", "ce73a5db44e3d5f9c0c061f0868ae209b59605f1", DISPLAY_SQUASH),
    ("NT36532 panel binding", "1c523f0a930107191c95d96340c69a295a086e5a", DISPLAY_SQUASH),
    ("NT36532 panel driver", "47b823940e38607dad180eb2f82094ae65565a4b", DISPLAY_SQUASH),
    ("SM8750 GPU clock and IOMMU DT", "f695831a7af1f8d39203357572683981ebe4de77", DISPLAY_SQUASH),
    ("q6v5 handover IRQ one-shot", "bb7c5d6f5b41d192fa81ce404e463f5d3ce70cb3", AUDIO_SQUASH),
    ("PAS attach handover IRQ fix", "34b8b2d78b6276dc2dc4ebc06625a39956f266e4", AUDIO_SQUASH),
    ("q6apm TDM DAI operations", "4d084017589312a0da2bec3474ef2035c5f4a407", AUDIO_SQUASH),
    ("QAIF clock IDs rename", "c41ac86802fc0a22a886915a43bcad2e8d482b02", AUDIO_SQUASH),
    ("Missing versus invalid TDM slots", "0a9e00d5ebdfcf460902f463e765f737d3fe935e", AUDIO_SQUASH),
    ("TDM hw_params error handling", "8593dc5f052e791748eaa76397ad95b9e32edac3", AUDIO_SQUASH),
]


class GitError(RuntimeError):
    pass


def change_fingerprint(patch: bytes) -> str:
    """Preserve changed bytes/paths/modes, ignoring metadata and hunk offsets."""
    kept = []
    binary = False
    for line in patch.splitlines(keepends=True):
        if line.startswith(b"diff --git "):
            binary = False
            kept.append(line)
        elif line.startswith(b"GIT binary patch"):
            binary = True
            kept.append(line)
        elif binary or line.startswith((b"+", b"-", b"old mode ", b"new mode ",
                                        b"new file mode ", b"deleted file mode ",
                                        b"Binary files ")):
            kept.append(line)
    return hashlib.sha256(b"".join(kept)).hexdigest()


def public_remote_url(value: str) -> str:
    if "://" not in value:
        return value
    parts = urlsplit(value)
    host = parts.netloc.rsplit("@", 1)[-1]
    return urlunsplit((parts.scheme, host, parts.path,
                       "[redacted]" if parts.query else "", ""))


class Repository:
    def __init__(self, path: Path, timeout: int = 30, max_patch_bytes: int = 8388608):
        self.path = path.resolve()
        self.timeout = timeout
        self.max_patch_bytes = max_patch_bytes
        self.env = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_NO_LAZY_FETCH="1",
                        GIT_TERMINAL_PROMPT="0")
        self.patch_cache = {}
        self.info_cache = {}
        self.reachable_cache = {}
        self.run("rev-parse", "--git-dir")
        shallow_path = self.run("rev-parse", "--git-path", "shallow").stdout.decode().strip()
        shallow = Path(shallow_path)
        if not shallow.is_absolute():
            shallow = self.path / shallow
        self.shallow = set(shallow.read_text().splitlines()) if shallow.exists() else set()

    def run(self, *args: str, data: bytes | None = None, check: bool = True):
        try:
            result = subprocess.run(
                ["git", "--no-lazy-fetch", "-C", str(self.path), "-c", "color.ui=false", *args],
                input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=self.env, timeout=self.timeout, check=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise GitError("git command timed out") from exc
        if check and result.returncode:
            message = result.stderr.decode(errors="replace").strip().splitlines()
            raise GitError(message[-1][:400] if message else f"git exited {result.returncode}")
        return result

    def resolve(self, ref: str) -> str:
        return self.run("rev-parse", "--verify", "--end-of-options", ref + "^{commit}").stdout.decode().strip()

    def ancestry(self, commit: str, target: str) -> dict:
        # Repeated negative merge-base queries on large shallow kernel graphs
        # are expensive and cannot establish absence anyway. Traverse the
        # visible graph once; membership is still positive ancestry proof.
        if self.shallow:
            if target not in self.reachable_cache:
                visible = self.run("rev-list", "--max-count=100001", target).stdout.decode().splitlines()
                self.reachable_cache[target] = (set(visible[:100000]), len(visible) > 100000)
            visible, limited = self.reachable_cache[target]
            if commit in visible:
                return {"status": "ancestor"}
            return {"status": "unknown", "reason": "ancestry_scan_limit" if limited else "shallow_history"}
        result = self.run("merge-base", "--is-ancestor", commit, target, check=False)
        if result.returncode == 0:
            return {"status": "ancestor"}
        if result.returncode == 1 and not self.shallow:
            return {"status": "not_ancestor"}
        return {"status": "unknown", "reason": "shallow_history" if self.shallow else "ancestry_query_failed"}

    def info(self, commit: str) -> dict:
        if commit not in self.info_cache:
            self.preload_info([commit])
        return dict(self.info_cache[commit])

    def preload_info(self, commits: list[str]) -> None:
        needed = list(dict.fromkeys(commit for commit in commits if commit not in self.info_cache))
        for offset in range(0, len(needed), 500):
            text = self.run("show", "--no-patch", "--format=%H%x00%s%x00%cI%x00%P",
                            *needed[offset:offset + 500]).stdout.decode(errors="replace")
            for line in text.splitlines():
                fields = line.split("\0")
                if len(fields) == 4:
                    self.info_cache[fields[0]] = {"commit": fields[0], "subject": fields[1],
                                                  "committer_date": fields[2], "parents": fields[3].split()}

    def patch(self, commit: str) -> dict:
        if commit not in self.patch_cache:
            try:
                if commit in self.shallow:
                    raise GitError("commit lies on a shallow boundary")
                if len(self.info(commit)["parents"]) > 1:
                    raise GitError("merge commit has no single canonical patch")
                # Raw tree deltas need no blob contents. Avoid expensive diff
                # attempts when a partial clone has not materialized them.
                raw = self.run("diff-tree", "--root", "--no-commit-id", "--raw", "-z", "-r",
                               "--no-renames", commit).stdout
                blobs = set()
                for record in raw.split(b"\0"):
                    if record.startswith(b":"):
                        fields = record.split()
                        if len(fields) < 5:
                            continue
                        for mode, sha in ((fields[0][1:], fields[2]), (fields[1], fields[3])):
                            if mode != b"160000" and sha.strip(b"0"):
                                blobs.add(sha)
                if blobs:
                    objects = self.run("cat-file", "--batch-check",
                                       data=b"\n".join(sorted(blobs)) + b"\n").stdout.splitlines()
                    missing = [line.split()[0].decode() for line in objects if line.endswith(b" missing")]
                    if missing:
                        raise GitError("missing blob objects; explicit fetch required: " + ", ".join(missing[:3]))
                patch = self.run("show", "--format=", "--patch", "--binary", "--no-renames",
                                 "--no-ext-diff", "--no-textconv", commit).stdout
                if len(patch) > self.max_patch_bytes:
                    raise GitError("patch exceeds max_patch_bytes")
                result = self.run("patch-id", "--stable", data=patch).stdout.decode().split()
                if not result:
                    raise GitError("empty commit has no patch-id")
                self.patch_cache[commit] = {"patch_id": result[0],
                                            "change_sha256": change_fingerprint(patch)}
            except GitError as exc:
                self.patch_cache[commit] = {"error": str(exc)}
        return dict(self.patch_cache[commit])

    def remotes(self) -> dict:
        result = self.run("config", "--get-regexp", r"^remote\..*\.url$", check=False)
        remotes = {}
        for line in result.stdout.decode(errors="replace").splitlines():
            key, value = line.split(None, 1)
            remotes[key[len("remote."):-len(".url")]] = public_remote_url(value)
        return remotes

    def candidates(self, target: str, next_base: str | None, limit: int) -> tuple[list[str], dict]:
        args = ["rev-list", "--topo-order", f"--max-count={limit + 1}", target]
        base_relation = self.ancestry(next_base, target) if next_base else None
        if next_base:
            args.append("^" + next_base)
        commits = self.run(*args).stdout.decode().splitlines()
        truncated = len(commits) > limit
        selected = commits[:limit]
        boundary = sorted(set(selected) & self.shallow)
        reasons = []
        if truncated:
            reasons.append("target_commit_limit")
        if boundary:
            reasons.append("shallow_target_range")
        if next_base and base_relation["status"] != "ancestor":
            reasons.append("target_base_ancestry_unverified")
        return selected, {"base": next_base, "base_relation": base_relation,
                          "target": target, "commit_limit": limit,
                          "scanned_commits": len(selected), "truncated": truncated,
                          "shallow_boundaries": boundary, "history_complete": not reasons,
                          "incomplete_reasons": reasons}


def canonical_catalog(path: Path | None) -> list[dict]:
    if path is None:
        return [{"name": name, "commit": sha, "carried_by": carried_by,
                 "url": f"https://github.com/torvalds/linux/commit/{sha}"}
                for name, sha, carried_by in CANONICAL_COMMITS]
    data = json.loads(path.read_text())
    if not isinstance(data, list) or any(not isinstance(row, dict) or
                                        not isinstance(row.get("commit"), str) for row in data):
        raise ValueError("canonical catalog must be a JSON array of objects with commit strings")
    return data


def audit(repo: Repository, base_ref: str, source_ref: str, target_ref: str,
          next_base_ref: str | None = None, canonical: list[dict] | None = None,
          max_target_commits: int = 1000, max_source_commits: int = 1000,
          search_patch_ids: bool = True) -> dict:
    base, source, target = (repo.resolve(x) for x in (base_ref, source_ref, target_ref))
    source_relation = repo.ancestry(base, source)
    if source_relation["status"] != "ancestor":
        raise ValueError("source base must be a locally provable ancestor of source ref; fetch the required history explicitly")
    source_list = repo.run("rev-list", "--topo-order", f"--max-count={max_source_commits + 1}",
                           source, "^" + base).stdout.decode().splitlines()
    source_truncated = len(source_list) > max_source_commits
    source_list = list(reversed(source_list[:max_source_commits]))

    target_base = None
    base_selection = "full_visible_target_history"
    if next_base_ref:
        target_base = repo.resolve(next_base_ref)
        base_selection = "explicit"
    else:
        common = repo.run("merge-base", "--all", base, target, check=False)
        bases = common.stdout.decode().splitlines()
        if common.returncode == 0 and len(bases) == 1:
            target_base = bases[0]
            base_selection = "source_base_merge_base"
    candidates, scope = repo.candidates(target, target_base, max_target_commits)
    repo.preload_info(source_list + (candidates if search_patch_ids else []))
    scope["base_selection"] = base_selection
    scope["requested_base_ref"] = next_base_ref

    index = defaultdict(list)
    unavailable = []
    for commit in candidates if search_patch_ids else []:
        evidence = repo.patch(commit)
        if "error" in evidence:
            unavailable.append({"commit": commit, "reason": evidence["error"]})
        else:
            index[evidence["patch_id"]].append({"commit": commit, **evidence})
    scope["patch_id_search_enabled"] = search_patch_ids
    scope["patches_available"] = len(candidates) - len(unavailable) if search_patch_ids else 0
    scope["patches_unavailable"] = unavailable
    scope["patch_search_complete"] = search_patch_ids and scope["history_complete"] and not unavailable

    def audit_one(requested: str) -> dict:
        try:
            commit = repo.resolve(requested)
        except GitError:
            return {"requested_commit": requested, "status": "unknown", "reason": "commit_object_unavailable"}
        row = repo.info(commit)
        relation = repo.ancestry(commit, target)
        row["ancestry"] = relation
        if relation["status"] == "ancestor":
            row["status"] = "ancestor"
            return row
        if not search_patch_ids:
            row.update(status="unknown", reason="patch_id_search_disabled")
            return row
        evidence = repo.patch(commit)
        row["patch"] = evidence
        if "error" in evidence:
            row.update(status="unknown", reason="source_patch_unavailable")
            return row
        matches = index.get(evidence["patch_id"], [])
        exact = [item for item in matches if item["change_sha256"] == evidence["change_sha256"]]
        row["patch_id_candidates"] = matches
        if exact:
            row.update(status="patch_id_match", matching_commits=[item["commit"] for item in exact])
        elif matches:
            row.update(status="unknown", reason="patch_id_matches_but_changed_bytes_differ")
        elif relation["status"] == "not_ancestor" and scope["patch_search_complete"]:
            row.update(status="no_patch_id_match_in_range",
                       reason="no_equal_patch_in_complete_declared_range")
        else:
            row.update(status="unknown", reason="target_history_or_patch_search_incomplete")
        return row

    source_rows = [audit_one(commit) for commit in source_list]
    canonical_rows = []
    for item in canonical or []:
        row = audit_one(item["commit"])
        row["canonical"] = dict(item)
        canonical_rows.append(row)
    for row in source_rows:
        row["canonical_components"] = [
            {"commit": item["canonical"]["commit"], "name": item["canonical"].get("name"),
             "status": item["status"]}
            for item in canonical_rows if item["canonical"].get("carried_by") == row["commit"]
        ]
    return {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "repo": str(repo.path), "remotes": repo.remotes(),
        "read_only": True, "automatic_fetch": False, "automatic_patch_removal": False,
        "source": {"requested_base_ref": base_ref, "base": base,
                   "requested_ref": source_ref, "ref": source,
                   "base_relation": source_relation, "commit_limit": max_source_commits,
                   "truncated": source_truncated},
        "target": {"requested_ref": target_ref, "ref": target, "scope": scope},
        "source_commits": source_rows, "canonical_commits": canonical_rows,
        "summary": {"source": dict(Counter(row["status"] for row in source_rows)),
                    "canonical": dict(Counter(row["status"] for row in canonical_rows))},
        "limitations": [
            "Ancestry does not prove the change was not later reverted or replaced.",
            "Patch-id plus changed-byte fingerprints prove a matching textual delta, not runtime semantics.",
            "No match in the declared range does not prove absence from earlier history or a squash.",
            "Squash components are reported independently; they do not establish equivalence of the whole squash.",
            "Missing objects, merges, shallow boundaries and scan limits are not silently treated as non-equivalence.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--base", required=True, help="vanilla base of the source patch range")
    parser.add_argument("--ref", required=True, help="source branch/commit containing the patches")
    parser.add_argument("--next-ref", required=True, help="target upstream/integration branch/commit")
    parser.add_argument("--next-base", help="explicit lower bound of the target patch-id search")
    parser.add_argument("--canonical-catalog", type=Path, help="JSON array replacing the built-in canonical commit catalog")
    parser.add_argument("--max-target-commits", type=int, default=1000)
    parser.add_argument("--max-source-commits", type=int, default=1000)
    parser.add_argument("--timeout", type=int, default=30, help="timeout per git subprocess")
    parser.add_argument("--max-patch-bytes", type=int, default=8388608)
    parser.add_argument("--metadata-only", action="store_true",
                        help="only prove ancestry; all other commits stay unknown without reading blob patches")
    parser.add_argument("--output", type=Path, help="JSON report; stdout when omitted")
    args = parser.parse_args()
    if min(args.max_target_commits, args.max_source_commits, args.timeout, args.max_patch_bytes) < 1:
        parser.error("limits and timeout must be positive")
    try:
        report = audit(Repository(args.repo, args.timeout, args.max_patch_bytes),
                       args.base, args.ref, args.next_ref, args.next_base,
                       canonical_catalog(args.canonical_catalog),
                       args.max_target_commits, args.max_source_commits,
                       search_patch_ids=not args.metadata_only)
        output = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(output)
            print(json.dumps({"output": str(args.output.resolve()), "summary": report["summary"]}))
        else:
            sys.stdout.write(output)
    except (GitError, ValueError, OSError) as exc:
        print(f"kernel upstream audit: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
