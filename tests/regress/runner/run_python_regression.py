#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binding", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--test", required=True)
    parser.add_argument("--timeout", required=True, type=int)
    args = parser.parse_args()

    binding = Path(args.binding).resolve()
    library = Path(args.library).resolve()
    test = Path(args.test).resolve()

    env = os.environ.copy()
    library_dir = str(library.parent)
    env["LIBUNICORN_PATH"] = library_dir
    env["PYTHONPATH"] = os.pathsep.join(
        (str(binding), str(test.parent), env.get("PYTHONPATH", ""))
    ).rstrip(os.pathsep)
    env["PATH"] = os.pathsep.join(
        (library_dir, env.get("PATH", ""))
    ).rstrip(os.pathsep)
    if sys.platform == "darwin":
        env["DYLD_LIBRARY_PATH"] = os.pathsep.join(
            (library_dir, env.get("DYLD_LIBRARY_PATH", ""))
        ).rstrip(os.pathsep)
    elif os.name != "nt":
        env["LD_LIBRARY_PATH"] = os.pathsep.join(
            (library_dir, env.get("LD_LIBRARY_PATH", ""))
        ).rstrip(os.pathsep)

    probe = subprocess.run(
        [
            sys.executable,
            "-c",
            "import ctypes, sys; ctypes.CDLL(sys.argv[1])",
            str(library),
        ],
        env=env,
        timeout=10,
        check=False,
    )
    if probe.returncode != 0:
        return probe.returncode

    try:
        completed = subprocess.run(
            [sys.executable, str(test)],
            cwd=str(test.parent),
            env=env,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        print(
            "Python regression timed out after {} seconds: {}".format(
                args.timeout, test.name
            ),
            file=sys.stderr,
        )
        return 124

    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
