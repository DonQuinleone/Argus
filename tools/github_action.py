#!/usr/bin/env python3
"""GitHub Action driver: run Argus on audio files and report through the Checks API.

Standard library only. Reads its configuration from the environment that
action.yml sets up (INPUT_* plus the usual GITHUB_* variables), then:

  1. collects the audio files to check: on a pull request, the ones the pull
     request adds or changes (from the REST API); otherwise every audio file
     under the configured paths;
  2. runs the argus CLI on each file and reads its JSON report;
  3. publishes a check run with a summary and one annotation per WARN/FAIL
     finding, writes the job summary and step outputs;
  4. exits non-zero when the worst verdict reaches the fail-on threshold.

If the token cannot create check runs (for example a pull request from a fork,
whose token is read-only) it falls back to workflow-command annotations.
"""

import json
import os
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

AUDIO_EXTS = {
    ".wav", ".wave", ".bwf", ".rf64", ".w64", ".aif", ".aiff", ".aifc", ".caf",
    ".flac", ".ogg", ".oga", ".opus", ".mp3", ".m4a",
}
RANK = {"PASS": 0, "INFO": 0, "WARN": 1, "FAIL": 2, "ERROR": 2}
CHECK_NAME = "Argus audio QA"
MAX_ANNOTATIONS = 50  # per Checks API request


def env(name, default=""):
    return os.environ.get(name, default).strip()


class GitHub:
    def __init__(self):
        self.api = env("GITHUB_API_URL", "https://api.github.com")
        self.repo = env("GITHUB_REPOSITORY")
        self.token = env("INPUT_GITHUB_TOKEN")

    def request(self, method, path, body=None):
        url = path if path.startswith("http") else f"{self.api}{path}"
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
        req.add_header("User-Agent", "argus-action")
        if self.token:
            req.add_header("Authorization", f"Bearer {self.token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read() or b"null")

    def pull_request_files(self, number):
        files, page = [], 1
        while True:
            batch = self.request(
                "GET", f"/repos/{self.repo}/pulls/{number}/files?per_page=100&page={page}")
            files += batch
            if len(batch) < 100 or page >= 30:  # the API lists at most 3,000 files
                return files
            page += 1


def is_audio(path):
    return os.path.splitext(path)[1].lower() in AUDIO_EXTS


def under(path, roots):
    norm = os.path.normpath(path)
    for root in roots:
        r = os.path.normpath(root)
        if r == "." or norm == r or norm.startswith(r + os.sep):
            return True
    return False


def collect_files(gh, event, roots):
    pr = event.get("pull_request")
    if pr and env("INPUT_CHANGED_ONLY", "true") == "true":
        changed = gh.pull_request_files(pr["number"])
        return sorted(
            f["filename"] for f in changed
            if f.get("status") != "removed" and is_audio(f["filename"])
            and under(f["filename"], roots) and os.path.isfile(f["filename"]))
    found = []
    for root in roots:
        if os.path.isfile(root):
            if is_audio(root):
                found.append(os.path.normpath(root))
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != ".git"]
            found += [os.path.normpath(os.path.join(dirpath, n)) for n in filenames if is_audio(n)]
    return sorted(set(found))


def run_argus(argus, path, profile):
    with tempfile.TemporaryDirectory() as out:
        cmd = [argus, "--json", out]
        if profile:
            cmd += ["--profile", profile]
        cmd.append(path)
        # argus exits 1/2 for warnings/failures; the JSON report is what matters.
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
        stem = os.path.splitext(os.path.basename(path))[0]
        try:
            with open(os.path.join(out, f"{stem}_QA.json"), encoding="utf-8") as f:
                return json.load(f)["report"]
        except (OSError, ValueError, KeyError):
            return {"ok": False, "verdict": "ERROR", "decodeError": "argus produced no report",
                    "issues": [], "counts": {}}


def annotation(path, issue):
    where = []
    if "startSec" in issue:
        where.append(f"at {issue['startSec']:.3f} s")
    if "channel" in issue:
        where.append(f"channel {issue['channel']}")
    message = issue.get("summary", "")
    if issue.get("detail"):
        message += "\n\n" + issue["detail"]
    fields = issue.get("fields") or {}
    if fields:
        message += "\n\n" + "\n".join(f"{k}: {v}" for k, v in fields.items())
    title = issue.get("check", "Argus")
    if where:
        title += " (" + ", ".join(where) + ")"
    return {
        "path": path, "start_line": 1, "end_line": 1,
        "annotation_level": "failure" if issue["severity"] == "FAIL" else "warning",
        "title": title[:255], "message": message[:64000],
    }


