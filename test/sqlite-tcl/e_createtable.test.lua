#!./tcltestrunner.lua

# 2010 September 25
#
# The author disclaims copyright to this source code.  In place of
# a legal notice, here is a blessing:
#
#    May you do good and not evil.
#    May you find forgiveness for yourself and forgive others.
#    May you share freely, never taking more than you give.
#
#***********************************************************************
#
# This file implements tests to verify that the "testable statements" in 
# the lang_createtable.html document are correct.
#

set testdir [file dirname $argv0]
source $testdir/tester.tcl

set ::testprefix e_createtable

# Test organization:
#
#   e_createtable-0.*: Test that the syntax diagrams are correct.
#
#   e_createtable-1.*: Test statements related to table and database names, 
#       the TEMP and TEMPORARY keywords, and the IF NOT EXISTS clause.
#
#   e_createtable-2.*: Test "CREATE TABLE AS" statements.
#

proc do_createtable_tests {nm args} {
  uplevel do_select_tests [list e_createtable-$nm] $args
}


#-------------------------------------------------------------------------
# This command returns a serialized tcl array mapping from the name of
# each attached database to a list of tables in that database. For example,
# if the database schema is created with:
#
#   CREATE TABLE t1(x);
#   CREATE TEMP TABLE t2(x);
#   CREATE TEMP TABLE t3(x);
#
# Then this command returns "main t1 temp {t2 t3}".
#
proc table_list {} {
  set res [list]
  db eval { pragma database_list } a {
    set dbname $a(name)
    set master $a(name).sqlite_master
    if {$dbname == "temp"} { set master sqlite_temp_master }
    lappend res $dbname [
      db eval "SELECT DISTINCT tbl_name FROM $master ORDER BY tbl_name"
    ]
  }
  set res
}


# MUST_WORK_TEST

# do_createtable_tests 0.1.1 -repair {
#   drop_all_tables
# } {
#   1   "CREATE TABLE t1(c1 one)"                        {}
#   2   "CREATE TABLE t1(c1 one two)"                    {}
#   3   "CREATE TABLE t1(c1 one two three)"              {}
#   4   "CREATE TABLE t1(c1 one two three four)"         {}
#   5   "CREATE TABLE t1(c1 one two three four(14))"     {}
#   6   "CREATE TABLE t1(c1 one two three four(14, 22))" {}
#   7   "CREATE TABLE t1(c1 var(+14, -22.3))"            {}
#   8   "CREATE TABLE t1(c1 var(1.0e10))"                {}
# }
# do_createtable_tests 0.1.2 -error {
#   near "%s": syntax error
# } {
#   1   "CREATE TABLE t1(c1 one(number))"                {number}
# }


# # syntax diagram column-constraint
# #
# do_createtable_tests 0.2.1 -repair {
#   drop_all_tables 
#   execsql { CREATE TABLE t2(x PRIMARY KEY) }
# } {
#   1.1   "CREATE TABLE t1(c1 text PRIMARY KEY)"                         {}
#   1.2   "CREATE TABLE t1(c1 text PRIMARY KEY ASC)"                     {}
#   1.3   "CREATE TABLE t1(c1 text PRIMARY KEY DESC)"                    {}
#   1.4   "CREATE TABLE t1(c1 text CONSTRAINT cons PRIMARY KEY DESC)"    {}

#   2.1   "CREATE TABLE t1(c1 text NOT NULL)"                            {}
#   2.2   "CREATE TABLE t1(c1 text CONSTRAINT nm NOT NULL)"              {}
#   2.3   "CREATE TABLE t1(c1 text NULL)"                                {}
#   2.4   "CREATE TABLE t1(c1 text CONSTRAINT nm NULL)"                  {}

#   3.1   "CREATE TABLE t1(c1 text UNIQUE)"                              {}
#   3.2   "CREATE TABLE t1(c1 text CONSTRAINT un UNIQUE)"                {}

#   4.1   "CREATE TABLE t1(c1 text CHECK(c1!=0))"                        {}
#   4.2   "CREATE TABLE t1(c1 text CONSTRAINT chk CHECK(c1!=0))"         {}

#   5.1   "CREATE TABLE t1(c1 text DEFAULT 1)"                           {}
#   5.2   "CREATE TABLE t1(c1 text DEFAULT -1)"                          {}
#   5.3   "CREATE TABLE t1(c1 text DEFAULT +1)"                          {}
#   5.4   "CREATE TABLE t1(c1 text DEFAULT -45.8e22)"                    {}
#   5.5   "CREATE TABLE t1(c1 text DEFAULT (1+1))"                       {}
#   5.6   "CREATE TABLE t1(c1 text CONSTRAINT \"1 2\" DEFAULT (1+1))"    {}

#   6.1   "CREATE TABLE t1(c1 text COLLATE nocase)"        {}
#   6.2   "CREATE TABLE t1(c1 text CONSTRAINT 'a x' COLLATE nocase)"     {}

#   7.1   "CREATE TABLE t1(c1 REFERENCES t2)"                            {}
#   7.2   "CREATE TABLE t1(c1 CONSTRAINT abc REFERENCES t2)"             {}

#   8.1   {
#     CREATE TABLE t1(c1 
#       PRIMARY KEY NOT NULL UNIQUE CHECK(c1 IS 'ten') DEFAULT 123 REFERENCES t1
#     );
#   } {}
#   8.2   {
#     CREATE TABLE t1(c1 
#       REFERENCES t1 DEFAULT 123 CHECK(c1 IS 'ten') UNIQUE NOT NULL PRIMARY KEY 
#     );
#   } {}
# }

# # -- syntax diagram table-constraint
# #
# do_createtable_tests 0.3.1 -repair {
#   drop_all_tables 
#   execsql { CREATE TABLE t2(x PRIMARY KEY) }
# } {
#   1.1   "CREATE TABLE t1(c1, c2, PRIMARY KEY(c1))"                         {}
#   1.2   "CREATE TABLE t1(c1, c2, PRIMARY KEY(c1, c2))"                     {}
#   1.3   "CREATE TABLE t1(c1, c2, PRIMARY KEY(c1, c2) ON CONFLICT IGNORE)"  {}

#   2.1   "CREATE TABLE t1(c1, c2, UNIQUE(c1))"                              {}
#   2.2   "CREATE TABLE t1(c1, c2, UNIQUE(c1, c2))"                          {}
#   2.3   "CREATE TABLE t1(c1, c2, UNIQUE(c1, c2) ON CONFLICT IGNORE)"       {}

#   3.1   "CREATE TABLE t1(c1, c2, CHECK(c1 IS NOT c2))"                     {}

#   4.1   "CREATE TABLE t1(c1, c2, FOREIGN KEY(c1) REFERENCES t2)"           {}
# }

# # -- syntax diagram column-def
# #
# do_createtable_tests 0.4.1 -repair {
#   drop_all_tables 
# } {
#   1     {CREATE TABLE t1(
#            col1,
#            col2 TEXT,
#            col3 INTEGER UNIQUE,
#            col4 VARCHAR(10, 10) PRIMARY KEY,
#            "name with spaces" REFERENCES t1
#          );
#         } {}
# }

# # -- syntax diagram create-table-stmt
# #
# do_createtable_tests 0.5.1 -repair {
#   drop_all_tables 
#   execsql { CREATE TABLE t2(a, b, c) }
# } {
#   1     "CREATE TABLE t1(a, b, c)"                                    {}
#   2     "CREATE TEMP TABLE t1(a, b, c)"                               {}
#   3     "CREATE TEMPORARY TABLE t1(a, b, c)"                          {}
#   4     "CREATE TABLE IF NOT EXISTS t1(a, b, c)"                      {}
#   5     "CREATE TEMP TABLE IF NOT EXISTS t1(a, b, c)"                 {}
#   6     "CREATE TEMPORARY TABLE IF NOT EXISTS t1(a, b, c)"            {}

#   7     "CREATE TABLE main.t1(a, b, c)"                               {}
#   8     "CREATE TEMP TABLE temp.t1(a, b, c)"                          {}
#   9     "CREATE TEMPORARY TABLE temp.t1(a, b, c)"                     {}
#   10    "CREATE TABLE IF NOT EXISTS main.t1(a, b, c)"                 {}
#   11    "CREATE TEMP TABLE IF NOT EXISTS temp.t1(a, b, c)"            {}
#   12    "CREATE TEMPORARY TABLE IF NOT EXISTS temp.t1(a, b, c)"       {}

#   13    "CREATE TABLE t1 AS SELECT * FROM t2"                         {}
#   14    "CREATE TEMP TABLE t1 AS SELECT c, b, a FROM t2"              {}
#   15    "CREATE TABLE t1 AS SELECT count(*), max(b), min(a) FROM t2"  {}
# }

# #
# #   1:         Explicit parent-key columns.
# #   2:         Implicit child-key columns.
# #
# #   1:         MATCH FULL
# #   2:         MATCH PARTIAL
# #   3:         MATCH SIMPLE
# #   4:         MATCH STICK
# #   5:         
# #
# #   1:         ON DELETE SET NULL
# #   2:         ON DELETE SET DEFAULT
# #   3:         ON DELETE CASCADE
# #   4:         ON DELETE RESTRICT
# #   5:         ON DELETE NO ACTION
# #   6:
# #
# #   1:         ON UPDATE SET NULL
# #   2:         ON UPDATE SET DEFAULT
# #   3:         ON UPDATE CASCADE
# #   4:         ON UPDATE RESTRICT
# #   5:         ON UPDATE NO ACTION
# #   6:
# #
# #   1:         NOT DEFERRABLE INITIALLY DEFERRED
# #   2:         NOT DEFERRABLE INITIALLY IMMEDIATE
# #   3:         NOT DEFERRABLE
# #   4:         DEFERRABLE INITIALLY DEFERRED
# #   5:         DEFERRABLE INITIALLY IMMEDIATE
# #   6:         DEFERRABLE
# #   7:         
# #
# do_createtable_tests 0.6.1 -repair {
#   drop_all_tables 
#   execsql { CREATE TABLE t2(x PRIMARY KEY, y) }
#   execsql { CREATE TABLE t3(i, j, UNIQUE(i, j) ) }
# } {
#   11146 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH FULL 
#     ON DELETE SET NULL ON UPDATE RESTRICT DEFERRABLE
#   )} {}
#   11412 { CREATE TABLE t1(a 
#     REFERENCES t2(x) 
#     ON DELETE RESTRICT ON UPDATE SET NULL MATCH FULL 
#     NOT DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   12135 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH PARTIAL 
#     ON DELETE SET NULL ON UPDATE CASCADE DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   12427 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH PARTIAL 
#     ON DELETE RESTRICT ON UPDATE SET DEFAULT 
#   )} {}
#   12446 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH PARTIAL 
#     ON DELETE RESTRICT ON UPDATE RESTRICT DEFERRABLE
#   )} {}
#   12522 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH PARTIAL 
#     ON DELETE NO ACTION ON UPDATE SET DEFAULT NOT DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   13133 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH SIMPLE 
#     ON DELETE SET NULL ON UPDATE CASCADE NOT DEFERRABLE
#   )} {}
#   13216 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH SIMPLE 
#     ON DELETE SET DEFAULT ON UPDATE SET NULL DEFERRABLE
#   )} {}
#   13263 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH SIMPLE 
#     ON DELETE SET DEFAULT  NOT DEFERRABLE
#   )} {}
#   13421 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH SIMPLE 
#     ON DELETE RESTRICT ON UPDATE SET DEFAULT NOT DEFERRABLE INITIALLY DEFERRED
#   )} {}
#   13432 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH SIMPLE 
#     ON DELETE RESTRICT ON UPDATE CASCADE NOT DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   13523 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH SIMPLE 
#     ON DELETE NO ACTION ON UPDATE SET DEFAULT NOT DEFERRABLE
#   )} {}
#   14336 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH STICK 
#     ON DELETE CASCADE ON UPDATE CASCADE DEFERRABLE
#   )} {}
#   14611 { CREATE TABLE t1(a 
#     REFERENCES t2(x) MATCH STICK 
#     ON UPDATE SET NULL NOT DEFERRABLE INITIALLY DEFERRED
#   )} {}
#   15155 { CREATE TABLE t1(a 
#     REFERENCES t2(x)
#     ON DELETE SET NULL ON UPDATE NO ACTION DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   15453 { CREATE TABLE t1(a 
#     REFERENCES t2(x) ON DELETE RESTRICT ON UPDATE NO ACTION NOT DEFERRABLE
#   )} {}
#   15661 { CREATE TABLE t1(a 
#     REFERENCES t2(x) NOT DEFERRABLE INITIALLY DEFERRED
#   )} {}
#   21115 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH FULL 
#     ON DELETE SET NULL ON UPDATE SET NULL DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   21123 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH FULL 
#     ON DELETE SET NULL ON UPDATE SET DEFAULT NOT DEFERRABLE
#   )} {}
#   21217 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH FULL ON DELETE SET DEFAULT ON UPDATE SET NULL 
#   )} {}
#   21362 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH FULL 
#     ON DELETE CASCADE NOT DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   22143 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH PARTIAL 
#     ON DELETE SET NULL ON UPDATE RESTRICT NOT DEFERRABLE
#   )} {}
#   22156 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH PARTIAL 
#     ON DELETE SET NULL ON UPDATE NO ACTION DEFERRABLE
#   )} {}
#   22327 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH PARTIAL ON DELETE CASCADE ON UPDATE SET DEFAULT 
#   )} {}
#   22663 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH PARTIAL NOT DEFERRABLE
#   )} {}
#   23236 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH SIMPLE 
#     ON DELETE SET DEFAULT ON UPDATE CASCADE DEFERRABLE
#   )} {}
#   24155 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH STICK 
#     ON DELETE SET NULL ON UPDATE NO ACTION DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   24522 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH STICK 
#     ON DELETE NO ACTION ON UPDATE SET DEFAULT NOT DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   24625 { CREATE TABLE t1(a 
#     REFERENCES t2 MATCH STICK 
#     ON UPDATE SET DEFAULT DEFERRABLE INITIALLY IMMEDIATE
#   )} {}
#   25454 { CREATE TABLE t1(a 
#     REFERENCES t2 
#     ON DELETE RESTRICT ON UPDATE NO ACTION DEFERRABLE INITIALLY DEFERRED
#   )} {}
# }

