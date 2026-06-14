#!/usr/bin/env python3
"""
R# Integration Test Runner
Runs all .rsl files in the integration test directory via the interpreter.
Usage: python3 run_tests.py [--dir <dir>] [--rsc <path>]
"""
import os, sys, subprocess, argparse, glob, time

def run_test(test_file: str, rsc: str, check_only: bool) -> tuple[bool, str]:
    start = time.time()
    try:
        if check_only:
            cmd = [rsc, test_file, "--check"]
        else:
            cmd = [rsc, test_file, "--check"]  # check-only until LLVM backend is wired
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        elapsed = time.time() - start
        ok = result.returncode == 0
        msg = f"{'PASS' if ok else 'FAIL'} ({elapsed:.2f}s)"
        if not ok:
            msg += f"\n  stdout: {result.stdout.strip()}"
            msg += f"\n  stderr: {result.stderr.strip()}"
        return ok, msg
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT"
    except FileNotFoundError:
        return False, f"rsc not found: {rsc}"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", default=os.path.dirname(__file__))
    parser.add_argument("--rsc", default="rsc")
    parser.add_argument("--check-only", action="store_true", default=True)
    args = parser.parse_args()

    test_files = sorted(glob.glob(os.path.join(args.dir, "*.rsl")) +
                        glob.glob(os.path.join(args.dir, "*.rss")))

    if not test_files:
        print("No integration test files found.")
        return 0

    passed = failed = 0
    for tf in test_files:
        name = os.path.basename(tf)
        ok, msg = run_test(tf, args.rsc, args.check_only)
        status = "\033[32mPASS\033[0m" if ok else "\033[31mFAIL\033[0m"
        print(f"  {status}  {name:40s}  {msg.split(chr(10))[0]}")
        if ok: passed += 1
        else:  failed += 1

    print(f"\n{passed}/{passed+failed} tests passed")
    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
