#include "net_sdk/NetSdk.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

struct UserInfo {
    std::string userId;
    std::string name;
    int age = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(UserInfo, userId, name, age)
};

struct LoginResult {
    std::string token;
    std::string userId;
    int expireTime = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LoginResult, token, userId, expireTime)
};

int main() {
    auto& sdk = net_sdk::NetSdk::getInstance();
    sdk.init();
    sdk.setLogLevel(net_sdk::LogLevel::Debug);
    sdk.enableFileLog(true, "app");  // 生成 YYYY-MM-DD_app.log，超过 5MB 依次轮转为 app1.log、app2.log ...

    std::cout << "=== Command Request Example ===" << std::endl;

    auto& cmdReq = sdk.getCommandRequest();

    const int CMD_LOGIN = 1001;
    const int CMD_GET_USER_INFO = 1002;
    const int CMD_LOGOUT = 1003;

    cmdReq.setRequestHandler(CMD_LOGIN, [](const net_sdk::CommandConfig& config) {
        net_sdk::CommandResult result;
        result.code = 0;
        result.message = "Login success";

        net_sdk::Json resp;
        resp["token"] = "token_abc123";
        resp["userId"] = "user_001";
        resp["expireTime"] = 3600;
        result.rawData = resp.dump();

        return result;
    });

    cmdReq.setRequestHandler(CMD_GET_USER_INFO, [](const net_sdk::CommandConfig& config) {
        net_sdk::CommandResult result;
        result.code = 0;
        result.message = "OK";

        net_sdk::Json resp;
        resp["userId"] = "user_001";
        resp["name"] = "Zhang San";
        resp["age"] = 28;
        result.rawData = resp.dump();

        return result;
    });

    cmdReq.setRequestHandler(CMD_LOGOUT, [](const net_sdk::CommandConfig& config) {
        net_sdk::CommandResult result;
        result.code = 0;
        result.message = "Logout success";
        return result;
    });

    std::mutex mtx;
    std::condition_variable cv;
    int completedCount = 0;

    std::cout << "\n--- Sending login command (async, callback on main thread) ---" << std::endl;
    cmdReq.sendAndParse<LoginResult>(CMD_LOGIN, "login",
        net_sdk::Json{{"username", "admin"}, {"password", "123456"}},
        [&](const LoginResult& loginData) {
            std::cout << "Login success! Token=" << loginData.token
                      << " UserId=" << loginData.userId
                      << " Expire=" << loginData.expireTime << std::endl;

            cmdReq.sendAndParse<UserInfo>(CMD_GET_USER_INFO, "getUserInfo",
                net_sdk::Json{{"userId", loginData.userId}},
                [&](const UserInfo& userInfo) {
                    std::cout << "User Info: userId=" << userInfo.userId
                              << " name=" << userInfo.name
                              << " age=" << userInfo.age << std::endl;

                    cmdReq.send(CMD_LOGOUT, "logout", [&](const net_sdk::CommandResult& result) {
                        std::cout << "Logout: " << result.message << std::endl;
                        std::lock_guard<std::mutex> lock(mtx);
                        completedCount++;
                        cv.notify_one();
                    });
                },
                [](net_sdk::NetErrorCode code, const std::string& msg) {
                    std::cout << "GetUserInfo failed: " << msg << std::endl;
                });
        },
        [](net_sdk::NetErrorCode code, const std::string& msg) {
            std::cout << "Login failed: " << msg << std::endl;
        });

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return completedCount >= 1; });
    }

    std::cout << "\n--- Direct send with raw callback ---" << std::endl;
    cmdReq.send(CMD_LOGIN, "login", [](const net_sdk::CommandResult& result) {
        std::cout << "Direct callback - code=" << result.code
                  << " msg=" << result.message
                  << " data=" << result.rawData << std::endl;
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "\nPending count: " << cmdReq.getPendingCount() << std::endl;

    sdk.shutdown();
    return 0;
}