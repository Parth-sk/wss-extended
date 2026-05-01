#!/usr/bin/env python3
"""
analyze.py - Semantic Memory Footprint Profiler
Joins WSS interval data with perf script samples to attribute
active memory pages to call stacks.

Usage:
    python3 analyze.py <wss_output_file> <perf_script_file> [--top N]

WSS output format (from wss-v1-extended):
    Interval     Start(s)       End(s)    Ref(MB)      Pages
    0         2256.036987  2258.866793       1.09        279
      PAGE 0x7ffd8d58c000
      PAGE 0x7564e1c02000
      ...

Perf script format (from perf script -F time,addr,period,ip,sym,callindent):
    1098.729072:         16                0
            ffffffffba6e0718 native_sched_clock
            ...
    31684.806593:     480068     7ffd8d58c848
            74d0ee0f655f __ieee754_logl
            56ac70c8d60c main
            ...
"""

import sys
import re
from collections import defaultdict


# ─────────────────────────────────────────────
# CONSTANTS
# ─────────────────────────────────────────────

PAGE_SIZE = 4096
PAGE_MASK = ~(PAGE_SIZE - 1)

# user-space addresses on x86_64 are below this value
# anything above is kernel space - discard
USER_SPACE_MAX = 0x0000800000000000

# maximum call stack depth to record
# deeper frames are noise and inflate dict keys
MAX_STACK_DEPTH = 7

# default number of top stacks to print per interval
DEFAULT_TOP_N = 5


# ─────────────────────────────────────────────
# DATA STRUCTURES
# ─────────────────────────────────────────────

class Interval:
    """
    One WSS measurement window.
    start, end: CLOCK_MONOTONIC timestamps in seconds (float)
    pages: set of virtual page base addresses (page-aligned, integer)
    ref_mb: referenced megabytes in this interval
    page_count: number of active pages
    """
    def __init__(self, index, start, end, ref_mb, page_count):
        self.index = index
        self.start = start
        self.end = end
        self.ref_mb = ref_mb
        self.page_count = page_count
        self.pages = set()  # populated by parse_wss_output()


class PerfSample:
    """
    One IBS sample from perf script output.
    timestamp: seconds (float), matches CLOCK_MONOTONIC
    addr: virtual address accessed (integer), 0 if not a memory op
    period: weight - number of ops this sample represents (integer)
    stack: tuple of symbol strings, shallowest frame first
           stored as tuple so it is hashable for use as dict key
    """
    def __init__(self, timestamp, addr, period, stack):
        self.timestamp = timestamp
        self.addr = addr
        self.period = period
        self.stack = stack


# ─────────────────────────────────────────────
# PARSING: WSS OUTPUT
# ─────────────────────────────────────────────

def parse_wss_output(filename):
    """
    Parse WSS output file into a list of Interval objects.

    Expected format:
        Interval     Start(s)       End(s)    Ref(MB)      Pages
        0         2256.036987  2258.866793       1.09        279
          PAGE 0x7ffd8d58c000
          PAGE 0x7564e1c02000

    Design decision: PAGE lines are hex virtual addresses stored as integers.
    The address is already page-aligned because WSS stores `p` which
    increments by pagesize. No masking needed here.
    """
    intervals = []
    current = None

    # matches summary line: 0   2256.036987  2258.866793   1.09   279
    interval_re = re.compile(
        r'^\s*(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)'
    )
    # matches page line: PAGE 0x7ffd8d58c000
    page_re = re.compile(r'^\s*PAGE\s+(0x[0-9a-fA-F]+)')

    with open(filename, 'r') as f:
        for line in f:
            m = interval_re.match(line)
            if m:
                current = Interval(
                    index=int(m.group(1)),
                    start=float(m.group(2)),
                    end=float(m.group(3)),
                    ref_mb=float(m.group(4)),
                    page_count=int(m.group(5))
                )
                intervals.append(current)
                continue

            m = page_re.match(line)
            if m and current is not None:
                current.pages.add(int(m.group(1), 16)) # has the entire address: 0x7564e1c02000

    return intervals  


