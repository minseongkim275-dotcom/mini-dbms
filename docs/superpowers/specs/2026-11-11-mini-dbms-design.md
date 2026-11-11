# 미니 DBMS 설계

날짜: 2026-11-11 · 언어: C11 · 환경: Linux (WSL Ubuntu 22.04, gcc, make)

## 목표

세 단계로 "동작하는 미니 DBMS"를 만든다.

1. SQL 처리기: 파일로 받은 SQL(`INSERT`, `SELECT`)을 파싱·실행하고 테이블을 파일에 저장한다.
2. B+ 트리 인덱스: 자동 부여되는 `id`를 메모리 기반 B+ 트리에 등록하고, `WHERE id ...` 조회에 사용한다. 100만 건으로 인덱스/선형 탐색 속도를 비교한다.
3. API 서버: 1·2단계 엔진을 스레드 풀 기반 HTTP 서버로 감싼다.

`CREATE TABLE`은 범위 밖이다. 테이블 스키마는 미리 만들어 둔 파일로 주어진다.

## 구조

```
SQL 텍스트 → lexer → parser(Stmt) → executor(db) → table(파일) + bptree(메모리)
                                        ↑
                     minidb (CLI)   /   minidb-server (HTTP + 스레드 풀)
```

| 파일 | 책임 | 의존 |
|---|---|---|
| `lexer` | SQL 문자열을 토큰으로 분해. 줄/열 위치 추적 | 없음 |
| `parser` | 토큰을 `Stmt` 구조체로 변환. 문법 오류 보고 | lexer |
| `bptree` | `int64 키 → int64 값` 메모리 B+ 트리 | 없음 |
| `table` | 스키마 파일 로드, 고정 길이 레코드 파일 읽기/쓰기, 인덱스 재구성 | bptree |
| `db` | 엔진 진입점. 실행 계획 선택, 타입 검사, 테이블별 잠금, 통계 | parser, table |
| `format` | 결과를 표(텍스트) 또는 JSON으로 출력 | db |
| `threadpool` | 고정 워커 + 유한 작업 큐 | pthread |
| `http` | HTTP 요청 헤더 파싱, JSON 문자열 추출 | 없음 |
| `server` | 소켓 accept, 라우팅, 응답 | db, threadpool, http, format |

## SQL 문법

```
stmt   := [EXPLAIN] (insert | select) [';']
insert := INSERT INTO table ['(' col {',' col} ')'] VALUES '(' value {',' value} ')'
select := SELECT ('*' | COUNT '(' '*' ')' | col {',' col}) FROM table
          [WHERE col op value] [LIMIT n]
op     := = | != | <> | < | <= | > | >=
value  := 정수 | '문자열'   (문자열 안의 작은따옴표는 '' 로 표기)
```

키워드와 식별자는 대소문자를 구분하지 않는다. `--` 주석을 지원한다.

## 저장 포맷

- `data/<table>.schema`: 한 줄에 `컬럼명 타입`. 타입은 `INT`(64비트)와 `VARCHAR(n)`(1 ≤ n ≤ 255, 바이트 기준). 첫 컬럼은 반드시 `id INT`.
- `data/<table>.tbl`: 16바이트 헤더(`MDB1` + 레코드 크기) 뒤에 고정 길이 레코드를 이어 붙인다. `INT`는 8바이트, `VARCHAR(n)`은 NUL로 채운 n+1바이트.
- 추가 전용이다. 파일 끝의 불완전한 레코드(쓰다 중단된 경우)는 열 때 무시한다.
- 스키마와 데이터 파일의 레코드 크기가 다르면 열기를 거부한다.

## 인덱스와 실행 계획

- `INSERT` 때 `id = next_id++`를 부여하고 `id → 파일 오프셋`을 B+ 트리에 넣는다. `id`를 직접 지정하는 것은 허용하지 않는다.
- 테이블을 처음 열 때 파일을 한 번 훑어 트리를 재구성한다.

| 조건 | 계획 |
|---|---|
| `WHERE id = v` | INDEX SCAN: 트리 탐색 후 레코드 1개 읽기 |
| `WHERE id < <= > >= v` | INDEX RANGE SCAN: 시작 리프를 찾고 리프 연결 리스트를 따라감 |
| `COUNT(*)` (조건 없음) | METADATA: 행 수 카운터 |
| 그 외 | FULL SCAN: 파일 전체를 순서대로 읽으며 비교 |

`EXPLAIN SELECT ...`는 실행하지 않고 계획만 돌려준다.

## 동시성

- 테이블마다 `pthread_rwlock` 하나. `SELECT`는 읽기 잠금(동시 실행), `INSERT`는 쓰기 잠금(배타).
- 파일 접근은 `pread`/`pwrite`만 사용해 파일 오프셋을 공유하지 않는다.
- 테이블 목록은 뮤텍스로 보호한다. 통계 카운터는 원자적 변수다.

## API 서버

- `POST /query`: 본문은 SQL 텍스트 또는 `{"sql": "..."}`. 요청당 문장 하나. JSON 응답.
- `GET /health`, `GET /stats`.
- accept 스레드가 연결을 유한 큐에 넣고 워커가 처리한다. 큐가 가득 차면 503.
- 요청당 연결 하나(`Connection: close`). 헤더 8KB, 본문 1MB 제한. 수신 타임아웃 5초.
- 응답 행 수는 기본 10,000개로 제한하고 넘으면 `"truncated": true`.
- SIGINT/SIGTERM을 받으면 새 연결을 받지 않고 큐에 있는 요청을 끝낸 뒤 종료한다.

## 오류 처리

- 문법 오류는 줄·열 위치와 함께 보고한다.
- 의미 오류: 없는 테이블/컬럼, 값 개수 불일치, 타입 불일치, 문자열 길이 초과, 중복 컬럼, `id` 직접 지정.
- HTTP: 400(잘못된 요청/SQL 오류), 404, 405, 408, 411, 413, 431, 503.

## 테스트

- 단위: lexer, parser, bptree(분할·순서·불변식), table/db, http, threadpool, 동시 삽입/조회.
- 기능: CLI로 SQL 파일을 실행해 기대 출력과 비교. 서버는 curl로 검증(동시 삽입 후 건수 확인, 재시작 후 데이터 유지 포함).
- `make bench`: 100만 건 삽입 후 인덱스 조회와 선형 탐색 비교.
- AddressSanitizer, ThreadSanitizer 빌드 지원.

## 범위 밖

`CREATE TABLE`, `DELETE`/`UPDATE`, 다중 조건 `WHERE`, 보조 인덱스, 트랜잭션, 디스크 기반 인덱스, HTTP keep-alive.
