// Minimal SQLite C API declarations for environments where only the
// runtime library (libsqlite3.so) is installed without dev headers.
// These prototypes match the public, stable SQLite C API exactly.
// On any normal dev machine, prefer the system <sqlite3.h> instead:
// define PNR_USE_SYSTEM_SQLITE3 and this shim is bypassed.
#pragma once

#ifdef PNR_USE_SYSTEM_SQLITE3
  #include <sqlite3.h>
#else
extern "C" {

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_OPEN_READWRITE 0x2
#define SQLITE_OPEN_CREATE 0x4
#define SQLITE_OPEN_FULLMUTEX 0x10000
#define SQLITE_TRANSIENT ((void(*)(void*))-1)

int sqlite3_open_v2(const char* filename, sqlite3** ppDb, int flags,
                    const char* zVfs);
int sqlite3_close_v2(sqlite3*);
const char* sqlite3_errmsg(sqlite3*);
int sqlite3_exec(sqlite3*, const char* sql, int (*callback)(void*,int,char**,char**),
                 void*, char** errmsg);
void sqlite3_free(void*);
int sqlite3_prepare_v2(sqlite3*, const char* zSql, int nByte,
                       sqlite3_stmt** ppStmt, const char** pzTail);
int sqlite3_step(sqlite3_stmt*);
int sqlite3_finalize(sqlite3_stmt*);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*));
int sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int sqlite3_bind_double(sqlite3_stmt*, int, double);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt*, int);
const unsigned char* sqlite3_column_text(sqlite3_stmt*, int);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3*);

}  // extern "C"
#endif
