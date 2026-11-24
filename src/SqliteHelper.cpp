#include "net_sdk/SqliteHelper.h"
#include "net_sdk/Logger.h"
#include "sqlite3.h"

namespace net_sdk {

SqliteHelper::SqliteHelper() {}

SqliteHelper::~SqliteHelper() {
    close();
}

bool SqliteHelper::open(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        NET_LOG_WARN("SqliteHelper", "Database already open, closing first");
        sqlite3_close(db_);
        db_ = nullptr;
    }
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        NET_LOG_ERROR("SqliteHelper", "Failed to open database: " + lastError_);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    NET_LOG_INFO("SqliteHelper", "Database opened: " + dbPath);
    return true;
}

void SqliteHelper::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        NET_LOG_INFO("SqliteHelper", "Database closed");
    }
}

bool SqliteHelper::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ != nullptr;
}

SqliteResult SqliteHelper::execute(const std::string& sql) {
    return execute(sql, {});
}

SqliteResult SqliteHelper::execute(const std::string& sql, const std::vector<std::string>& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    SqliteResult result;
    if (!db_) {
        result.errorMessage = "Database not open";
        NET_LOG_ERROR("SqliteHelper", result.errorMessage);
        return result;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        result.errorMessage = sqlite3_errmsg(db_);
        lastError_ = result.errorMessage;
        NET_LOG_ERROR("SqliteHelper", "Prepare failed: " + result.errorMessage + " SQL: " + sql);
        return result;
    }

    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        result.ok = true;
        result.affectedRows = sqlite3_changes(db_);
        result.lastInsertRowId = static_cast<int>(sqlite3_last_insert_rowid(db_));
        affectedRows_ = result.affectedRows;
        lastInsertRowId_ = result.lastInsertRowId;
    } else if (rc == SQLITE_ROW) {
        result.ok = true;
        int colCount = sqlite3_column_count(stmt);
        while (rc == SQLITE_ROW) {
            std::map<std::string, std::string> row;
            for (int i = 0; i < colCount; ++i) {
                const char* colName = sqlite3_column_name(stmt, i);
                const unsigned char* colValue = sqlite3_column_text(stmt, i);
                row[colName] = colValue ? reinterpret_cast<const char*>(colValue) : "";
            }
            result.rows.push_back(row);
            rc = sqlite3_step(stmt);
        }
        result.affectedRows = static_cast<int>(result.rows.size());
    } else {
        result.errorMessage = sqlite3_errmsg(db_);
        lastError_ = result.errorMessage;
        NET_LOG_ERROR("SqliteHelper", "Execute failed: " + result.errorMessage);
    }

    sqlite3_finalize(stmt);
    return result;
}

SqliteResult SqliteHelper::query(const std::string& sql) {
    return execute(sql, {});
}

SqliteResult SqliteHelper::query(const std::string& sql, const std::vector<std::string>& params) {
    return execute(sql, params);
}

void SqliteHelper::queryWithCallback(const std::string& sql, SqliteRowCallback callback) {
    queryWithCallback(sql, {}, callback);
}

void SqliteHelper::queryWithCallback(const std::string& sql, const std::vector<std::string>& params, SqliteRowCallback callback) {
    auto result = execute(sql, params);
    for (const auto& row : result.rows) {
        if (!callback(row)) break;
    }
}

bool SqliteHelper::beginTransaction() {
    auto result = execute("BEGIN TRANSACTION");
    return result.ok;
}

bool SqliteHelper::commitTransaction() {
    auto result = execute("COMMIT");
    return result.ok;
}

bool SqliteHelper::rollbackTransaction() {
    auto result = execute("ROLLBACK");
    return result.ok;
}

bool SqliteHelper::tableExists(const std::string& tableName) {
    auto result = execute("SELECT name FROM sqlite_master WHERE type='table' AND name='" + tableName + "'");
    return result.ok && !result.rows.empty();
}

bool SqliteHelper::createTable(const std::string& tableName, const std::string& columnsDef) {
    auto result = execute("CREATE TABLE IF NOT EXISTS " + tableName + " (" + columnsDef + ")");
    return result.ok;
}

SqliteResult SqliteHelper::insert(const std::string& tableName, const std::map<std::string, std::string>& values) {
    std::string sql = "INSERT INTO " + tableName + " (";
    std::string placeholders = ") VALUES (";
    std::vector<std::string> params;
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            sql += ", ";
            placeholders += ", ";
        }
        sql += key;
        placeholders += "?";
        params.push_back(value);
        first = false;
    }
    sql += placeholders + ")";
    return execute(sql, params);
}

SqliteResult SqliteHelper::update(const std::string& tableName, const std::map<std::string, std::string>& values, const std::string& whereClause) {
    std::string sql = "UPDATE " + tableName + " SET ";
    std::vector<std::string> params;
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) sql += ", ";
        sql += key + "=?";
        params.push_back(value);
        first = false;
    }
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    return execute(sql, params);
}

SqliteResult SqliteHelper::remove(const std::string& tableName, const std::string& whereClause) {
    std::string sql = "DELETE FROM " + tableName;
    if (!whereClause.empty()) {
        sql += " WHERE " + whereClause;
    }
    return execute(sql);
}

int SqliteHelper::getLastInsertRowId() const {
    return lastInsertRowId_;
}

int SqliteHelper::getAffectedRows() const {
    return affectedRows_;
}

std::string SqliteHelper::getLastError() const {
    return lastError_;
}

} // namespace net_sdk