def summarise(results, profile):
    worst = max((RANK.get(r["verdict"], 2) for _, r in results), default=0)
    counts = {"FAIL": 0, "WARN": 0, "ERROR": 0, "PASS": 0}
    lines = [
        "| File | Verdict | Fails | Warnings | Findings |",
        "| --- | --- | --- | --- | --- |",
    ]
    for path, rep in results:
        verdict = rep.get("verdict", "ERROR")
        counts[verdict if verdict in counts else "PASS"] += 1
        c = rep.get("counts") or {}
        if not rep.get("ok", True):
            notes = rep.get("decodeError", "")
        else:
            notes = "; ".join(
                f"{i['check']}: {i['summary']}" for i in rep.get("issues", [])
                if i.get("severity") in ("WARN", "FAIL"))
        notes = notes.replace("|", "\\|") or "clean"
        lines.append(f"| `{path}` | {verdict} | {c.get('fail', 0)} | {c.get('warn', 0)} | {notes} |")
    head = (f"Checked {len(results)} audio file(s)"
            + (f" against the **{profile}** profile" if profile else "")
            + f": {counts['FAIL']} failed, {counts['WARN']} with warnings, "
            f"{counts['ERROR']} could not be decoded.")
    return worst, counts, head + "\n\n" + "\n".join(lines)


def publish_check(gh, head_sha, conclusion, title, summary, annotations):
    batches = [annotations[i:i + MAX_ANNOTATIONS]
               for i in range(0, len(annotations), MAX_ANNOTATIONS)] or [[]]
    run = gh.request("POST", f"/repos/{gh.repo}/check-runs", {
        "name": CHECK_NAME, "head_sha": head_sha, "status": "completed",
        "conclusion": conclusion,
        "output": {"title": title, "summary": summary[:65535], "annotations": batches[0]},
    })
    for batch in batches[1:]:
        gh.request("PATCH", f"/repos/{gh.repo}/check-runs/{run['id']}", {
            "output": {"title": title, "summary": summary[:65535], "annotations": batch},
        })
    return run.get("html_url", "")


def workflow_command_annotations(annotations):
    def esc(s):
        return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    for a in annotations:
        level = "error" if a["annotation_level"] == "failure" else "warning"
        title = esc(a["title"]).replace(",", "%2C").replace("::", "%3A%3A")
        print(f"::{level} file={a['path']},title={title}::{esc(a['message'])}")


def write_file(var, text):
    path = env(var)
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text)


def main():
    argus = env("ARGUS_BIN")
    profile = env("INPUT_PROFILE")
    fail_on = env("INPUT_FAIL_ON", "fail").lower()
    if fail_on not in ("fail", "warn", "never"):
        print(f"::error::fail-on must be fail, warn or never (got '{fail_on}')")
        return 2
    roots = env("INPUT_PATHS", ".").split() or ["."]

    event = {}
    if env("GITHUB_EVENT_PATH") and os.path.isfile(env("GITHUB_EVENT_PATH")):
        with open(env("GITHUB_EVENT_PATH"), encoding="utf-8") as f:
            event = json.load(f)
    gh = GitHub()
    pr = event.get("pull_request")
    head_sha = pr["head"]["sha"] if pr else env("GITHUB_SHA")

    files = collect_files(gh, event, roots)
    results = [(path, run_argus(argus, path, profile)) for path in files]
    worst, counts, summary = summarise(results, profile)
    annotations = [annotation(p, i) for p, r in results for i in r.get("issues", [])
                   if i.get("severity") in ("WARN", "FAIL")]
    annotations += [{"path": p, "start_line": 1, "end_line": 1, "annotation_level": "failure",
                     "title": "Could not decode", "message": r.get("decodeError", "")}
                    for p, r in results if not r.get("ok", True)]

    threshold = {"fail": 2, "warn": 1, "never": 99}[fail_on]
    failed = worst >= threshold
    conclusion = "failure" if failed else ("neutral" if worst > 0 else "success")
    if not files:
        conclusion, summary = "skipped", "No audio files to check."
    title = {"success": "All audio files passed", "neutral": "Warnings found",
             "failure": "Defects found", "skipped": "No audio files"}[conclusion]

    url = ""
    if env("INPUT_CHECK_RUN", "true") == "true" and gh.token and gh.repo:
        try:
            url = publish_check(gh, head_sha, conclusion, title, summary, annotations)
        except urllib.error.HTTPError as e:
            print(f"::notice::Could not create a check run ({e.code}); "
                  "falling back to workflow annotations. Grant 'checks: write' to enable it.")
            workflow_command_annotations(annotations)
    else:
        workflow_command_annotations(annotations)

    write_file("GITHUB_STEP_SUMMARY", f"## {CHECK_NAME}: {title}\n\n{summary}\n")
    write_file("GITHUB_OUTPUT",
               f"files={len(files)}\nfailed={counts['FAIL'] + counts['ERROR']}\n"
               f"warned={counts['WARN']}\nconclusion={conclusion}\ncheck-run-url={url}\n")
    print(f"{CHECK_NAME}: {title} ({len(files)} file(s))")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
