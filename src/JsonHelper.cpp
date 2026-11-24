#include "net_sdk/JsonHelper.h"
#include "net_sdk/Logger.h"

namespace net_sdk {

Json JsonHelper::parse(const std::string& jsonStr, bool* ok) {
    try {
        Json j = Json::parse(jsonStr);
        if (ok) *ok = true;
        return j;
    } catch (const std::exception& e) {
        if (ok) *ok = false;
        NET_LOG_ERROR("JsonHelper", std::string("Parse error: ") + e.what());
        return Json();
    }
}

std::string JsonHelper::stringify(const Json& json, bool indent) {
    if (indent) {
        return json.dump(4);
    }
    return json.dump();
}

std::string JsonHelper::dump(const Json& json, int indent) {
    return json.dump(indent);
}

bool JsonHelper::hasKey(const Json& json, const std::string& key) {
    return json.contains(key);
}

bool JsonHelper::isNull(const Json& json, const std::string& key) {
    return json.contains(key) && json[key].is_null();
}

Json JsonHelper::merge(const Json& a, const Json& b) {
    Json result = a;
    if (b.is_object()) {
        for (auto it = b.begin(); it != b.end(); ++it) {
            result[it.key()] = it.value();
        }
    }
    return result;
}

Json JsonHelper::extract(const Json& json, const std::vector<std::string>& keys) {
    Json result;
    for (const auto& key : keys) {
        if (json.contains(key)) {
            result[key] = json[key];
        }
    }
    return result;
}

} // namespace net_sdk