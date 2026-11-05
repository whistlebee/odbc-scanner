#include "odbc_storage_metadata.hpp"

#include "connection.hpp"
#include "diagnostics.hpp"
#include "scanner_exception.hpp"
#include "widechar.hpp"

namespace odbcscanner {
namespace {

void Check(SQLRETURN result, SQLHANDLE handle, SQLSMALLINT kind, const char *operation) {
	if (!SQL_SUCCEEDED(result)) {
		throw ScannerException(std::string("ODBC ATTACH: ") + operation +
		                       " failed: " + Diagnostics::Read(handle, kind));
	}
}

std::string InfoString(OdbcConnection &conn, SQLUSMALLINT info) {
	char buffer[1024] = {};
	SQLSMALLINT length = 0;
	Check(SQLGetInfo(conn.dbc, info, buffer, sizeof(buffer), &length), conn.dbc, SQL_HANDLE_DBC, "SQLGetInfo");
	if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
		throw ScannerException("ODBC ATTACH: driver metadata exceeds the supported length");
	}
	return std::string(buffer, length);
}

std::string MetadataString(HSTMT statement, SQLUSMALLINT column) {
	// ODBC identifiers are small, but drivers may return long catalog names. Fetch in chunks.
	std::vector<SQLWCHAR> result;
	for (;;) {
		SQLWCHAR buffer[256] = {};
		SQLLEN length = 0;
		auto status = SQLGetData(statement, column, SQL_C_WCHAR, buffer, sizeof(buffer), &length);
		if (status == SQL_NO_DATA || length == SQL_NULL_DATA) {
			break;
		}
		Check(status, statement, SQL_HANDLE_STMT, "SQLGetData");
		size_t count = 0;
		while (count < 255 && buffer[count]) {
			++count;
		}
		result.insert(result.end(), buffer, buffer + count);
		if (status == SQL_SUCCESS) {
			break;
		}
	}
	return result.empty() ? std::string() : WideChar::Narrow(result.data(), result.size());
}

std::string Quote(const std::string &identifier, const std::string &quote) {
	if (quote.empty() || quote == " ") {
		// Never interpolate an unquoted driver-supplied name into SQL.
		throw ScannerException("ODBC ATTACH requires a driver that supports quoted identifiers");
	}
	auto close = quote == "[" ? "]" : quote;
	std::string escaped;
	for (size_t pos = 0; pos < identifier.size();) {
		if (identifier.compare(pos, close.size(), close) == 0) {
			escaped += close + close;
			pos += close.size();
		} else {
			escaped += identifier[pos++];
		}
	}
	return quote + escaped + close;
}

} // namespace

std::vector<OdbcRemoteTable> DiscoverOdbcTables(const std::string &connection_string,
                                                const std::optional<std::string> &catalog_filter,
                                                const std::optional<std::string> &schema_filter) {
	OdbcConnection conn(connection_string);
	auto quote = InfoString(conn, SQL_IDENTIFIER_QUOTE_CHAR);
	auto separator = InfoString(conn, SQL_CATALOG_NAME_SEPARATOR);
	SQLUSMALLINT catalog_location = SQL_CL_START;
	Check(SQLGetInfo(conn.dbc, SQL_CATALOG_LOCATION, &catalog_location, sizeof(catalog_location), nullptr), conn.dbc,
	      SQL_HANDLE_DBC, "SQLGetInfo(SQL_CATALOG_LOCATION)");
	SQLUINTEGER catalog_usage = 0, schema_usage = 0;
	Check(SQLGetInfo(conn.dbc, SQL_CATALOG_USAGE, &catalog_usage, sizeof(catalog_usage), nullptr), conn.dbc,
	      SQL_HANDLE_DBC, "SQLGetInfo(SQL_CATALOG_USAGE)");
	Check(SQLGetInfo(conn.dbc, SQL_SCHEMA_USAGE, &schema_usage, sizeof(schema_usage), nullptr), conn.dbc,
	      SQL_HANDLE_DBC, "SQLGetInfo(SQL_SCHEMA_USAGE)");

	HSTMT handle = SQL_NULL_HSTMT;
	Check(SQLAllocHandle(SQL_HANDLE_STMT, conn.dbc, &handle), conn.dbc, SQL_HANDLE_DBC, "SQLAllocHandle");
	StmtHandlePtr statement(handle, StmtHandleDeleter);
	const std::string table_types = "'TABLE','VIEW'";
	auto types = WideChar::Widen(table_types.data(), table_types.size());
	// Filter exact names locally: SQLTables arguments otherwise interpret % and _ as patterns.
	Check(SQLTablesW(handle, nullptr, 0, nullptr, 0, nullptr, 0, types.data(), SQL_NTS), handle, SQL_HANDLE_STMT,
	      "SQLTablesW");

	std::vector<OdbcRemoteTable> tables;
	for (;;) {
		auto status = SQLFetch(handle);
		if (status == SQL_NO_DATA) {
			break;
		}
		Check(status, handle, SQL_HANDLE_STMT, "SQLFetch");
		auto catalog = MetadataString(handle, 1);
		auto schema = MetadataString(handle, 2);
		auto name = MetadataString(handle, 3);
		if ((catalog_filter && catalog != *catalog_filter) || (schema_filter && schema != *schema_filter) ||
		    name.empty()) {
			continue;
		}
		if ((!schema.empty() && !(schema_usage & SQL_SU_DML_STATEMENTS)) ||
		    (!catalog.empty() && !(catalog_usage & SQL_CU_DML_STATEMENTS))) {
			throw ScannerException(
			    "ODBC ATTACH: driver reports a table namespace it cannot qualify in SELECT statements");
		}
		auto qualified = Quote(name, quote);
		if (!schema.empty() && (schema_usage & SQL_SU_DML_STATEMENTS)) {
			qualified = Quote(schema, quote) + "." + qualified;
		}
		if (!catalog.empty() && (catalog_usage & SQL_CU_DML_STATEMENTS)) {
			qualified = catalog_location == SQL_CL_END ? qualified + separator + Quote(catalog, quote)
			                                           : Quote(catalog, quote) + separator + qualified;
		}
		tables.push_back({schema.empty() ? "main" : schema, name, "SELECT * FROM " + qualified});
	}
	return tables;
}

} // namespace odbcscanner
