#!/usr/bin/env bash
# API server functional test: starts build/minidb-server and drives it with curl.
#   bash tests/functional/test_server.sh [BUILD_DIR]
set -u
BUILD=${1:-build}
BIN="$BUILD/minidb-server"
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=${PORT:-18473}
URL="http://127.0.0.1:$PORT"
TMP=$(mktemp -d)
PID=""
trap '[ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$TMP"' EXIT
fail=0

check() {
    if [ "$2" -eq 0 ]; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi
}

start_server() { # extra args...
    "$BIN" -p "$PORT" -d "$TMP/data" "$@" >"$TMP/server.log" 2>&1 &
    PID=$!
    for _ in $(seq 100); do
        curl -s -o /dev/null "$URL/health" && return 0
        sleep 0.05
    done
    echo "server did not start"; cat "$TMP/server.log"; exit 1
}

stop_server() {
    kill -INT "$PID"
    wait "$PID"
    local rc=$?
    PID=""
    return $rc
}

# q SQL -> response body ; code/body are also left in $TMP/code and $TMP/body
q() {
    curl -s -o "$TMP/body" -w '%{http_code}' -X POST --data-binary "$1" "$URL/query" >"$TMP/code"
    cat "$TMP/body"
}
has() { grep -qF -- "$1" "$TMP/body"; }
code() { [ "$(cat "$TMP/code")" = "$1" ]; }

mkdir "$TMP/data"
cp "$HERE/../../data/users.schema" "$TMP/data/"
start_server -t 8 -q 64 -m 100

echo "basic endpoints"
curl -s "$URL/health" >"$TMP/body"; has '{"status":"ok"}'; check "GET /health" $?
q "SELECT COUNT(*) FROM users" >/dev/null; code 200 && has '"rows":[[0]]'; check "empty table count" $?

echo "insert and select"
q "INSERT INTO users VALUES ('alice', 30, 'alice@example.com')" >/dev/null
code 200 && has '"type":"insert","id":1'; check "insert returns the new id" $?
q "INSERT INTO users (name, age) VALUES ('bob \"quoted\"', 25)" >/dev/null; has '"id":2'; check "second insert gets id 2" $?
q "SELECT * FROM users WHERE id = 1" >/dev/null
has '"columns":["id","name","age","email"],"rows":[[1,"alice",30,"alice@example.com"]]'; check "select by id returns the row" $?
has '"plan":"INDEX SCAN'; check "id lookup uses the index" $?
q "SELECT name FROM users WHERE age = 25" >/dev/null
has '"rows":[["bob \"quoted\""]]' && has '"plan":"FULL SCAN'; check "non-id lookup scans, JSON is escaped" $?
q "SELECT id FROM users WHERE id >= 1" >/dev/null; has '"plan":"INDEX RANGE SCAN' && has '"rows":[[1],[2]]'; check "id range uses the leaf chain" $?
q "EXPLAIN SELECT * FROM users WHERE id = 1" >/dev/null; has '"type":"explain"'; check "EXPLAIN" $?

echo "JSON request body"
curl -s -o "$TMP/body" -X POST -H 'Content-Type: application/json' \
    -d '{"sql": "SELECT name FROM users WHERE name = '"'"'alice'"'"'"}' "$URL/query"
has '"rows":[["alice"]]'; check "{\"sql\": ...} body" $?
curl -s -o "$TMP/body" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{"query": 1}' "$URL/query" >"$TMP/code"
code 400 && has '"ok":false'; check "JSON body without \"sql\" -> 400" $?

echo "errors"
q "SELEC * FROM users" >/dev/null; code 400 && has 'syntax error'; check "syntax error -> 400" $?
q "SELECT * FROM ghost" >/dev/null; code 400 && has "table 'ghost' does not exist"; check "unknown table -> 400" $?
q "SELECT 1; SELECT 2" >/dev/null; code 400; check "garbage statement -> 400" $?
q "SELECT * FROM users; SELECT * FROM users" >/dev/null; code 400 && has 'only one statement'; check "two statements -> 400" $?
q "" >/dev/null; code 400 && has 'empty statement'; check "empty body -> 400" $?
curl -s -o "$TMP/body" -w '%{http_code}' "$URL/nope" >"$TMP/code"; code 404; check "unknown path -> 404" $?
curl -s -o "$TMP/body" -w '%{http_code}' "$URL/query" >"$TMP/code"; code 405; check "GET /query -> 405" $?
curl -s -o "$TMP/body" -w '%{http_code}' -X POST "$URL/health" >"$TMP/code"; code 405; check "POST /health -> 405" $?
head -c 1100000 /dev/zero | tr '\0' 'a' >"$TMP/big"
curl -s -o "$TMP/body" -w '%{http_code}' -X POST -H 'Expect:' --data-binary "@$TMP/big" "$URL/query" >"$TMP/code"
code 413; check "body over 1 MB -> 413" $?
printf 'this is not http\r\n\r\n' | curl -s -o "$TMP/body" -m 3 "telnet://127.0.0.1:$PORT" 2>/dev/null
has '"error":"malformed request"'; check "malformed request line -> 400" $?
{ printf 'GET /health HTTP/1.1\r\nX-Pad: '; head -c 9000 /dev/zero | tr '\0' 'a'; printf '\r\n\r\n'; } |
    curl -s -o "$TMP/body" -m 3 "telnet://127.0.0.1:$PORT" 2>/dev/null
