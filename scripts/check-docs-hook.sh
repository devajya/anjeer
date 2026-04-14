#!/usr/bin/env bash
# Pre-commit hook: warns when a new engine/server header or frontend component
# is staged without a corresponding change to the architecture docs.
#
# Prints a reminder to stdout — does NOT block the commit (exit 0 always).
# If you want it to block, change the final `exit 0` to `exit 1`.

staged_new_headers=$(git diff --cached --name-only --diff-filter=A \
    | grep -E '^(engine|server)/include/.*\.h$')

staged_new_components=$(git diff --cached --name-only --diff-filter=A \
    | grep -E '^frontend/src/components/.*\.tsx$')

staged_doc_changes=$(git diff --cached --name-only \
    | grep -E '^(CLAUDE\.md|docs/ARCHITECTURE\.md)$')

needs_warning=0

if [ -n "$staged_new_headers" ] && [ -z "$staged_doc_changes" ]; then
    needs_warning=1
    echo ""
    echo "  [docs] New header(s) staged without doc update:"
    echo "$staged_new_headers" | sed 's/^/    /'
fi

if [ -n "$staged_new_components" ] && [ -z "$staged_doc_changes" ]; then
    needs_warning=1
    echo ""
    echo "  [docs] New component(s) staged without doc update:"
    echo "$staged_new_components" | sed 's/^/    /'
fi

if [ "$needs_warning" -eq 1 ]; then
    echo ""
    echo "  Consider updating CLAUDE.md (Module Map) and/or docs/ARCHITECTURE.md."
    echo "  See the 'When to Update This Document' section in ARCHITECTURE.md."
    echo "  To skip this check: git commit --no-verify"
    echo ""
fi

exit 1