# #-------------------------------------------------------------------------
# # Test cases e_createtable-1.* - test statements related to table and
# # database names, the TEMP and TEMPORARY keywords, and the IF NOT EXISTS
# # clause.
# #
# drop_all_tables
# forcedelete test.db2 test.db3

# do_execsql_test e_createtable-1.0 {
#   ATTACH 'test.db2' AS auxa;
#   ATTACH 'test.db3' AS auxb;
# } {}

# # EVIDENCE-OF: R-17899-04554 Table names that begin with "sqlite_" are
# # reserved for internal use. It is an error to attempt to create a table
# # with a name that starts with "sqlite_".
# #
# do_createtable_tests 1.1.1 -error {
#   object name reserved for internal use: %s
# } {
#   1    "CREATE TABLE sqlite_abc(a, b, c)"        sqlite_abc
#   2    "CREATE TABLE temp.sqlite_helloworld(x)"  sqlite_helloworld
#   3    {CREATE TABLE auxa."sqlite__"(x, y)}      sqlite__
#   4    {CREATE TABLE auxb."sqlite_"(z)}          sqlite_
#   5    {CREATE TABLE "SQLITE_TBL"(z)}            SQLITE_TBL
# }
# do_createtable_tests 1.1.2 {
#   1    "CREATE TABLE sqlit_abc(a, b, c)"         {}
#   2    "CREATE TABLE temp.sqlitehelloworld(x)"   {}
#   3    {CREATE TABLE auxa."sqlite"(x, y)}        {}
#   4    {CREATE TABLE auxb."sqlite-"(z)}          {}
#   5    {CREATE TABLE "SQLITE-TBL"(z)}            {}
# }


# # EVIDENCE-OF: R-18448-33677 If a schema-name is specified, it must be
# # either "main", "temp", or the name of an attached database.
# #
# # EVIDENCE-OF: R-39822-07822 In this case the new table is created in
# # the named database.
# #
# #   Test cases 1.2.* test the first of the two requirements above. The
# #   second is verified by cases 1.3.*.
# #
# do_createtable_tests 1.2.1 -error {
#   unknown database %s
# } {
#   1    "CREATE TABLE george.t1(a, b)"            george
#   2    "CREATE TABLE _.t1(a, b)"                 _
# }
# do_createtable_tests 1.2.2 {
#   1    "CREATE TABLE main.abc(a, b, c)"          {}
#   2    "CREATE TABLE temp.helloworld(x)"         {}
#   3    {CREATE TABLE auxa."t 1"(x, y)}           {}
#   4    {CREATE TABLE auxb.xyz(z)}                {}
# }
# drop_all_tables
# do_createtable_tests 1.3 -tclquery {
#   unset -nocomplain X
#   array set X [table_list]
#   list $X(main) $X(temp) $X(auxa) $X(auxb)
# } {
#   1    "CREATE TABLE main.abc(a, b, c)"  {abc {} {} {}}
#   2    "CREATE TABLE main.t1(a, b, c)"   {{abc t1} {} {} {}}
#   3    "CREATE TABLE temp.tmp(a, b, c)"  {{abc t1} tmp {} {}}
#   4    "CREATE TABLE auxb.tbl(x, y)"     {{abc t1} tmp {} tbl}
#   5    "CREATE TABLE auxb.t1(k, v)"      {{abc t1} tmp {} {t1 tbl}}
#   6    "CREATE TABLE auxa.next(c, d)"    {{abc t1} tmp next {t1 tbl}}
# }

# # EVIDENCE-OF: R-18895-27365 If the "TEMP" or "TEMPORARY" keyword occurs
# # between the "CREATE" and "TABLE" then the new table is created in the
# # temp database.
# #
# drop_all_tables
# do_createtable_tests 1.4 -tclquery {
#   unset -nocomplain X
#   array set X [table_list]
#   list $X(main) $X(temp) $X(auxa) $X(auxb)
# } {
#   1    "CREATE TEMP TABLE t1(a primary key, b)"      {{} t1 {} {}}
#   2    "CREATE TEMPORARY TABLE t2(a PRIMARY key, b)" {{} {t1 t2} {} {}}
# }

# # EVIDENCE-OF: R-23976-43329 It is an error to specify both a
# # schema-name and the TEMP or TEMPORARY keyword, unless the schema-name
# # is "temp".
# #
# drop_all_tables
# do_createtable_tests 1.5.1 -error {
#   temporary table name must be unqualified
# } {
#   1    "CREATE TEMP TABLE main.t1(a, b)"        {}
#   2    "CREATE TEMPORARY TABLE auxa.t2(a, b)"   {}
#   3    "CREATE TEMP TABLE auxb.t3(a, b)"        {}
#   4    "CREATE TEMPORARY TABLE main.xxx(x)"     {}
# }
# drop_all_tables
# do_createtable_tests 1.5.2 -tclquery {
#   unset -nocomplain X
#   array set X [table_list]
#   list $X(main) $X(temp) $X(auxa) $X(auxb)
# } {
#   1    "CREATE TEMP TABLE temp.t1(a, b)"        {{} t1 {} {}}
#   2    "CREATE TEMPORARY TABLE temp.t2(a, b)"   {{} {t1 t2} {} {}}
#   3    "CREATE TEMP TABLE TEMP.t3(a, b)"        {{} {t1 t2 t3} {} {}}
#   4    "CREATE TEMPORARY TABLE TEMP.xxx(x)"     {{} {t1 t2 t3 xxx} {} {}}
# }

# # EVIDENCE-OF: R-31997-24564 If no schema name is specified and the TEMP
# # keyword is not present then the table is created in the main database.
# #
# drop_all_tables
# do_createtable_tests 1.6 -tclquery {
#   unset -nocomplain X
#   array set X [table_list]
#   list $X(main) $X(temp) $X(auxa) $X(auxb)
# } {
#   1    "CREATE TABLE t1(a, b)"   {t1 {} {} {}}
#   2    "CREATE TABLE t2(a, b)"   {{t1 t2} {} {} {}}
#   3    "CREATE TABLE t3(a, b)"   {{t1 t2 t3} {} {} {}}
#   4    "CREATE TABLE xxx(x)"     {{t1 t2 t3 xxx} {} {} {}}
# }

# # drop_all_tables
# do_execsql_test e_createtable-1.7.0 {
#   CREATE TABLE t1(x, y);
#   CREATE INDEX i1 ON t1(x);
#   CREATE VIEW  v1 AS SELECT * FROM t1;

#   CREATE TABLE auxa.tbl1(x, y);
#   CREATE INDEX auxa.idx1 ON tbl1(x);
#   CREATE VIEW auxa.view1 AS SELECT * FROM tbl1;
# } {}

# # EVIDENCE-OF: R-01232-54838 It is usually an error to attempt to create
# # a new table in a database that already contains a table, index or view
# # of the same name.
# #
# #   Test cases 1.7.1.* verify that creating a table in a database with a
# #   table/index/view of the same name does fail. 1.7.2.* tests that creating
# #   a table with the same name as a table/index/view in a different database
# #   is Ok.
# #
# do_createtable_tests 1.7.1 -error { %s } {
#   1    "CREATE TABLE t1(a, b)"   {{table t1 already exists}}
#   2    "CREATE TABLE i1(a, b)"   {{there is already an index named i1}}
#   3    "CREATE TABLE v1(a, b)"   {{table v1 already exists}}
#   4    "CREATE TABLE auxa.tbl1(a, b)"   {{table tbl1 already exists}}
#   5    "CREATE TABLE auxa.idx1(a, b)"   {{there is already an index named idx1}}
#   6    "CREATE TABLE auxa.view1(a, b)"  {{table view1 already exists}}
# }
# do_createtable_tests 1.7.2 {
#   1    "CREATE TABLE auxa.t1(a, b)"   {}
#   2    "CREATE TABLE auxa.i1(a, b)"   {}
#   3    "CREATE TABLE auxa.v1(a, b)"   {}
#   4    "CREATE TABLE tbl1(a, b)"      {}
#   5    "CREATE TABLE idx1(a, b)"      {}
#   6    "CREATE TABLE view1(a, b)"     {}
# }

# # EVIDENCE-OF: R-33917-24086 However, if the "IF NOT EXISTS" clause is
# # specified as part of the CREATE TABLE statement and a table or view of
# # the same name already exists, the CREATE TABLE command simply has no
# # effect (and no error message is returned).
# #
# drop_all_tables
# do_execsql_test e_createtable-1.8.0 {
#   CREATE TABLE t1(x, y);
#   CREATE INDEX i1 ON t1(x);
#   CREATE VIEW  v1 AS SELECT * FROM t1;
#   CREATE TABLE auxa.tbl1(x, y);
#   CREATE INDEX auxa.idx1 ON tbl1(x);
#   CREATE VIEW auxa.view1 AS SELECT * FROM tbl1;
# } {}
# do_createtable_tests 1.8 {
#   1    "CREATE TABLE IF NOT EXISTS t1(a, b)"          {}
#   2    "CREATE TABLE IF NOT EXISTS auxa.tbl1(a, b)"   {}
#   3    "CREATE TABLE IF NOT EXISTS v1(a, b)"          {}
#   4    "CREATE TABLE IF NOT EXISTS auxa.view1(a, b)"  {}
# }

# # EVIDENCE-OF: R-16465-40078 An error is still returned if the table
# # cannot be created because of an existing index, even if the "IF NOT
# # EXISTS" clause is specified.
# #
# do_createtable_tests 1.9 -error { %s } {
#   1    "CREATE TABLE IF NOT EXISTS i1(a, b)"   
#        {{there is already an index named i1}}
#   2    "CREATE TABLE IF NOT EXISTS auxa.idx1(a, b)"   
#        {{there is already an index named idx1}}
# }

# # EVIDENCE-OF: R-05513-33819 It is not an error to create a table that
# # has the same name as an existing trigger.
# #
# drop_all_tables
# do_execsql_test e_createtable-1.10.0 {
#   CREATE TABLE t1(x, y);
#   CREATE TABLE auxb.t2(x, y);

#   CREATE TRIGGER tr1 AFTER INSERT ON t1 BEGIN
#     SELECT 1;
#   END;
#   CREATE TRIGGER auxb.tr2 AFTER INSERT ON t2 BEGIN
#     SELECT 1;
#   END;
# } {}
# do_createtable_tests 1.10 {
#   1    "CREATE TABLE tr1(a, b)"          {}
#   2    "CREATE TABLE tr2(a, b)"          {}
#   3    "CREATE TABLE auxb.tr1(a, b)"     {}
#   4    "CREATE TABLE auxb.tr2(a, b)"     {}
# }

