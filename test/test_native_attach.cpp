#include "test_common.hpp"

namespace {

struct AttachedConn : ScannerConn {
	AttachedConn() : ScannerConn(false) {
		auto connection = std::getenv("ODBC_CONN_STRING");
		REQUIRE(connection != nullptr);
		std::string escaped;
		for (char c : std::string(connection)) {
			escaped += c;
			if (c == '\'')
				escaped += c;
		}
		Result result;
		auto ok = Query(conn, "ATTACH '" + escaped + "' AS remote (TYPE odbc)", result);
		REQUIRE(QuerySuccess(result, ok));
	}
};

int64_t Count(ScannerConn &sc, const std::string &query) {
	Result result;
	auto ok = Query(sc.conn, query, result);
	REQUIRE(QuerySuccess(result, ok));
	REQUIRE(result.NextChunk());
	return result.Value<int64_t>(0, 0);
}

} // namespace

TEST_CASE("Native ATTACH discovers schemas, tables and columns", "[native_attach]") {
	AttachedConn sc;
	REQUIRE(
	    Count(
	        sc,
	        "SELECT count(*) FROM information_schema.tables WHERE table_catalog='remote' AND table_name='employee'") ==
	    1);
	REQUIRE(
	    Count(
	        sc,
	        "SELECT count(*) FROM information_schema.columns WHERE table_catalog='remote' AND table_name='employee'") ==
	    3);
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.employee_view") == 3);
	Result result;
	REQUIRE(Query(sc.conn, "SHOW TABLES FROM remote", result));
	REQUIRE(result.NextChunk());
}

TEST_CASE("Native ATTACH scans and filters live tables", "[native_attach]") {
	AttachedConn sc;
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.employee WHERE department=10") == 2);
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.employee WHERE name IS NULL") == 1);
	Result result;
	REQUIRE(Query(sc.conn, "SELECT name FROM remote.main.EMPLOYEE WHERE id=1", result));
	REQUIRE(result.NextChunk());
	REQUIRE(result.Value<std::string>(0, 0) == "Ada");
}

TEST_CASE("Native ATTACH streams multiple chunks", "[native_attach]") {
	AttachedConn sc;
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.large_values") == 5003);
	REQUIRE(Count(sc, "SELECT CAST(sum(id) AS BIGINT) FROM remote.main.large_values") == 12517506);
}

TEST_CASE("Native ATTACH supports self joins and local joins", "[native_attach]") {
	AttachedConn sc;
	Result result;
	REQUIRE(Query(sc.conn, "SET threads=4", result));
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.employee a JOIN remote.main.employee b ON a.id=b.id") == 3);
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.employee a JOIN (VALUES (10)) d(id) ON a.department=d.id") ==
	        2);
}

TEST_CASE("Native ATTACH prepared scans execute repeatedly", "[native_attach]") {
	AttachedConn sc;
	Result prepared;
	REQUIRE(Query(sc.conn, "PREPARE p AS SELECT count(*) FROM remote.main.employee WHERE id <= $1", prepared));
	REQUIRE(Count(sc, "EXECUTE p(1)") == 1);
	REQUIRE(Count(sc, "EXECUTE p(2)") == 2);
}

TEST_CASE("Native ATTACH allows local materialization and detachment", "[native_attach]") {
	AttachedConn sc;
	Result result;
	REQUIRE(Query(sc.conn, "CREATE TABLE snapshot AS SELECT * FROM remote.main.employee", result));
	Result detached;
	REQUIRE(Query(sc.conn, "DETACH remote", detached));
	REQUIRE(Count(sc, "SELECT count(*) FROM snapshot") == 3);
	Result missing;
	REQUIRE_FALSE(Query(sc.conn, "SELECT * FROM remote.main.employee", missing));
}

TEST_CASE("Native ATTACH quotes remote identifiers", "[native_attach]") {
	AttachedConn sc;
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.\"odd\"\" table; --\"") == 1);
}

TEST_CASE("Native ATTACH rejects catalog writes", "[native_attach]") {
	AttachedConn sc;
	for (auto sql : {"INSERT INTO remote.main.employee VALUES (4,'x',10)", "UPDATE remote.main.employee SET name='x'",
	                 "DELETE FROM remote.main.employee", "DROP TABLE remote.main.employee",
	                 "CREATE TABLE remote.main.extra(id INTEGER)"}) {
		Result result;
		REQUIRE_FALSE(Query(sc.conn, sql, result));
	}
	REQUIRE(Count(sc, "SELECT count(*) FROM remote.main.employee") == 3);
}

TEST_CASE("Native ATTACH respects external access controls", "[native_attach]") {
	ScannerConn sc(false);
	Result setting;
	REQUIRE(Query(sc.conn, "SET enable_external_access=false", setting));
	Result attach;
	REQUIRE_FALSE(Query(sc.conn, "ATTACH 'Driver=nonexistent' AS remote (TYPE odbc)", attach));
	REQUIRE(attach.Error().find("enable_external_access") != std::string::npos);
}
