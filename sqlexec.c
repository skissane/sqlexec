/*
** SQLEXEC is a sqlite3 extension which allows you to define virtual tables
** in terms of SQL. Why do you want to do this? Well, you can't use PRAGMA
** in queries, views, etc. Suppose you want to define a view which includes
** the results of a sqlite3 PRAGMA call, you can't. But, with this extension
** you can. For example:
**
** sqlite> .load sqlexec
** sqlite> create virtual table pragma_database_list
**    ...> using sqlexec(pragma database_list);
** sqlite> .head on
** sqlite> select * from pragma_database_list;
** seq|name|file
** 0|main|
*/
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <string.h>
#include <ctype.h>

/*
** Stores definition of each virtual table. We need to store the underlying
** SQL we will be executing to get the data of this virtual table.
*/
typedef struct sqlexec_vtab sqlexec_vtab;
struct sqlexec_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char * sql;
};

/*
** Stores the cursor used to return data from our virtual table. Keep track
** of row number (iRowid) and also the underlying statement handle we are
** executing.
*/
typedef struct sqlexec_cursor sqlexec_cursor;
struct sqlexec_cursor {
  sqlite3_vtab_cursor base;
  sqlite3_int64 iRowid;
  sqlite3_stmt *pStmt;
};

/*
** Sqlite calls this function when CREATE VIRTUAL TABLE is executed. We get
** passed the USING clause. We need to declare the columns of the virtual
** table, allocate the virtual table object, and do anything else needed to
** get the virtual table ready. In our case, that means preparing the
** underlying SQL (passed via the USING clause) to validate its syntax and
** find out what columns it returns.
*/
static int sqlexecConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  sqlexec_vtab *pNew;
  int rc;

  /*
  ** Check passed arguments are what we expected. Note that argv vector looks
  ** like this:
  ** argv[0]: module name (in our case "sqlexec")
  ** argv[1]: name of database (most commonly "main")
  ** argv[2]: name of virtual table
  ** argv[3]: first USING clause argument
  **
  ** Note although we are module/database/table names, we don't use them for
  ** anything at the moment; all we use is what is in the USING clause. At
  ** present we expect exactly one parameter in the USING clause so that is
  ** what we are validating for below.
  */
  if (argc != 4) {
    if (pzErr)
      *pzErr = sqlite3_mprintf("sqlexecConnect: expected 1 argument in USING clause, got %d\n",
                               argc - 3);
    return SQLITE_ERROR;
  }

  /*
  ** In first parameter of USING clause we were passed SQL to execute.
  ** Optionally it is surrounded by parenthesis so that any commas in it
  ** are ignored (and not considered another USING clause argument).
  ** So now we check if the argument starts with ( and ends with ) and
  ** if so we will strip the ( and ) off the start/end. We allow whitespace
  ** before the ( and after the ) but not any other character.
  */
  char *sql = NULL;
  char *parenOpen = strchr(argv[3],'(');
  if (parenOpen != NULL) {
    for (const char *p = argv[3]; p != parenOpen; p++) {
      if (!isspace(*p)) {
        parenOpen = NULL;
        break;
      }
    }
  }
  if (parenOpen != NULL) {
    char *parenClose = strrchr(argv[3],')');
    if (parenClose != NULL) {
      for (char *p = strchr(argv[3],0)-1; p != parenClose; p--) {
        if (!isspace(*p)) {
          parenClose = NULL;
          break;
        }
      }
    }
    if (parenClose != NULL) {
      sql = sqlite3_mprintf("%s", parenOpen+1);
      if (sql == NULL)
        return SQLITE_NOMEM;
      *(strrchr(sql,')')) = 0;
    }
  }
  if (sql == NULL)
    sql = sqlite3_mprintf("%s", argv[3]);
  if (sql == NULL)
    return SQLITE_NOMEM;

  /*
  ** Now we prepare the statement to validate its syntax and find out the
  ** columns it returns.
  */
  sqlite3_stmt * pStmt;
  rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, NULL);
  if (rc != SQLITE_OK) {
    if (pzErr)
      *pzErr = sqlite3_mprintf("Error preparing: %s; reason: %s", sql,
          sqlite3_errmsg(db));
    return rc;
  }

  /*
  ** Find out number of columns prepared SQL returns. If it returns zero
  ** columns we return an error here.
  */
  int colCount = sqlite3_column_count(pStmt);
  if (colCount == 0) {
    sqlite3_finalize(pStmt); /* avoid memory leak of prepared stmt */
    if (pzErr)
      *pzErr = sqlite3_mprintf("SQL statement returns no data: %s", sql);
    return SQLITE_ERROR;
  }

  /*
  ** Now we begin constructing the CREATE TABLE statement we need to pass
  ** to the sqlite3_declare_vtab function. This should always work unless
  ** we run out of memory.
  */
  char *decl = sqlite3_mprintf("%s", "create table x(");
  if (decl == NULL) {
    sqlite3_free(sql);       /* avoid memory leak of SQL text */
    sqlite3_finalize(pStmt); /* avoid memory leak of prepared stmt */
    return SQLITE_NOMEM;
  }

  for (int i = 0; i < colCount; i++) {
    const char *colName = sqlite3_column_name(pStmt, i);
    if (colName == NULL) {
      sqlite3_free(sql);       /* avoid memory leak of SQL text */
      sqlite3_free(decl);
      sqlite3_finalize(pStmt); /* avoid memory leak of prepared stmt */
      return SQLITE_NOMEM;
    }
    char *decl2 = sqlite3_mprintf("%s%s'%s'%s",
                                  decl,
                                  i == 0 ? "" : ",",
                                  colName,
                                  i + 1 == colCount ? ")" : "");
    if (decl2 == NULL) {
      sqlite3_free(sql);       /* avoid memory leak of SQL text */
      sqlite3_free(decl);
      sqlite3_finalize(pStmt); /* avoid memory leak of prepared stmt */
      return SQLITE_NOMEM;
    }
    sqlite3_free(decl);
    decl = decl2;
  }

  /*
  ** Now we have constructed the CREATE TABLE statement we don't need the
  ** statement handle any more.
  */
  rc = sqlite3_finalize(pStmt);
  if (rc != SQLITE_OK) {
    if (pzErr)
      *pzErr = sqlite3_mprintf("sqlexecConnect: sqlite3_finalize failed for: %s\n",
                               sql);
   sqlite3_free(decl);
   return rc;
  }
  pStmt = NULL;

  /*
  ** Declare columns of this virtual table.
  */
  rc = sqlite3_declare_vtab(db, decl);
  if (rc != SQLITE_OK) {
    if (pzErr)
      *pzErr = sqlite3_mprintf("sqlexecConnect: sqlite3_declare_vtab failed for %s\n",
                               decl);
    sqlite3_free(decl);
    return rc;
  }
  sqlite3_free(decl); /* Don't need CREATE TABLE string any more */
  decl = NULL;

  /*
  ** Allocate memory for virtual table object.
  */
  pNew = sqlite3_malloc( sizeof(*pNew) );
  if( pNew==0 ) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(*pNew));
  pNew->db = db;
  pNew->sql = sql;

  /*
  ** Return to caller.
  */
  *ppVtab = (sqlite3_vtab *) pNew;
  return SQLITE_OK;
}

