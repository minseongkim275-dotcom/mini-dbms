-- 데모 스크립트: ./build/minidb -f examples/demo.sql
-- (data/users.schema 가 정의한 users 테이블을 사용한다)

INSERT INTO users VALUES ('alice', 30, 'alice@example.com');
INSERT INTO users VALUES ('bob', 25, 'bob@example.com');
INSERT INTO users (name, age) VALUES ('carol', 41);
INSERT INTO users (email, name, age) VALUES ('dave@example.com', 'dave', 25);
INSERT INTO users VALUES ('김민성', 27, 'minseong@example.com');
INSERT INTO users VALUES ('O''Brien', 52, 'obrien@example.com');

SELECT * FROM users;
SELECT COUNT(*) FROM users;

-- id 조건은 B+ 트리 인덱스를 사용한다
SELECT * FROM users WHERE id = 3;
SELECT id, name FROM users WHERE id >= 4;
EXPLAIN SELECT * FROM users WHERE id = 3;
EXPLAIN SELECT * FROM users WHERE id < 3;

-- 다른 컬럼 조건은 파일 전체를 선형 탐색한다
SELECT name, email FROM users WHERE age = 25;
SELECT name FROM users WHERE name > 'c' LIMIT 2;
EXPLAIN SELECT * FROM users WHERE age = 25;
