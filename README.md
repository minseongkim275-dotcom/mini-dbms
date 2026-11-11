# Mini-DBMS

크래프톤 정글 13기 · SQL 파싱부터 B+ 트리 인덱스, 스레드 풀 API 서버까지 C로 직접 구현한 미니 DBMS

외부 라이브러리 없이 C11과 POSIX API(pthread, 소켓)만 사용했다.

| 단계 | 내용 | 결과물 |
|---|---|---|
| Phase 1 | SQL 처리기: 파싱 → 실행 → 파일 저장 | `minidb` (CLI) |
| Phase 2 | B+ 트리 인덱스, 100만 건 성능 비교 | `bench` |
| Phase 3 | 스레드 풀 기반 HTTP API 서버 | `minidb-server` |

## 빠른 시작

Linux 또는 WSL에서 gcc와 make가 필요하다.

```bash
make                 # build/minidb, build/minidb-server, build/bench
make test            # 단위 테스트 + CLI/서버 기능 테스트
make bench           # 100만 건 삽입 후 인덱스 vs 선형 탐색 비교
```

```bash
./build/minidb -f examples/demo.sql          # SQL 파일 실행
./build/minidb -e "SELECT * FROM users;"     # 한 줄 실행
./build/minidb                               # 대화형 프롬프트
./build/minidb-server -p 8080 -t 8           # API 서버
```

## 구조

```
SQL 텍스트 → lexer → parser(Stmt) → executor(db) → table(파일) + bptree(메모리)
                                        ↑
                     minidb (CLI)   /   minidb-server (HTTP + 스레드 풀)
```

엔진은 하나이고, CLI와 서버는 같은 함수 `db_execute()`를 호출한다.

| 파일 | 역할 |
|---|---|
| `src/lexer.c` | SQL 문자열을 토큰으로 분해. 오류 위치(줄, 열) 추적 |
| `src/parser.c` | 재귀 하강 파서. 토큰을 `Stmt` 구조체로 변환 |
| `src/bptree.c` | 메모리 기반 B+ 트리 (`id → 파일 오프셋`) |
| `src/table.c` | 스키마 로드, 고정 길이 레코드 파일 읽기/쓰기, 인덱스 재구성 |
| `src/db.c` | 실행기. 타입 검사, 실행 계획 선택, 테이블 잠금 |
| `src/format.c` | 결과를 표 또는 JSON으로 출력 |
| `src/threadpool.c` | 고정 워커 스레드 + 유한 작업 큐 |
| `src/http.c` | HTTP 요청 헤더 파싱, JSON 문자열 추출 |
| `src/server.c` | 소켓 accept, 라우팅, 응답 |
| `src/main.c`, `src/bench.c` | CLI, 벤치마크 |

## Phase 1 — SQL 처리기

### 지원 문법

```
[EXPLAIN] SELECT * | COUNT(*) | col, ... FROM table [WHERE col op value] [LIMIT n];
INSERT INTO table [(col, ...)] VALUES (value, ...);

op    : =  !=  <>  <  <=  >  >=
value : 정수 | '문자열'   (작은따옴표는 '' 로 표기)
```

키워드와 식별자는 대소문자를 구분하지 않고, `--` 주석을 쓸 수 있다.

```
$ ./build/minidb -f examples/demo.sql
inserted 1 row (id = 1)
...
+----+---------+-----+----------------------+
| id | name    | age | email                |
+----+---------+-----+----------------------+
| 1  | alice   | 30  | alice@example.com    |
| 2  | bob     | 25  | bob@example.com      |
| 5  | 김민성  | 27  | minseong@example.com |
+----+---------+-----+----------------------+
```

스크립트 중간의 문장이 실패해도 오류를 출력하고 다음 문장을 계속 실행한다. 하나라도 실패하면 종료 코드는 1이다.

### 저장 포맷

`CREATE TABLE`은 구현하지 않았다. 테이블은 스키마 파일로 미리 정의한다.

```
# data/users.schema
id INT
name VARCHAR(32)
age INT
email VARCHAR(64)
```

- 타입은 `INT`(64비트 정수)와 `VARCHAR(n)`(1 ≤ n ≤ 255, 바이트 기준)이다.
- 첫 컬럼은 반드시 `id INT`이고, `INSERT` 때 1부터 자동으로 부여된다.
- 데이터는 `data/<table>.tbl`에 고정 길이 바이너리 레코드로 저장한다.

```
[16바이트 헤더: "MDB1" + 레코드 크기] [레코드 1] [레코드 2] ...
레코드 = INT 8바이트 | VARCHAR(n) n+1바이트(NUL 채움) | ...
```

