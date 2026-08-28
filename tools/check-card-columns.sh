#!/bin/sh
# check-card-columns.sh -- no assembler card may reach column 72.
#
# WHY THIS EXISTS, since a green build no longer says it:
#
# IFOX00 applies the column-72 continuation rule to COMMENT statements as well
# as instructions, so a card reaching column 72 CONSUMES THE CARD AFTER IT.
# as370 agrees since mvslovers/cc370#72 and reports IFO026/IFO069 at severity 4.
#
# Since cc370#84 the assembler sorts the two cases, and this script is no longer
# the only thing standing between us and a silent miss: an eaten COMMENT stays
# severity 4 (it costs nothing), an eaten STATEMENT is raised to RC 8 and stops
# the build. So why keep this?
#
#   - it runs BEFORE the toolchain, so it answers in seconds and needs nothing
#     installed;
#   - it sees sources the assembler is never asked to read;
#   - it reports the harmless cards too, which is a house-style call the
#     assembler deliberately does not make for us;
#   - IFOX does not warn AT ALL when the eaten card's columns 1-15 are blank,
#     so a macro operand card can still vanish without a diagnostic.
#
# Measured on 2026-08-28, both directions:
#   asm/nsfctcio.asm line 95, a 74-byte comment, swallowed the next line's
#   DCBD DSORG=PS. Every DCB DSECT symbol then resolved to 0, so
#   MVC DCBDDNAM-IHADCB(8,R2),0(R3) assembled as MVC 0(8,R2),0(R3) and
#   TM DCBOFLGS-IHADCB(R2),DCBOFOPN as TM 0(R2),X'00'.
#   asm/nsfvsvc.asm lost FOUR cards -- ANCVERNO EQU 3 (its own anchor-version
#   check then compares against 0), ASCBASID EQU 36 (the caller ASID is read
#   from ASCB+0), L R3,REQFUNC(,R8), and DOUNSTG DS 0H, which turns
#   BE DOUNSTG into 4780 0000: a branch to address 0, supervisor state key 0.
#
# AND THE CHAINS ARE LONGER THAN ONE CARD. 573 and 574 are both overlong, so
# 573 continues onto 574 and 574 onto 575 -- the statement. A card-by-card
# reading stops at 574, sees a comment, and reports nothing lost. That is why
# this script flags the CARD and leaves naming the victim to the assembler.
#
# LENGTH IS IN BYTES, NOT CHARACTERS -- as370 reads bytes, and a UTF-8 'S'
# (U+00A7) costs two of them. Several cards measured 73 while looking like 72.
# LC_ALL=C makes awk's length() count bytes.
#
# Usage: tools/check-card-columns.sh [file...]   (default: asm/ and test/asm/)
set -eu

if [ "$#" -gt 0 ]; then
    files="$*"
else
    files=$(find asm test/asm -name '*.asm' -o -name '*.s' 2>/dev/null | sort)
fi

[ -n "$files" ] || { echo "check-card-columns: no assembler sources found" >&2; exit 2; }

bad=$(LC_ALL=C awk 'length($0) >= 72 { printf "%s:%d: %d bytes\n", FILENAME, FNR, length($0) }' $files)

if [ -n "$bad" ]; then
    echo "$bad"
    n=$(printf '%s\n' "$bad" | wc -l | tr -d ' ')
    echo ""
    echo "check-card-columns: FAILED -- $n card(s) reach column 72."
    echo "Each one eats the card after it. If that card carries a statement the"
    echo "assembler stops the build (RC 8, cc370#84); if it is a comment it is"
    echo "only a warning, and if its columns 1-15 are blank IFOX says nothing at"
    echo "all. Keep every card inside column 71."
    exit 1
fi

echo "check-card-columns: OK -- every card is inside column 71"
