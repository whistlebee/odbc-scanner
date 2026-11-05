#!/usr/bin/env python3
"""Native ATTACH integration tests using a real SQLite ODBC driver and DuckDB CLI.

ODBC_ATTACH_DUCKDB=/path/to/duckdb ODBC_ATTACH_EXTENSION=/path/to/odbc_scanner.duckdb_extension \
ODBC_ATTACH_SQLITE_DRIVER=/path/to/libsqlite3odbc.so python3 -m unittest discover -s test -p test_native_attach.py -v
"""
import json
import os
import pathlib
import sqlite3
import subprocess
import tempfile
import unittest


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


@unittest.skipUnless(all(os.environ.get(key) for key in (
    "ODBC_ATTACH_DUCKDB", "ODBC_ATTACH_EXTENSION", "ODBC_ATTACH_SQLITE_DRIVER")),
    "Set ODBC_ATTACH_DUCKDB, ODBC_ATTACH_EXTENSION and ODBC_ATTACH_SQLITE_DRIVER")
class NativeAttach(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.database = pathlib.Path(self.temp.name) / "source.sqlite"
        with sqlite3.connect(self.database) as conn:
            conn.executescript('''
                CREATE TABLE employee(id INTEGER, name TEXT, department INTEGER);
                INSERT INTO employee VALUES (1,'Ada',10),(2,'Grace',20),(3,NULL,10);
                CREATE VIEW employee_view AS SELECT id, name FROM employee;
                CREATE TABLE "odd"" table; --" ("odd column" TEXT);
                INSERT INTO "odd"" table; --" VALUES ('café');
            ''')
        self.connection = f"Driver={os.environ['ODBC_ATTACH_SQLITE_DRIVER']};Database={self.database}"
        self.load = f"LOAD {literal(pathlib.Path(os.environ['ODBC_ATTACH_EXTENSION']).resolve())};"
        self.attach = f"ATTACH {literal(self.connection)} AS remote (TYPE odbc);"

    def sql(self, query, *, attach=True, error=None):
        proc = subprocess.run(
            [os.environ["ODBC_ATTACH_DUCKDB"], "-no-agent", "-unsigned", "-json", "-batch", "-bail"],
            input=self.load + (self.attach if attach else "") + query,
            capture_output=True, text=True, timeout=30,
        )
        if error:
            self.assertNotEqual(proc.returncode, 0, proc.stdout)
            self.assertIn(error.lower(), proc.stderr.lower())
            return
        self.assertEqual(proc.returncode, 0, proc.stderr)
        # CLI prints one JSON array for each statement with results.
        results = []
        remaining = proc.stdout.strip()
        decoder = json.JSONDecoder()
        while remaining:
            value, consumed = decoder.raw_decode(remaining)
            results.append(value)
            remaining = remaining[consumed:].lstrip()
        return results

    def test_discovery_and_columns(self):
        rows = self.sql("""
            SELECT table_name FROM information_schema.tables WHERE table_catalog='remote' ORDER BY table_name;
            SELECT column_name FROM information_schema.columns
            WHERE table_catalog='remote' AND table_name='employee' ORDER BY ordinal_position;
            SHOW TABLES FROM remote;
            DESCRIBE remote.main.employee;
        """)
        self.assertEqual([r['table_name'] for r in rows[0]], ['employee', 'employee_view', 'odd" table; --'])
        self.assertEqual([r['column_name'] for r in rows[1]], ['id', 'name', 'department'])
        self.assertEqual(len(rows[2]), 3)
        self.assertEqual(len(rows[3]), 3)

    def test_select_filter_and_null(self):
        rows = self.sql("SELECT id,name FROM remote.main.EMPLOYEE WHERE department=10 ORDER BY id;")
        self.assertEqual(rows[-1], [{'id': 1, 'name': 'Ada'}, {'id': 3, 'name': None}])

    def test_view(self):
        self.assertEqual(self.sql("SELECT count(*) AS n FROM remote.main.employee_view;")[-1], [{'n': 3}])

    def test_quoted_identifiers(self):
        rows = self.sql('SELECT "odd column" AS value FROM remote.main."odd"" table; --";')
        self.assertEqual(rows[-1], [{'value': 'café'}])

    def test_parallel_self_join_and_local_join(self):
        rows = self.sql("""
            SET threads=4;
            SELECT a.name FROM remote.main.employee a JOIN remote.main.employee b ON a.id=b.id WHERE b.id=2;
            CREATE TABLE departments AS SELECT 10 AS id, 'Research' AS label;
            SELECT d.label,count(*) AS n FROM remote.main.employee e JOIN departments d ON e.department=d.id GROUP BY d.label;
        """)
        self.assertEqual(rows[0], [{'name': 'Grace'}])
        self.assertEqual(rows[-1], [{'label': 'Research', 'n': 2}])

    def test_materialization(self):
        rows = self.sql("CREATE TABLE snapshot AS SELECT * FROM remote.main.employee; DETACH remote; SELECT count(*) AS n FROM snapshot;")
        self.assertEqual(rows[-1], [{'n': 3}])

    def test_prepared_repeated_execution(self):
        rows = self.sql("PREPARE lookup AS SELECT name FROM remote.main.employee WHERE id=$1; EXECUTE lookup(1); EXECUTE lookup(2);")
        self.assertEqual(rows, [[{'name': 'Ada'}], [{'name': 'Grace'}]])

    def test_existing_query_function_and_live_reads(self):
        query = "INSERT INTO employee VALUES (4,'Edsger',20)"
        rows = self.sql(f"FROM odbc_query({literal(self.connection)}, {literal(query)}); SELECT count(*) AS n FROM remote.main.employee;")
        self.assertEqual(rows[-1], [{'n': 4}])

    def test_multiple_streaming_chunks(self):
        with sqlite3.connect(self.database) as conn:
            conn.executemany("INSERT INTO employee VALUES (?, 'extra', 10)", ((i,) for i in range(4, 5004)))
        rows = self.sql("SELECT count(*) AS n, CAST(sum(id) AS BIGINT) AS total FROM remote.main.employee;")
        self.assertEqual(rows[-1], [{'n': 5003, 'total': 5003 * 5004 // 2}])

    def test_existing_handle_and_parameter_binding(self):
        rows = self.sql(f"""
            SET VARIABLE handle = odbc_connect({literal(self.connection)});
            PREPARE q AS FROM odbc_query(getvariable('handle'), 'SELECT name FROM employee WHERE id=?', params=row($1));
            EXECUTE q(1);
            EXECUTE q(2);
            DEALLOCATE q;
            SELECT odbc_close(getvariable('handle')) AS closed;
        """)
        self.assertEqual(rows[:2], [[{'name': 'Ada'}], [{'name': 'Grace'}]])
        self.assertEqual(rows[-1], [{'closed': None}])

    def test_mapping_matches_existing_scanner(self):
        rows = self.sql(f"""
            SELECT * FROM remote.main.employee ORDER BY id;
            FROM odbc_query({literal(self.connection)}, 'SELECT * FROM employee ORDER BY id');
        """)
        self.assertEqual(rows[0], rows[1])

    def test_write_rejection(self):
        statements = [
            "INSERT INTO remote.main.employee VALUES (4,'X',10)",
            "UPDATE remote.main.employee SET name='X'",
            "DELETE FROM remote.main.employee",
            "DROP TABLE remote.main.employee",
            "ALTER TABLE remote.main.employee ADD COLUMN extra INTEGER",
            "CREATE TABLE remote.main.extra(id INTEGER)",
            "CREATE TABLE remote.main.extra AS SELECT 1 AS id",
            "CREATE VIEW remote.main.extra AS SELECT 1 AS id",
        ]
        for statement in statements:
            with self.subTest(statement=statement):
                self.sql(statement, error="base table" if statement.startswith(("UPDATE", "DELETE")) else "read-only")
        with sqlite3.connect(self.database) as conn:
            self.assertEqual(conn.execute("SELECT count(*) FROM employee").fetchone()[0], 3)

    def test_detach_reattach_and_alias(self):
        rows = self.sql(f"DETACH remote; ATTACH {literal(self.connection)} AS other (TYPE odbc_scanner, READ_ONLY); SELECT count(*) AS n FROM other.main.employee;")
        self.assertEqual(rows[-1], [{'n': 3}])
        self.sql("DETACH remote; SELECT * FROM remote.main.employee;", error="does not exist")

    def test_unknown_option(self):
        self.sql(f"ATTACH {literal(self.connection)} AS remote (TYPE odbc, typo 'x');", attach=False, error="Unknown ODBC ATTACH option")

    def test_schema_and_catalog_filters(self):
        for option in ("SCHEMA", "CATALOG"):
            with self.subTest(option=option):
                rows = self.sql(f"ATTACH {literal(self.connection)} AS remote (TYPE odbc, {option} 'does_not_exist'); SELECT count(*) AS n FROM information_schema.tables WHERE table_catalog='remote';", attach=False)
                self.assertEqual(rows[-1], [{'n': 0}])

    def test_explicit_empty_namespace_filters(self):
        rows = self.sql(f"ATTACH {literal(self.connection)} AS remote (TYPE odbc, CATALOG '', SCHEMA ''); SELECT count(*) AS n FROM information_schema.tables WHERE table_catalog='remote';", attach=False)
        self.assertEqual(rows[-1], [{'n': 3}])

    def test_external_access(self):
        self.sql("SET enable_external_access=false;" + self.attach, attach=False, error="enable_external_access")
        self.sql("SET enable_external_access=false; SELECT * FROM remote.main.employee;", error="external access")

    def test_bad_driver(self):
        self.sql("ATTACH 'Driver=nonexistent_attach_test_driver;Database=none' AS remote (TYPE odbc);", attach=False, error="SQLDriverConnect")

    def test_database_metadata_is_readonly_and_hides_connection(self):
        rows = self.sql("SELECT readonly,path FROM duckdb_databases() WHERE database_name='remote';")
        self.assertTrue(rows[-1][0]['readonly'])
        self.assertNotIn('Driver=', rows[-1][0]['path'] or '')

    def test_schema_change_requires_reattach(self):
        query = 'ALTER TABLE employee ADD COLUMN extra INTEGER'
        self.sql(f"FROM odbc_query({literal(self.connection)}, {literal(query)}); SELECT * FROM remote.main.employee;", error="schema changed")


if __name__ == '__main__':
    unittest.main()
