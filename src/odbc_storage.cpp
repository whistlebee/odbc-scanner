// Native DuckDB catalog adapter. ODBC reads still use the existing C API scanner.
#include "odbc_storage_metadata.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

#include <mutex>

struct duckdb_v2_extension_input;
extern "C" void odbc_scanner_init_c_api_v2(duckdb_v2_extension_input *input);

namespace duckdb {
namespace {

[[noreturn]] void ReadOnly() {
	throw NotImplementedException("ODBC attached catalogs are read-only");
}

// Let the normal function binder fill the signature slots and defaults required by C API V2.
TableFunction BindQuery(ClientContext &context, const string &connection, const string &query,
                        unique_ptr<FunctionData> &bind_data, vector<LogicalType> &types, vector<Identifier> &names) {
	auto &entry = Catalog::GetEntry<TableFunctionCatalogEntry>(
	    context, QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(), Identifier("odbc_query")));
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(make_uniq<BoundConstantExpression>(Value(connection)));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value(query)));
	vector<pair<Identifier, unique_ptr<Expression>>> keywords;
	vector<Value> parameters;
	named_argument_map_t named_parameters;
	ErrorData error;
	FunctionBinder binder(context);
	auto index = binder.BindFunction(Identifier("odbc_query"), entry.functions, arguments, keywords, parameters,
	                                 named_parameters, error);
	if (!index.IsValid()) {
		error.Throw();
	}
	auto function = entry.functions.GetFunctionByOffset(index.GetIndex());
	BoundTableFunction bound(function);
	bound.SetCallArguments(parameters, named_parameters);
	vector<LogicalType> input_types;
	vector<Identifier> input_names;
	TableFunctionRef ref;
	TableFunctionBindInput input(parameters, named_parameters, input_types, input_names, function->function_info,
	                             nullptr, bound, ref);
	bind_data = function->bind(context, input, types, names);
	return *function;
}

class OdbcTableEntry final : public TableCatalogEntry {
public:
	OdbcTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, string connection, string query)
	    : TableCatalogEntry(catalog, schema, info), columns(info.columns.Copy()), connection(std::move(connection)),
	      query(std::move(query)) {
	}

	const ColumnList &GetColumns() const override {
		return columns;
	}
	// The query scanner does not implement row IDs. Without this, COUNT(*) may choose
	// DuckDB's default virtual rowid, which requires projection pushdown in the scanner.
	virtual_column_map_t GetVirtualColumns() const override {
		return {};
	}
	vector<column_t> GetRowIdColumns() const override {
		return {};
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &, column_t) override {
		return nullptr;
	}
	TableStorageInfo GetStorageInfo(ClientContext &) override {
		return TableStorageInfo();
	}
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override {
		vector<LogicalType> types;
		vector<Identifier> names;
		auto function = BindQuery(context, connection, query, bind_data, types, names);
		if (types.size() != columns.LogicalColumnCount()) {
			throw BinderException("ODBC table schema changed; DETACH and ATTACH the database again");
		}
		for (idx_t i = 0; i < types.size(); ++i) {
			if (types[i] != columns.GetColumn(LogicalIndex(i)).Type() ||
			    names[i] != columns.GetColumn(LogicalIndex(i)).Name()) {
				throw BinderException("ODBC table schema changed; DETACH and ATTACH the database again");
			}
		}
		return function;
	}

private:
	ColumnList columns;
	string connection;
	string query;
};

class OdbcSchemaEntry final : public SchemaCatalogEntry {
public:
	OdbcSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
	}

	void AddTable(ClientContext &context, const string &connection, const odbcscanner::OdbcRemoteTable &table) {
		Identifier name(table.name);
		if (tables.find(name) != tables.end()) {
			throw BinderException(
			    "ODBC ATTACH: duplicate table '%s.%s'; select a remote catalog with the CATALOG option", this->name,
			    name);
		}
		vector<LogicalType> types;
		vector<Identifier> names;
		unique_ptr<FunctionData> probe;
		BindQuery(context, connection, table.query, probe, types, names);
		CreateTableInfo info(QualifiedName(catalog.GetName(), this->name, name));
		for (idx_t i = 0; i < types.size(); ++i) {
			info.columns.AddColumn(ColumnDefinition(names[i], types[i]));
		}
		tables.emplace(name, make_uniq<OdbcTableEntry>(catalog, *this, info, connection, table.query));
	}

	void Scan(ClientContext &, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		Scan(type, callback);
	}
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		if (type == CatalogType::TABLE_ENTRY) {
			for (auto &table : tables)
				callback(*table.second);
		}
	}
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction, const EntryLookupInfo &lookup) override {
		if (lookup.GetCatalogType() != CatalogType::TABLE_ENTRY)
			return nullptr;
		auto it = tables.find(Identifier(lookup.GetEntryName()));
		return it == tables.end() ? nullptr : it->second.get();
	}

	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction, CreateFunctionInfo &) override {
		ReadOnly();
	}
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction, BoundCreateTableInfo &) override {
		ReadOnly();
	}
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction, CreateViewInfo &) override {
		ReadOnly();
	}
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction, CreateSequenceInfo &) override {
		ReadOnly();
	}
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction, CreateTypeInfo &) override {
		ReadOnly();
	}
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) override {
		ReadOnly();
	}
	void DropEntry(ClientContext &, DropInfo &) override {
		ReadOnly();
	}
	void Alter(CatalogTransaction, AlterInfo &) override {
		ReadOnly();
	}

private:
	identifier_map_t<unique_ptr<OdbcTableEntry>> tables;
};

