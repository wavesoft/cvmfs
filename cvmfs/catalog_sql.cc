/**
 * This file is part of the CernVM file system.
 */

#include "catalog_sql.h"

#include <fcntl.h>
#include <errno.h>

#include <cstdlib>
#include <cstring>
#include <cstdlib>

#include "platform.h"
#include "catalog.h"
#include "logging.h"
#include "util.h"

using namespace std;  // NOLINT

namespace catalog {

Database::Database(const std::string filename, const OpenMode open_mode) {
  int retval;

  filename_ = filename;
  ready_ = false;
  schema_version_ = 0.0;
  sqlite_db_ = NULL;

  int flags = SQLITE_OPEN_NOMUTEX;
  switch (open_mode) {
    case kOpenReadOnly:
      flags |= SQLITE_OPEN_READONLY;
      read_write_ = false;
      break;
    case kOpenReadWrite:
      flags |= SQLITE_OPEN_READWRITE;
      read_write_ = true;
      break;
    default:
      abort();
  }

  // Open database file (depending on the flags read-only or read-write)
  LogCvmfs(kLogCatalog, kLogDebug, "opening database file %s",
           filename_.c_str());
  if (SQLITE_OK != sqlite3_open_v2(filename_.c_str(), &sqlite_db_, flags, NULL))
  {
    LogCvmfs(kLogCatalog, kLogDebug, "cannot open catalog database file %s",
             filename_.c_str());
    return;
  }
  sqlite3_extended_result_codes(sqlite_db_, 1);

  // Read-ahead into file system buffers
  int fd_readahead = open(filename_.c_str(), O_RDONLY);
  if (fd_readahead < 0) {
    LogCvmfs(kLogCatalog, kLogDebug, "failed to open %s for read-ahead (%d)",
             filename_.c_str(), errno);
    goto database_failure;
    return;
  }
  retval = platform_readahead(fd_readahead);
  if (retval != 0) {
    LogCvmfs(kLogCatalog, kLogDebug, "failed to read-ahead %s (%d)",
             filename_.c_str(), errno);
    close(fd_readahead);
    goto database_failure;
  }
  close(fd_readahead);

  {  // Get schema version
    Sql sql_schema(sqlite_db_,
                   "SELECT value FROM properties WHERE key='schema';");
    if (sql_schema.FetchRow()) {
      schema_version_ = sql_schema.RetrieveDouble(0);
    } else {
      schema_version_ = 1.0;
    }
  }

  ready_ = true;
  return;

 database_failure:
  sqlite3_close(sqlite_db_);
  sqlite_db_ = NULL;
}


Database::~Database() {
  if (sqlite_db_)
    sqlite3_close(sqlite_db_);
}


/**
 * This method creates a new database file and initializes the database schema.
 */
bool Database::Create(const string &filename,
                      const DirectoryEntry &root_entry,
                      const string &root_path)
{
  sqlite3 *sqlite_db;
  SqlDirentInsert *sql_insert;
  int open_flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE |
                   SQLITE_OPEN_CREATE;

  // Path hashes
  hash::Md5 root_path_hash = hash::Md5(hash::AsciiPtr(root_path));
  hash::Md5 root_parent_hash;
  if (root_path == "")
    root_parent_hash = hash::Md5();
  else
    root_parent_hash = hash::Md5(hash::AsciiPtr(GetParentPath(root_path)));

  // Create the new catalog file and open it
  LogCvmfs(kLogCatalog, kLogVerboseMsg, "creating new catalog at '%s'",
           filename.c_str());
  if (sqlite3_open_v2(filename.c_str(), &sqlite_db, open_flags, NULL) !=
      SQLITE_OK)
  {
    LogCvmfs(kLogCatalog, kLogStderr,
             "Cannot create and open catalog database file '%s'",
             filename.c_str());
    return false;
  }
  sqlite3_extended_result_codes(sqlite_db, 1);

  bool retval;
  string sql;
  retval = Sql(sqlite_db,
    "CREATE TABLE IF NOT EXISTS catalog "
    "(md5path_1 INTEGER, md5path_2 INTEGER, parent_1 INTEGER, parent_2 INTEGER,"
    " inode INTEGER, hash BLOB, size INTEGER, mode INTEGER, mtime INTEGER,"
    " flags INTEGER, name TEXT, symlink TEXT,"
    " CONSTRAINT pk_catalog PRIMARY KEY (md5path_1, md5path_2));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(sqlite_db,
               "CREATE INDEX IF NOT EXISTS idx_catalog_parent "
               "ON catalog (parent_1, parent_2);").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(sqlite_db,
               "CREATE TABLE IF NOT EXISTS properties (key TEXT, value TEXT, "
               "CONSTRAINT pk_properties PRIMARY KEY (key));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(sqlite_db,
               "CREATE TABLE IF NOT EXISTS nested_catalogs (path TEXT, sha1 TEXT, "
               "CONSTRAINT pk_nested_catalogs PRIMARY KEY (path));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(sqlite_db, "INSERT OR IGNORE INTO properties "
               "(key, value) VALUES ('revision', 0);").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(sqlite_db, "INSERT OR REPLACE INTO properties "
               "(key, value) VALUES ('schema', '2.0');").Execute();
  if (!retval)
    goto create_schema_fail;

  // Insert root entry
  sql_insert = new SqlDirentInsert(sqlite_db);
  retval = sql_insert->BindPathHash(root_path_hash) &&
           sql_insert->BindParentPathHash(root_parent_hash) &&
           sql_insert->BindDirent(root_entry) &&
           sql_insert->Execute();
  delete sql_insert;
  if (!retval)
    goto create_schema_fail;

  if (root_path != "") {
    retval = Sql(sqlite_db, "INSERT OR REPLACE INTO properties "
      "(key, value) VALUES ('root_prefix', '" + root_path + "');").Execute();
    if (!retval)
      goto create_schema_fail;
  }

  sqlite3_close(sqlite_db);
  return true;

create_schema_fail:
  sqlite3_close(sqlite_db);
  return false;
}


//------------------------------------------------------------------------------


Sql::Sql(const sqlite3 *database, const std::string &statement) {
  Init(database, statement);
  database_ = (sqlite3 *)database;
}


Sql::~Sql() {
  last_error_code_ = sqlite3_finalize(statement_);

  if (!Successful()) {
    LogCvmfs(kLogSql, kLogDebug,
             "failed to finalize statement - error code: %d", last_error_code_);
  }
  LogCvmfs(kLogSql, kLogDebug, "successfully finalized statement");
}


/**
 * Executes the prepared statement.
 * (this method should be used for modifying statements like DELETE or INSERT)
 * @return true on success otherwise false
 */
bool Sql::Execute() {
  last_error_code_ = sqlite3_step(statement_);
  return Successful();
}


/**
 * Execute the prepared statement or fetch its next row.
 * This method is intended to step through the result set.
 * If it returns false this does not neccessarily mean, that the actual
 * statement execution failed, but that no row was fetched.
 * @return true if a new row was fetched otherwise false
 */
bool Sql::FetchRow() {
  last_error_code_ = sqlite3_step(statement_);
  return SQLITE_ROW == last_error_code_;
}


/**
 * Reset a prepared statement to make it reusable.
 * @return true on success otherwise false
 */
bool Sql::Reset() {
  last_error_code_ = sqlite3_reset(statement_);
  return Successful();
}


bool Sql::Init(const sqlite3 *database, const std::string &statement) {
  last_error_code_ = sqlite3_prepare_v2((sqlite3*)database,
                                        statement.c_str(),
                                        -1, // parse until null termination
                                        &statement_,
                                        NULL);

  if (!Successful()) {
    LogCvmfs(kLogSql, kLogDebug, "failed to prepare statement '%s' (%d: %s)",
             statement.c_str(), GetLastError(),
             sqlite3_errmsg((sqlite3*)database));
    return false;
  }

  LogCvmfs(kLogSql, kLogDebug, "successfully prepared statement '%s'",
           statement.c_str());
  return true;
}


//------------------------------------------------------------------------------


unsigned SqlDirent::CreateDatabaseFlags(const DirectoryEntry &entry) const {
  unsigned int database_flags = 0;

  if (entry.IsNestedCatalogRoot())
    database_flags |= kFlagDirNestedRoot;
  else if (entry.IsNestedCatalogMountpoint())
    database_flags |= kFlagDirNestedMountpoint;

  if (entry.IsDirectory())
    database_flags |= kFlagDir;
  else if (entry.IsLink())
    database_flags |= kFlagFile | kFlagLink;
  else
    database_flags |= kFlagFile;

  return database_flags;
}


/**
 * Expands variant symlinks containing $(VARIABLE) string.  Uses the environment
 * variables of the current process (cvmfs2)
 */
void SqlDirent::ExpandSymlink(LinkString *raw_symlink) const {
  const char *c = raw_symlink->GetChars();
  const char *cEnd = c+raw_symlink->GetLength();
  for (; c <= cEnd; ++c) {
    if (*c == '$')
      goto expand_symlink;
  }
  return;

 expand_symlink:
  LinkString result;
  for (; c <= cEnd; ++c) {
    if ((*c == '$') && (c < cEnd-2) && (*(c+1) == '(')) {
      c += 2;
      const char *rpar = c;
      while (rpar <= cEnd) {
        if (*rpar == ')')
          goto expand_symlink_getenv;
        rpar++;
      }
      // right parenthesis missing
      result.Append("$(", 2);
      continue;

     expand_symlink_getenv:
      const unsigned environ_var_length = rpar-c;
      char environ_var[environ_var_length+1];
      environ_var[environ_var_length] = '\0';
      memcpy(environ_var, c, environ_var_length);
      const char *environ_value = getenv(environ_var);  // Don't free!
      if (environ_value)
        result.Append(environ_value, strlen(environ_value));
      c = rpar+1;
      continue;
    }
    result.Append(c, 1);
  }
  raw_symlink->Assign(result);
  return;
}


//------------------------------------------------------------------------------


bool SqlDirentWrite::BindDirentFields(const int hash_idx,
                                      const int inode_idx,
                                      const int size_idx,
                                      const int mode_idx,
                                      const int mtime_idx,
                                      const int flags_idx,
                                      const int name_idx,
                                      const int symlink_idx,
                                      const DirectoryEntry &entry)
{
  return (
    BindSha1Blob(hash_idx, entry.checksum_) &&
    BindInt64(inode_idx, entry.inode_) && // quirky database layout here ( legacy ;-) )
    BindInt64(size_idx, entry.size_) &&
    BindInt(mode_idx, entry.mode_) &&
    BindInt64(mtime_idx, entry.mtime_) &&
    BindInt(flags_idx, CreateDatabaseFlags(entry)) &&
    BindText(name_idx, entry.name_.GetChars(), entry.name_.GetLength()) &&
    BindText(symlink_idx, entry.symlink_.GetChars(), entry.symlink_.GetLength())
  );
}


//------------------------------------------------------------------------------


string SqlLookup::GetFieldsToSelect() const {
  return "hash, inode, size, mode, mtime, flags, name, symlink, "
      //    0     1      2     3     4      5      6      7
         "md5path_1, md5path_2, parent_1, parent_2, rowid";
      //    8          9           10        11       12
}


hash::Md5 SqlLookup::GetPathHash() const {
  return RetrieveMd5(8, 9);
}


hash::Md5 SqlLookup::GetParentPathHash() const {
  return RetrieveMd5(10, 11);
}


/**
 * This method is a friend of DirectoryEntry.
 */
DirectoryEntry SqlLookup::GetDirent(const Catalog *catalog) const {
  DirectoryEntry result;

  const unsigned database_flags = RetrieveInt(5);
  result.catalog_ = (Catalog*)catalog;
  result.is_nested_catalog_root_ = (database_flags & kFlagDirNestedRoot);
  result.is_nested_catalog_mountpoint_ =
    (database_flags & kFlagDirNestedMountpoint);
  uint64_t hardlinks = RetrieveInt64(1);
  const char *name = reinterpret_cast<const char *>(RetrieveText(6));
  const char *symlink = reinterpret_cast<const char *>(RetrieveText(7));

  // must be set later by a second catalog lookup
  result.parent_inode_ = DirectoryEntry::kInvalidInode;
  result.inode_ = ((Catalog*)catalog)->GetMangledInode(RetrieveInt64(12),
                  (catalog->schema() < 2.0) ?
                    0 : Hardlinks2HardlinkGroup(hardlinks));
  result.linkcount_ = (catalog->schema() < 2.0) ?
                          1 : Hardlinks2Linkcount(hardlinks);
  result.mode_ = RetrieveInt(3);
  result.size_ = RetrieveInt64(2);
  result.mtime_ = RetrieveInt64(4);
  result.checksum_ = RetrieveSha1Blob(0);
  result.name_.Assign(name, strlen(name));
  result.symlink_.Assign(symlink, strlen(symlink));
  ExpandSymlink(&result.symlink_);

  return result;
}


//------------------------------------------------------------------------------


SqlListing::SqlListing(const sqlite3 *database) {
  const string statement =
    "SELECT " + GetFieldsToSelect() + " FROM catalog "
    "WHERE (parent_1 = :p_1) AND (parent_2 = :p_2);";
  Init(database, statement);
}


bool SqlListing::BindPathHash(const struct hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


//------------------------------------------------------------------------------


SqlLookupPathHash::SqlLookupPathHash(const sqlite3 *database) {
  const string statement =
    "SELECT " + GetFieldsToSelect() + " FROM catalog "
    "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);";
  Init(database, statement);
}

bool SqlLookupPathHash::BindPathHash(const struct hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


//------------------------------------------------------------------------------


SqlLookupInode::SqlLookupInode(const sqlite3 *database) {
  const string statement =
    "SELECT " + GetFieldsToSelect() + " FROM catalog WHERE rowid = :rowid;";
  Init(database, statement);
}


bool SqlLookupInode::BindRowId(const uint64_t inode) {
  return BindInt64(1, inode);
}


//------------------------------------------------------------------------------


SqlNestedCatalogLookup::SqlNestedCatalogLookup(const sqlite3 *database) {
  Init(database, "SELECT sha1 FROM nested_catalogs WHERE path=:path;");
}


bool SqlNestedCatalogLookup::BindSearchPath(const PathString &path) {
  return BindText(1, path.GetChars(), path.GetLength());
}


hash::Any SqlNestedCatalogLookup::GetContentHash() const {
  const string sha1 = string(reinterpret_cast<const char *>(RetrieveText(0)));
  return (sha1.empty()) ? hash::Any(hash::kSha1) :
                          hash::Any(hash::kSha1, hash::HexPtr(sha1));
}


//------------------------------------------------------------------------------


SqlNestedCatalogListing::SqlNestedCatalogListing(const sqlite3 *database) {
  Init(database, "SELECT path, sha1 FROM nested_catalogs;");
}


PathString SqlNestedCatalogListing::GetMountpoint() const {
  const char *mountpoint = reinterpret_cast<const char *>(RetrieveText(0));
  return PathString(mountpoint, strlen(mountpoint));
}


hash::Any SqlNestedCatalogListing::GetContentHash() const {
  const string sha1 = string(reinterpret_cast<const char *>(RetrieveText(1)));
  return (sha1.empty()) ? hash::Any(hash::kSha1) :
                          hash::Any(hash::kSha1, hash::HexPtr(sha1));
}


//------------------------------------------------------------------------------


SqlDirentInsert::SqlDirentInsert(const sqlite3 *database) {
  const string statement =
    "INSERT OR IGNORE INTO catalog "
    "(md5path_1, md5path_2, parent_1, parent_2, hash, inode, size, mode, mtime,"
//       1           2         3         4       5     6      7     8      9
    " flags, name, symlink) "
//      10    11     12
    "VALUES (:md5_1, :md5_2, :p_1, :p_2, :hash, :ino, :size, :mode, :mtime,"
    " :flags, :name, :symlink);";
  Init(database, statement);
}


bool SqlDirentInsert::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


bool SqlDirentInsert::BindParentPathHash(const hash::Md5 &hash) {
  return BindMd5(3, 4, hash);
}


bool SqlDirentInsert::BindDirent(const DirectoryEntry &entry) {
  return BindDirentFields(5, 6, 7, 8, 9, 10, 11, 12, entry);
}


//------------------------------------------------------------------------------


SqlDirentUpdate::SqlDirentUpdate(const sqlite3 *database) {
  const string statement =
    "UPDATE catalog "
    "SET hash = :hash, size = :size, mode = :mode, mtime = :mtime, "
//            1             2             3               4
    "flags = :flags, name = :name, symlink = :symlink, inode = :inode "
//          5             6                  7                8
    "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);";
//                     9                       10
  Init(database, statement);
}


bool SqlDirentUpdate::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(9, 10, hash);
}


bool SqlDirentUpdate::BindDirent(const DirectoryEntry &entry) {
  return BindDirentFields(1, 8, 2, 3, 4, 5, 6, 7, entry);
}


//------------------------------------------------------------------------------


SqlDirentTouch::SqlDirentTouch(const sqlite3 *database) {
  Init(database, "UPDATE catalog SET mtime = :mtime "
                 "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);");
}


bool SqlDirentTouch::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(2, 3, hash);
}


bool SqlDirentTouch::BindTimestamp(const time_t timestamp) {
  return BindInt64(1, timestamp);
}


//------------------------------------------------------------------------------


SqlDirentUnlink::SqlDirentUnlink(const sqlite3 *database) {
  Init(database, "DELETE FROM catalog "
                 "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);");
}

bool SqlDirentUnlink::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


//------------------------------------------------------------------------------


SqlIncLinkcount::SqlIncLinkcount(const sqlite3 *database) {
  const string statememt =
    "UPDATE catalog SET inode="
    "CASE (inode << 32) >> 32 WHEN 2 THEN 0 ELSE inode+1*(:delta) END "
    "WHERE inode = (SELECT inode from catalog WHERE md5path_1 = :md5_1 AND "
    "md5path_2 = :md5_2);";
  Init(database, statememt);
}


bool SqlIncLinkcount::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(2, 3, hash);
}


bool SqlIncLinkcount::BindDelta(const int delta) {
  return BindInt(1, delta);
}


//------------------------------------------------------------------------------


SqlMaxHardlinkGroup::SqlMaxHardlinkGroup(const sqlite3 *database) {
  Init(database, "SELECT max(inode) FROM catalog;");
}

uint32_t SqlMaxHardlinkGroup::GetMaxGroupId() const {
  return RetrieveInt64(0) >> 32;
}

} // namespace catalog