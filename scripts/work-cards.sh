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
    # The card is fetched here, by the orchestrator, and pasted into the
    # prompt.  A `claude -p` session has no interactive approval, so any gh
    # call it makes is denied; making it fetch its own card meant it could not
    # read its acceptance criterion and correctly refused to do anything.
    local body stage_note=""
    body=$(gh issue view "$1" --repo "$REPO" \
        --json title,body,labels \
        --jq '"TITLE: \(.title)\nLABELS: \([.labels[].name] | join(", "))\n\n\(.body)"' \
        2>/dev/null) || body="(could not fetch card #$1)"

    # Only a stage card is an architectural stage.  Telling a bug card to
    # "build the stage, not a scene fix" is at best noise and at worst an
    # invitation to read an architecture task into a ten-line fix.
    case "$body" in
        *"LABELS:"*"stage"*) stage_note="
Build the architectural stage, not a scene fix. No per-scene tuning." ;;
    esac

    cat <<PROMPT
/ponytail

Work card #$1 in $REPO (project board: Lumen-lite GI). The card body is below;
you do not need to fetch it.

--- BEGIN CARD #$1 ---
$body
--- END CARD #$1 ---

Read docs/next_session_handoff.md for current state. Don't re-derive it. Skim
docs/lumen_lite_design.md only for its "Settled, do not re-investigate" list.

The acceptance criterion is in the card above. Meet it, report the number with
the camera recorded beside it, then commit.

If you cannot meet it, commit nothing. Say plainly what you found and why you
stopped, and try to leave it on the card with 'gh issue comment $1'; if that is
denied, printing it is enough. A wrong fix committed unattended costs more than
a card left open.
$stage_note
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
