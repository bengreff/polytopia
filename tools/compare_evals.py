"""Paired comparison of two checkpoints' evaluations on the same seeds (eval_vs_bot.py --out files).

Games are matched by (seed, agent seat): same map, tribes and side. Files are paired in order. Only the games one checkpoint won
and the other didn't carry information; a two-sided sign test on them gives the p-value. Several
pairs of files (one per difficulty) can be pooled: pass them as A1,A2 B1,B2.

    python3 tools/compare_evals.py runs/evals/r4_5k_d1.tsv,runs/evals/r4_5k_d3.tsv runs/evals/r3_100k_d1.tsv,runs/evals/r3_100k_d3.tsv
"""
import math
import sys


def read(paths):
    """{(file position, seed, seat): won} over comma-separated files."""
    out = {}
    for i, path in enumerate(paths.split(',')):
        for line in open(path):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 5:
                out[(i, f[0], f[1])] = f[4] == 'win'
    return out


def sign_test(a, b):
    """Two-sided exact binomial test of a successes in a + b trials at p = 1/2."""
    n = a + b
    if n == 0:
        return 1.0
    k = min(a, b)
    p = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * p)


def main():
    A, B = read(sys.argv[1]), read(sys.argv[2])
    pairs = [(A[k], B[k]) for k in A if k in B]
    n = len(pairs)
    wa = sum(a for a, _ in pairs)
    wb = sum(b for _, b in pairs)
    only_a = sum(a and not b for a, b in pairs)
    only_b = sum(b and not a for a, b in pairs)
    print(f'{n} paired games: A won {wa} ({wa / max(n, 1):.1%}), B won {wb} ({wb / max(n, 1):.1%}); '
          f'only A won {only_a}, only B won {only_b}; sign test p = {sign_test(only_a, only_b):.3g}')


if __name__ == '__main__':
    main()