# ─────────────────────────────────────────────
# PARSING: PERF SCRIPT OUTPUT
# ─────────────────────────────────────────────

def parse_perf_script(filename):
    """
    Parse perf script output into a list of PerfSample objects.

    Expected format from:
        perf script -F time,addr,period,ip,sym,callindent

    Header line:
        31684.806593:     480068     7ffd8d58c848
    Call stack lines (indented):
            74d0ee0f655f __ieee754_logl
            56ac70c8d60c main

    Design decisions:

    1. addr=0 samples are discarded immediately.
       These are IBS samples that fired on non-memory instructions.
       They carry no address information and cannot be matched to pages.

    2. Kernel addresses are discarded (addr >= USER_SPACE_MAX).
       AMD IBS cannot filter to user-space at collection time,
       so we filter here. Kernel addresses begin at 0x0000800000000000
       on x86_64.

    3. Call stack frames that are kernel symbols are also discarded.
       We only want user-space attribution. Kernel frames appear as
       addresses >= USER_SPACE_MAX or in [kernel.kallsyms].

    4. Stack depth is capped at MAX_STACK_DEPTH frames.
       Deep stacks inflate the key space of the attribution dict
       and add noise. The top 5 frames capture the meaningful context.

    5. Stack is stored as a tuple (hashable) for use as dict key.
       Frames are ordered shallowest-first (innermost call first).

    6. period is the IBS sampling weight - the number of ops between
       the previous sample and this one. Used for weighted attribution
       so that samples representing more work count more.
    """
    samples = []

    # matches header: 31684.806593:     480068     7ffd8d58c848
    # groups: (timestamp, period, addr)
    header_re = re.compile(
        r'^\s*([\d.]+):\s+(\d+)\s+([0-9a-fA-F]+)\s*$'
    )

    # matches call stack frame (indented line with hex addr and symbol)
    frame_re = re.compile(
        r'^\s+([0-9a-fA-F]+)\s+(.+?)(?:\s+\(.*\))?\s*$'
    )

    current_timestamp = None
    current_addr = None
    current_period = None
    current_stack = []

    def flush():
        """Emit current sample if it has a valid user-space addr."""
        if current_addr is None:
            return
        if current_addr == 0:
            return
        if current_addr >= USER_SPACE_MAX:
            return
        # filter stack to user-space frames only, cap depth
        user_frames = []
        for (ip, sym) in current_stack:
            if ip < USER_SPACE_MAX and '[kernel' not in sym:
                user_frames.append(sym)
        user_frames = user_frames[:MAX_STACK_DEPTH]
        if not user_frames:
            return
        samples.append(PerfSample(
            timestamp=current_timestamp,
            addr=current_addr,
            period=current_period,
            stack=tuple(user_frames)
        ))

    with open(filename, 'r') as f:
        for line in f:
            m = header_re.match(line)
            if m:
                # new sample starts - flush previous
                flush()
                current_timestamp = float(m.group(1))
                current_period = int(m.group(2))
                addr_str = m.group(3)
                current_addr = int(addr_str, 16)
                current_stack = []
                continue

            m = frame_re.match(line)
            if m and current_timestamp is not None:
                ip = int(m.group(1), 16)
                sym = m.group(2).strip()
                current_stack.append((ip, sym))

    # flush last sample
    flush()

    return samples


# ─────────────────────────────────────────────
# JOIN: WSS INTERVALS × PERF SAMPLES
# ─────────────────────────────────────────────