# # EVIDENCE-OF: R-22283-14179 Tables are removed using the DROP TABLE
# # statement.
# #
# drop_all_tables
# do_execsql_test e_createtable-1.11.0 {
#   CREATE TABLE t1(a, b);
#   CREATE TABLE t2(a, b);
#   CREATE TABLE auxa.t3(a, b);
#   CREATE TABLE auxa.t4(a, b);
# } {}

# do_execsql_test e_createtable-1.11.1.1 {
#   SELECT * FROM t1;
#   SELECT * FROM t2;
#   SELECT * FROM t3;
#   SELECT * FROM t4;
# } {}
# do_execsql_test  e_createtable-1.11.1.2 { DROP TABLE t1 } {}
# do_catchsql_test e_createtable-1.11.1.3 { 
#   SELECT * FROM t1 
# } {1 {no such table: t1}}
# do_execsql_test  e_createtable-1.11.1.4 { DROP TABLE t3 } {}
# do_catchsql_test e_createtable-1.11.1.5 { 
#   SELECT * FROM t3 
# } {1 {no such table: t3}}

# do_execsql_test e_createtable-1.11.2.1 {
#   SELECT name FROM sqlite_master;
#   SELECT name FROM auxa.sqlite_master;
# } {t2 t4}
# do_execsql_test  e_createtable-1.11.2.2 { DROP TABLE t2 } {}
# do_execsql_test  e_createtable-1.11.2.3 { DROP TABLE t4 } {}
# do_execsql_test e_createtable-1.11.2.4 {
#   SELECT name FROM sqlite_master;
#   SELECT name FROM auxa.sqlite_master;
# } {}

# #-------------------------------------------------------------------------
# # Test cases e_createtable-2.* - test statements related to the CREATE
# # TABLE AS ... SELECT statement.
# #

# # Three Tcl commands:
# #
# #   select_column_names SQL
# #     The argument must be a SELECT statement. Return a list of the names
# #     of the columns of the result-set that would be returned by executing
# #     the SELECT.
# #
# #   table_column_names TBL
# #     The argument must be a table name. Return a list of column names, from
# #     left to right, for the table.
# #
# #   table_column_decltypes TBL
# #     The argument must be a table name. Return a list of column declared
# #     types, from left to right, for the table.
# #
# proc sci {select cmd} {
#   set res [list]
#   set STMT [sqlite3_prepare_v2 db $select -1 dummy]
#   for {set i 0} {$i < [sqlite3_column_count $STMT]} {incr i} {
#     lappend res [$cmd $STMT $i]
#   }
#   sqlite3_finalize $STMT
#   set res
# }
# proc tci {tbl cmd} { sci "SELECT * FROM $tbl" $cmd }
# proc select_column_names    {sql} { sci $sql sqlite3_column_name }
# proc table_column_names     {tbl} { tci $tbl sqlite3_column_name }
# proc table_column_decltypes {tbl} { tci $tbl sqlite3_column_decltype }

# MUST_WORK_TEST

# # Create a database schema. This schema is used by tests 2.1.* through 2.3.*.
# #
# drop_all_tables
# do_execsql_test e_createtable-2.0 {
#   CREATE TABLE t1(a, b, c);
#   CREATE TABLE t2(d, e, f);
#   CREATE TABLE t3(g BIGINT, h VARCHAR(10));
#   CREATE TABLE t4(i BLOB, j ANYOLDATA);
#   CREATE TABLE t5(k FLOAT, l INTEGER);
#   CREATE TABLE t6(m DEFAULT 10, n DEFAULT 5, PRIMARY KEY(m, n));
#   CREATE TABLE t7(x INTEGER PRIMARY KEY);
#   CREATE TABLE t8(o COLLATE nocase DEFAULT 'abc');
#   CREATE TABLE t9(p NOT NULL, q DOUBLE CHECK (q!=0), r STRING UNIQUE);
# } {}

# # EVIDENCE-OF: R-64828-59568 The table has the same number of columns as
# # the rows returned by the SELECT statement. The name of each column is
# # the same as the name of the corresponding column in the result set of
# # the SELECT statement.
# #
# do_createtable_tests 2.1 -tclquery {
#   table_column_names x1
# } -repair {
#   catchsql { DROP TABLE x1 }
# } {
#   1    "CREATE TABLE x1 AS SELECT * FROM t1"                     {a b c}
#   2    "CREATE TABLE x1 AS SELECT c, b, a FROM t1"               {c b a}
#   3    "CREATE TABLE x1 AS SELECT * FROM t1, t2"                 {a b c d e f}
#   4    "CREATE TABLE x1 AS SELECT count(*) FROM t1"              {count(*)}
#   5    "CREATE TABLE x1 AS SELECT count(a) AS a, max(b) FROM t1" {a max(b)}
# }

# # EVIDENCE-OF: R-37111-22855 The declared type of each column is
# # determined by the expression affinity of the corresponding expression
# # in the result set of the SELECT statement, as follows: Expression
# # Affinity Column Declared Type TEXT "TEXT" NUMERIC "NUM" INTEGER "INT"
# # REAL "REAL" NONE "" (empty string)
# #
# do_createtable_tests 2.2 -tclquery {
#   table_column_decltypes x1
# } -repair {
#   catchsql { DROP TABLE x1 }
# } {
#   1    "CREATE TABLE x1 AS SELECT a FROM t1"     {""}
#   2    "CREATE TABLE x1 AS SELECT * FROM t3"     {INT TEXT}
#   3    "CREATE TABLE x1 AS SELECT * FROM t4"     {"" NUM}
#   4    "CREATE TABLE x1 AS SELECT * FROM t5"     {REAL INT}
# }

# # EVIDENCE-OF: R-16667-09772 A table created using CREATE TABLE AS has
# # no PRIMARY KEY and no constraints of any kind. The default value of
# # each column is NULL. The default collation sequence for each column of
# # the new table is BINARY.
# #
# #   The following tests create tables based on SELECT statements that read
# #   from tables that have primary keys, constraints and explicit default 
# #   collation sequences. None of this is transfered to the definition of
# #   the new table as stored in the sqlite_master table.
# #
# #   Tests 2.3.2.* show that the default value of each column is NULL.
# #
# do_createtable_tests 2.3.1 -query {
#   SELECT sql FROM sqlite_master ORDER BY rowid DESC LIMIT 1
# } {
#   1    "CREATE TABLE x1 AS SELECT * FROM t6" {{CREATE TABLE x1(m,n)}}
#   2    "CREATE TABLE x2 AS SELECT * FROM t7" {{CREATE TABLE x2(x INT)}}
#   3    "CREATE TABLE x3 AS SELECT * FROM t8" {{CREATE TABLE x3(o)}}
#   4    "CREATE TABLE x4 AS SELECT * FROM t9" {{CREATE TABLE x4(p,q REAL,r NUM)}}
# }
# do_execsql_test e_createtable-2.3.2.1 {
#   INSERT INTO x1 DEFAULT VALUES;
#   INSERT INTO x2 DEFAULT VALUES;
#   INSERT INTO x3 DEFAULT VALUES;
#   INSERT INTO x4 DEFAULT VALUES;
# } {}
# db nullvalue null
# do_execsql_test e_createtable-2.3.2.2 { SELECT * FROM x1 } {null null}
# do_execsql_test e_createtable-2.3.2.3 { SELECT * FROM x2 } {null}
# do_execsql_test e_createtable-2.3.2.4 { SELECT * FROM x3 } {null}
# do_execsql_test e_createtable-2.3.2.5 { SELECT * FROM x4 } {null null null}
# db nullvalue {}

# drop_all_tables
# do_execsql_test e_createtable-2.4.0 {
#   CREATE TABLE t1(x, y);
#   INSERT INTO t1 VALUES('i',   'one');
#   INSERT INTO t1 VALUES('ii',  'two');
#   INSERT INTO t1 VALUES('iii', 'three');
# } {}

# # EVIDENCE-OF: R-24153-28352 Tables created using CREATE TABLE AS are
# # initially populated with the rows of data returned by the SELECT
# # statement.
# #
# # EVIDENCE-OF: R-08224-30249 Rows are assigned contiguously ascending
# # rowid values, starting with 1, in the order that they are returned by
# # the SELECT statement.
# #
# #   Each test case below is specified as the name of a table to create
# #   using "CREATE TABLE ... AS SELECT ..." and a SELECT statement to use in
# #   creating it. The table is created. 
# #
# #   Test cases 2.4.*.1 check that after it has been created, the data in the
# #   table is the same as the data returned by the SELECT statement executed as
# #   a standalone command, verifying the first testable statement above.
# #
# #   Test cases 2.4.*.2 check that the rowids were allocated contiguously
# #   as required by the second testable statement above. That the rowids
# #   from the contiguous block were allocated to rows in the order rows are
# #   returned by the SELECT statement is verified by 2.4.*.1.
# #
# # EVIDENCE-OF: R-32365-09043 A "CREATE TABLE ... AS SELECT" statement
# # creates and populates a database table based on the results of a
# # SELECT statement.
# #
# #   The above is also considered to be tested by the following. It is
# #   clear that tables are being created and populated by the command in
# #   question.
# #
# foreach {tn tbl select} {
#   1   x1   "SELECT * FROM t1"
#   2   x2   "SELECT * FROM t1 ORDER BY x DESC"
#   3   x3   "SELECT * FROM t1 ORDER BY x ASC"
# } {
#   # Create the table using a "CREATE TABLE ... AS SELECT ..." command.
#   execsql [subst {CREATE TABLE $tbl AS $select}]

#   # Check that the rows inserted into the table, sorted in ascending rowid
#   # order, match those returned by executing the SELECT statement as a
#   # standalone command.
#   do_execsql_test e_createtable-2.4.$tn.1 [subst {
#     SELECT * FROM $tbl ORDER BY rowid;
#   }] [execsql $select]

#   # Check that the rowids in the new table are a contiguous block starting
#   # with rowid 1. Note that this will fail if SELECT statement $select 
#   # returns 0 rows (as max(rowid) will be NULL).
#   do_execsql_test e_createtable-2.4.$tn.2 [subst {
#     SELECT min(rowid), count(rowid)==max(rowid) FROM $tbl
#   }] {1 1}
# }

# #--------------------------------------------------------------------------
# # Test cases for column defintions in CREATE TABLE statements that do not
# # use a SELECT statement. Not including data constraints. In other words,
# # tests for the specification of:
# #
# #   * declared types,
# #   * default values, and
# #   * default collation sequences.
# #

# # EVIDENCE-OF: R-27219-49057 Unlike most SQL databases, SQLite does not
# # restrict the type of data that may be inserted into a column based on
# # the columns declared type.
# #
# #   Test this by creating a few tables with varied declared types, then
# #   inserting various different types of values into them.
# #
# drop_all_tables
# do_execsql_test e_createtable-3.1.0 {
#   CREATE TABLE t1(x VARCHAR(10), y INTEGER, z DOUBLE);
#   CREATE TABLE t2(a DATETIME, b STRING, c REAL);
#   CREATE TABLE t3(o, t);
# } {}

# # value type -> declared column type
# # ----------------------------------
# # integer    -> VARCHAR(10)
# # string     -> INTEGER
# # blob       -> DOUBLE
# #
# do_execsql_test e_createtable-3.1.1 {
#   INSERT INTO t1 VALUES(14, 'quite a lengthy string', X'555655');
#   SELECT * FROM t1;
# } {14 {quite a lengthy string} UVU}

# # string     -> DATETIME
# # integer    -> STRING
# # time       -> REAL
# #
# do_execsql_test e_createtable-3.1.2 {
#   INSERT INTO t2 VALUES('not a datetime', 13, '12:41:59');
#   SELECT * FROM t2;
# } {{not a datetime} 13 12:41:59}

# # EVIDENCE-OF: R-10565-09557 The declared type of a column is used to
# # determine the affinity of the column only.
# #
# #     Affinities are tested in more detail elsewhere (see document
# #     datatype3.html). Here, just test that affinity transformations
# #     consistent with the expected affinity of each column (based on
# #     the declared type) appear to take place.
# #
# # Affinities of t1 (test cases 3.2.1.*): TEXT, INTEGER, REAL
# # Affinities of t2 (test cases 3.2.2.*): NUMERIC, NUMERIC, REAL
# # Affinities of t3 (test cases 3.2.3.*): NONE, NONE
# #
# do_execsql_test e_createtable-3.2.0 { DELETE FROM t1; DELETE FROM t2; } {}

