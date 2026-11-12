# mini-dbms

크래프톤 정글 13기 프로젝트. C11로 구현하는 미니 DBMS (SQL 처리기, B+ 트리 인덱스, 스레드 풀 HTTP API 서버).

## 빌드와 테스트
- 환경: Linux 또는 WSL(`Ubuntu-22.04`)의 gcc·make. Windows의 gcc(MinGW)로는 빌드하지 않는다.
- 전체 검사: `bash .claude/check.sh` (작업을 마치기 전에 반드시 통과해야 함. Stop 훅과 CI가 같은 파일을 실행함. 경고는 오류로 다룸)
- WSL 안에서 직접: `make`, `make test`, `make asan`, `make tsan`, `make bench`
- Windows 쪽 셸에서 WSL 명령을 직접 돌릴 때: `wsl -d Ubuntu-22.04 --cd "<폴더>" -- make test` (기본 배포판이 docker-desktop이라 이름을 꼭 지정)

## 문서
- 설계와 범위: `docs/superpowers/specs/2026-11-11-mini-dbms-design.md`
- 과제 요구사항 원문: `docs/requirements.md` (아직 없음. 개발자가 원문을 주면 이 파일로 저장하고, 대화에 다시 붙여넣지 않는다)
- 고치지 않기로 한 지적: `docs/known-limits.md` (리뷰의 "선택" 항목을 한 줄씩 기록. 없으면 처음 기록할 때 만든다)

## 디렉터리
- `src/lexer`, `src/parser`: SQL 문자열 → 토큰 → `Stmt`
- `src/bptree`: 메모리 B+ 트리 (`id → 파일 오프셋`)
- `src/table`: 스키마 로드, 고정 길이 레코드 파일, 인덱스 재구성
- `src/db`: 실행기. 타입 검사, 실행 계획, 테이블 잠금. CLI와 서버가 함께 쓰는 진입점 `db_execute()`
- `src/format`: 표·JSON 출력
- `src/threadpool`, `src/http`, `src/server`: 스레드 풀 HTTP 서버
- `tests/test_*.c`: 단위 테스트, `tests/functional/`: CLI·서버 기능 테스트

## 도메인 규칙
규칙마다 그것을 검사하는 테스트를 적는다. "검사 없음"은 리뷰어가 직접 봐야 하는 항목이고, 테스트로 옮길 후보다.

| 규칙 | 검사하는 곳 |
|---|---|
| 테이블 파일은 추가 전용이다. 파일 끝의 불완전한 레코드는 열 때 무시하고, 레코드 크기가 스키마와 다르면 열기를 거부한다 | `tests/test_table.c` |
| `id`는 엔진이 부여한다. `INSERT`에서 `id`를 직접 지정하면 오류다 | `tests/test_db.c` |
| B+ 트리를 바꾼 뒤에는 불변식(`bpt_validate`)이 성립한다 | `tests/test_bptree.c` |
| 테이블마다 `pthread_rwlock` 하나. `SELECT`는 읽기 잠금, `INSERT`는 쓰기 잠금 | `tests/test_concurrency.c`, `make tsan` (게이트에는 없음) |
| 파일 접근은 `pread`/`pwrite`만 쓴다 (파일 오프셋을 스레드끼리 공유하지 않음) | 검사 없음 |
| 서버는 큐가 가득 차면 기다리지 않고 503을 준다. 응답 행 수가 제한을 넘으면 `"truncated": true` | `tests/functional/test_server.sh` |
| CLI와 서버는 같은 `db_execute()`를 부른다. 한쪽에만 실행 로직을 두지 않는다 | 검사 없음 |
| 실패 경로에서도 `malloc`한 것과 연 파일·잠금을 정리한다 | `make asan` (게이트에는 없음) |
| 외부 라이브러리를 쓰지 않는다 (C11, POSIX API만) | 검사 없음 |

## 팀 규칙
@.claude/team-rules.md
