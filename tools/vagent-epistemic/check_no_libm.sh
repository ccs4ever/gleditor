#!/bin/sh
# The reference must not import a floating-point math library, call a floating exp/log/sqrt, or use a
# random-number method whose stream changed across Python releases (only random() and uniform() are
# stable). Run from any directory: the reference sits beside this script.
ref="$(dirname "$0")/vagent_epistemic_reference.py"
if grep -nE '^[[:space:]]*(import|from)[[:space:]]+(math|numpy|cmath|scipy)\b|^[[:space:]]*import[[:space:]]+.*\b(math|numpy|cmath|scipy)\b|\bnp\.|\bmath\.' "$ref"; then
  echo "FAIL: forbidden floating-point library in the reference"
  exit 1
fi
if grep -nE '\brng\.(choice|choices|shuffle|randrange|randint|sample|gauss|normalvariate)\b|\brandom\.(choice|shuffle|randrange|randint|sample|gauss)\b' "$ref"; then
  echo "FAIL: random method with a release-dependent stream in the reference"
  exit 1
fi
echo "ok: reference imports only mpmath, random, sys, hashlib; draws only random() and uniform()"