# do_createtable_tests 3.2.1 -query {
#   SELECT quote(x), quote(y), quote(z) FROM t1 ORDER BY rowid DESC LIMIT 1;
# } {
#   1   "INSERT INTO t1 VALUES(15,   '22.0', '14')"   {'15' 22 14.0}
#   2   "INSERT INTO t1 VALUES(22.0, 22.0, 22.0)"     {'22.0' 22 22.0}
# }
# do_createtable_tests 3.2.2 -query {
#   SELECT quote(a), quote(b), quote(c) FROM t2 ORDER BY rowid DESC LIMIT 1;
# } {
#   1   "INSERT INTO t2 VALUES(15,   '22.0', '14')"   {15   22  14.0}
#   2   "INSERT INTO t2 VALUES(22.0, 22.0, 22.0)"     {22   22  22.0}
# }
# do_createtable_tests 3.2.3 -query {
#   SELECT quote(o), quote(t) FROM t3 ORDER BY rowid DESC LIMIT 1;
# } {
#   1   "INSERT INTO t3 VALUES('15', '22.0')"         {'15' '22.0'}
#   2   "INSERT INTO t3 VALUES(15, 22.0)"             {15 22.0}
# }

# # EVIDENCE-OF: R-42316-09582 If there is no explicit DEFAULT clause
# # attached to a column definition, then the default value of the column
# # is NULL.
# #
# #     None of the columns in table t1 have an explicit DEFAULT clause.
# #     So testing that the default value of all columns in table t1 is
# #     NULL serves to verify the above.
# #     
# do_createtable_tests 3.2.3 -query {
#   SELECT quote(x), quote(y), quote(z) FROM t1
# } -repair {
#   execsql { DELETE FROM t1 }
# } {
#   1   "INSERT INTO t1(x, y) VALUES('abc', 'xyz')"   {'abc' 'xyz' NULL}
#   2   "INSERT INTO t1(x, z) VALUES('abc', 'xyz')"   {'abc' NULL 'xyz'}
#   3   "INSERT INTO t1 DEFAULT VALUES"               {NULL NULL NULL}
# }

# MUST_WORK_TEST

# # EVIDENCE-OF: R-07343-35026 An explicit DEFAULT clause may specify that
# # the default value is NULL, a string constant, a blob constant, a
# # signed-number, or any constant expression enclosed in parentheses. A
# # default value may also be one of the special case-independent keywords
# # CURRENT_TIME, CURRENT_DATE or CURRENT_TIMESTAMP.
# #
# do_execsql_test e_createtable-3.3.1 {
#   CREATE TABLE t4(
#     a DEFAULT NULL,
#     b DEFAULT 'string constant',
#     c DEFAULT X'424C4F42',
#     d DEFAULT 1,
#     e DEFAULT -1,
#     f DEFAULT 3.14,
#     g DEFAULT -3.14,
#     h DEFAULT ( substr('abcd', 0, 2) || 'cd' ),
#     i DEFAULT CURRENT_TIME,
#     j DEFAULT CURRENT_DATE,
#     k DEFAULT CURRENT_TIMESTAMP
#   );
# } {}

# EVIDENCE-OF: R-18415-27776 For the purposes of the DEFAULT clause, an
# expression is considered constant if it does contains no sub-queries,
# column or table references, bound parameters, or string literals
# enclosed in double-quotes instead of single-quotes.
#
do_createtable_tests 3.4.1 -error {
  default value of column [x] is not constant
} {
  1   {CREATE TABLE t5(x DEFAULT ( (SELECT 1) ))}  {}
  2   {CREATE TABLE t5(x DEFAULT ( "abc" ))}  {}
  3   {CREATE TABLE t5(x DEFAULT ( 1 IN (SELECT 1) ))}  {}
  4   {CREATE TABLE t5(x DEFAULT ( EXISTS (SELECT 1) ))}  {}
  5   {CREATE TABLE t5(x DEFAULT ( x!=?1 ))}  {}
}
do_createtable_tests 3.4.2 -repair {
  catchsql { DROP TABLE t5 }
} {
  1   {CREATE TABLE t5(id primary key, x DEFAULT ( 'abc' ))}  {}
  2   {CREATE TABLE t5(id primary key, x DEFAULT ( 1 IN (1, 2, 3) ))}  {}
}

# # EVIDENCE-OF: R-18814-23501 Each time a row is inserted into the table
# # by an INSERT statement that does not provide explicit values for all
# # table columns the values stored in the new row are determined by their
# # default values
# #
# #     Verify this with some assert statements for which all, some and no
# #     columns lack explicit values.
# #
# set sqlite_current_time 1000000000
# do_createtable_tests 3.5 -query {
#   SELECT quote(a), quote(b), quote(c), quote(d), quote(e), quote(f), 
#          quote(g), quote(h), quote(i), quote(j), quote(k)
#   FROM t4 ORDER BY rowid DESC LIMIT 1;
# } {
#   1 "INSERT INTO t4 DEFAULT VALUES" {
#     NULL {'string constant'} X'424C4F42' 1 -1 3.14 -3.14 
#     'acd' '01:46:40' '2001-09-09' {'2001-09-09 01:46:40'}
#   }

#   2 "INSERT INTO t4(a, b, c) VALUES(1, 2, 3)" {
#     1 2 3 1 -1 3.14 -3.14 'acd' '01:46:40' '2001-09-09' {'2001-09-09 01:46:40'}
#   }

#   3 "INSERT INTO t4(k, j, i) VALUES(1, 2, 3)" {
#     NULL {'string constant'} X'424C4F42' 1 -1 3.14 -3.14 'acd' 3 2 1
#   }

#   4 "INSERT INTO t4(a,b,c,d,e,f,g,h,i,j,k) VALUES(1,2,3,4,5,6,7,8,9,10,11)" {
#     1 2 3 4 5 6 7 8 9 10 11
#   }
# }

# MUST_WORK_TEST

# # EVIDENCE-OF: R-12572-62501 If the default value of the column is a
# # constant NULL, text, blob or signed-number value, then that value is
# # used directly in the new row.
# #
# do_execsql_test e_createtable-3.6.1 {
#   CREATE TABLE t5(
#     a DEFAULT NULL,  
#     b DEFAULT 'text value',  
#     c DEFAULT X'424C4F42',
#     d DEFAULT -45678.6,
#     e DEFAULT 394507
#   );
# } {}
# do_execsql_test e_createtable-3.6.2 {
#   INSERT INTO t5 DEFAULT VALUES;
#   SELECT quote(a), quote(b), quote(c), quote(d), quote(e) FROM t5;
# } {NULL {'text value'} X'424C4F42' -45678.6 394507}

# # EVIDENCE-OF: R-60616-50251 If the default value of a column is an
# # expression in parentheses, then the expression is evaluated once for
# # each row inserted and the results used in the new row.
# #
# #   Test case 3.6.4 demonstrates that the expression is evaluated 
# #   separately for each row if the INSERT is an "INSERT INTO ... SELECT ..."
# #   command.
# #

# MUST_WORK_TEST

# set ::nextint 0
# proc nextint {} { incr ::nextint }
# db func nextint nextint

# do_execsql_test e_createtable-3.7.1 {
#   CREATE TABLE t6(a DEFAULT ( nextint() ), b DEFAULT ( nextint() ));
# } {}
# do_execsql_test e_createtable-3.7.2 {
#   INSERT INTO t6 DEFAULT VALUES;
#   SELECT quote(a), quote(b) FROM t6;
# } {1 2}
# do_execsql_test e_createtable-3.7.3 {
#   INSERT INTO t6(a) VALUES('X');
#   SELECT quote(a), quote(b) FROM t6;
# } {1 2 'X' 3}
# do_execsql_test e_createtable-3.7.4 {
#   INSERT INTO t6(a) SELECT a FROM t6;
#   SELECT quote(a), quote(b) FROM t6;
# } {1 2 'X' 3 1 4 'X' 5}

# MUST_WORK_TEST

# # EVIDENCE-OF: R-15363-55230 If the default value of a column is
# # CURRENT_TIME, CURRENT_DATE or CURRENT_TIMESTAMP, then the value used
# # in the new row is a text representation of the current UTC date and/or
# # time.
# #
# #     This is difficult to test literally without knowing what time the 
# #     user will run the tests. Instead, we test that the three cases
# #     above set the value to the current date and/or time according to
# #     the xCurrentTime() method of the VFS. Which is usually the same
# #     as UTC. In this case, however, we instrument it to always return
# #     a time equivalent to "2001-09-09 01:46:40 UTC".
# #
# set sqlite_current_time 1000000000
# do_execsql_test e_createtable-3.8.1 {
#   CREATE TABLE t7(
#     a DEFAULT CURRENT_TIME, 
#     b DEFAULT CURRENT_DATE, 
#     c DEFAULT CURRENT_TIMESTAMP
#   );
# } {}
# do_execsql_test e_createtable-3.8.2 {
#   INSERT INTO t7 DEFAULT VALUES;
#   SELECT quote(a), quote(b), quote(c) FROM t7;
# } {'01:46:40' '2001-09-09' {'2001-09-09 01:46:40'}}


# # EVIDENCE-OF: R-62327-53843 For CURRENT_TIME, the format of the value
# # is "HH:MM:SS".
# #
# # EVIDENCE-OF: R-03775-43471 For CURRENT_DATE, "YYYY-MM-DD".
# #
# # EVIDENCE-OF: R-07677-44926 The format for CURRENT_TIMESTAMP is
# # "YYYY-MM-DD HH:MM:SS".
# #
# #     The three above are demonstrated by tests 1, 2 and 3 below. 
# #     Respectively.
# #
# do_createtable_tests 3.8.3 -query {
#   SELECT a, b, c FROM t7 ORDER BY rowid DESC LIMIT 1;
# } {
#   1 "INSERT INTO t7(b, c) VALUES('x', 'y')" {01:46:40 x y}
#   2 "INSERT INTO t7(c, a) VALUES('x', 'y')" {y 2001-09-09 x}
#   3 "INSERT INTO t7(a, b) VALUES('x', 'y')" {x y {2001-09-09 01:46:40}}
# }

# # EVIDENCE-OF: R-55061-47754 The COLLATE clause specifies the name of a
# # collating sequence to use as the default collation sequence for the
# # column.
# #
# # EVIDENCE-OF: R-40275-54363 If no COLLATE clause is specified, the
# # default collation sequence is BINARY.
# #

# MUST_WORK_TEST

# do_execsql_test e_createtable-3-9.1 {
#   CREATE TABLE t8(a COLLATE nocase, b COLLATE rtrim, c COLLATE binary, d);
#   INSERT INTO t8 VALUES('abc',   'abc',   'abc',   'abc');
#   INSERT INTO t8 VALUES('abc  ', 'abc  ', 'abc  ', 'abc  ');
#   INSERT INTO t8 VALUES('ABC  ', 'ABC  ', 'ABC  ', 'ABC  ');
#   INSERT INTO t8 VALUES('ABC',   'ABC',   'ABC',   'ABC');
# } {}
# do_createtable_tests 3.9 {
#   2    "SELECT a FROM t8 ORDER BY a, rowid"    {abc ABC {abc  } {ABC  }}
#   3    "SELECT b FROM t8 ORDER BY b, rowid"    {{ABC  } ABC abc {abc  }}
#   4    "SELECT c FROM t8 ORDER BY c, rowid"    {ABC {ABC  } abc {abc  }}
#   5    "SELECT d FROM t8 ORDER BY d, rowid"    {ABC {ABC  } abc {abc  }}
# }

# EVIDENCE-OF: R-25473-20557 The number of columns in a table is limited
# by the SQLITE_MAX_COLUMN compile-time parameter.
#
proc columns {n} {
  set res [list]
  for {set i 0} {$i < $n} {incr i} { lappend res "c$i" }
  join $res ", "
}

do_execsql_test e_createtable-3.10.1 [subst {
  CREATE TABLE t9(id primary key, [columns $::SQLITE_MAX_COLUMN-1]);
}] {}
do_catchsql_test e_createtable-3.10.2 [subst {
  CREATE TABLE t10(id primary key, [columns [expr $::SQLITE_MAX_COLUMN]]);
}] {1 {too many columns on t10}}

