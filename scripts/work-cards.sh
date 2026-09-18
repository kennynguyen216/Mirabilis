#!/usr/bin/env bash
# Work the Lumen-lite GI board, one fresh Claude session per card.
#
#   bash scripts/work-cards.sh --dry-run   # show what it would do, change nothing
#   bash scripts/work-cards.sh --once      # work one card, then stop
#   bash scripts/work-cards.sh --card 13   # work one specific card
#   bash scripts/work-cards.sh             # work every unblocked card in turn
#
# Cards labelled `stage` are skipped on purpose: each one has a design decision
# in it (where probes live, how many clipmap cascades, how a trace crosses
# between them) and an unattended session will pick one and write hundreds of
# lines against it.  Run those attended, one per session.  `deferred` and
# `blocked` (waiting on another card) are skipped too.  --card takes any card,
# including a skipped one, if you really mean it.
#
# Each card runs with --permission-mode acceptEdits: it edits files and commits
# without asking.  That is the trade for running unattended.  It also spends
# tokens with nobody watching.

set -uo pipefail

REPO=${REPO:-kennynguyen216/Mirabilis}
DRY_RUN=0
ONCE=0
CARD=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1 ;;
        --once)    ONCE=1 ;;
        --card)    CARD="${2:-}"; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

next_card() {
    gh issue list --repo "$REPO" --state open --json number,labels --jq \
        '[.[] | select(([.labels[].name] | index("stage")) == null
                   and ([.labels[].name] | index("deferred")) == null
                   and ([.labels[].name] | index("blocked")) == null)]
         | min_by(.number) | .number // empty'
}

card_title() {
    gh issue view "$1" --repo "$REPO" --json title --jq .title 2>/dev/null
}

prompt_for() {
    cat <<PROMPT
/ponytail

Work card #$1 in $REPO (project board: Lumen-lite GI).

Read the issue body, then docs/next_session_handoff.md for current state. Don't
re-derive either one. Skim docs/lumen_lite_design.md only for its "Settled, do
not re-investigate" list.

The acceptance criterion is in the card. Meet it, report the number with the
camera recorded beside it, then commit.

If you cannot meet it, commit nothing. Post what you found to the card with
'gh issue comment $1' and exit. A wrong fix committed unattended costs more
than a card left open.

Build the architectural stage, not a scene fix. No per-scene tuning.
PROMPT
}

worked=0
while :; do
    n=${CARD:-$(next_card)}

    if [ -z "$n" ]; then
        echo "No unblocked cards left (stage and deferred cards are skipped)."
        break
    fi

    echo "=== card #$n: $(card_title "$n") ==="

    if [ "$DRY_RUN" = "1" ]; then
        echo "--- would run: claude -p --permission-mode acceptEdits ---"
        prompt_for "$n"
        echo "--- end ---"
    else
        claude -p --permission-mode acceptEdits "$(prompt_for "$n")" || {
            echo "card #$n exited non-zero; stopping." >&2
            exit 1
        }
    fi

    worked=$((worked + 1))
    [ -n "$CARD" ] && break
    [ "$ONCE" = "1" ] && break
    # An open card that was just worked but not closed would be picked again
    # forever.  Stop rather than spin.
    if [ "$n" = "$(next_card)" ]; then
        echo "Card #$n is still open and would be picked again; stopping." >&2
        break
    fi
done

echo "Worked $worked card(s)."