고정 길이를 고른 이유는 레코드 위치를 오프셋 하나로 가리킬 수 있어서다. B+ 트리가 `id → 오프셋`만 갖고 있으면 `pread` 한 번으로 레코드를 읽는다. CSV는 사람이 읽기 쉽지만 행마다 길이가 달라 파싱 없이는 원하는 행으로 건너뛸 수 없다.

## Phase 2 — B+ 트리 인덱스

- 차수 64. 내부 노드는 구분 키만, 리프는 `(id, 파일 오프셋)`을 갖는다.
- 리프가 넘치면 반으로 나누고 오른쪽 첫 키를 부모로 **복사**한다. 내부 노드가 넘치면 가운데 키를 부모로 **이동**한다. 루트가 나뉘면 트리 높이가 1 늘어난다.
- 리프끼리 연결 리스트로 이어져 있어 범위 조회는 시작 리프를 찾은 뒤 옆으로만 이동한다.
- 인덱스는 메모리에만 있다. 테이블을 처음 열 때 데이터 파일을 한 번 훑어 재구성한다.

### 실행 계획

| 조건 | 계획 | 읽는 레코드 수 |
|---|---|---|
| `WHERE id = v` | INDEX SCAN | 1 |
| `WHERE id < <= > >= v` | INDEX RANGE SCAN | 결과 행 수만큼 |
| `COUNT(*)` (조건 없음) | METADATA | 0 |
| 그 외 (`id != v` 포함) | FULL SCAN | 전체 |

`EXPLAIN`으로 확인할 수 있다.

```
$ ./build/minidb -e "EXPLAIN SELECT * FROM users WHERE id = 3; EXPLAIN SELECT * FROM users WHERE age = 25;"
plan: INDEX SCAN (B+Tree point lookup on id)
plan: FULL SCAN (linear)
```

### 100만 건 성능 비교

`make bench` 결과 (WSL2 Ubuntu 22.04, 12코어, 데이터는 `/tmp`의 ext4):

```
== minidb benchmark: 1000000 rows ==

INSERT x 1000000            :    2100.8 ms total  (476000 rows/s)

query                              plan                     avg time
WHERE id = ?    (10000 runs)       INDEX SCAN              0.0019 ms
WHERE name = ?  (10 runs)          FULL SCAN              21.4456 ms
WHERE email = ? (10 runs)          FULL SCAN              19.9098 ms
WHERE id > N-1000 (1000 rows)      INDEX RANGE SCAN        0.3031 ms

index lookup is 11193x faster than a linear scan on name
reopen + index rebuild              :      82.4 ms
```

삽입은 SQL 문자열 100만 개를 하나씩 파서에 통과시킨 것이다. 인덱스 조회는 트리 높이(100만 건에서 4단계)만큼만 내려가므로 데이터가 늘어도 거의 일정하고, 선형 탐색은 행 수에 비례한다. 수치는 파일이 OS 페이지 캐시에 올라와 있는 상태의 값이다.

SQL 파일로 대량 데이터를 넣어 볼 수도 있다.

```bash
python3 tools/gen_sql.py 1000000 > /tmp/users_1m.sql
./build/minidb -q -f /tmp/users_1m.sql        # -q: INSERT 결과 줄 생략
./build/minidb -t -e "SELECT * FROM users WHERE id = 777777;"   # -t: 계획과 시간 출력
```

## Phase 3 — API 서버

```bash
./build/minidb-server -p 8080 -d data -t 8 -q 256 -m 10000
#   -t 워커 스레드 수   -q 대기 큐 크기   -m SELECT 한 번에 돌려줄 최대 행 수
```

| 엔드포인트 | 설명 |
|---|---|
| `POST /query` | 본문은 SQL 텍스트, 또는 `Content-Type: application/json`과 `{"sql": "..."}`. 요청당 문장 하나 |
| `GET /health` | 상태 확인 |
| `GET /stats` | 요청 수, 503 거절 수, 활성/대기 작업 수, 계획별 실행 횟수 |

```
$ curl -X POST localhost:8080/query -d "INSERT INTO users VALUES ('alice', 30, 'alice@example.com')"
{"ok":true,"type":"insert","id":1,"rows_affected":1,"elapsed_ms":0.021}

$ curl -X POST localhost:8080/query -H 'Content-Type: application/json' \
       -d '{"sql": "SELECT * FROM users WHERE id = 1"}'
{"ok":true,"type":"select","columns":["id","name","age","email"],
 "rows":[[1,"alice",30,"alice@example.com"]],"row_count":1,"truncated":false,
 "plan":"INDEX SCAN (B+Tree point lookup on id)","rows_examined":1,"elapsed_ms":0.012}

$ curl -X POST localhost:8080/query -d "SELECT * FROM ghost"
{"ok":false,"error":"table 'ghost' does not exist"}        # HTTP 400
```

