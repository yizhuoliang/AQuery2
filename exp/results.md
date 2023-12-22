## 5 Million Lines 3-Month Moving Average

| Feature / Database        | Traditional RDBMS   | Traditional RDBMS   | Traditional RDBMS   | Analytical DBs      | Analytical DBs      | Analytical DBs      | Libraries            | AQuery |
|---------------------------|---------------------|---------------------|---------------------|---------------------|---------------------|---------------------|----------------------|--------------|
|                           | SQL Server In-Mem OLTP| SQL Server OLAP Cube| PostgreSQL          | DuckDB              | Clickhouse          | Vertica              | Pandas               |              |
| Execution Time         | 13.71              | -                  | -                  | -                  | -                  | -                  |    -                | 1.76        |
| Total TIme                      | 14.47              | -                  | 9.85               | 1.23               | 0.622               | -                  |     1.31            | 2.52        |

### Testing Notes
- 5 million lines is about the maximum for SQL Server's free express in-mem table
- PostgreSQL does not natively support in-memory database, so it is tested with `UNLOGGED` table, extended `shared_buffers`, and `pg_prewarm` loading into memory. (there are also columnar extention)
- all other DBs are default or tuned to use in-memory mode
- all ouputs are redirected to a result table