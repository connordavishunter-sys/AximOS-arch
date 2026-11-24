"""Runs tests/*_test.py, or the ones named, several at once. The slowest go first,
going by how long each took last time, so a long one doesn't start last."""
import argparse, glob, json, os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
TIMES = os.path.join(HERE, '..', 'build', 'test-times.json')

parser = argparse.ArgumentParser()
parser.add_argument('-j', '--jobs', type=int, default=max(1, min(6, os.cpu_count() // 4)))
parser.add_argument('tests', nargs='*')
args = parser.parse_args()

try:
    times = json.load(open(TIMES))
except (OSError, ValueError):
    times = {}
tests = [os.path.basename(t) for t in args.tests] or sorted(map(os.path.basename, glob.glob(HERE + '/*_test.py')))
tests.sort(key=lambda t: -times.get(t, 1e9))


def run(test):
    start = time.time()
    r = subprocess.run([sys.executable, os.path.join(HERE, test)], capture_output=True, text=True)
    return test, r.returncode, r.stdout + r.stderr, time.time() - start


start, failed = time.time(), []
with ThreadPoolExecutor(args.jobs) as pool:
    for done in as_completed([pool.submit(run, t) for t in tests]):
        test, code, out, secs = done.result()
        times[test] = round(secs, 1)
        if code:
            failed.append(test)
            print(f'== {test} FAILED in {secs:.0f}s\n{out}', flush=True)
        else:
            print(f'== {test} ok in {secs:.0f}s, {out.count("PASS ")} checks', flush=True)

json.dump(times, open(TIMES, 'w'), indent=1, sort_keys=True)
print(f'{len(tests) - len(failed)}/{len(tests)} passed in {time.time() - start:.0f}s'
      + (f', failed: {" ".join(sorted(failed))}' if failed else ''))
sys.exit(bool(failed))
