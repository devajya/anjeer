#!/usr/bin/env bash
# Pre-commit: block commits that contain secrets or gitignored config files.

FAIL=0

# -- 1. Block gitignored config files that may hold real credentials ----------
BLOCKED=(config/default.json config/dev.json config/local.json)
for f in "${BLOCKED[@]}"; do
    if git diff --cached --name-only --diff-filter=ACM | grep -qF "$f"; then
        echo ""
        echo "  [secrets] BLOCKED: $f is gitignored and may contain real credentials."
        echo "  Unstage with: git reset HEAD $f"
        echo ""
        FAIL=1
    fi
done

# -- 2. Scan staged diff for credential-like key/value pairs ------------------
# Exclude test fixtures and seeds — they intentionally contain placeholder values.
STAGED=$(git diff --cached -U0 -- ':!server/tests/fixtures/' ':!db/seeds/')

# Secret key names paired with values longer than 10 chars (real creds are long).
CRED_PATTERN='"(client_secret|client_id|jwt_secret|api_key|private_key|refresh_token)"\s*:\s*"([^"]{10,})"'

# Values that are obviously fake — allow these through.
SAFE_PATTERN='test[_-]|fake[_-]|dummy|placeholder|example|changeme|do.not.use|localhost|127\.0\.'

HITS=$(printf '%s' "$STAGED" \
    | grep -E '^\+' \
    | grep -iE "$CRED_PATTERN" \
    | grep -ivE "$SAFE_PATTERN")

if [ -n "$HITS" ]; then
    echo ""
    echo "  [secrets] Possible real credential in staged diff:"
    printf '%s\n' "$HITS" | head -5 | sed 's/^/    /'
    echo ""
    echo "  If this is a false positive: git commit --no-verify"
    echo ""
    FAIL=1
fi

exit $FAIL