# EVIDENCE-OF: R-27775-64721 Both of these limits can be lowered at
# runtime using the sqlite3_limit() C/C++ interface.
#
#   A 30,000 byte blob consumes 30,003 bytes of record space. A record 
#   that contains 3 such blobs consumes (30,000*3)+1 bytes of space. Tests
#   3.11.4 and 3.11.5, which verify that SQLITE_MAX_LENGTH may be lowered
#   at runtime, are based on this calculation.
#
sqlite3_limit db SQLITE_LIMIT_COLUMN 500
do_execsql_test e_createtable-3.11.1 [subst {
  CREATE TABLE t10(id PRIMARY KEY, [columns 499]);
}] {}
do_catchsql_test e_createtable-3.11.2 [subst {
  CREATE TABLE t11(id PRIMARY KEY, [columns 500]);
}] {1 {too many columns on t11}}

# Check that it is not possible to raise the column limit above its 
# default compile time value.
#
sqlite3_limit db SQLITE_LIMIT_COLUMN [expr $::SQLITE_MAX_COLUMN+2]
do_catchsql_test e_createtable-3.11.3 [subst {
  CREATE TABLE t11(id PRIMARY KEY, [columns [expr $::SQLITE_MAX_COLUMN]]);
}] {1 {too many columns on t11}}

sqlite3_limit db SQLITE_LIMIT_LENGTH 90010
do_execsql_test e_createtable-3.11.4 {
  CREATE TABLE t12(a PRIMARY KEY, b, c);
  INSERT INTO t12 VALUES(randomblob(30000),randomblob(30000),randomblob(30000));
} {}
do_catchsql_test e_createtable-3.11.5 {
  INSERT INTO t12 VALUES(randomblob(30001),randomblob(30000),randomblob(30000));
} {1 {string or blob too big}}

#-------------------------------------------------------------------------
# Tests for statements regarding constraints (PRIMARY KEY, UNIQUE, NOT 
# NULL and CHECK constraints).
#

# EVIDENCE-OF: R-52382-54248 Each table in SQLite may have at most one
# PRIMARY KEY.
# 
# EVIDENCE-OF: R-31826-01813 An error is raised if more than one PRIMARY
# KEY clause appears in a CREATE TABLE statement.
#
#     To test the two above, show that zero primary keys is Ok, one primary
#     key is Ok, and two or more primary keys is an error.
#
#drop_all_tables
execsql {DROP TABLE IF EXISTS t1; DROP TABLE IF EXISTS t2; DROP TABLE IF EXISTS t3; DROP TABLE IF EXISTS t4;}
do_createtable_tests 4.1.1 {
  2    "CREATE TABLE t2(a PRIMARY KEY, b, c)"                            {}
  3    "CREATE TABLE t3(a, b, c, PRIMARY KEY(a))"                        {}
  4    "CREATE TABLE t4(a, b, c, PRIMARY KEY(c,b,a))"                    {}
}
do_createtable_tests 4.1.2 -error {
  table "t5" has more than one primary key
} {
  1    "CREATE TABLE t5(a PRIMARY KEY, b PRIMARY KEY, c)"                {}
  2    "CREATE TABLE t5(a, b PRIMARY KEY, c, PRIMARY KEY(a))"            {}
  3    "CREATE TABLE t5(a INTEGER PRIMARY KEY, b PRIMARY KEY, c)"        {}
  4    "CREATE TABLE t5(a INTEGER PRIMARY KEY, b, c, PRIMARY KEY(b, c))" {}
  5    "CREATE TABLE t5(a PRIMARY KEY, b, c, PRIMARY KEY(a))"            {}
  6    "CREATE TABLE t5(a INTEGER PRIMARY KEY, b, c, PRIMARY KEY(a))"    {}
}

# EVIDENCE-OF: R-54755-39291 The PRIMARY KEY is optional for ordinary
# tables but is required for WITHOUT ROWID tables.
#
do_catchsql_test 4.1.3 {
  CREATE TABLE t6(a PRIMARY KEY, b); --ok
} {0 {}}
do_catchsql_test 4.1.4 {
  CREATE TABLE t7(a, b) WITHOUT ROWID; --Error, no PRIMARY KEY
} {1 {Index must be created with table}}


proc table_pk {tbl} { 
  set pk [list]
  db eval "pragma table_info($tbl)" a {
    if {$a(pk)} { lappend pk $a(name) }
  }
  set pk
}

# MUST_WORK_TEST

# # EVIDENCE-OF: R-41411-18837 If the keywords PRIMARY KEY are added to a
# # column definition, then the primary key for the table consists of that
# # single column.
# #
# #     The above is tested by 4.2.1.*
# #
# # EVIDENCE-OF: R-31775-48204 Or, if a PRIMARY KEY clause is specified as
# # a table-constraint, then the primary key of the table consists of the
# # list of columns specified as part of the PRIMARY KEY clause.
# #
# #     The above is tested by 4.2.2.*
# #
# do_createtable_tests 4.2 -repair {
#   catchsql { DROP TABLE t5 }
# } -tclquery {
#   table_pk t5
# } {
#   1.1    "CREATE TABLE t5(a, b INTEGER PRIMARY KEY, c)"       {b}
#   1.2    "CREATE TABLE t5(a PRIMARY KEY, b, c)"               {a}

#   2.1    "CREATE TABLE t5(a, b, c, PRIMARY KEY(a))"           {a}
#   2.2    "CREATE TABLE t5(a, b, c, PRIMARY KEY(c,b,a))"       {a b c}
#   2.3    "CREATE TABLE t5(a, b INTEGER PRIMARY KEY, c)"       {b}
# }

# EVIDENCE-OF: R-59124-61339 Each row in a table with a primary key must
# have a unique combination of values in its primary key columns.
#
# EVIDENCE-OF: R-06471-16287 If an INSERT or UPDATE statement attempts
# to modify the table content so that two or more rows have identical
# primary key values, that is a constraint violation.
#
#drop_all_tables
catchsql {DROP TABLE t1;}
catchsql {DROP TABLE t2;}
do_execsql_test 4.3.0 {
  CREATE TABLE t1(x PRIMARY KEY, y);
  INSERT INTO t1 VALUES(0,          'zero');
  INSERT INTO t1 VALUES(45.5,       'one');
  INSERT INTO t1 VALUES('brambles', 'two');
  INSERT INTO t1 VALUES(X'ABCDEF',  'three');

  CREATE TABLE t2(x, y, PRIMARY KEY(x, y));
  INSERT INTO t2 VALUES(0,          'zero');
  INSERT INTO t2 VALUES(45.5,       'one');
  INSERT INTO t2 VALUES('brambles', 'two');
  INSERT INTO t2 VALUES(X'ABCDEF',  'three');
} {}

# MUST_WORK_TEST

# do_createtable_tests 4.3.1 -error {UNIQUE constraint failed: t1.x} {
#   1    "INSERT INTO t1 VALUES(0, 0)"                 {"column x is"}
#   2    "INSERT INTO t1 VALUES(45.5, 'abc')"          {"column x is"}
#   3    "INSERT INTO t1 VALUES(0.0, 'abc')"           {"column x is"}
#   4    "INSERT INTO t1 VALUES('brambles', 'abc')"    {"column x is"}
#   5    "INSERT INTO t1 VALUES(X'ABCDEF', 'abc')"     {"column x is"}
# }
# do_createtable_tests 4.3.1 -error {UNIQUE constraint failed: t2.x, t2.y} {
#   6    "INSERT INTO t2 VALUES(0, 'zero')"            {"columns x, y are"}
#   7    "INSERT INTO t2 VALUES(45.5, 'one')"          {"columns x, y are"}
#   8    "INSERT INTO t2 VALUES(0.0, 'zero')"          {"columns x, y are"}
#   9    "INSERT INTO t2 VALUES('brambles', 'two')"    {"columns x, y are"}
#   10   "INSERT INTO t2 VALUES(X'ABCDEF', 'three')"   {"columns x, y are"}
# }
# do_createtable_tests 4.3.2 {
#   1    "INSERT INTO t1 VALUES(-1, 0)"                {}
#   2    "INSERT INTO t1 VALUES(45.2, 'abc')"          {}
#   3    "INSERT INTO t1 VALUES(0.01, 'abc')"          {}
#   4    "INSERT INTO t1 VALUES('bramble', 'abc')"     {}
#   5    "INSERT INTO t1 VALUES(X'ABCDEE', 'abc')"     {}

#   6    "INSERT INTO t2 VALUES(0, 0)"                 {}
#   7    "INSERT INTO t2 VALUES(45.5, 'abc')"          {}
#   8    "INSERT INTO t2 VALUES(0.0, 'abc')"           {}
#   9    "INSERT INTO t2 VALUES('brambles', 'abc')"    {}
#   10   "INSERT INTO t2 VALUES(X'ABCDEF', 'abc')"     {}
# }
# do_createtable_tests 4.3.3 -error {UNIQUE constraint failed: t1.x} {
#   1    "UPDATE t1 SET x=0           WHERE y='two'"    {"column x is"}
#   2    "UPDATE t1 SET x='brambles'  WHERE y='three'"  {"column x is"}
#   3    "UPDATE t1 SET x=45.5        WHERE y='zero'"   {"column x is"}
#   4    "UPDATE t1 SET x=X'ABCDEF'   WHERE y='one'"    {"column x is"}
#   5    "UPDATE t1 SET x=0.0         WHERE y='three'"  {"column x is"}
# }
# do_createtable_tests 4.3.3 -error {UNIQUE constraint failed: t2.x, t2.y} {
#   6    "UPDATE t2 SET x=0, y='zero' WHERE y='two'"    {"columns x, y are"}
#   7    "UPDATE t2 SET x='brambles', y='two' WHERE y='three'"  
#        {"columns x, y are"}
#   8    "UPDATE t2 SET x=45.5, y='one' WHERE y='zero'" {"columns x, y are"}
#   9    "UPDATE t2 SET x=X'ABCDEF', y='three' WHERE y='one'" 
#        {"columns x, y are"}
#   10   "UPDATE t2 SET x=0.0, y='zero'        WHERE y='three'"  
#        {"columns x, y are"}
# }

# MUST_WORK_TEST

# # EVIDENCE-OF: R-52572-02078 For the purposes of determining the
# # uniqueness of primary key values, NULL values are considered distinct
# # from all other values, including other NULLs.
# #
# do_createtable_tests 4.4 {
#   1    "INSERT INTO t1 VALUES(NULL, 0)"              {}
#   2    "INSERT INTO t1 VALUES(NULL, 0)"              {}
#   3    "INSERT INTO t1 VALUES(NULL, 0)"              {}

#   4    "INSERT INTO t2 VALUES(NULL, 'zero')"         {}
#   5    "INSERT INTO t2 VALUES(NULL, 'one')"          {}
#   6    "INSERT INTO t2 VALUES(NULL, 'two')"          {}
#   7    "INSERT INTO t2 VALUES(NULL, 'three')"        {}

#   8    "INSERT INTO t2 VALUES(0, NULL)"              {}
#   9    "INSERT INTO t2 VALUES(45.5, NULL)"           {}
#   10   "INSERT INTO t2 VALUES(0.0, NULL)"            {}
#   11   "INSERT INTO t2 VALUES('brambles', NULL)"     {}
#   12   "INSERT INTO t2 VALUES(X'ABCDEF', NULL)"      {}

#   13   "INSERT INTO t2 VALUES(NULL, NULL)"           {}
#   14   "INSERT INTO t2 VALUES(NULL, NULL)"           {}
# }

# MUST_WORK_TEST

# # EVIDENCE-OF: R-35113-43214 Unless the column is an INTEGER PRIMARY KEY
# # or the table is a WITHOUT ROWID table or the column is declared NOT
# # NULL, SQLite allows NULL values in a PRIMARY KEY column.
# #
# #     If the column is an integer primary key, attempting to insert a NULL
# #     into the column triggers the auto-increment behavior. Attempting
# #     to use UPDATE to set an ipk column to a NULL value is an error.
# #
# do_createtable_tests 4.5.1 {
#   1    "SELECT count(*) FROM t1 WHERE x IS NULL"                   3
#   2    "SELECT count(*) FROM t2 WHERE x IS NULL"                   6
#   3    "SELECT count(*) FROM t2 WHERE y IS NULL"                   7
#   4    "SELECT count(*) FROM t2 WHERE x IS NULL AND y IS NULL"     2
# }
# do_execsql_test 4.5.2 {
#   CREATE TABLE t3(s, u INTEGER PRIMARY KEY, v);
#   INSERT INTO t3 VALUES(1, NULL, 2);
#   INSERT INTO t3 VALUES('x', NULL, 'y');
#   SELECT u FROM t3;
# } {1 2}
# do_catchsql_test 4.5.3 {
#   INSERT INTO t3 VALUES(2, 5, 3);
#   UPDATE t3 SET u = NULL WHERE s = 2;
# } {1 {datatype mismatch}}
# do_catchsql_test 4.5.4 {
#   CREATE TABLE t4(s, u INT PRIMARY KEY, v) WITHOUT ROWID;
#   INSERT INTO t4 VALUES(1, NULL, 2);
# } {1 {NOT NULL constraint failed: t4.u}}
# do_catchsql_test 4.5.5 {
#   CREATE TABLE t5(s, u INT PRIMARY KEY NOT NULL, v);
#   INSERT INTO t5 VALUES(1, NULL, 2);
# } {1 {NOT NULL constraint failed: t5.u}}

