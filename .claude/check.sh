#!/usr/bin/env bash
# 🟩 특화: mini-dbms의 "통과/실패" 기준. 공통 게이트(Stop 훅, CI)는 이 파일의 종료 코드만 본다.
#   - 경고를 오류로 다룬다(-Werror). 팀 규칙 "경고가 늘어난 PR은 병합하지 않는다"를 여기서 강제한다.
#   - Linux에서는 바로, Windows(Git Bash)에서는 WSL로 넘겨 `make test`를 돌린다.
set -euo pipefail
cd "$(dirname "$0")/.."

# 여러 세션(worktree 포함)이 동시에 검사하면 서버 기능 테스트가 같은 포트(18473)를 두고 다툰다.
# 잠금 폴더를 모든 worktree가 공유하는 .git 아래에 두어 한 번에 하나씩만 돈다.
lock="$(git rev-parse --git-common-dir 2>/dev/null || echo .)/check.lock"
# 10분 넘은 잠금은 비정상 종료로 남은 것으로 보고 치운다.
[ -d "$lock" ] && [ -n "$(find "$lock" -maxdepth 0 -mmin +10 2>/dev/null)" ] && rmdir "$lock" 2>/dev/null || true
locked=0
for _ in $(seq 1 180); do
  if mkdir "$lock" 2>/dev/null; then locked=1; break; fi
  sleep 1
done
if [ "$locked" -eq 1 ]; then
  trap 'rmdir "$lock" 2>/dev/null || true' EXIT
else
  echo "경고: 3분 동안 잠금($lock)을 얻지 못해 잠금 없이 진행한다." >&2
fi

flags="-O2 -g -Werror"
if [ "$(uname -s)" = "Linux" ]; then
  CFLAGS="$flags" make test
else
  distro="${MINIDB_WSL_DISTRO:-Ubuntu-22.04}"   # 기본 배포판이 docker-desktop이라 이름을 지정한다
  if ! command -v wsl.exe >/dev/null 2>&1; then
    echo "Linux가 아니고 WSL도 없어. gcc·make가 있는 Linux 또는 WSL($distro)에서 실행해." >&2
    exit 1
  fi
  MSYS_NO_PATHCONV=1 wsl.exe -d "$distro" --cd "$(pwd -W)" -- env CFLAGS="$flags" make test
fi
