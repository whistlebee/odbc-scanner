#pragma once

#include <optional>
#include <string>
#include <vector>

namespace odbcscanner {

// Plain C++ boundary: the native storage adapter must not include the C API wrapper headers.
struct OdbcRemoteTable {
	std::string schema;
	std::string name;
	std::string query;
};

std::vector<OdbcRemoteTable> DiscoverOdbcTables(const std::string &connection_string,
                                                const std::optional<std::string> &catalog_filter,
                                                const std::optional<std::string> &schema_filter);

} // namespace odbcscanner
