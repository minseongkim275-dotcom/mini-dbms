#!/usr/bin/env python3
"""users 테이블용 INSERT 문을 대량으로 생성한다.

    python3 tools/gen_sql.py 1000000 > /tmp/users_1m.sql
    ./build/minidb -q -f /tmp/users_1m.sql
"""
import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
out = sys.stdout
for i in range(1, n + 1):
    out.write(f"INSERT INTO users VALUES ('user_{i}', {i * 7919 % 80 + 10}, 'user_{i}@example.com');\n")
