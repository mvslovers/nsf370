#!/bin/sh
# check-card-columns.sh -- no assembler card may reach column 72.
#
# WHY THIS EXISTS, since a green build no longer says it:
#
# IFOX00 applies the column-72 continuation rule to COMMENT statements as well
# as instructions, so a card reaching column 72 CONSUMES THE CARD AFTER IT.
# as370 agrees since mvslovers/cc370#72 and reports IFO026/IFO069 at severity 4.
#
# Severity 4 is a warning, and a warned assembly still punches its deck -- so
# since mbt#94 the build rule keeps RC < 8 and make sees exit 0. That is correct
# for warnings in general and it means this particular defect is SILENT again:
# the object is built, the card is missing from it, and nothing complains.
#
# Measured on 2026-08-28, both directions:
#   asm/nsfctcio.asm line 95, a 74-byte comment, swallowed the next line's
#   DCBD DSORG=PS. Every DCB DSECT symbol then resolved to 0, so
#   MVC DCBDDNAM-IHADCB(8,R2),0(R3) assembled as MVC 0(8,R2),0(R3) and
#   TM DCBOFLGS-IHADCB(R2),DCBOFOPN as TM 0(R2),X'00'.
#   asm/nsfvsvc.asm dropped L R3,REQFUNC(,R8), the SVC dispatch value.
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
    echo "Each one silently eats the card after it (IFO026/IFO069, severity 4,"
    echo "which does NOT fail the build). Keep every card inside column 71."
    exit 1
fi

echo "check-card-columns: OK -- every card is inside column 71"
