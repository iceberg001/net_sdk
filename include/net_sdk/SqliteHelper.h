/**
 * @file SqliteHelper.h
 * @brief NetSDK SQLite 数据库封装
 *
 * 基于 SQLite3 提供线程安全的数据库操作封装，支持连接管理、SQL 执行、
 * 参数化查询、事务、CRUD 操作等。适用于本地数据缓存、配置存储等场景。
 *
 * 典型用法：
 * @code
 *   SqliteHelper db;
 *   db.open("mydata.db");
 *   db.createTable("users", "id INTEGER PRIMARY KEY, name TEXT, age INTEGER");
 *   db.insert("users", {{"name", "Zhang San"}, {"age", "28"}});
 *   auto result = db.query("SELECT * FROM users");
 *   for (const auto& row : result.rows) {
 *       std::cout << row.at("name") << std::endl;
 *   }
 *   db.close();
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>
#include <memory>

struct sqlite3;

namespace net_sdk {

/**
 * @brief SQLite 操作结果结构体
 *
 * 封装 SQL 执行或查询的返回结果，包含成功状态、错误信息、影响行数、查询数据等。
 */
struct SqliteResult {
    bool ok = false;                                    ///< 是否成功
    std::string errorMessage;                           ///< 错误信息（失败时）
    int affectedRows = 0;                               ///< 影响的行数
    int lastInsertRowId = 0;                            ///< 最后插入的自增 ID
    std::vector<std::map<std::string, std::string>> rows; ///< 查询结果行集合

    operator bool() const { return ok; }                ///< 隐式转换为 bool
};

/// 查询行回调函数类型（返回 false 停止遍历）
using SqliteRowCallback = std::function<bool(const std::map<std::string, std::string>& row)>;

/**
 * @brief SQLite 数据库操作封装类
 *
 * 线程安全的 SQLite 操作类，所有公开方法均通过互斥锁保护。
 * 支持 WAL 模式、参数化查询、事务等高级特性。
 */
class SqliteHelper {
public:
    SqliteHelper();
    ~SqliteHelper();

    SqliteHelper(const SqliteHelper&) = delete;
    SqliteHelper& operator=(const SqliteHelper&) = delete;

    /**
     * @brief 打开数据库连接
     * @param dbPath 数据库文件路径
     * @return 是否成功打开
     */
    bool open(const std::string& dbPath);

    /// 关闭数据库连接
    void close();

    /// 数据库是否已打开
    bool isOpen() const;

    /**
     * @brief 执行 SQL 语句（无参数）
     * @param sql SQL 语句
     * @return 执行结果
     */
    SqliteResult execute(const std::string& sql);

    /**
     * @brief 执行 SQL 语句（带参数）
     * @param sql SQL 语句（使用 ? 占位符）
     * @param params 参数列表（按顺序对应 ? 占位符）
     * @return 执行结果
     */
    SqliteResult execute(const std::string& sql, const std::vector<std::string>& params);

    /**
     * @brief 查询 SQL 语句（无参数）
     * @param sql SELECT SQL 语句
     * @return 查询结果（包含 rows）
     */
    SqliteResult query(const std::string& sql);

    /**
     * @brief 查询 SQL 语句（带参数）
     * @param sql SELECT SQL 语句（使用 ? 占位符）
     * @param params 参数列表
     * @return 查询结果
     */
    SqliteResult query(const std::string& sql, const std::vector<std::string>& params);

    /**
     * @brief 查询并逐行回调处理
     * @param sql SELECT SQL 语句
     * @param callback 行回调函数（返回 false 停止遍历）
     */
    void queryWithCallback(const std::string& sql, SqliteRowCallback callback);

    /**
     * @brief 查询并逐行回调处理（带参数）
     * @param sql SELECT SQL 语句（使用 ? 占位符）
     * @param params 参数列表
     * @param callback 行回调函数
     */
    void queryWithCallback(const std::string& sql, const std::vector<std::string>& params, SqliteRowCallback callback);

    /// 开始事务
    bool beginTransaction();

    /// 提交事务
    bool commitTransaction();

    /// 回滚事务
    bool rollbackTransaction();

    /// 检查表是否存在
    bool tableExists(const std::string& tableName);

    /**
     * @brief 创建表
     * @param tableName 表名
     * @param columnsDef 列定义（如 "id INTEGER PRIMARY KEY, name TEXT"）
     * @return 是否成功
     */
    bool createTable(const std::string& tableName, const std::string& columnsDef);

    /**
     * @brief 插入数据
     * @param tableName 表名
     * @param values 键值对（列名 -> 值）
     * @return 插入结果（包含 lastInsertRowId）
     */
    SqliteResult insert(const std::string& tableName, const std::map<std::string, std::string>& values);

    /**
     * @brief 更新数据
     * @param tableName 表名
     * @param values 键值对（列名 -> 新值）
     * @param whereClause WHERE 条件（如 "name='Zhang San'"）
     * @return 更新结果
     */
    SqliteResult update(const std::string& tableName, const std::map<std::string, std::string>& values, const std::string& whereClause);

    /**
     * @brief 删除数据
     * @param tableName 表名
     * @param whereClause WHERE 条件
     * @return 删除结果
     */
    SqliteResult remove(const std::string& tableName, const std::string& whereClause);

    /// 获取最后插入的自增 ID
    int getLastInsertRowId() const;

    /// 获取上次操作影响的行数
    int getAffectedRows() const;

    /// 获取上次错误信息
    std::string getLastError() const;

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
    std::string lastError_;
    int affectedRows_ = 0;
    int lastInsertRowId_ = 0;
};

} // namespace net_sdk