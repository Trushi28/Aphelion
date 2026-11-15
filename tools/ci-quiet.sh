#!/bin/sh
# Decides whether a scheduled CI run has anything to do.
#
# GitHub has no "run when nothing has happened" trigger, so the workflow keeps a
# weekly schedule and asks this script whether the run is worth doing. Exit 0
# (run everything) only when both hold:
#   1. nothing has been pushed to the repository for QUIET_DAYS days, and
#   2. no earlier scheduled run already covered this quiet spell,
# so a long quiet spell costs one full run, not one every week. Exit 1 means skip.
#
# Needs GH_TOKEN (actions: read), GITHUB_REPOSITORY and GITHUB_RUN_ID.
set -eu

QUIET_DAYS="${QUIET_DAYS:-5}"
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}"
WORKFLOW="${CI_WORKFLOW:-ci.yml}"

now="$(date +%s)"

# The time of the last push to any branch, which is what "activity" means here.
last=""
pushed="$(gh api "repos/$REPO" --jq .pushed_at 2>/dev/null || true)"
if [ -n "$pushed" ]; then
    last="$(date -u -d "$pushed" +%s 2>/dev/null || true)"
fi
if [ -z "$last" ]; then
    last="$(git log -1 --format=%ct)"
    echo "ci-quiet: could not read the last push time from GitHub; using the last commit time" >&2
fi

quiet=$(( now - last ))
need=$(( QUIET_DAYS * 86400 ))
if [ "$quiet" -lt "$need" ]; then
    echo "ci-quiet: last push was $(( quiet / 86400 )) day(s) ago, less than $QUIET_DAYS; skipping" >&2
    exit 1
fi

# A scheduled run created after the quiet spell began has already passed the check
# above, so it was a real run. Earlier ones were skipped and do not count.
since="$(date -u -d "@$(( last + need ))" +%Y-%m-%dT%H:%M:%SZ)"
if earlier="$(gh run list --repo "$REPO" --workflow "$WORKFLOW" --event schedule --limit 50 \
        --json databaseId,createdAt \
        --jq "[.[] | select(.createdAt >= \"$since\" and .databaseId != ${GITHUB_RUN_ID:-0})] | length" 2>/dev/null)"; then
    if [ "$earlier" -gt 0 ]; then
        echo "ci-quiet: quiet for $(( quiet / 86400 )) day(s), but a scheduled run already covered it; skipping" >&2
        exit 1
    fi
else
    # Not knowing is not a reason to skip; at worst this repeats one run.
    echo "ci-quiet: could not list earlier scheduled runs; assuming there are none" >&2
fi

echo "ci-quiet: quiet for $(( quiet / 86400 )) day(s) and not yet covered; running everything" >&2
exit 0
