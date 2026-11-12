#!/usr/bin/env bash
# 🟦 공통 — Stop 훅 = 품질 게이트: Claude가 "다 했어요"라고 끝내려는 순간 실행된다.
# 무엇을 검사할지는 모른다. .claude/check.sh를 실행해서 0이면 통과, 아니면 막는다.
source "$(dirname "$0")/json-field.sh"

input=$(cat)

# 세션이 실제로 일하는 트리에서 검사한다. worktree 세션에서 CLAUDE_PROJECT_DIR은 본 체크아웃을
# 가리킬 수 있어서, 훅 입력의 cwd를 먼저 쓰고 없을 때만 CLAUDE_PROJECT_DIR로 돌아간다.
cwd=$(printf '%s' "$input" | json_field '.cwd')
[ -d "$cwd" ] || cwd="${CLAUDE_PROJECT_DIR:-.}"
root=$(git -C "$cwd" rev-parse --show-toplevel 2>/dev/null) || exit 0
cd "$root" || exit 0
[ -f .claude/check.sh ] || exit 0

# 기준점: 통합 브랜치(dev)가 있으면 그것, 없으면 main. 로컬 브랜치가 없으면 원격 추적 브랜치를 본다.
base=""
for ref in dev origin/dev main origin/main; do
  if git rev-parse --verify -q "$ref" >/dev/null; then
    base=$(git merge-base "$ref" HEAD 2>/dev/null) && break
  fi
done

# 이 브랜치에서 코드가 바뀌었는지 확인한다 (문서와 하네스 파일만 바뀐 경우는 검사하지 않음).
#  - git status: 커밋 안 한 수정 + 새로 만든(untracked) 파일
#  - 기준점..HEAD: 이미 커밋한 변경
changed=$( {
  git status --porcelain | cut -c4-
  [ -n "$base" ] && git diff --name-only "$base" HEAD
} 2>/dev/null | grep -Ev '(\.md$|^\.claude/|^\.github/|^docs/|^\.git(ignore|attributes)$)' )
[ -z "$changed" ] && exit 0   # 질문만 한 세션, 읽기만 한 리뷰 세션은 검사하지 않음

# 실패하는 테스트를 먼저 쓰는 단계: `touch .claude/logs/red-phase`로 표시하면 30분 동안 통과시킨다.
# (.claude/logs/는 커밋되지 않고, CI는 이 표시를 보지 않는다. 구현을 마치면 지운다.)
red=.claude/logs/red-phase
if [ -f "$red" ] && [ -n "$(find "$red" -mmin -30 2>/dev/null)" ]; then
  echo '{"systemMessage": "품질 게이트: 실패 테스트 단계 표시(.claude/logs/red-phase)가 있어 이번 검사는 건너뛰었어."}'
  exit 0
fi

if out=$(bash .claude/check.sh 2>&1); then
  exit 0
fi

# 게이트에 걸려 이미 한 번 이어서 작업했는데도 실패하면, 무한 반복을 막고 개발자에게 넘긴다.
# 조용히 통과시키지 않고 사용자 화면에 경고를 띄운다.
if [ "$(printf '%s' "$input" | json_field '.stop_hook_active')" = "true" ]; then
  echo '{"systemMessage": "⚠ 품질 게이트가 두 번 연속 실패했어. check.sh 결과를 직접 확인해 줘."}'
  exit 0
fi

echo "품질 게이트 실패: .claude/check.sh가 통과하지 않았어. 원인을 찾아 고친 뒤에 끝내." >&2
echo "(검사한 폴더: $root)" >&2
printf '%s\n' "$out" | tail -n 30 >&2
exit 2   # exit 2 = 종료를 막고, 실패 로그를 Claude에게 돌려줌
