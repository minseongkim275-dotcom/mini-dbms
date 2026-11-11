#!/usr/bin/env bash
# CLI functional test: runs SQL through build/minidb and checks the output.
#   bash tests/functional/test_cli.sh [BUILD_DIR]
set -u
BUILD=${1:-build}
BIN="$BUILD/minidb"
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

check() { # name, condition result
    if [ "$2" -eq 0 ]; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi
}

mkdir "$TMP/data"
cp "$HERE/../../data/users.schema" "$TMP/data/"

# 1. A script file: output (stdout and stderr together) must match exactly.
"$BIN" -d "$TMP/data" -f "$HERE/cli_cases.sql" >"$TMP/actual.txt" 2>&1
status=$?
diff -u "$HERE/cli_expected.txt" "$TMP/actual.txt" >"$TMP/diff.txt"
rc=$?
check "script output matches cli_expected.txt" $rc
[ $rc -ne 0 ] && head -40 "$TMP/diff.txt"
[ "$status" -eq 1 ]; check "exit status is 1 when a statement failed" $?

# 2. Data persists across processes, and ids continue where they stopped.
out=$("$BIN" -d "$TMP/data" -e "INSERT INTO users VALUES ('eve', 33, 'eve@example.com'); SELECT name FROM users WHERE id = 7;" 2>&1)
status=$?
echo "$out" | grep -q "inserted 1 row (id = 7)"; check "ids continue after restart" $?
echo "$out" | grep -q "| eve  |"; check "row inserted by -e is readable" $?
[ "$status" -eq 0 ]; check "exit status is 0 on success" $?

# 3. Statements piped on standard input.
out=$(printf 'SELECT COUNT(*)\nFROM users;\n' | "$BIN" -d "$TMP/data" 2>&1)
echo "$out" | grep -q "| 7        |"; check "reads statements from stdin" $?

# 4. -t prints the plan used.
out=$("$BIN" -d "$TMP/data" -t -e "SELECT * FROM users WHERE id = 2" 2>&1)
echo "$out" | grep -q "INDEX SCAN"; check "-t shows the index plan" $?

# 5. Bad invocations.
"$BIN" -d "$TMP/no-such-dir" -e "SELECT * FROM users" >"$TMP/o.txt" 2>&1
[ $? -eq 1 ] && grep -q "does not exist" "$TMP/o.txt"; check "missing data directory is reported" $?
"$BIN" -d "$TMP/data" -f "$TMP/no-such.sql" >/dev/null 2>&1
[ $? -eq 1 ]; check "missing SQL file is reported" $?
: >"$TMP/empty.sql"
"$BIN" -d "$TMP/data" -f "$TMP/empty.sql" >"$TMP/o.txt" 2>&1
[ $? -eq 0 ] && [ ! -s "$TMP/o.txt" ]; check "empty SQL file is a no-op" $?

if [ $fail -eq 0 ]; then echo "CLI FUNCTIONAL TESTS PASSED"; else echo "CLI FUNCTIONAL TESTS FAILED"; exit 1; fi
