// 日志系统：命名规则（YYYY-MM-DD_app.log）、超阈值轮转为 app1/app2、内容写入。
#include "test_util.h"
#include <net_sdk/Logger.h>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

int main() {
    using namespace net_sdk;
    auto& L = Logger::getInstance();
    L.setConsoleOutput(false);
    L.setLevel(LogLevel::Info);

    fs::path dir = fs::temp_directory_path() / "netsdk_test_logger";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string base = (dir / "app").string();

    L.setMaxFileSize(512);
    L.setMaxFileCount(3);
    L.setFileOutput(true, base);

    for (int i = 0; i < 300; ++i) {
        L.info("T", "line " + std::to_string(i) + " padding-padding-padding-padding");
    }
    L.setFileOutput(false);

    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".log") names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());

    testutil::check(!names.empty(), "logger: log files created");
    bool allPrefixed = true;
    bool hasIndex = false;
    for (auto& n : names) {
        // 名称形如 YYYY-MM-DD_app.log 或 YYYY-MM-DD_appN.log
        if (n.find("_app") == std::string::npos) allPrefixed = false;
        // 去掉 .log 后，若最后一个字符是数字则说明是轮转产生的 appN 文件
        std::string stem = n.substr(0, n.size() - 4);
        if (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') hasIndex = true;
    }
    testutil::check(allPrefixed, "logger: date_app naming");
    testutil::check(hasIndex, "logger: rotation produced appN files");
    testutil::check(names.size() <= 3, "logger: maxFileCount respected");

    // 首个文件不带序号的命名（单独一次、足够小的写入量）
    fs::path dir2 = fs::temp_directory_path() / "netsdk_test_logger2";
    fs::remove_all(dir2);
    fs::create_directories(dir2);
    L.setFileOutput(true, (dir2 / "app").string());
    L.info("T", "first file line");
    L.setFileOutput(false);
    bool foundBase = false;
    for (auto& e : fs::directory_iterator(dir2)) {
        auto n = e.path().filename().string();
        if (n.size() > 4 && n.substr(n.size() - 8) == "_app.log") foundBase = true;
    }
    testutil::check(foundBase, "logger: base file YYYY-MM-DD_app.log");

    // --- 面向外部工程的便捷 API：configureLogging / 流式 / printf 风格 ---
    fs::path dir3 = fs::temp_directory_path() / "netsdk_test_logger_api";
    fs::remove_all(dir3);
    fs::create_directories(dir3);
    configureLogging(LogLevel::Debug, false, (dir3 / "api").string());
    NET_LOG_I("Api") << "stream=" << 7 << ", ok=" << true;
    NET_LOG_INFOF("Api", "printf=%d/%.2f", 9, 1.5);
    NET_LOG_D("Api") << "debug line";
    Logger::getInstance().setFileOutput(false);

    std::string all;
    for (auto& e : fs::directory_iterator(dir3)) {
        std::ifstream in(e.path());
        std::stringstream ss;
        ss << in.rdbuf();
        all += ss.str();
    }
    testutil::check(all.find("stream=7, ok=1") != std::string::npos,
                    "logger: stream API writes formatted line");
    testutil::check(all.find("printf=9/1.50") != std::string::npos,
                    "logger: printf API writes formatted line");
    testutil::check(all.find("debug line") != std::string::npos,
                    "logger: configureLogging enables Debug level");
    fs::remove_all(dir3);

    fs::remove_all(dir);
    fs::remove_all(dir2);
    return testutil::finish("logger");
}
