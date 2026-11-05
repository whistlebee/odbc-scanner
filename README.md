# DuckDB ODBC extension

[Extension documentation](https://duckdb.org/docs/current/core_extensions/odbc/overview).

[ODBC functions API reference](https://duckdb.org/docs/current/core_extensions/odbc/functions).

The extension is no longer compatible with older versions of DuckDB. To install the latest published versiobn that is still binary compatible with DuckDB v1.2.0+ use the following command:

```sql
FORCE INSTALL 'http://nightly-extensions.duckdb.org/odbc_scanner/02132ae/v1.2.0/<platform>/odbc_scanner.duckdb_extension.gz'
```

Where the `<platform>` is one of:

 - `linux_amd64`
 - `linux_arm64`
 - `linux_amd64_musl`
 - `linux_arm64_musl`
 - `osx_amd64`
 - `osx_arm64`
 - `windows_amd64`
 - `windows_arm64`

To install the latest available version on DuckDB v2.0+ use:

```
FORCE INSTALL odbc FROM core_nightly;
```

## Building

Dependencies: 

- Python3
- Python3-venv
- GNU Make
- CMake
- unixODBC

Building is a two-step process. Firstly run:

```shell
make configure
```
This will ensure a Python venv is set up with DuckDB and DuckDB's test runner installed. Additionally, depending on configuration,
DuckDB will be used to determine the correct platform for which you are compiling.

Then, to build the extension run:
```shell
make debug
```
This produces a shared library in `target/debug/<shared_lib_name>`. After this step, 
a script is run to transform the shared library into a loadable extension by appending a binary footer. The resulting extension is written
to the `build/debug` directory.

To create optimized release binaries, simply run `make release` instead.

## Native read-only ATTACH (this fork)

Supported hosts: the macOS v2.0.0 CLI and applications using the official v2.0.0
shared library. The official Linux v2.0.0 CLI omits catalog symbols needed by
native storage extensions, so it cannot load this adapter. Linux integration
uses the full shared library. A custom CLI must export the full C++ catalog API.

This fork adds an optional native catalog adapter for **DuckDB v2.0.0**. It uses
DuckDB's internal C++ storage interface and the existing `odbc_query` scanner.
The native binary must match the DuckDB version, platform, and C++ build settings; ordinary C API
builds remain available without `ATTACH`.

```sql
-- Use -unsigned / allow_unsigned_extensions for a locally built extension.
LOAD '/absolute/path/to/odbc_scanner.duckdb_extension';

ATTACH 'Driver={PostgreSQL Unicode};Server=localhost;Database=analytics;UID=reader;PWD=secret'
    AS source (TYPE odbc);

SHOW TABLES FROM source;
DESCRIBE source.public.customers;
SELECT * FROM source.public.customers WHERE id = 42;

-- Sources with no remote schema, such as SQLite, appear under main.
-- SELECT * FROM source.main.customers;

-- Local materialization is supported.
CREATE TABLE customer_snapshot AS SELECT * FROM source.public.customers;
DETACH source;
```

`TYPE odbc_scanner` is also accepted. `CATALOG 'name'` and `SCHEMA 'name'`
attachment options select exact remote metadata names. Remote schemas retain
their names; an empty schema maps to `main`. Tables and views are discovered
through `SQLTablesW`, and their columns use the scanner's existing type mapping.
Identifiers are quoted using the driver's metadata. Colliding names from
multiple remote catalogs produce an error; select one with `CATALOG`.
The connection string is omitted from `duckdb_databases().path`.

Behavior and limits:

- The attachment is always read-only: `INSERT`, `UPDATE`, `DELETE`, and schema
  changes through the attached catalog are rejected. Existing explicit ODBC
  query and copy functions retain their own behavior.
- Rows are read live. Discovery eagerly prepares each table/view to obtain its
  columns without executing the scan. Metadata stays cached until reattachment;
  a changed column layout produces an error asking you to detach and attach again.
- Each bound scan has its own ODBC connection, allowing self-joins without sharing
  an active connection between scan threads. There is no connection pool.
- DuckDB performs filters, projections, joins, and aggregates locally. This first
  version generates `SELECT *` for remote reads and adds no remote pushdown.
- DuckDB transactions do not coordinate remote transactions or provide one shared
  remote snapshot across scans. Remote reads use the scanner's existing behavior.
- Driver-specific metadata, encoding, and type limitations still apply. The native
  integration suite uses a real SQLite ODBC driver; other backends need validation.

### Build and test native ATTACH

Install CMake, a C++17 compiler, Python 3, unixODBC development headers (or the
Windows ODBC SDK), and an ODBC driver. Check out the matching DuckDB source and
use its matching CLI:

```shell
git clone --depth 1 --branch v2.0.0 https://github.com/duckdb/duckdb.git ../duckdb-v2
python3 resources/scripts/build_native.py \
    --duckdb-source ../duckdb-v2 \
    --duckdb /path/to/duckdb-v2.0.0
```

The output is `cmake_build/native/odbc_scanner.duckdb_extension`. The build checks
that the source checkout's C API headers match this fork's vendored headers.
Native Windows builds additionally need the matching DuckDB import library;
pass `--duckdb-library /path/to/duckdb.lib` to the build script. Windows builds
have not been validated locally.
Linux and macOS resolve native DuckDB symbols from the host process. Custom
hosts built with `DISABLE_RTTI` must also set `ODBC_NATIVE_DISABLE_RTTI=ON` in
the native CMake configuration.

Run the integration suite with an installed SQLite ODBC driver:

```shell
ODBC_ATTACH_DUCKDB=/path/to/duckdb-v2.0.0 \
ODBC_ATTACH_EXTENSION="$PWD/cmake_build/native/odbc_scanner.duckdb_extension" \
ODBC_ATTACH_SQLITE_DRIVER=/path/to/libsqlite3odbc.so \
python3 -m unittest discover -s test -p test_native_attach.py -v
```

For Homebrew, install `unixodbc sqliteodbc`; the driver is normally
`/opt/homebrew/opt/sqliteodbc/lib/libsqlite3odbc.dylib` on Apple Silicon.

For shared-library hosts (including Linux), build and run the C++ integration
suite using the matching library and the SQLite fixture defined in
`.github/workflows/native-attach.yml`:

```shell
DUCKDB_CAPI_LIB_PATH=/path/to/libduckdb.so \
python3 resources/scripts/build_native.py \
    --duckdb-source ../duckdb-v2 --duckdb /path/to/duckdb-v2.0.0 --c-api-tests
mkdir -p build/release
cp cmake_build/native/odbc_scanner.duckdb_extension build/release/
ODBC_CONN_STRING='Driver=/path/to/libsqlite3odbc.so;Database=/path/to/source.sqlite' \
cmake_build/native/test/test_odbc_scanner '[native_attach]'
```