/*
** Disconnect virtual table. All we need to do is make sure that memory for
** virtual table object is deallocated.
*/
static int sqlexecDisconnect(sqlite3_vtab *pVtab){
  sqlexec_vtab *vtab = (sqlexec_vtab *)pVtab;
  sqlite3_free(vtab->sql);
  sqlite3_free(vtab);
  return SQLITE_OK;
}

/*
** Opens a cursor on our virtual table. We need to prepare the underlying
** SQL and stash the statement handle into our cursor.
*/
static int sqlexecOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor){
  sqlexec_vtab *vtab = (sqlexec_vtab*)p;
  sqlite3 *db = vtab->db;

  /*
  ** Allocate memory for cursor object.
  */
  sqlexec_cursor *pCur;
  pCur = sqlite3_malloc( sizeof(*pCur) );
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));

  /*
  ** Prepare the SQL statement.
  */
  int rc = sqlite3_prepare_v2(db, vtab->sql, -1, &pCur->pStmt, NULL);
  if (rc != SQLITE_OK) {
    vtab->base.zErrMsg = sqlite3_mprintf("Error preparing: %s; reason: %s",
                                         vtab->sql, sqlite3_errmsg(db));
    sqlite3_free(pCur); /* don't leak memory */
    return rc;
  }

  /*
  ** Success: provide cursor object to caller and return SQLITE_OK.
  */
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

/*
** Close the cursor. We have to finalize the underlying statement handle
** and deallocate memory.
*/
static int sqlexecClose(sqlite3_vtab_cursor *cur){
  sqlexec_cursor *pCur = (sqlexec_cursor *)cur;
  int rc = SQLITE_OK;
  if (pCur->pStmt != NULL) {
    rc = sqlite3_finalize(pCur->pStmt);
    pCur->pStmt = NULL;
  }
  sqlite3_free(pCur);
  return rc;
}

