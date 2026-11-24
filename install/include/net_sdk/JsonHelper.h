/**
 * @file JsonHelper.h
 * @brief NetSDK JSON 工具类
 *
 * 基于 nlohmann/json（libhv 内置）封装的 JSON 操作工具，
 * 提供解析、序列化、字段获取、结构体映射等便捷方法。
 *
 * 典型用法：
 * @code
 *   Json j = JsonHelper::parse(R"({"name":"test","age":18})");
 *   std::string name = JsonHelper::get<std::string>(j, "name", "default");
 *   int age = JsonHelper::get<int>(j, "age", 0);
 * @endcode
 *
 * @author NetSDK Team
 * @version 1.0.0
 * @date 2026-11-18
 */

#pragma once

#include "json.hpp"  // 随 SDK 一起发布的 nlohmann/json 单头文件（自包含）
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace net_sdk {

/// nlohmann/json 的别名
using Json = nlohmann::json;

/**
 * @brief JSON 操作工具类
 *
 * 提供静态方法，无需实例化即可使用。所有方法均为线程安全（nlohmann/json 本身线程安全）。
 */
class JsonHelper {
public:
    /**
     * @brief 解析 JSON 字符串
     * @param jsonStr JSON 格式字符串
     * @param ok 解析是否成功的输出参数（可选）
     * @return 解析后的 Json 对象，失败时返回空 Json
     */
    static Json parse(const std::string& jsonStr, bool* ok = nullptr);

    /**
     * @brief 序列化 Json 为字符串
     * @param json Json 对象
     * @param indent 是否缩进格式化
     * @return JSON 字符串
     */
    static std::string stringify(const Json& json, bool indent = false);

    /**
     * @brief 格式化输出 Json（带缩进）
     * @param json Json 对象
     * @param indent 缩进空格数
     * @return 格式化的 JSON 字符串
     */
    static std::string dump(const Json& json, int indent = 4);

    /**
     * @brief 安全获取字段值（带默认值）
     * @tparam T 值类型
     * @param json Json 对象
     * @param key 字段名
     * @param defaultValue 默认值（字段不存在或类型不匹配时返回）
     * @return 字段值或默认值
     */
    template<typename T>
    static T get(const Json& json, const std::string& key, const T& defaultValue = T{}) {
        if (json.contains(key) && !json[key].is_null()) {
            try {
                return json[key].get<T>();
            } catch (...) {
                return defaultValue;
            }
        }
        return defaultValue;
    }

    /**
     * @brief 安全获取字段值（返回 optional）
     * @tparam T 值类型
     * @param json Json 对象
     * @param key 字段名
     * @return 字段值（存在时）或 std::nullopt（不存在或类型不匹配）
     */
    template<typename T>
    static std::optional<T> getOptional(const Json& json, const std::string& key) {
        if (json.contains(key) && !json[key].is_null()) {
            try {
                return json[key].get<T>();
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 安全获取字段值到输出参数
     * @tparam T 值类型
     * @param json Json 对象
     * @param key 字段名
     * @param out 输出参数
     * @return 是否成功获取
     */
    template<typename T>
    static bool getTo(const Json& json, const std::string& key, T& out) {
        if (json.contains(key) && !json[key].is_null()) {
            try {
                json[key].get_to(out);
                return true;
            } catch (...) {
                return false;
            }
        }
        return false;
    }

    /// 检查 Json 中是否存在指定字段
    static bool hasKey(const Json& json, const std::string& key);

    /// 检查 Json 中指定字段是否为 null
    static bool isNull(const Json& json, const std::string& key);

    /**
     * @brief 构建 Json 对象（支持多键值对）
     * @code
     *   Json j = JsonHelper::build("name", "test", "age", 18);
     * @endcode
     */
    template<typename T>
    static Json build(const std::string& key, const T& value) {
        Json j;
        j[key] = value;
        return j;
    }

    template<typename T, typename... Args>
    static Json build(const std::string& key, const T& value, Args&&... args) {
        Json j = build(std::forward<Args>(args)...);
        j[key] = value;
        return j;
    }

    /// 合并两个 Json 对象（b 的键值对覆盖 a 中同名的）
    static Json merge(const Json& a, const Json& b);

    /// 从 Json 中提取指定键列表的子集
    static Json extract(const Json& json, const std::vector<std::string>& keys);

    /**
     * @brief 从 Json 反序列化到结构体
     * @tparam T 目标结构体类型（需定义 NLOHMANN_DEFINE_TYPE_INTRUSIVE）
     * @param json Json 对象
     * @return 反序列化后的结构体
     */
    template<typename T>
    static T fromJson(const Json& json) {
        T obj;
        fromJson(json, obj);
        return obj;
    }

    /**
     * @brief 从 Json 反序列化到结构体（输出参数版本）
     * @tparam T 目标结构体类型
     * @param json Json 对象
     * @param obj 输出结构体
     */
    template<typename T>
    static void fromJson(const Json& json, T& obj) {
        obj = json.get<T>();
    }

    /**
     * @brief 序列化结构体到 Json
     * @tparam T 源结构体类型（需定义 NLOHMANN_DEFINE_TYPE_INTRUSIVE）
     * @param obj 结构体对象
     * @return Json 对象
     */
    template<typename T>
    static Json toJson(const T& obj) {
        Json j = obj;
        return j;
    }

    /**
     * @brief 序列化结构体到 JSON 字符串
     * @tparam T 源结构体类型
     * @param obj 结构体对象
     * @return JSON 字符串
     */
    template<typename T>
    static std::string toJsonString(const T& obj) {
        return toJson(obj).dump();
    }
};

} // namespace net_sdk