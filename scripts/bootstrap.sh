#!/usr/bin/env bash
# One-time setup for a fresh clone. Git does not version hooks, so point it at .githooks.
set -eo pipefail
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath .githooks
chmod +x .githooks/* scripts/*.sh
echo "Hooks enabled: every git push now runs scripts/check-linux.sh first."
