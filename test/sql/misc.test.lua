test_run = require('test_run').new()

box.sql.execute("CREATE TABLE test1(id primary key);");
box.sql.execute("CREATE TABLE test2(id primary key);");
space1 = box.space.test1
space2 = box.space.test2
assert(space1 ~= nil)
assert(space2 ~= nil)
space1:replace({1})
space2:replace({2})
box.sql.execute('SELECT * FROM test1;')
box.sql.execute('SELECT * FROM test2;')

test_run:cmd("setopt delimiter ';'")
box.sql.execute('    SELECT * FROM test1;            ');
test_run:cmd("setopt delimiter ''");

box.sql.execute('SELECT * FROM test1; SELECT * FROM test2')
box.sql.execute('SELECT * FROM test1, test2; blah blah invalid query')
box.sql.execute('DROP TABLE test1')
box.sql.execute('DROP TABLE test2')
assert(box.space.test1 == nil)
assert(box.space.test2 == nil)
