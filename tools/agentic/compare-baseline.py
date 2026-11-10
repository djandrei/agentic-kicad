#!/usr/bin/env python3
#
# Agentic-KiCad: record and compare upstream test results against doc/agentic/baseline-tests.txt.
#
#   compare-baseline.py record  QA_DIR OUTPUT [CTEST_ARGS...]
#       Run ctest in QA_DIR (the qa directory of a build) and write the baseline: one line per
#       ctest test with its status and time, followed by the failing Boost test cases of each
#       failing test.
#
#   compare-baseline.py compare BASELINE QA_DIR SUITE
#       Run the ctest tests whose names start with SUITE in QA_DIR and fail when a Boost test case fails that is not
#       listed as failing for SUITE in BASELINE, or when SUITE crashed without naming a case
#       and the baseline does not record that suite as failing.
#
# Baseline format:
#   PASS <test> <seconds>
#   FAIL <test> <seconds>
#     case <Suite/Case>
#
# Standard library only.

import re
import subprocess
import sys

CASE_RE = re.compile( r'(?:fatal )?error: in "([^"]+)"' )
RESULT_RE = re.compile( r'^\s*\d+/\d+ Test\s+#\d+: (\S+) \.+\s*(\*\*\*)?\s*(\S.*?)\s+([\d.]+) sec' )


def run_ctest( qa_dir, args ):
    proc = subprocess.run( [ "ctest", "--output-on-failure" ] + args, cwd=qa_dir,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           errors="replace" )
    return proc.stdout


def parse( output ):
    """Return {test: (status, seconds, set(failing cases))} from ctest --output-on-failure."""
    results = {}
    current = None
    cases = {}

    for line in output.splitlines():
        m = RESULT_RE.match( line )
        if m:
            name, status, seconds = m.group( 1 ), m.group( 3 ), float( m.group( 4 ) )
            results[name] = ( "PASS" if status == "Passed" else "FAIL", seconds )
            current = name
            cases.setdefault( name, set() )
            continue

        if current is not None:
            for case in CASE_RE.findall( line ):
                cases[current].add( case )

    return { name: ( status, seconds, cases.get( name, set() ) )
             for name, ( status, seconds ) in results.items() }


def read_baseline( path ):
    baseline = {}
    current = None

    with open( path, encoding="utf-8" ) as f:
        for raw in f:
            line = raw.rstrip( "\n" )

            if not line or line.startswith( "#" ):
                continue

            if line.startswith( "  case " ):
                baseline[current][1].add( line[len( "  case " ):] )
                continue

            status, name = line.split()[:2]
            baseline[name] = ( status, set() )
            current = name

    return baseline


def record( qa_dir, output_path, ctest_args ):
    results = parse( run_ctest( qa_dir, ctest_args ) )

    with open( output_path, "w", encoding="utf-8" ) as f:
        f.write( "# Upstream ctest baseline (tools/agentic/compare-baseline.py record)\n" )
        f.write( "# status test seconds; failing Boost cases indented below a FAIL\n" )

        for name in sorted( results ):
            status, seconds, cases = results[name]
            f.write( f"{status} {name} {seconds:.2f}\n" )

            if status == "FAIL":
                for case in sorted( cases ):
                    f.write( f"  case {case}\n" )

    failed = sum( 1 for r in results.values() if r[0] == "FAIL" )
    print( f"recorded {len( results )} tests, {failed} failing, in {output_path}" )
    return 0


def compare( baseline_path, qa_dir, suite ):
    """Run every ctest test whose name starts with SUITE (qa_pcbnew is split into
    qa_pcbnew_<group> tests) and check each against the baseline."""
    baseline = read_baseline( baseline_path )
    results = parse( run_ctest( qa_dir, [ "-R", f"^{re.escape( suite )}" ] ) )

    if not results:
        print( f"{suite}: not run (no such ctest test)", file=sys.stderr )
        return 1

    regressions = 0

    for name in sorted( results ):
        status, _, cases = results[name]

        if status == "PASS":
            print( f"{name}: passed" )
            continue

        base_status, base_cases = baseline.get( name, ( "PASS", set() ) )
        new_cases = sorted( cases - base_cases )

        if new_cases:
            print( f"{name}: cases failing that passed in the baseline:", file=sys.stderr )

            for case in new_cases:
                print( f"  {case}", file=sys.stderr )

            regressions += 1
        elif not cases and base_status != "FAIL":
            print( f"{name}: failed without naming a case and passed in the baseline",
                   file=sys.stderr )
            regressions += 1
        else:
            print( f"{name}: only baseline failures ({len( cases )} cases)" )

    return 1 if regressions else 0


def main( argv ):
    if len( argv ) >= 3 and argv[1] == "record":
        return record( argv[2], argv[3], argv[4:] )

    if len( argv ) == 5 and argv[1] == "compare":
        return compare( argv[2], argv[3], argv[4] )

    print( __doc__ or "usage: compare-baseline.py record QA_DIR OUTPUT [CTEST_ARGS...] | "
           "compare BASELINE QA_DIR SUITE", file=sys.stderr )
    return 2


if __name__ == "__main__":
    sys.exit( main( sys.argv ) )
