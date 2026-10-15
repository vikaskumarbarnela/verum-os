"""primes.py - the plain-Python twin of cells/abhajya.sv.

Counts the primes below a limit and sums them, by trial division over every
smaller number (deliberately the same naive algorithm as the Sanskrit cell, so
the comparison is language-to-language, not algorithm-to-algorithm).
"""

import sys


def divides(n: int, d: int) -> bool:
    return n % d == 0


def is_prime(n: int) -> bool:
    return n > 1 and not any(divides(n, d) for d in range(2, n))


def main(limit: int) -> tuple[int, int]:
    primes = [n for n in range(limit) if is_prime(n)]
    return len(primes), sum(primes)


if __name__ == "__main__":
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    count, total = main(limit)
    print(f"limit {limit} primes {count} sum {total}")