# # EVIDENCE-OF: R-00227-21080 A UNIQUE constraint is similar to a PRIMARY
# # KEY constraint, except that a single table may have any number of
# # UNIQUE constraints.
# #
# drop_all_tables
# do_createtable_tests 4.6 {
#   1    "CREATE TABLE t1(a UNIQUE, b UNIQUE)"                       {}
#   2    "CREATE TABLE t2(a UNIQUE, b, c, UNIQUE(c, b))"             {}
#   3    "CREATE TABLE t3(a, b, c, UNIQUE(a), UNIQUE(b), UNIQUE(c))" {}
#   4    "CREATE TABLE t4(a, b, c, UNIQUE(a, b, c))"                 {}
# }

# # EVIDENCE-OF: R-30981-64168 For each UNIQUE constraint on the table,
# # each row must contain a unique combination of values in the columns
# # identified by the UNIQUE constraint.
# #
# # EVIDENCE-OF: R-59124-61339 Each row in a table with a primary key must
# # have a unique combination of values in its primary key columns.
# #
# do_execsql_test 4.7.0 {
#   INSERT INTO t1 VALUES(1, 2);
#   INSERT INTO t1 VALUES(4.3, 5.5);
#   INSERT INTO t1 VALUES('reveal', 'variableness');
#   INSERT INTO t1 VALUES(X'123456', X'654321');

#   INSERT INTO t4 VALUES('xyx', 1, 1);
#   INSERT INTO t4 VALUES('xyx', 2, 1);
#   INSERT INTO t4 VALUES('uvw', 1, 1);
# }
# do_createtable_tests 4.7.1 -error {UNIQUE constraint failed: %s} {
#   1    "INSERT INTO t1 VALUES(1, 'one')"             {{t1.a}}
#   2    "INSERT INTO t1 VALUES(4.3, 'two')"           {{t1.a}}
#   3    "INSERT INTO t1 VALUES('reveal', 'three')"    {{t1.a}}
#   4    "INSERT INTO t1 VALUES(X'123456', 'four')"    {{t1.a}}

#   5    "UPDATE t1 SET a = 1 WHERE rowid=2"           {{t1.a}}
#   6    "UPDATE t1 SET a = 4.3 WHERE rowid=3"         {{t1.a}}
#   7    "UPDATE t1 SET a = 'reveal' WHERE rowid=4"    {{t1.a}}
#   8    "UPDATE t1 SET a = X'123456' WHERE rowid=1"   {{t1.a}}

#   9    "INSERT INTO t4 VALUES('xyx', 1, 1)"          {{t4.a, t4.b, t4.c}}
#   10   "INSERT INTO t4 VALUES('xyx', 2, 1)"          {{t4.a, t4.b, t4.c}}
#   11   "INSERT INTO t4 VALUES('uvw', 1, 1)"          {{t4.a, t4.b, t4.c}}

#   12   "UPDATE t4 SET a='xyx' WHERE rowid=3"         {{t4.a, t4.b, t4.c}}
#   13   "UPDATE t4 SET b=1 WHERE rowid=2"             {{t4.a, t4.b, t4.c}}
#   14   "UPDATE t4 SET a=0, b=0, c=0"                 {{t4.a, t4.b, t4.c}}
# }

# # EVIDENCE-OF: R-00404-17670 For the purposes of UNIQUE constraints,
# # NULL values are considered distinct from all other values, including
# # other NULLs.
# #
# do_createtable_tests 4.8 {
#   1    "INSERT INTO t1 VALUES(NULL, NULL)"           {}
#   2    "INSERT INTO t1 VALUES(NULL, NULL)"           {}
#   3    "UPDATE t1 SET a = NULL"                      {}
#   4    "UPDATE t1 SET b = NULL"                      {}

#   5    "INSERT INTO t4 VALUES(NULL, NULL, NULL)"     {}
#   6    "INSERT INTO t4 VALUES(NULL, NULL, NULL)"     {}
#   7    "UPDATE t4 SET a = NULL"                      {}
#   8    "UPDATE t4 SET b = NULL"                      {}
#   9    "UPDATE t4 SET c = NULL"                      {}
# }

# # EVIDENCE-OF: R-55820-29984 In most cases, UNIQUE and PRIMARY KEY
# # constraints are implemented by creating a unique index in the
# # database.
# do_createtable_tests 4.9 -repair drop_all_tables -query {
#   SELECT count(*) FROM sqlite_master WHERE type='index'
# } {
#   1    "CREATE TABLE t1(a TEXT PRIMARY KEY, b)"              1
#   2    "CREATE TABLE t1(a INTEGER PRIMARY KEY, b)"           0
#   3    "CREATE TABLE t1(a TEXT UNIQUE, b)"                   1
#   4    "CREATE TABLE t1(a PRIMARY KEY, b TEXT UNIQUE)"       2
#   5    "CREATE TABLE t1(a PRIMARY KEY, b, c, UNIQUE(c, b))"  2
# }

# # Obsolete: R-02252-33116 Such an index is used like any other index
# # in the database to optimize queries.
# #
# do_execsql_test 4.10.0 {
#   CREATE TABLE t1(a, b PRIMARY KEY);
#   CREATE TABLE t2(a, b, c, UNIQUE(b, c));
# }
# do_createtable_tests 4.10 {
#   1    "EXPLAIN QUERY PLAN SELECT * FROM t1 WHERE b = 5" 
#        {0 0 0 {SEARCH TABLE t1 USING INDEX sqlite_autoindex_t1_1 (b=?)}}

#   2    "EXPLAIN QUERY PLAN SELECT * FROM t2 ORDER BY b, c"
#        {0 0 0 {SCAN TABLE t2 USING INDEX sqlite_autoindex_t2_1}}

#   3    "EXPLAIN QUERY PLAN SELECT * FROM t2 WHERE b=10 AND c>10"
#        {0 0 0 {SEARCH TABLE t2 USING INDEX sqlite_autoindex_t2_1 (b=? AND c>?)}}
# }

# # EVIDENCE-OF: R-45493-35653 A CHECK constraint may be attached to a
# # column definition or specified as a table constraint. In practice it
# # makes no difference.
# #
# #   All the tests that deal with CHECK constraints below (4.11.* and 
# #   4.12.*) are run once for a table with the check constraint attached
# #   to a column definition, and once with a table where the check 
# #   condition is specified as a table constraint.
# #
# # EVIDENCE-OF: R-55435-14303 Each time a new row is inserted into the
# # table or an existing row is updated, the expression associated with
# # each CHECK constraint is evaluated and cast to a NUMERIC value in the
# # same way as a CAST expression. If the result is zero (integer value 0
# # or real value 0.0), then a constraint violation has occurred.
# #

# MUST_WORK_TEST

# drop_all_tables
# do_execsql_test 4.11 {
#   CREATE TABLE x1(a TEXT, b INTEGER CHECK( b>0 ));
#   CREATE TABLE t1(a TEXT, b INTEGER, CHECK( b>0 ));
#   INSERT INTO x1 VALUES('x', 'xx');
#   INSERT INTO x1 VALUES('y', 'yy');
#   INSERT INTO t1 SELECT * FROM x1;

#   CREATE TABLE x2(a CHECK( a||b ), b);
#   CREATE TABLE t2(a, b, CHECK( a||b ));
#   INSERT INTO x2 VALUES(1, 'xx');
#   INSERT INTO x2 VALUES(1, 'yy');
#   INSERT INTO t2 SELECT * FROM x2;
# }

# do_createtable_tests 4.11 -error {CHECK constraint failed: %s} {
#   1a    "INSERT INTO x1 VALUES('one', 0)"       {x1}
#   1b    "INSERT INTO t1 VALUES('one', -4.0)"    {t1}

#   2a    "INSERT INTO x2 VALUES('abc', 1)"       {x2}
#   2b    "INSERT INTO t2 VALUES('abc', 1)"       {t2}

#   3a    "INSERT INTO x2 VALUES(0, 'abc')"       {x2}
#   3b    "INSERT INTO t2 VALUES(0, 'abc')"       {t2}

#   4a    "UPDATE t1 SET b=-1 WHERE rowid=1"      {t1}
#   4b    "UPDATE x1 SET b=-1 WHERE rowid=1"      {x1}

#   4a    "UPDATE x2 SET a='' WHERE rowid=1"      {x2}
#   4b    "UPDATE t2 SET a='' WHERE rowid=1"      {t2}
# }

# # EVIDENCE-OF: R-34109-39108 If the CHECK expression evaluates to NULL,
# # or any other non-zero value, it is not a constraint violation.
# #
# do_createtable_tests 4.12 {
#   1a    "INSERT INTO x1 VALUES('one', NULL)"    {}
#   1b    "INSERT INTO t1 VALUES('one', NULL)"    {}

#   2a    "INSERT INTO x1 VALUES('one', 2)"    {}
#   2b    "INSERT INTO t1 VALUES('one', 2)"    {}

#   3a    "INSERT INTO x2 VALUES(1, 'abc')"       {}
#   3b    "INSERT INTO t2 VALUES(1, 'abc')"       {}
# }

# # EVIDENCE-OF: R-02060-64547 A NOT NULL constraint may only be attached
# # to a column definition, not specified as a table constraint.
# #
# drop_all_tables
# do_createtable_tests 4.13.1 {
#   1     "CREATE TABLE t1(a NOT NULL, b)"                               {}
#   2     "CREATE TABLE t2(a PRIMARY KEY NOT NULL, b)"                   {}
#   3     "CREATE TABLE t3(a NOT NULL, b NOT NULL, c NOT NULL UNIQUE)"   {}
# }
# do_createtable_tests 4.13.2 -error {
#   near "NOT": syntax error
# } {
#   1     "CREATE TABLE t4(a, b, NOT NULL(a))"                   {}
#   2     "CREATE TABLE t4(a PRIMARY KEY, b, NOT NULL(a))"       {}
#   3     "CREATE TABLE t4(a, b, c UNIQUE, NOT NULL(a, b, c))"   {}
# }

# # EVIDENCE-OF: R-31795-57643 a NOT NULL constraint dictates that the
# # associated column may not contain a NULL value. Attempting to set the
# # column value to NULL when inserting a new row or updating an existing
# # one causes a constraint violation.
# #
# #     These tests use the tables created by 4.13.
# #
# do_execsql_test 4.14.0 {
#   INSERT INTO t1 VALUES('x', 'y');
#   INSERT INTO t1 VALUES('z', NULL);

#   INSERT INTO t2 VALUES('x', 'y');
#   INSERT INTO t2 VALUES('z', NULL);

#   INSERT INTO t3 VALUES('x', 'y', 'z');
#   INSERT INTO t3 VALUES(1, 2, 3);
# }
# do_createtable_tests 4.14 -error {NOT NULL constraint failed: %s} {
#   1    "INSERT INTO t1 VALUES(NULL, 'a')"         {t1.a}
#   2    "INSERT INTO t2 VALUES(NULL, 'b')"         {t2.a}
#   3    "INSERT INTO t3 VALUES('c', 'd', NULL)"    {t3.c}
#   4    "INSERT INTO t3 VALUES('e', NULL, 'f')"    {t3.b}
#   5    "INSERT INTO t3 VALUES(NULL, 'g', 'h')"    {t3.a}
# }