has 'headers too large'; check "oversized headers -> 431" $?
curl -s "$URL/health" | grep -q ok; check "server still healthy after bad requests" $?

echo "concurrency: 16 clients x 50 inserts, with readers running"
insert_many() { # client number
    for i in $(seq 50); do
        curl -s -X POST --data-binary "INSERT INTO users VALUES ('c$1_$i', $1, 'c$1@example.com')" "$URL/query"
        echo
    done
}
read_many() {
    for i in $(seq 40); do
        curl -s -X POST --data-binary "SELECT * FROM users WHERE id = $i" "$URL/query"; echo
        curl -s -X POST --data-binary "SELECT COUNT(*) FROM users WHERE age = 3" "$URL/query"; echo
    done
}
for c in $(seq 16); do insert_many "$c" >"$TMP/ins_$c.txt" & done
for c in 1 2 3 4; do read_many >"$TMP/read_$c.txt" & done
wait $(jobs -p | grep -v "^$PID\$")
cat "$TMP"/ins_*.txt | grep -o '"id":[0-9]*' | sort | uniq -d >"$TMP/dups"
n_ok=$(cat "$TMP"/ins_*.txt | grep -c '"ok":true')
[ "$n_ok" -eq 800 ]; check "all 800 concurrent inserts succeeded (got $n_ok)" $?
[ ! -s "$TMP/dups" ]; check "no id was handed out twice" $?
! cat "$TMP"/read_*.txt | grep -q '"ok":false'; check "concurrent reads never failed" $?
q "SELECT COUNT(*) FROM users" >/dev/null; has '"rows":[[802]]'; check "row count is 2 + 800" $?
q "SELECT COUNT(*) FROM users WHERE age = 7" >/dev/null; has '"rows":[[50]]'; check "each client's 50 rows are all stored" $?

echo "row cap and stats"
q "SELECT id FROM users" >/dev/null; has '"row_count":100,"truncated":true'; check "result capped at -m rows" $?
q "SELECT id FROM users LIMIT 5" >/dev/null; has '"row_count":5,"truncated":false'; check "LIMIT below the cap is not truncated" $?
curl -s "$URL/stats" >"$TMP/body"
has '"inserts":802' && has '"threads":8' && has '"rejected_503":0'; check "GET /stats" $?

echo "graceful shutdown and restart"
stop_server; check "SIGINT exits with status 0" $?
grep -q "bye" "$TMP/server.log"; check "server logged a clean shutdown" $?
start_server -t 2
q "SELECT COUNT(*) FROM users" >/dev/null; has '"rows":[[802]]'; check "data survives a restart" $?
q "SELECT name FROM users WHERE id = 1" >/dev/null; has '"rows":[["alice"]]' && has 'INDEX SCAN'; check "index is rebuilt on restart" $?
q "INSERT INTO users VALUES ('after', 1, 'a')" >/dev/null; has '"id":803'; check "ids continue after restart" $?
stop_server

echo "overload: 1 worker, queue of 1"
start_server -t 1 -q 1
# Hold the single worker with a connection that sends nothing, fill the queue
# with a second one, then a third must be turned away immediately.
sleep 6 | curl -s -o /dev/null -m 8 "telnet://127.0.0.1:$PORT" 2>/dev/null &
H1=$!
sleep 0.3
sleep 6 | curl -s -o /dev/null -m 8 "telnet://127.0.0.1:$PORT" 2>/dev/null &
H2=$!
sleep 0.3
curl -s -o "$TMP/body" -w '%{http_code}' -m 3 "$URL/health" >"$TMP/code"
code 503 && has 'server is busy'; check "full queue -> 503 without blocking" $?
kill $H1 $H2 2>/dev/null; wait $H1 $H2 2>/dev/null
sleep 0.3
for _ in $(seq 100); do curl -s -m 2 "$URL/health" | grep -q ok && break; sleep 0.1; done
curl -s -m 2 "$URL/health" | grep -q ok; check "server recovers once the queue drains" $?
curl -s "$URL/stats" | grep -q '"rejected_503":[1-9]'; check "rejections are counted in /stats" $?
stop_server

if [ $fail -eq 0 ]; then echo "SERVER FUNCTIONAL TESTS PASSED"; else echo "SERVER FUNCTIONAL TESTS FAILED"; exit 1; fi
