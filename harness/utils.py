#!/usr/bin/env python3

# Copyright (c) 2025 HomomorphicEncryption.org
# All rights reserved.
#
# This software is licensed under the terms of the Apache v2 License.
# See the LICENSE.md file for details.

"""
utils.py - Scaffolding code for running the submission.
"""

import sys
import os
import platform
import subprocess
import json
from datetime import datetime
from pathlib import Path

# Global variable to track the last timestamp
_last_timestamp: datetime | None = None
# Global variable to store measured times
_timestamps = {}
_timestampsStr = {}
# Global variable to store measured sizes
_bandwidth = {}

def ensure_directories(rootdir: Path):
    """ Check that the current directory has the rquired sub-directories
    """
    required_dirs = ['harness', 'scripts', 'submission']
    for dir_name in required_dirs:
        if not (rootdir / dir_name).exists():
            print(f"Error: Required directory '{dir_name}'",
                  f"not found in {rootdir}")
            sys.exit(1)

def build_submission(script_dir: Path, remote_be: bool):
    """
    Build the submission, including pulling dependencies as neeed
    """
    if remote_be:
        subprocess.run([sys.executable, "-m", "pip", "install", "-r", "./submission_remote/requirements.txt"], check=True)
    else:
        # Build the niobium-client submodule, which builds its bundled
        # OpenFHE (under vendor/lib/openfhe), the fhetch library, and
        # the client examples. We use this OpenFHE for the submission
        # build instead of scripts/get_openfhe.sh.
        client_dir = script_dir.parent / "submission" / "niobium-client"
        subprocess.run(["make", "-C", str(client_dir), "release"], check=True)
        # CMake build of the submission itself, pointed at the OpenFHE
        # that niobium-client just installed.
        openfhe_prefix = client_dir / "vendor" / "lib" / "openfhe"
        subprocess.run([script_dir/"build_task.sh", "./submission", str(openfhe_prefix)], check=True)


class TextFormat:
    BOLD = "\033[1m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    BLUE = "\033[34m"
    RED = "\033[31m"
    PURPLE = "\033[35m"
    RESET = "\033[0m"

def log_step(step_num: float, step_name: str, start: bool = False):
    """ 
    Helper function to print timestamp after each step with second precision 
    """
    # step_num is float to allow Step 1.1 and similar
    global _last_timestamp
    global _timestamps
    global _timestampsStr
    now = datetime.now()
    # Format with milliseconds precision
    timestamp = now.strftime("%H:%M:%S")

    # Calculate elapsed time if this isn't the first call
    elapsed_str = ""
    elapsed_seconds = 0
    if _last_timestamp is not None:
        elapsed_seconds = (now - _last_timestamp).total_seconds()
        elapsed_str = f" (elapsed: {round(elapsed_seconds, 4)}s)"

    # Update the last timestamp for the next call
    _last_timestamp = now

    if not start:
        print(f"{TextFormat.BLUE}{timestamp} [harness] {step_num}: {step_name} completed{elapsed_str}{TextFormat.RESET}")
        _timestampsStr[step_name] = f"{round(elapsed_seconds, 4)}s"
        _timestamps[step_name] = elapsed_seconds

def log_size(path: Path, object_name: str, flag: bool = False, previous: int = 0):
    """Measure the size of a directory or file on disk
    """
    global _bandwidth

    if not path.exists():
        size = 0
    elif platform.system() == "Darwin":  # macOS
        # Use -s for summary and multiply by 1024 since macOS du reports in 1K blocks by default
        result = subprocess.run(["du", "-sk", path], check=True,
                               capture_output=True, text=True)
        size = int(result.stdout.split()[0]) * 1024
    else:
        # Linux/other systems support -b flag
        size = int(subprocess.run(["du", "-sb", path], check=True,
                           capture_output=True, text=True).stdout.split()[0])
    if flag:
        size -= previous

    print(f"{TextFormat.YELLOW}         [harness] {object_name} size: {human_readable_size(size)}{TextFormat.RESET}")

    _bandwidth[object_name] = human_readable_size(size)
    return size

def human_readable_size(n: int):
    """Pretty print for size in bytes"""
    n_float : float = n
    for unit in ["B","K","M","G","T"]:
        if n_float < 1024:
            return f"{n_float:.1f}{unit}"
        n_float /= 1024
    return f"{n_float:.1f}P"

def save_run(path: Path, submission_report_path: Path):
    """Save the timing from the current run to disk"""
    global _timestamps
    global _timestampsStr
    global _bandwidth

    _timestampsStr["Total"] = f"{round(sum(_timestamps.values()), 4)}s"

    _timestampsRemote = {}
    if submission_report_path.exists():
        with open(submission_report_path, "r") as f:
            server_reported_times = json.load(f)
            for step_name, time_str in server_reported_times.items():
                _timestampsRemote[step_name] = f"{time_str}s"
                print(f"{TextFormat.PURPLE}         [submission] {step_name}: {time_str}s{TextFormat.RESET}")

    json.dump({
        "Timing": _timestampsStr,
        "Bandwidth": _bandwidth,
        "Server Reported": _timestampsRemote,
    }, open(path,"w"), indent=2)

    print("[total latency]", f"{round(sum(_timestamps.values()), 4)}s")

def run_exe_or_python(base, file_name, *args, check=True):
    """
        If {base}/{file_name}.py exists, run it with the current Python.
        Otherwise, run {base}/build/{file_name} as an executable.
    """
    py =  base / f"{file_name}.py"
    exe = base / "build" / file_name
    env = os.environ.copy()

    if py.exists():
        env["PYTHONPATH"] = "."
        cmd = [sys.executable, py, *args]
    elif exe.exists():
        cmd = [exe, *args]
    else:
        cmd = None
    if cmd is not None:
        subprocess.run(cmd, check=check, env=env)