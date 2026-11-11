-- CLI 기능 테스트 입력. 기대 출력은 cli_expected.txt.

-- 빈 테이블
SELECT * FROM users;
SELECT COUNT(*) FROM users;
SELECT * FROM users WHERE id = 1;

-- INSERT: 컬럼 목록 생략 / 지정 / 순서 변경, 대소문자 혼용, 여러 줄
INSERT INTO users VALUES ('alice', 30, 'alice@example.com');
insert into USERS values ('bob', 25, 'bob@example.com');
INSERT INTO users (name, age) VALUES ('carol', 41);
INSERT INTO users (email, age, name)
    VALUES ('dave@example.com',
            25,
            'dave');
INSERT INTO users VALUES ('김민성', 27, 'minseong@example.com');
INSERT INTO users VALUES ('O''Brien', -3, 'semi;colon -- not a comment');

-- SELECT
SELECT * FROM users;
SELECT COUNT(*) FROM users;
SELECT name, id FROM users WHERE id = 4;
SELECT id, name FROM users WHERE id >= 5;
SELECT id FROM users WHERE id < 3;
SELECT id FROM users WHERE id = 100;
SELECT name FROM users WHERE age = 25;
SELECT name FROM users WHERE age != 25 LIMIT 2;
SELECT name FROM users WHERE name = 'O''Brien';
SELECT COUNT(*) FROM users WHERE age <= 27;
SELECT * FROM users LIMIT 0;

-- 실행 계획
EXPLAIN SELECT * FROM users WHERE id = 4;
EXPLAIN SELECT * FROM users WHERE id > 4;
EXPLAIN SELECT * FROM users WHERE id != 4;
EXPLAIN SELECT * FROM users WHERE name = 'bob';
EXPLAIN SELECT COUNT(*) FROM users;

-- 오류: 뒤 문장은 계속 실행되어야 한다
SELEC * FROM users;
SELECT * FROM ghost;
SELECT nope FROM users;
SELECT * FROM users WHERE age = 'old';
INSERT INTO users VALUES ('too', 'few');
INSERT INTO users VALUES (1, 2, 3);
INSERT INTO users (id, name) VALUES (9, 'x');
INSERT INTO users VALUES ('this name is far too long for a VARCHAR(32) column', 1, 'e');
SELECT COUNT(*) FROM users;

-- 닫히지 않은 따옴표는 파일 끝까지 문자열로 읽히므로 맨 마지막에 둔다
INSERT INTO users VALUES ('unterminated, 1, 'e');
