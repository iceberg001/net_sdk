// JSON 工具：解析、序列化、安全取值、结构体映射。
#include "test_util.h"
#include <net_sdk/JsonHelper.h>

struct Person {
    std::string name;
    int age = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Person, name, age)
};

int main() {
    using namespace net_sdk;
    bool ok = false;
    Json j = JsonHelper::parse(R"({"name":"alice","age":30,"nil":null})", &ok);
    testutil::check(ok, "json: parse ok");
    testutil::eq(JsonHelper::get<std::string>(j, "name", ""), "alice", "json: get string");
    testutil::check(JsonHelper::get<int>(j, "age", 0) == 30, "json: get int");
    testutil::eq(JsonHelper::get<std::string>(j, "missing", "dft"), "dft", "json: get default");
    testutil::check(JsonHelper::isNull(j, "nil"), "json: isNull");
    testutil::check(JsonHelper::hasKey(j, "name"), "json: hasKey");

    Json built = JsonHelper::build("k", 42);
    testutil::check(JsonHelper::get<int>(built, "k", 0) == 42, "json: build");

    Person p = JsonHelper::fromJson<Person>(JsonHelper::parse(R"({"name":"bob","age":20})"));
    testutil::eq(p.name, "bob", "json: fromJson name");
    testutil::check(p.age == 20, "json: fromJson age");
    std::string s = JsonHelper::toJsonString(p);
    testutil::check(s.find("bob") != std::string::npos, "json: toJsonString");

    return testutil::finish("json");
}