def join(intervals, samples, top_n):
    """
    For each WSS interval, find all perf samples whose:
      1. timestamp falls within [interval.start, interval.end]
      2. page (addr & PAGE_MASK) is in interval.pages

    Attribute weighted sum to call stack.

    Design decision: attribution is per-interval, not per-page.
    We aggregate all matching samples across all active pages in
    the window into a single call_stack → weight_sum mapping.
    This answers: "what code was responsible for the active working
    set in this time window?" which is the core profiler question.

    Per-page attribution is a possible extension but produces much
    more output and is harder to interpret.

    Design decision: samples are sorted by timestamp once before
    joining, then a simple linear scan with early termination is
    used per interval. This is O(S log S + I*S) in the worst case
    but in practice each interval covers a small slice of samples.
    """

    # sort samples by timestamp once for efficient windowing
    ### MIGHT NOT BE NECESSARY
    #samples.sort(key=lambda s: s.timestamp)

    results = []


    for interval in intervals:
        # weighted attribution: stack -> total weight
        attribution = defaultdict(int)
        total_matched_weight = 0
        matched_sample_count = 0

        for sample in samples:
            # skip samples before this window
            if sample.timestamp < interval.start:
                continue
            # stop once past this window
            if sample.timestamp > interval.end:
                break

            # page-align the address
            page = sample.addr & PAGE_MASK

            # check if this page was active in this WSS window
            if page in interval.pages:
                attribution[sample.stack] += sample.period
                total_matched_weight += sample.period
                matched_sample_count += 1

        results.append({
            'interval': interval,
            'attribution': attribution,
            'total_weight': total_matched_weight,
            'matched_samples': matched_sample_count
        })

    return results


# ─────────────────────────────────────────────
# OUTPUT
# ─────────────────────────────────────────────

def print_results(results, top_n):
    """
    Print per-interval summary with top N attributed call stacks.

    Weight percentage is relative to total matched weight in the
    interval so percentages sum to 100% across shown stacks
    (modulo rounding and stacks below top_n cutoff).
    """
    for r in results:
        interval = r['interval']
        attribution = r['attribution']
        total_weight = r['total_weight']
        matched_samples = r['matched_samples']

        print("=" * 70)
        print(f"Interval {interval.index}")
        print(f"  Start:          {interval.start:.6f} s")
        print(f"  End:            {interval.end:.6f} s")
        print(f"  Ref(MB):        {interval.ref_mb:.2f}")
        print(f"  Active pages:   {interval.page_count}")
        print(f"  Matched samples:{matched_samples}")
        print(f"  Total weight:   {total_weight}")
        print()

        if not attribution:
            print("  No matching perf samples in this interval.")
            print("  (Either no perf samples fall in this time window,")
            print("   or no sampled addresses match active WSS pages.)")
            print()
            continue

        # sort by weight descending
        ranked = sorted(attribution.items(), key=lambda x: x[1], reverse=True)

        print(f"  Top {top_n} call stacks by weight:")
        print()
        for rank, (stack, weight) in enumerate(ranked[:top_n], 1):
            pct = (weight / total_weight * 100) if total_weight > 0 else 0
            print(f"  #{rank}  weight={weight:>12,}  ({pct:5.1f}%)")
            for frame in stack:
                print(f"         {frame}")
            print()


# ─────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 analyze.py <wss_output> <perf_script_output> [--top N]")
        print()
        print("  wss_output:         output file from wss-v1-extended")
        print("  perf_script_output: output of perf script -F time,addr,period,ip,sym,callindent")
        print("  --top N:            show top N call stacks per interval (default 5)")
        sys.exit(1)

    wss_file = sys.argv[1]
    perf_file = sys.argv[2]
    top_n = DEFAULT_TOP_N

    for i, arg in enumerate(sys.argv):
        if arg == '--top' and i + 1 < len(sys.argv):
            top_n = int(sys.argv[i + 1])

    print(f"Parsing WSS output:  {wss_file}")
    intervals = parse_wss_output(wss_file)
    print(f"  {len(intervals)} intervals loaded")

    print(f"Parsing perf script: {perf_file}")
    samples = parse_perf_script(perf_file)
    print(f"  {len(samples)} valid user-space memory samples loaded")
    print()

    results = join(intervals, samples, top_n)
    print_results(results, top_n)


if __name__ == '__main__':
    main()