# # EVIDENCE-OF: R-42511-39459 PRIMARY KEY, UNIQUE and NOT NULL
# # constraints may be explicitly assigned a default conflict resolution
# # algorithm by including a conflict-clause in their definitions.
# #
# #     Conflict clauses: ABORT, ROLLBACK, IGNORE, FAIL, REPLACE
# #
# #     Test cases 4.15.*, 4.16.* and 4.17.* focus on PRIMARY KEY, NOT NULL
# #     and UNIQUE constraints, respectively.
# #
# drop_all_tables
# do_execsql_test 4.15.0 {
#   CREATE TABLE t1_ab(a PRIMARY KEY ON CONFLICT ABORT, b);
#   CREATE TABLE t1_ro(a PRIMARY KEY ON CONFLICT ROLLBACK, b);
#   CREATE TABLE t1_ig(a PRIMARY KEY ON CONFLICT IGNORE, b);
#   CREATE TABLE t1_fa(a PRIMARY KEY ON CONFLICT FAIL, b);
#   CREATE TABLE t1_re(a PRIMARY KEY ON CONFLICT REPLACE, b);
#   CREATE TABLE t1_xx(a PRIMARY KEY, b);

#   INSERT INTO t1_ab VALUES(1, 'one');
#   INSERT INTO t1_ab VALUES(2, 'two');
#   INSERT INTO t1_ro SELECT * FROM t1_ab;
#   INSERT INTO t1_ig SELECT * FROM t1_ab;
#   INSERT INTO t1_fa SELECT * FROM t1_ab;
#   INSERT INTO t1_re SELECT * FROM t1_ab;
#   INSERT INTO t1_xx SELECT * FROM t1_ab;

#   CREATE TABLE t2_ab(a, b NOT NULL ON CONFLICT ABORT);
#   CREATE TABLE t2_ro(a, b NOT NULL ON CONFLICT ROLLBACK);
#   CREATE TABLE t2_ig(a, b NOT NULL ON CONFLICT IGNORE);
#   CREATE TABLE t2_fa(a, b NOT NULL ON CONFLICT FAIL);
#   CREATE TABLE t2_re(a, b NOT NULL ON CONFLICT REPLACE);
#   CREATE TABLE t2_xx(a, b NOT NULL);

#   INSERT INTO t2_ab VALUES(1, 'one');
#   INSERT INTO t2_ab VALUES(2, 'two');
#   INSERT INTO t2_ro SELECT * FROM t2_ab;
#   INSERT INTO t2_ig SELECT * FROM t2_ab;
#   INSERT INTO t2_fa SELECT * FROM t2_ab;
#   INSERT INTO t2_re SELECT * FROM t2_ab;
#   INSERT INTO t2_xx SELECT * FROM t2_ab;

#   CREATE TABLE t3_ab(a, b, UNIQUE(a, b) ON CONFLICT ABORT);
#   CREATE TABLE t3_ro(a, b, UNIQUE(a, b) ON CONFLICT ROLLBACK);
#   CREATE TABLE t3_ig(a, b, UNIQUE(a, b) ON CONFLICT IGNORE);
#   CREATE TABLE t3_fa(a, b, UNIQUE(a, b) ON CONFLICT FAIL);
#   CREATE TABLE t3_re(a, b, UNIQUE(a, b) ON CONFLICT REPLACE);
#   CREATE TABLE t3_xx(a, b, UNIQUE(a, b));

#   INSERT INTO t3_ab VALUES(1, 'one');
#   INSERT INTO t3_ab VALUES(2, 'two');
#   INSERT INTO t3_ro SELECT * FROM t3_ab;
#   INSERT INTO t3_ig SELECT * FROM t3_ab;
#   INSERT INTO t3_fa SELECT * FROM t3_ab;
#   INSERT INTO t3_re SELECT * FROM t3_ab;
#   INSERT INTO t3_xx SELECT * FROM t3_ab;
# }

# foreach {tn tbl res ac data} {
#   1   t1_ab    {1 {UNIQUE constraint failed: t1_ab.a}} 0 {1 one 2 two 3 three}
#   2   t1_ro    {1 {UNIQUE constraint failed: t1_ro.a}} 1 {1 one 2 two}
#   3   t1_fa    {1 {UNIQUE constraint failed: t1_fa.a}} 0 {1 one 2 two 3 three 4 string}
#   4   t1_ig    {0 {}} 0 {1 one 2 two 3 three 4 string 6 string}
#   5   t1_re    {0 {}} 0 {1 one 2 two 4 string 3 string 6 string}
#   6   t1_xx    {1 {UNIQUE constraint failed: t1_xx.a}} 0 {1 one 2 two 3 three}
# } {
#   catchsql COMMIT
#   do_execsql_test  4.15.$tn.1 "BEGIN; INSERT INTO $tbl VALUES(3, 'three')"

#   do_catchsql_test 4.15.$tn.2 " 
#     INSERT INTO $tbl SELECT ((a%2)*a+3), 'string' FROM $tbl;
#   " $res

#   do_test e_createtable-4.15.$tn.3 { sqlite3_get_autocommit db } $ac
#   do_execsql_test 4.15.$tn.4 "SELECT * FROM $tbl" $data
# }
# foreach {tn tbl res ac data} {
#   1   t2_ab    {1 {NOT NULL constraint failed: t2_ab.b}} 0 {1 one 2 two 3 three}
#   2   t2_ro    {1 {NOT NULL constraint failed: t2_ro.b}} 1 {1 one 2 two}
#   3   t2_fa    {1 {NOT NULL constraint failed: t2_fa.b}} 0 {1 one 2 two 3 three 4 xx}
#   4   t2_ig    {0 {}} 0 {1 one 2 two 3 three 4 xx 6 xx}
#   5   t2_re    {1 {NOT NULL constraint failed: t2_re.b}} 0 {1 one 2 two 3 three}
#   6   t2_xx    {1 {NOT NULL constraint failed: t2_xx.b}} 0 {1 one 2 two 3 three}
# } {
#   catchsql COMMIT
#   do_execsql_test  4.16.$tn.1 "BEGIN; INSERT INTO $tbl VALUES(3, 'three')"

#   do_catchsql_test 4.16.$tn.2 " 
#     INSERT INTO $tbl SELECT a+3, CASE a WHEN 2 THEN NULL ELSE 'xx' END FROM $tbl
#   " $res

#   do_test e_createtable-4.16.$tn.3 { sqlite3_get_autocommit db } $ac
#   do_execsql_test 4.16.$tn.4 "SELECT * FROM $tbl" $data
# }
# foreach {tn tbl res ac data} {
#   1   t3_ab    {1 {UNIQUE constraint failed: t3_ab.a, t3_ab.b}}
#                0 {1 one 2 two 3 three}
#   2   t3_ro    {1 {UNIQUE constraint failed: t3_ro.a, t3_ro.b}}
#                1 {1 one 2 two}
#   3   t3_fa    {1 {UNIQUE constraint failed: t3_fa.a, t3_fa.b}}
#                0 {1 one 2 two 3 three 4 three}
#   4   t3_ig    {0 {}} 0 {1 one 2 two 3 three 4 three 6 three}
#   5   t3_re    {0 {}} 0 {1 one 2 two 4 three 3 three 6 three}
#   6   t3_xx    {1 {UNIQUE constraint failed: t3_xx.a, t3_xx.b}}
#                0 {1 one 2 two 3 three}
# } {
#   catchsql COMMIT
#   do_execsql_test  4.17.$tn.1 "BEGIN; INSERT INTO $tbl VALUES(3, 'three')"

#   do_catchsql_test 4.17.$tn.2 " 
#     INSERT INTO $tbl SELECT ((a%2)*a+3), 'three' FROM $tbl
#   " $res

#   do_test e_createtable-4.17.$tn.3 { sqlite3_get_autocommit db } $ac
#   do_execsql_test 4.17.$tn.4 "SELECT * FROM $tbl ORDER BY rowid" $data
# }
# catchsql COMMIT

# # EVIDENCE-OF: R-12645-39772 Or, if a constraint definition does not
# # include a conflict-clause or it is a CHECK constraint, the default
# # conflict resolution algorithm is ABORT.
# #
# #     The first half of the above is tested along with explicit ON 
# #     CONFLICT clauses above (specifically, the tests involving t1_xx, t2_xx
# #     and t3_xx). The following just tests that the default conflict
# #     handling for CHECK constraints is ABORT.
# #
# do_execsql_test 4.18.1 {
#   CREATE TABLE t4(a, b CHECK (b!=10));
#   INSERT INTO t4 VALUES(1, 2);
#   INSERT INTO t4 VALUES(3, 4);
# }
# do_execsql_test  4.18.2 { BEGIN; INSERT INTO t4 VALUES(5, 6) }
# do_catchsql_test 4.18.3 { 
#   INSERT INTO t4 SELECT a+4, b+4 FROM t4
# } {1 {CHECK constraint failed: t4}}
# do_test e_createtable-4.18.4 { sqlite3_get_autocommit db } 0
# do_execsql_test 4.18.5 { SELECT * FROM t4 } {1 2 3 4 5 6}

# # EVIDENCE-OF: R-19114-56113 Different constraints within the same table
# # may have different default conflict resolution algorithms.
# #
# do_execsql_test 4.19.0 {
#   CREATE TABLE t5(a NOT NULL ON CONFLICT IGNORE, b NOT NULL ON CONFLICT ABORT);
# }
# do_catchsql_test 4.19.1 { INSERT INTO t5 VALUES(NULL, 'not null') } {0 {}}
# do_execsql_test  4.19.2 { SELECT * FROM t5 } {}
# do_catchsql_test 4.19.3 { INSERT INTO t5 VALUES('not null', NULL) } \
#   {1 {NOT NULL constraint failed: t5.b}}
# do_execsql_test  4.19.4 { SELECT * FROM t5 } {}

# #------------------------------------------------------------------------
# # Tests for INTEGER PRIMARY KEY and rowid related statements.
# #

# # EVIDENCE-OF: R-52584-04009 The rowid value can be accessed using one
# # of the special case-independent names "rowid", "oid", or "_rowid_" in
# # place of a column name.
# #
# # EVIDENCE-OF: R-06726-07466 A column name can be any of the names
# # defined in the CREATE TABLE statement or one of the following special
# # identifiers: "ROWID", "OID", or "_ROWID_".
# #
# drop_all_tables
# do_execsql_test 5.1.0 {
#   CREATE TABLE t1(x, y);
#   INSERT INTO t1 VALUES('one', 'first');
#   INSERT INTO t1 VALUES('two', 'second');
#   INSERT INTO t1 VALUES('three', 'third');
# }
# do_createtable_tests 5.1 {
#   1   "SELECT rowid FROM t1"        {1 2 3}
#   2   "SELECT oid FROM t1"          {1 2 3}
#   3   "SELECT _rowid_ FROM t1"      {1 2 3}
#   4   "SELECT ROWID FROM t1"        {1 2 3}
#   5   "SELECT OID FROM t1"          {1 2 3}
#   6   "SELECT _ROWID_ FROM t1"      {1 2 3}
#   7   "SELECT RoWiD FROM t1"        {1 2 3}
#   8   "SELECT OiD FROM t1"          {1 2 3}
#   9   "SELECT _RoWiD_ FROM t1"      {1 2 3}
# }

# # EVIDENCE-OF: R-26501-17306 If a table contains a user defined column
# # named "rowid", "oid" or "_rowid_", then that name always refers the
# # explicitly declared column and cannot be used to retrieve the integer
# # rowid value.
# #
# # EVIDENCE-OF: R-44615-33286 The special identifiers only refer to the
# # row key if the CREATE TABLE statement does not define a real column
# # with the same name.
# #
# do_execsql_test 5.2.0 {
#   CREATE TABLE t2(oid, b);
#   CREATE TABLE t3(a, _rowid_);
#   CREATE TABLE t4(a, b, rowid);

#   INSERT INTO t2 VALUES('one', 'two');
#   INSERT INTO t2 VALUES('three', 'four');

#   INSERT INTO t3 VALUES('five', 'six');
#   INSERT INTO t3 VALUES('seven', 'eight');

#   INSERT INTO t4 VALUES('nine', 'ten', 'eleven');
#   INSERT INTO t4 VALUES('twelve', 'thirteen', 'fourteen');
# }
# do_createtable_tests 5.2 {
#   1   "SELECT oid, rowid, _rowid_ FROM t2"   {one 1 1      three 2 2}
#   2   "SELECT oid, rowid, _rowid_ FROM t3"   {1 1 six      2 2 eight} 
#   3   "SELECT oid, rowid, _rowid_ FROM t4"   {1 eleven 1   2 fourteen 2}
# }


