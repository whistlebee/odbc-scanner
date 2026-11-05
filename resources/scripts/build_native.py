#!/usr/bin/env python3
"""Build the optional native catalog adapter against a matching DuckDB checkout."""
import argparse
import pathlib
import shutil
import subprocess


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb-source", required=True, type=pathlib.Path)
    parser.add_argument("--duckdb", default="duckdb")
    parser.add_argument("--build-dir", default="cmake_build/native", type=pathlib.Path)
    parser.add_argument("--jobs", default="4")
    parser.add_argument("--duckdb-library", type=pathlib.Path, help="Matching DuckDB import library for Windows")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    source = args.duckdb_source.resolve()
    build = args.build_dir.resolve()
    version = run(args.duckdb, "-no-agent", "-csv", "-noheader", "-c", "SELECT version();", capture_output=True, text=True).stdout.strip()
    if version != "v2.0.0":
        parser.error(f"native ATTACH currently targets DuckDB v2.0.0, found {version}")
    platform = run(args.duckdb, "-no-agent", "-csv", "-noheader", "-c", "PRAGMA platform;", capture_output=True, text=True).stdout.strip()
    library_flags = [f"-DODBC_DUCKDB_LIBRARY={args.duckdb_library.resolve()}"] if args.duckdb_library else []
    run("cmake", "-S", root, "-B", build, "-DEXTENSION_NAME=odbc_scanner",
        "-DTARGET_DUCKDB_VERSION_MAJOR=2", "-DTARGET_DUCKDB_VERSION_MINOR=0", "-DTARGET_DUCKDB_VERSION_PATCH=0",
        "-DDUCKDB_EXTENSION_API_VERSION_UNSTABLE=1", "-DODBC_NATIVE_ATTACH=ON", "-DENABLE_C_API_TESTS=OFF",
        f"-DODBC_DUCKDB_SOURCE_DIR={source}", "-DCMAKE_BUILD_TYPE=Release", *library_flags)
    run("cmake", "--build", build, "--parallel", args.jobs)
    candidates = list(build.glob("*odbc_scanner.dylib")) + list(build.glob("*odbc_scanner.so")) + list(build.glob("*odbc_scanner.dll"))
    if len(candidates) != 1:
        raise RuntimeError(f"Expected exactly one built library in {build}: {candidates}")
    output = build / "odbc_scanner.duckdb_extension"
    shutil.copyfile(candidates[0], output)
    platform_file = build / "native_platform.txt"
    platform_file.write_text(platform)
    extension_version = run("git", "-C", root, "rev-parse", "--short=10", "HEAD", capture_output=True, text=True).stdout.strip()
    run("cmake", f"-DEXTENSION={output}", f"-DPLATFORM_FILE={platform_file}",
        f"-DVERSION_FIELD={version}", f"-DEXTENSION_VERSION={extension_version}", "-DABI_TYPE=CPP",
        f"-DNULL_FILE={source / 'scripts/null.txt'}", "-P", source / "scripts/append_metadata.cmake")
    print(f"Built {output}")


if __name__ == "__main__":
    main()
