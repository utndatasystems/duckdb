This is what I use to compile DuckDB with the corresponding Python package.

It's pretty important to clean the python library before building, otherwise it's still there.

```
./tools/pythonpkg/clean.sh && BUILD_PYTHON=1 GEN=ninja make

BUILD_PYTHON=1 GEN=ninja make
```

I don't remember what this is for, probably just to use the very same binary.

```
sudo cp duckdb /usr/local/bin/duckdb
```