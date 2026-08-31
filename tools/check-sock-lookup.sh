#!/bin/sh
# check-sock-lookup.sh -- every sock_lookup call site must be classified.
#
# WHY THIS EXISTS:
#
# sock_lookup() turns a client-supplied descriptor into a SOCKCB. It validates
# the table index and the slot generation -- and NOTHING about who is asking.
# Until M5-2d1 that was the whole of the story on two paths at once:
#
#   src/nsfreq.c  req_socket()  was sock_lookup(r->sockdesc) and nothing else
#   src/nsfsel.c  sel_scan()    resolved N descriptors from a client's SELECT
#                               mask and went nowhere near req_socket
#
# so an unauthorised client in another address space could drive another
# client's sockets. The descriptor is not merely forgeable but GUESSABLE: the
# socket table is one array, sock_alloc hands out the first free index
# regardless of which app is asking, and the descriptor is (gen<<16)|idx with
# idx in 0..63 -- so on a fresh STC the first descriptors are small integers a
# client can calibrate against its own.
#
# THE POINT IS NOT THAT THE CHECK WAS MISSING. It is that NOBODY SAW sel_scan.
# One call site was the obvious one and got all the attention; the other
# resolved more descriptors per request than the first and was invisible. A
# rule that lives only in review notes gets forgotten the same way, so this
# script makes a NEW, unclassified caller break the build until someone
# decides which kind it is.
#
# HOW TO SATISFY IT: put one of these markers on the call line or the line
# above it.
#
#   SOCK_LOOKUP: CHECKED  <why>   ownership is enforced here (or by the
#                                 function this call is the body of)
#   SOCK_LOOKUP: INTERNAL <why>   not a client-directed resolution -- the
#                                 stack is resolving a socket it already owns
#
# Do not add a marker to silence the script. The marker is a claim, and the
# reason is the part a reviewer reads.
#
# SCOPE IS src/ AND include/ -- PRODUCTION CODE ONLY, and that is a decision
# rather than an oversight. Tests resolve descriptors directly all the time
# (22 call sites across four files) precisely because they are probing the
# table; they are not a client-facing surface and marking them would be churn
# that teaches people to add the marker without reading it. Tests therefore
# carry no marker AT ALL, so nobody can mistake one for guard coverage.
#
# Runs before the toolchain: needs nothing installed and answers in a second.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

rc=0
found=0

# Definition and declaration are not call sites. Match a call: an identifier
# immediately followed by '(' that is not preceded by another identifier char.
for f in src/*.c include/*.h; do
    [ -f "$f" ] || continue
    while IFS=: read -r line text; do
        [ -n "${line:-}" ] || continue

        case $text in
            *"SOCKCB *sock_lookup"*) continue ;;   # the definition
            *"sock_lookup(UINT desc)"*) continue ;; # the declaration
        esac

        found=$((found + 1))

        prev=$((line - 1))
        ctx=$(sed -n "${prev},${line}p" "$f")

        case $ctx in
            *"SOCK_LOOKUP: CHECKED"*|*"SOCK_LOOKUP: INTERNAL"*) ;;
            *)
                echo "$f:$line: UNCLASSIFIED sock_lookup call site"
                echo "    $(echo "$text" | sed 's/^[[:space:]]*//')"
                echo "    add 'SOCK_LOOKUP: CHECKED <why>' or"
                echo "        'SOCK_LOOKUP: INTERNAL <why>' on this line or the one above"
                rc=1
                ;;
        esac
    done <<EOF
$(grep -n '[^A-Za-z0-9_]sock_lookup *(' "$f" 2>/dev/null)
EOF
done

# A POSITIVE CONTROL ON THE SCRIPT ITSELF. An empty result must mean "every
# call site is classified", never "the pattern matched nothing" -- a broken
# grep and a clean tree look identical, and that is the failure class this
# whole family of checks exists to prevent (CLAUDE.md 8.5).
if [ "$found" -eq 0 ]; then
    echo "check-sock-lookup.sh: found NO sock_lookup call sites at all."
    echo "    That is not credible -- req_socket and sel_scan both resolve"
    echo "    descriptors. The search is broken, not the tree."
    exit 2
fi

if [ "$rc" -eq 0 ]; then
    echo "check-sock-lookup.sh: $found call site(s), all classified."
fi

exit $rc
