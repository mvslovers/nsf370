#!/usr/bin/env python3
"""Every source card must appear in the as370 listing, in source order.

This is the direct check against a column-72 continuation merge swallowing a
card (CLAUDE.md 3): reading the assembler's own output beats deriving which
card ate which.  Matched as an in-order SUBSEQUENCE, not 1:1 -- macro expansion
makes listing statement numbers not correspond to source line numbers.

The listing puts the source card at a fixed column; CARDCOL is verified below
against a card we know is there, so a listing-format change fails loudly
instead of matching nothing and reporting success.
"""
import sys

CARDCOL = 40          # 0-based index at which the source card begins

src_path, lst_path = sys.argv[1], sys.argv[2]
src = [l.rstrip() for l in open(src_path, encoding='utf-8')]
lst_raw = [l.rstrip('\n') for l in open(lst_path, encoding='utf-8',
                                       errors='replace')]
lst = [l[CARDCOL:].rstrip() for l in lst_raw if len(l) > CARDCOL]

# POSITIVE CONTROL on the parse itself: the CSECT card must be in there.  A
# wrong CARDCOL yields an all-blank card list, which would report every source
# line missing -- loud -- but a subtly wrong one could still match, so anchor
# on a card whose text is unmistakable.
if not any(c.startswith('NSFVSVC  CSECT') for c in lst):
    print("PARSE CONTROL FAILED -- no 'NSFVSVC  CSECT' card at column %d; "
          "the listing format changed and this check is not reading cards"
          % (CARDCOL + 1))
    sys.exit(2)

i = 0
missing = []
for n, card in enumerate(src, 1):
    if not card.strip():
        continue
    j = i
    while j < len(lst) and lst[j] != card:
        j += 1
    if j < len(lst):
        i = j + 1
    else:
        missing.append((n, card))

if missing:
    print("STATEMENT CHECK FAILED -- %d card(s) not present in source order:"
          % len(missing))
    for n, card in missing[:20]:
        print("  line %d: %s" % (n, card))
    sys.exit(1)
print("statement check OK -- all %d source cards present in the listing, "
      "in source order" % sum(1 for l in src if l.strip()))