### 요청 처리 흐름

```
accept 스레드 ──▶ [유한 작업 큐] ──▶ 워커 스레드 N개 ──▶ db_execute() ──▶ 응답 후 연결 종료
      └─ 큐가 가득 차면 accept 스레드가 직접 503 응답
```

- 스레드를 요청마다 만들지 않고 미리 만들어 둔 워커가 큐에서 작업을 꺼낸다. 워커는 뮤텍스와 조건 변수로 대기한다.
- 큐 크기에 상한이 있어서 과부하 때 메모리가 무한히 늘지 않는다. 넘치는 요청은 기다리게 하지 않고 바로 503으로 돌려보낸다.
- SIGINT/SIGTERM을 받으면 새 연결을 받지 않고, 큐에 남은 요청을 모두 처리한 뒤 종료한다.

### 동시성

| 공유 자원 | 보호 방법 |
|---|---|
| 테이블 (파일 끝 위치, 다음 id, B+ 트리) | 테이블별 `pthread_rwlock`. `SELECT`는 읽기 잠금으로 동시 실행, `INSERT`는 쓰기 잠금으로 단독 실행 |
| 데이터 파일 | `pread`/`pwrite`만 사용. 파일 오프셋을 공유하지 않으므로 읽기끼리 간섭이 없다 |
| 열린 테이블 목록 | 뮤텍스 |
| 통계 카운터 | C11 atomic |
| 작업 큐 | 뮤텍스 + 조건 변수 |

rwlock은 **쓰기 우선**으로 설정했다. glibc 기본값은 읽기 우선이라, `SELECT`가 끊이지 않으면 읽기 잠금이 풀릴 틈이 없어 `INSERT`가 영원히 대기한다. 실제로 AddressSanitizer 빌드에서 동시성 테스트가 10분 넘게 끝나지 않는 것으로 이 문제를 발견했고, `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`로 해결했다.

## 테스트

```bash
make test     # 전체
make unit     # 단위 테스트만
make asan     # AddressSanitizer로 전체 테스트
make tsan     # ThreadSanitizer로 단위 테스트
```

| 대상 | 확인하는 것 |
|---|---|
| `test_lexer` | 토큰 종류, 따옴표 이스케이프, 정수 범위, 길이 제한, 주석, 줄/열 위치 |
| `test_parser` | 정상 문장, 문법 오류 메시지와 위치, 예약어, 오류 뒤 다음 문장 복구 |
| `test_bptree` | 오름차순/내림차순/무작위 20만 건 삽입 후 구조 불변식 검사, 분할 경계, 중복 키, 범위 조회 |
| `test_table` | 스키마 오류, 추가/읽기/스캔, 재시작 후 인덱스 재구성, 잘린 레코드 복구, 스키마 불일치 감지 |
| `test_db` | 실행 계획 선택, 읽은 레코드 수, 타입/개수/길이 오류, int64 경계값, LIMIT, 행 수 제한, JSON 출력 |
| `test_http` | 요청 헤더 파싱과 잘못된 요청, JSON 이스케이프(`\uXXXX`, 서로게이트 쌍) |
| `test_threadpool` | 모든 작업이 한 번씩 실행, 큐가 가득 차면 거절, 실제 병렬 실행, 종료 시 큐 비우기 |
| `test_concurrency` | 쓰기 8 + 읽기 4 스레드 동시 실행. id 중복/누락 없음, 읽은 행이 항상 완전함 |
| `test_cli.sh` | SQL 파일 실행 결과를 기대 출력과 비교, 프로세스 재시작 후 데이터 유지 |
| `test_server.sh` | 엔드포인트, 오류 코드(400/404/405/413/431/503), 동시 삽입 800건, 정상 종료, 재시작 |

## 한계

- `CREATE TABLE`, `UPDATE`, `DELETE`, `JOIN`, 다중 조건 `WHERE`(AND/OR)는 없다.
- 인덱스는 `id`에만 있고 메모리에만 존재한다. 시작할 때 재구성하므로 테이블이 클수록 첫 조회가 느리다(100만 건에 약 80ms).
- `INSERT`마다 `fsync`하지 않는다. 프로세스가 죽어도 데이터는 남지만, 전원이 나가면 마지막 몇 건을 잃을 수 있다.
- 정수는 호스트 바이트 순서로 저장하므로 데이터 파일을 엔디언이 다른 기기로 옮길 수 없다.
- HTTP는 요청당 연결 하나(keep-alive 없음)이고 TLS와 인증이 없다.
- 설계 문서: [docs/superpowers/specs/2026-11-11-mini-dbms-design.md](docs/superpowers/specs/2026-11-11-mini-dbms-design.md)
