#!/usr/bin/env bash
# 🟦 공통 — PreToolUse 훅: Bash 명령이 실행되기 직전에 main으로 가는 push를 막는다.
# 이 훅은 "실수 방지"용이다. 따옴표 쪼개기(ma'i'n) 같은 우회까지 막지는 못하므로,
# 진짜 보호는 GitHub의 main 브랜치 규칙이 맡는다.
source "$(dirname "$0")/json-field.sh"

input=$(cat)
cmd=$(printf '%s' "$input" | json_field '.tool_input.command')

block() {
  echo "main 브랜치로 가는 push는 막혀 있어. 작업 브랜치(feat/…, fix/…)나 dev로 push하고, main은 PR로 올려." >&2
  exit 2   # exit 2 = 도구 실행을 막고, stderr를 Claude에게 전달
}

# push가 아닌 명령은 바로 통과
printf '%s' "$cmd" | grep -Eq '(^|[^[:alnum:]_-])git([[:space:]].*)?[[:space:]]push([[:space:]]|$)' || exit 0

# push 뒤의 인자만 본다. (git commit -m "fix main loop" && git push 처럼 앞쪽에 나온 main은 무시)
args=" ${cmd##*push}"

# 1) 대상 ref에 main이 단어로 등장: origin main, HEAD:main, +main, refs/heads/main
#    (feat/12-main-loop 같은 브랜치 이름은 걸리지 않도록 앞뒤 문자를 제한)
printf '%s' "$args" | grep -Eq '[[:space:]:+/]main([[:space:]]|$)' && block

# 2) 모든 브랜치를 한꺼번에 보내는 옵션
printf '%s' "$args" | grep -Eq '[[:space:]]--(all|mirror)([[:space:]]|$)' && block

# 3) 대상 브랜치 없이 push하면 현재 브랜치로 간다: main에 체크아웃한 채 `git push`, `git push origin`
#    대상을 적은 push(`git push origin dev`)는 main에 있어도 막지 않는다.
#    push 명령 하나만 떼어(&&, ;, | 앞까지) 옵션이 아닌 인자를 센다: 원격 + 대상이면 2개 이상.
only=$(printf '%s' "$args" | sed -E 's/(&&|\|\||;|\|).*$//')
n=0
for w in $only; do
  case "$w" in -*) ;; *) n=$((n + 1)) ;; esac
done
if [ "$n" -lt 2 ]; then
  # 세션이 실제로 일하는 트리(worktree 포함)의 브랜치를 본다.
  cwd=$(printf '%s' "$input" | json_field '.cwd')
  [ -d "$cwd" ] || cwd="${CLAUDE_PROJECT_DIR:-.}"
  branch=$(git -C "$cwd" rev-parse --abbrev-ref HEAD 2>/dev/null)
  [ "$branch" = "main" ] && block
fi

exit 0