class OdbcCatalog final : public Catalog {
public:
	OdbcCatalog(AttachedDatabase &db, const string &connection, ClientContext &context,
	            const std::optional<string> &remote_catalog, const std::optional<string> &remote_schema)
	    : Catalog(db) {
		// No credentials in GetDBPath()/duckdb_databases().
		AddSchema(Identifier::DefaultSchema());
		for (auto &table : odbcscanner::DiscoverOdbcTables(connection, remote_catalog, remote_schema)) {
			auto &schema = AddSchema(Identifier(table.schema));
			schema.AddTable(context, connection, table);
		}
	}
	void Initialize(bool) override {
	}
	string GetCatalogType() override {
		return "odbc";
	}
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return string();
	}
	DatabaseSize GetDatabaseSize(ClientContext &) override {
		return DatabaseSize();
	}
	void ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) override {
		for (auto &schema : schemas)
			callback(*schema.second);
	}
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction, const EntryLookupInfo &lookup,
	                                              OnEntryNotFound if_not_found) override {
		auto name = lookup.GetEntryName().empty() ? Identifier::DefaultSchema() : Identifier(lookup.GetEntryName());
		auto it = schemas.find(name);
		if (it != schemas.end())
			return it->second.get();
		if (if_not_found == OnEntryNotFound::RETURN_NULL)
			return nullptr;
		throw BinderException("Schema '%s' not found in ODBC catalog", name);
	}
	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction, CreateSchemaInfo &) override {
		ReadOnly();
	}
	void DropSchema(ClientContext &, DropInfo &) override {
		ReadOnly();
	}

	PhysicalOperator &PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
	                                    PhysicalOperator &) override {
		ReadOnly();
	}
	PhysicalOperator &PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
	                             optional_ptr<PhysicalOperator>) override {
		ReadOnly();
	}
	PhysicalOperator &PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
	                             PhysicalOperator &) override {
		ReadOnly();
	}
	PhysicalOperator &PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
	                             PhysicalOperator &) override {
		ReadOnly();
	}

private:
	OdbcSchemaEntry &AddSchema(const Identifier &name) {
		auto it = schemas.find(name);
		if (it != schemas.end())
			return *it->second;
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName(GetName(), name, Identifier()));
		auto schema = make_uniq<OdbcSchemaEntry>(*this, info);
		auto &result = *schema;
		schemas.emplace(name, std::move(schema));
		return result;
	}
	identifier_map_t<unique_ptr<OdbcSchemaEntry>> schemas;
};

class OdbcTransaction final : public Transaction {
public:
	OdbcTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {
	}
};

class OdbcTransactionManager final : public TransactionManager {
public:
	explicit OdbcTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}
	Transaction &StartTransaction(ClientContext &context) override {
		auto transaction = make_uniq<OdbcTransaction>(*this, context);
		auto &result = *transaction;
		std::lock_guard<std::mutex> guard(lock);
		transactions.emplace(&result, std::move(transaction));
		return result;
	}
	ErrorData CommitTransaction(ClientContext &, Transaction &transaction) override {
		RollbackTransaction(transaction);
		return ErrorData();
	}
	void RollbackTransaction(Transaction &transaction) override {
		std::lock_guard<std::mutex> guard(lock);
		transactions.erase(&transaction);
	}
	void Checkpoint(ClientContext &, bool) override {
	}

private:
	std::mutex lock;
	unordered_map<Transaction *, unique_ptr<OdbcTransaction>> transactions;
};

unique_ptr<Catalog> AttachOdbc(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                               const string &, AttachInfo &info, AttachOptions &options) {
	Value external_access;
	if (context.TryGetCurrentSetting(Identifier("enable_external_access"), external_access) &&
	    !external_access.GetValue<bool>()) {
		throw PermissionException("ODBC ATTACH is disabled through enable_external_access");
	}
	std::optional<string> remote_catalog, remote_schema;
	for (auto &option : info.options) {
		if (option.first == "type" || option.first == "read_only")
			continue;
		if (option.first == "catalog")
			remote_catalog = option.second.GetValue<string>();
		else if (option.first == "schema")
			remote_schema = option.second.GetValue<string>();
		else
			throw BinderException("Unknown ODBC ATTACH option '%s'", option.first);
	}
	options.access_mode = AccessMode::READ_ONLY;
	db.SetReadOnlyDatabase();
	return make_uniq<OdbcCatalog>(db, info.path, context, remote_catalog, remote_schema);
}

unique_ptr<TransactionManager> CreateTransactions(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db, Catalog &) {
	return make_uniq<OdbcTransactionManager>(db);
}

} // namespace
} // namespace duckdb

// CPP metadata selects this entrypoint. Invoke the original C API entrypoint through DuckDB's own adapter,
// then add the native storage interface. This keeps all existing functions and conversions unchanged.
extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(odbc_scanner, loader) {
	auto &db = loader.GetDatabaseInstance();
	duckdb::ExtensionInitResult init;
	init.abi_type = duckdb::ExtensionABIType::C_STRUCT_UNSTABLE;
	db.InvokeExtensionEntrypointV2(init, "odbc_scanner", odbc_scanner_init_c_api_v2, nullptr, false);
	auto storage = duckdb::make_shared_ptr<duckdb::StorageExtension>();
	storage->attach = duckdb::AttachOdbc;
	storage->create_transaction_manager = duckdb::CreateTransactions;
	duckdb::StorageExtension::Register(duckdb::DBConfig::GetConfig(db), "odbc_scanner", storage);
}
}
