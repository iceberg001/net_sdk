#include "net_sdk/NetSdk.h"
#include <iostream>

int main() {
    auto& sdk = net_sdk::NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(net_sdk::LogLevel::Debug);
    sdk.enableFileLog(true, "app");  // 生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为 app1.log、app2.log ...

    std::cout << "=== SQLite Example ===" << std::endl;

    net_sdk::SqliteHelper db;

    if (!db.open("test.db")) {
        std::cout << "Failed to open database" << std::endl;
        sdk.shutdown();
        return 1;
    }

    std::cout << "Database opened successfully" << std::endl;

    db.createTable("users", "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, age INTEGER, email TEXT");

    auto insertResult = db.insert("users", {
        {"name", "Zhang San"},
        {"age", "28"},
        {"email", "zhangsan@example.com"}
    });
    std::cout << "Insert: ok=" << insertResult.ok
              << " lastInsertRowId=" << insertResult.lastInsertRowId << std::endl;

    db.insert("users", {
        {"name", "Li Si"},
        {"age", "32"},
        {"email", "lisi@example.com"}
    });

    db.insert("users", {
        {"name", "Wang Wu"},
        {"age", "25"},
        {"email", "wangwu@example.com"}
    });

    std::cout << "\n--- Query all users ---" << std::endl;
    auto queryResult = db.query("SELECT * FROM users");
    for (const auto& row : queryResult.rows) {
        std::cout << "id=" << row.at("id")
                  << " name=" << row.at("name")
                  << " age=" << row.at("age")
                  << " email=" << row.at("email") << std::endl;
    }

    std::cout << "\n--- Query with condition ---" << std::endl;
    auto filteredResult = db.query("SELECT * FROM users WHERE age > 26");
    std::cout << "Users with age > 26: " << filteredResult.rows.size() << std::endl;
    for (const auto& row : filteredResult.rows) {
        std::cout << "  " << row.at("name") << " (age=" << row.at("age") << ")" << std::endl;
    }

    std::cout << "\n--- Update ---" << std::endl;
    db.update("users", {{"age", "30"}}, "name='Zhang San'");
    auto updateCheck = db.query("SELECT * FROM users WHERE name='Zhang San'");
    if (!updateCheck.rows.empty()) {
        std::cout << "Updated: Zhang San age=" << updateCheck.rows[0].at("age") << std::endl;
    }

    std::cout << "\n--- Delete ---" << std::endl;
    db.remove("users", "name='Wang Wu'");
    auto deleteCheck = db.query("SELECT COUNT(*) as cnt FROM users");
    if (!deleteCheck.rows.empty()) {
        std::cout << "Remaining users: " << deleteCheck.rows[0].at("cnt") << std::endl;
    }

    std::cout << "\n--- Transaction ---" << std::endl;
    db.beginTransaction();
    db.insert("users", {{"name", "Transaction User"}, {"age", "20"}, {"email", "trans@example.com"}});
    db.rollbackTransaction();
    auto transCheck = db.query("SELECT COUNT(*) as cnt FROM users");
    std::cout << "After rollback: " << transCheck.rows[0].at("cnt") << " users" << std::endl;

    db.close();
    sdk.shutdown();
    return 0;
}