# # Argument $tbl is the name of a table in the database. Argument $col is
# # the name of one of the tables columns. Return 1 if $col is an alias for
# # the rowid, or 0 otherwise.
# #
# proc is_integer_primary_key {tbl col} {
#   lindex [db eval [subst {
#     DELETE FROM $tbl;
#     INSERT INTO $tbl ($col) VALUES(0);
#     SELECT (rowid==$col) FROM $tbl;
#     DELETE FROM $tbl;
#   }]] 0
# }

# # EVIDENCE-OF: R-47901-33947 With one exception noted below, if a rowid
# # table has a primary key that consists of a single column and the
# # declared type of that column is "INTEGER" in any mixture of upper and
# # lower case, then the column becomes an alias for the rowid.
# #
# # EVIDENCE-OF: R-45951-08347 if the declaration of a column with
# # declared type "INTEGER" includes an "PRIMARY KEY DESC" clause, it does
# # not become an alias for the rowid and is not classified as an integer
# # primary key.
# #
# do_createtable_tests 5.3 -tclquery { 
#   is_integer_primary_key t5 pk
# } -repair {
#   catchsql { DROP TABLE t5 }
# } {
#   1   "CREATE TABLE t5(pk integer primary key)"                         1
#   2   "CREATE TABLE t5(pk integer, primary key(pk))"                    1
#   3   "CREATE TABLE t5(pk integer, v integer, primary key(pk))"         1
#   4   "CREATE TABLE t5(pk integer, v integer, primary key(pk, v))"      0
#   5   "CREATE TABLE t5(pk int, v integer, primary key(pk, v))"          0
#   6   "CREATE TABLE t5(pk int, v integer, primary key(pk))"             0
#   7   "CREATE TABLE t5(pk int primary key, v integer)"                  0
#   8   "CREATE TABLE t5(pk inTEger primary key)"                         1
#   9   "CREATE TABLE t5(pk inteGEr, primary key(pk))"                    1
#   10  "CREATE TABLE t5(pk INTEGER, v integer, primary key(pk))"         1
# }

# # EVIDENCE-OF: R-41444-49665 Other integer type names like "INT" or
# # "BIGINT" or "SHORT INTEGER" or "UNSIGNED INTEGER" causes the primary
# # key column to behave as an ordinary table column with integer affinity
# # and a unique index, not as an alias for the rowid.
# #
# do_execsql_test 5.4.1 {
#   CREATE TABLE t6(pk INT primary key);
#   CREATE TABLE t7(pk BIGINT primary key);
#   CREATE TABLE t8(pk SHORT INTEGER primary key);
#   CREATE TABLE t9(pk UNSIGNED INTEGER primary key);
# } 
# do_test e_createtable-5.4.2.1 { is_integer_primary_key t6 pk } 0
# do_test e_createtable-5.4.2.2 { is_integer_primary_key t7 pk } 0
# do_test e_createtable-5.4.2.3 { is_integer_primary_key t8 pk } 0
# do_test e_createtable-5.4.2.4 { is_integer_primary_key t9 pk } 0

# do_execsql_test 5.4.3 {
#   INSERT INTO t6 VALUES('2.0');
#   INSERT INTO t7 VALUES('2.0');
#   INSERT INTO t8 VALUES('2.0');
#   INSERT INTO t9 VALUES('2.0');
#   SELECT typeof(pk), pk FROM t6;
#   SELECT typeof(pk), pk FROM t7;
#   SELECT typeof(pk), pk FROM t8;
#   SELECT typeof(pk), pk FROM t9;
# } {integer 2 integer 2 integer 2 integer 2}

# do_catchsql_test 5.4.4.1 { 
#   INSERT INTO t6 VALUES(2) 
# } {1 {UNIQUE constraint failed: t6.pk}}
# do_catchsql_test 5.4.4.2 { 
#   INSERT INTO t7 VALUES(2) 
# } {1 {UNIQUE constraint failed: t7.pk}}
# do_catchsql_test 5.4.4.3 { 
#   INSERT INTO t8 VALUES(2) 
# } {1 {UNIQUE constraint failed: t8.pk}}
# do_catchsql_test 5.4.4.4 { 
#   INSERT INTO t9 VALUES(2) 
# } {1 {UNIQUE constraint failed: t9.pk}}

# # EVIDENCE-OF: R-56094-57830 the following three table declarations all
# # cause the column "x" to be an alias for the rowid (an integer primary
# # key): CREATE TABLE t(x INTEGER PRIMARY KEY ASC, y, z); CREATE TABLE
# # t(x INTEGER, y, z, PRIMARY KEY(x ASC)); CREATE TABLE t(x INTEGER, y,
# # z, PRIMARY KEY(x DESC));
# #
# # EVIDENCE-OF: R-20149-25884 the following declaration does not result
# # in "x" being an alias for the rowid: CREATE TABLE t(x INTEGER PRIMARY
# # KEY DESC, y, z);
# #
# do_createtable_tests 5 -tclquery { 
#   is_integer_primary_key t x
# } -repair {
#   catchsql { DROP TABLE t }
# } {
#   5.1    "CREATE TABLE t(x INTEGER PRIMARY KEY ASC, y, z)"      1
#   5.2    "CREATE TABLE t(x INTEGER, y, z, PRIMARY KEY(x ASC))"  1
#   5.3    "CREATE TABLE t(x INTEGER, y, z, PRIMARY KEY(x DESC))" 1
#   6.1    "CREATE TABLE t(x INTEGER PRIMARY KEY DESC, y, z)"     0
# }

# # EVIDENCE-OF: R-03733-29734 Rowid values may be modified using an
# # UPDATE statement in the same way as any other column value can, either
# # using one of the built-in aliases ("rowid", "oid" or "_rowid_") or by
# # using an alias created by an integer primary key.
# #
# do_execsql_test 5.7.0 {
#   CREATE TABLE t10(a, b);
#   INSERT INTO t10 VALUES('ten', 10);

#   CREATE TABLE t11(a, b INTEGER PRIMARY KEY);
#   INSERT INTO t11 VALUES('ten', 10);
# }
# do_createtable_tests 5.7.1 -query { 
#   SELECT rowid, _rowid_, oid FROM t10;
# } {
#   1    "UPDATE t10 SET rowid = 5"   {5 5 5}
#   2    "UPDATE t10 SET _rowid_ = 6" {6 6 6}
#   3    "UPDATE t10 SET oid = 7"     {7 7 7}
# }
# do_createtable_tests 5.7.2 -query { 
#   SELECT rowid, _rowid_, oid, b FROM t11;
# } {
#   1    "UPDATE t11 SET rowid = 5"   {5 5 5 5}
#   2    "UPDATE t11 SET _rowid_ = 6" {6 6 6 6}
#   3    "UPDATE t11 SET oid = 7"     {7 7 7 7}
#   4    "UPDATE t11 SET b = 8"       {8 8 8 8}
# }

# # EVIDENCE-OF: R-58706-14229 Similarly, an INSERT statement may provide
# # a value to use as the rowid for each row inserted.
# #
# do_createtable_tests 5.8.1 -query { 
#   SELECT rowid, _rowid_, oid FROM t10;
# } -repair { 
#   execsql { DELETE FROM t10 } 
# } {
#   1    "INSERT INTO t10(oid) VALUES(15)"           {15 15 15}
#   2    "INSERT INTO t10(rowid) VALUES(16)"         {16 16 16}
#   3    "INSERT INTO t10(_rowid_) VALUES(17)"       {17 17 17}
#   4    "INSERT INTO t10(a, b, oid) VALUES(1,2,3)"  {3 3 3}
# }
# do_createtable_tests 5.8.2 -query { 
#   SELECT rowid, _rowid_, oid, b FROM t11;
# } -repair { 
#   execsql { DELETE FROM t11 } 
# } {
#   1    "INSERT INTO t11(oid) VALUES(15)"           {15 15 15 15}
#   2    "INSERT INTO t11(rowid) VALUES(16)"         {16 16 16 16}
#   3    "INSERT INTO t11(_rowid_) VALUES(17)"       {17 17 17 17}
#   4    "INSERT INTO t11(a, b) VALUES(1,2)"         {2 2 2 2}
# }

# # EVIDENCE-OF: R-32326-44592 Unlike normal SQLite columns, an integer
# # primary key or rowid column must contain integer values. Integer
# # primary key or rowid columns are not able to hold floating point
# # values, strings, BLOBs, or NULLs.
# #
# #     This is considered by the tests for the following 3 statements,
# #     which show that:
# #
# #       1. Attempts to UPDATE a rowid column to a non-integer value fail,
# #       2. Attempts to INSERT a real, string or blob value into a rowid 
# #          column fail, and
# #       3. Attempting to INSERT a NULL value into a rowid column causes the
# #          system to automatically select an integer value to use.
# #


# # EVIDENCE-OF: R-64224-62578 If an UPDATE statement attempts to set an
# # integer primary key or rowid column to a NULL or blob value, or to a
# # string or real value that cannot be losslessly converted to an
# # integer, a "datatype mismatch" error occurs and the statement is
# # aborted.
# #
# drop_all_tables
# do_execsql_test 5.9.0 {
#   CREATE TABLE t12(x INTEGER PRIMARY KEY, y);
#   INSERT INTO t12 VALUES(5, 'five');
# }
# do_createtable_tests 5.9.1 -query { SELECT typeof(x), x FROM t12 } {
#   1   "UPDATE t12 SET x = 4"       {integer 4}
#   2   "UPDATE t12 SET x = 10.0"    {integer 10}
#   3   "UPDATE t12 SET x = '12.0'"  {integer 12}
#   4   "UPDATE t12 SET x = '-15.0'" {integer -15}
# }
# do_createtable_tests 5.9.2 -error {
#   datatype mismatch
# } {
#   1   "UPDATE t12 SET x = 4.1"         {}
#   2   "UPDATE t12 SET x = 'hello'"     {}
#   3   "UPDATE t12 SET x = NULL"        {}
#   4   "UPDATE t12 SET x = X'ABCD'"     {}
#   5   "UPDATE t12 SET x = X'3900'"     {}
#   6   "UPDATE t12 SET x = X'39'"       {}
# }

# # EVIDENCE-OF: R-05734-13629 If an INSERT statement attempts to insert a
# # blob value, or a string or real value that cannot be losslessly
# # converted to an integer into an integer primary key or rowid column, a
# # "datatype mismatch" error occurs and the statement is aborted.
# #
# do_execsql_test 5.10.0 { DELETE FROM t12 }
# do_createtable_tests 5.10.1 -error { 
#   datatype mismatch
# } {
#   1   "INSERT INTO t12(x) VALUES(4.1)"     {}
#   2   "INSERT INTO t12(x) VALUES('hello')" {}
#   3   "INSERT INTO t12(x) VALUES(X'ABCD')" {}
#   4   "INSERT INTO t12(x) VALUES(X'3900')" {}
#   5   "INSERT INTO t12(x) VALUES(X'39')"   {}
# }
# do_createtable_tests 5.10.2 -query { 
#   SELECT typeof(x), x FROM t12 
# } -repair {
#   execsql { DELETE FROM t12 }
# } {
#   1   "INSERT INTO t12(x) VALUES(4)"       {integer 4}
#   2   "INSERT INTO t12(x) VALUES(10.0)"    {integer 10}
#   3   "INSERT INTO t12(x) VALUES('12.0')"  {integer 12}
#   4   "INSERT INTO t12(x) VALUES('4e3')"   {integer 4000}
#   5   "INSERT INTO t12(x) VALUES('-14.0')" {integer -14}
# }

# # EVIDENCE-OF: R-07986-46024 If an INSERT statement attempts to insert a
# # NULL value into a rowid or integer primary key column, the system
# # chooses an integer value to use as the rowid automatically.
# #
# do_execsql_test 5.11.0 { DELETE FROM t12 }
# do_createtable_tests 5.11 -query { 
#   SELECT typeof(x), x FROM t12 WHERE y IS (SELECT max(y) FROM t12)
# } {
#   1   "INSERT INTO t12 DEFAULT VALUES"                {integer 1}
#   2   "INSERT INTO t12(y)   VALUES(5)"                {integer 2}
#   3   "INSERT INTO t12(x,y) VALUES(NULL, 10)"         {integer 3}
#   4   "INSERT INTO t12(x,y) SELECT NULL, 15 FROM t12" 
#       {integer 4 integer 5 integer 6}
#   5   "INSERT INTO t12(y) SELECT 20 FROM t12 LIMIT 3"
#       {integer 7 integer 8 integer 9}
# }

finish_test
