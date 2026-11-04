#!/bin/sh
# One-shot repo setup for changelog fragments (see changelog.d/README.md). Needs `gh` logged in as the repo admin.
# 1. deploy key (write) + CHANGELOG_DEPLOY_KEY secret: the assemble job pushes CHANGELOG.md to main with it.
# 2. ruleset "main": Test Status, fragment and check must pass (so auto-merge waits for CI); deploy keys
#    and repository admins bypass it. Re-runnable: skips what already exists.
set -eu
R=${1:-prokrypt/CrossDink}
k=$(mktemp -d)/ck; trap 'rm -rf "$(dirname "$k")"' EXIT
gh repo deploy-key list -R "$R" | grep -q changelog-assemble || {
  ssh-keygen -q -t ed25519 -N '' -C changelog-assemble -f "$k"
  gh repo deploy-key add -R "$R" --allow-write -t changelog-assemble "$k.pub"
  gh secret set CHANGELOG_DEPLOY_KEY -R "$R" < "$k"
  echo "deploy key + secret added"; }
gh api "repos/$R/rulesets" --jq '.[].name' | grep -qx main && { echo "ruleset main exists"; exit 0; }
gh api -X POST "repos/$R/rulesets" --input - <<'JSON' --jq '"ruleset \(.name) id \(.id) \(.enforcement)"'
{"name":"main","target":"branch","enforcement":"active",
 "conditions":{"ref_name":{"include":["~DEFAULT_BRANCH"],"exclude":[]}},
 "rules":[{"type":"deletion"},{"type":"non_fast_forward"},
  {"type":"required_status_checks","parameters":{"strict_required_status_checks_policy":false,
   "required_status_checks":[{"context":"Test Status"},{"context":"fragment"},{"context":"check"}]}}],
 "bypass_actors":[{"actor_type":"DeployKey","bypass_mode":"always"},{"actor_id":5,"actor_type":"RepositoryRole","bypass_mode":"always"}]}
JSON
