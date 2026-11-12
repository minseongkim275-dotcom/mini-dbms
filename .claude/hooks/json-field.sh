#!/usr/bin/env bash
# 🟦 공통: stdin의 JSON에서 필드 하나를 꺼내 출력한다. 훅들이 source해서 쓴다.
#   사용: value=$(printf '%s' "$input" | json_field '.tool_input.command')
# jq → python → grep 순서로 시도한다. (Windows Git Bash에는 jq가 없는 경우가 많다)
json_field() {
  local path="$1" data
  data=$(cat)
  if command -v jq >/dev/null 2>&1; then
    printf '%s' "$data" | jq -r "$path // empty"
    return
  fi
  local py
  for py in python python3; do
    if "$py" -c 'import json' >/dev/null 2>&1; then
      printf '%s' "$data" | "$py" -c '
import json, sys
cur = json.load(sys.stdin)
for key in sys.argv[1].lstrip(".").split("."):
    cur = cur.get(key) if isinstance(cur, dict) else None
print("" if cur is None else (str(cur).lower() if isinstance(cur, bool) else cur))
' "$path"
      return
    fi
  done
  # 마지막 수단: 최상위 문자열/불리언 값만 대충 꺼낸다 (이스케이프된 따옴표는 못 읽음)
  local key="${path##*.}"
  printf '%s' "$data" | grep -o "\"$key\" *: *\(\"[^\"]*\"\|true\|false\)" | head -n 1 \
    | sed -E "s/^\"$key\" *: *\"?//; s/\"$//"
}
