#!/usr/bin/env python3
"""Extract the source commit recorded in an image provenance attestation.

Input: the JSON output of `gh attestation verify --json`.
Output: the git commit the image was built from (prints nothing if absent).
"""
import base64
import json
import sys


def decode_payload(raw):
    """gh attestation verify --json emits the statement payload either as
    raw json or base64-encoded; handle both."""
    if isinstance(raw, dict):
        return raw
    try:
        return json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return json.loads(base64.b64decode(raw).decode())


def main():
    if len(sys.argv) != 2:
        print("usage: extract-provenance-commit.py <attestations.json>", file=sys.stderr)
        return 2
    with open(sys.argv[1]) as f:
        atts = json.load(f)
    if isinstance(atts, dict):
        atts = [atts]
    for a in atts:
        # gh >= 2.50 wraps the verified statement under verificationResult
        if "verificationResult" in a:
            payload = decode_payload(a["verificationResult"]["statement"]["payload"])
        else:
            payload = decode_payload(a["payload"])
        predicate = payload.get("predicate", {})
        # buildx provenance records the source in invocation.environment
        env = predicate.get("invocation", {}).get("environment", {})
        commit = env.get("github_sha")
        if not commit:
            # fallback: first material digest (the source repo)
            materials = predicate.get("materials", [])
            if materials:
                commit = materials[0].get("digest", {}).get("sha1")
        if commit:
            print(commit)
            return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