/*
** Advance to next row.
*/
static int sqlexecNext(sqlite3_vtab_cursor *cur){
  sqlexec_cursor *pCur = (sqlexec_cursor*)cur;

  /*
  ** If statement handle is NULL, that indicates we are at end of data,
  ** don't advance any further.
  */
  if (pCur->pStmt == NULL)
    return SQLITE_OK;

  /*
  ** Advance underlying statement handle.
  */
  int rc = sqlite3_step(pCur->pStmt);
  if (rc == SQLITE_DONE) { /* Handle end of data */
    rc = sqlite3_finalize(pCur->pStmt);
    if (rc != SQLITE_OK)
      return rc;
    pCur->pStmt = NULL;
    return SQLITE_OK;
  }
  if (rc == SQLITE_ROW) { /* Handle a row */
    pCur->iRowid++;
    return SQLITE_OK;
  }
  return rc; /* Anything else means an error, just return the error */
}

/*
** Purpose of this method is to determine if we are at EOF.
** We use NULL statement handle as a marker for EOF.
*/
static int sqlexecEof(sqlite3_vtab_cursor *cur){
  sqlexec_cursor *pCur = (sqlexec_cursor*)cur;
  return pCur->pStmt == NULL;
}

/*
** Return value for a specific column of the current row. We just delegate
** to the underlying statement handle.
*/
static int sqlexecColumn(
  sqlite3_vtab_cursor *cur,
  sqlite3_context *ctx,
  int i
){
  sqlexec_cursor *pCur = (sqlexec_cursor*)cur;
  sqlite3_value *val = sqlite3_column_value(pCur->pStmt, i);
  sqlite3_result_value(ctx, val);
  return SQLITE_OK;
}

/*
** Return row identifier for current row. We just use an incrementing counter
** stored in the cursor.
*/
static int sqlexecRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  sqlexec_cursor *pCur = (sqlexec_cursor*)cur;
  *pRowid = pCur->iRowid;
  return SQLITE_OK;
}

/*
** Sqlite calls this to find the best index to use. However, since we don't
** support any indices, just return a dummy result.
*/
static int sqlexecBestIndex(
  sqlite3_vtab *tab,
  sqlite3_index_info *pIdxInfo
){
  pIdxInfo->estimatedCost = (double)2147483647;
  pIdxInfo->estimatedRows = 2147483647;
  pIdxInfo->idxNum = 0;
  return SQLITE_OK;
}

/*
** Sqlite calls this to filter the data with a given index. But, we don't
** implement any indices, so we just provide a dummy implementation here.
*/
static int sqlexecFilter(
  sqlite3_vtab_cursor *pVtabCursor,
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  sqlexec_cursor *pCur = (sqlexec_cursor *)pVtabCursor;
  if( idxNum ){
    return SQLITE_INTERNAL;
  }
  return sqlexecNext(pVtabCursor);
}

/*
** Declare interface for sqlexec module.
*/
static sqlite3_module sqlexecModule = {
  0,                      /* iVersion */
  sqlexecConnect,         /* xCreate */
  sqlexecConnect,         /* xConnect */
  sqlexecBestIndex,       /* xBestIndex */
  sqlexecDisconnect,      /* xDisconnect */
  sqlexecDisconnect,      /* xDestroy */
  sqlexecOpen,            /* xOpen - open a cursor */
  sqlexecClose,           /* xClose - close a cursor */
  sqlexecFilter,          /* xFilter - configure scan constraints */
  sqlexecNext,            /* xNext - advance a cursor */
  sqlexecEof,             /* xEof - check for end of scan */
  sqlexecColumn,          /* xColumn - read data */
  sqlexecRowid,           /* xRowid - read data */
  0,                      /* xUpdate */
  0,                      /* xBegin */
  0,                      /* xSync */
  0,                      /* xCommit */
  0,                      /* xRollback */
  0,                      /* xFindMethod */
  0,                      /* xRename */
};

/*
** Called when our extension is loaded. We just declare our virtual table
** module.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_sqlexec_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  rc = sqlite3_create_module(db, "sqlexec", &sqlexecModule, 0);
  if (rc != SQLITE_OK) {
      if (pzErrMsg)
        *pzErrMsg = sqlite3_mprintf("%s", "Error declaring module - maybe you are loading this extension twice?");
    return rc;
  }
  return rc;
}

/* vim: tabstop=8 expandtab shiftwidth=2 softtabstop=2 cc=80 */
