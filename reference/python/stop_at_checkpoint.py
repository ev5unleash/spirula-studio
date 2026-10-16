#!/usr/bin/env python3
"""Usage: stop_at_checkpoint.py RUN_DIR STEP EXECUTABLE [ARGS...]."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import time


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    step = int(sys.argv[2])
    checkpoint = Path(sys.argv[1]) / f"step-{step:09d}.ckpt"
    if checkpoint.exists():
        raise SystemExit(f"Refusing existing checkpoint: {checkpoint}")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SS_B1_", "SSPLAT_B1_"))
           and k not in ("SS_WORKER_CONTROL", "SSPLAT_WORKER_CONTROL")}
    env["SS_WORKER_CONTROL"] = "1"
    start = time.monotonic()
    with subprocess.Popen(sys.argv[3:], stdin=subprocess.PIPE, env=env) as child:
        while child.poll() is None and not checkpoint.is_dir():
            time.sleep(0.1)
        if checkpoint.is_dir():
            with tarfile.open(checkpoint / "state.tar") as archive:
                state = json.load(archive.extractfile("state.json"))
            if state["step"] != step or not state["full_resume"]:
                raise RuntimeError("Published checkpoint is not the requested full state")
            if child.poll() is None:
                child.stdin.write(b"S")
                child.stdin.flush()
        result = child.wait()
    print(f"stop_at_checkpoint: exit={result} wall_seconds={time.monotonic() - start:.6f}",
          flush=True)
    return result if checkpoint.is_dir() else result or 1


if __name__ == "__main__":
    sys.exit(main())
