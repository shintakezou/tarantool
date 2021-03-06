test_run = require('test_run').new()

-- box.cfg()

-- create space
box.sql.execute("CREATE TABLE t1(a integer primary key, b UNIQUE, e);");

-- Debug
-- box.sql.execute("PRAGMA vdbe_debug=ON ; INSERT INTO zoobar VALUES (111, 222, 'c3', 444)")

-- Seed entries
box.sql.execute("INSERT INTO t1 VALUES(1,4,6);");
box.sql.execute("INSERT INTO t1 VALUES(2,5,7);");

-- Both entries must be updated
box.sql.execute("UPDATE t1 SET e=e+1 WHERE b IN (SELECT b FROM t1);");

-- Check
box.sql.execute("SELECT e FROM t1");

-- Cleanup
box.sql.execute("DROP TABLE t1;");

-- Debug
-- require("console").start()
