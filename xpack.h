/*
 * =====================================================================================
 *                               XPACK - C++ 数据打包库
 * =====================================================================================
 *
 * 功能简介:
 *   一个轻量级的C++头文件库，用于将多种基本数据类型和二进制缓冲区
 *   打包成单一字节流，并能解包回来。支持跨字节序。
 *
 * -------------------------------------------------------------------------------------
 *
 * 核心接口:
 *
 *   1. 数据结构:
 *      - XPackBuff:      核心数据结构，用于持有二进制数据块。它既是输入，也是输出。
 *      - VariantType:    解包后返回的数据类型，是一个 std::variant，可以持有多种类型的值。
 *
 *   2. 主要函数:
 *      - template<typename... Args>
 *        XPackBuff xpack_pack(bool target_big_endian, const Args&... args);
 *        // 将任意数量的参数打包成一个 XPackBuff 对象。
 *
 *      - std::vector<VariantType> xpack_unpack(const char* packed_data, size_t packed_size);
 *        // 从打包好的数据流中解包，返回一个 VariantType 的向量。
 *
 *      - template<typename T>
 *        T xpack_cast(VariantType& var);
 *        // 辅助函数，用于安全地从 VariantType 中提取指定类型的数据。
 *    3. C++ 标准:17
 *
 * -------------------------------------------------------------------------------------
 *
 * 使用示例:
 *
 *   #include "xpack.h"
 *
 *   int main() {
 *       // 1. 准备要打包的数据
 *       int i = 123;
 *       float f = 3.14f;
 *       std::string str = "hello world";
 *       XPackBuff input_buf("binary data", 11);
 *
 *       // 2. 打包 (目标为大端序)，结果直接存储在 XPackBuff 中
 *       XPackBuff packed = xpack_pack(true, i, f, str, input_buf);
 *
 *       // 3. 解包
 *       auto unpacked = xpack_unpack(packed.get(), packed.len);
 *
 *       // 4. 提取并使用数据
 *       auto unpacked_i = xpack_cast<int>(unpacked[0]);
 *       auto unpacked_f = xpack_cast<float>(unpacked[1]);
 *       auto unpacked_str = xpack_cast<std::string>(unpacked[2]);
 *       auto unpacked_buf = xpack_cast<XPackBuff>(unpacked[3]);
 *
 *       // 验证类型和值
 *       std::cout << "unpacked_i: " << unpacked_i << std::endl;
 *       std::cout << "unpacked_f: " << unpacked_f << std::endl;
 *       std::cout << "unpacked_str: " << unpacked_str << std::endl;
 *   }
 *
 * =====================================================================================
 */
 /*
  * =====================================================================================
  *                               XPACK - C++ 数据打包库
  * =====================================================================================
  *
  * 功能简介:
  *   一个轻量级的C++头文件库，用于将多种基本数据类型和二进制缓冲区
  *   打包成单一字节流，并能解包回来。支持跨字节序。
  *
  * -------------------------------------------------------------------------------------
  *
  * 修改说明:
  *   - 将 len 字段从 size_t 改为 int，支持错误码
  *   - 添加 success() 和 error_code() 方法
  *   - 所有内部计算使用 int 类型
  *   - 添加 std::string 类型支持
  *
  * =====================================================================================
  */
#ifndef __XPACK_H__
#define __XPACK_H__

#include <tuple>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <variant>
#include <type_traits>
#include <vector>
#include <typeinfo>
#include <string>
#include <optional>

#include <map>
#include <unordered_set>

  // =====================================================================================
  //                                 公共数据结构定义
  // =====================================================================================
    struct XPackBuff {
    std::unique_ptr<char[]> data;
    int len;

    XPackBuff() : data(nullptr), len(0) {}
    XPackBuff(const char* source_data, int length) : len(length) {
        if (len > 0) {
            data = std::make_unique<char[]>(len);
            if (source_data) std::memcpy(data.get(), source_data, len);
        }
    }
    XPackBuff(const char* cstr) {
        if (!cstr) { len = 0; return; }
        len = static_cast<int>(std::strlen(cstr) + 1);
        if (len > 0) {
            data = std::make_unique<char[]>(len);
            std::memcpy(data.get(), cstr, len);
        }
    }
    XPackBuff(std::unique_ptr<char[]> d, int l) : data(std::move(d)), len(l) {}

    // 移动构造函数
    XPackBuff(XPackBuff&& other) noexcept : data(std::move(other.data)), len(other.len) {
        other.len = 0;
    }

    // 移动赋值运算符
    XPackBuff& operator=(XPackBuff&& other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            len = other.len;
            other.len = 0;
        }
        return *this;
    }

    // 删除拷贝构造函数和拷贝赋值运算符
    XPackBuff(const XPackBuff&) = delete;
    XPackBuff& operator=(const XPackBuff&) = delete;

    const char* get() const { return data.get(); }
    const char* get(int& pack_len) const { pack_len = len; return data.get(); }

    bool success() const { return len >= 0; }
    int error_code() const { return len < 0 ? len : 0; }
};

enum class TypeEnum : uint8_t {
    Char, SignedChar, UnsignedChar,
    Short, UnsignedShort,
    Int, UnsignedInt,
    Long, UnsignedLong,
    LongLong, UnsignedLongLong,
    Float, Double, LongDouble,
    Bool, XPackBuff, String
};

using VariantType = std::variant<
    char, signed char, unsigned char,
    short, unsigned short,
    int, unsigned int,
    long, unsigned long,
    long long, unsigned long long,
    float, double, long double,
    bool, XPackBuff, std::string,
    std::vector<std::string>,
    std::map<std::string, std::string>,
    std::unordered_set<std::string>
>;

// =====================================================================================
//                                 公共函数接口声明
// =====================================================================================
template<typename T>
T xpack_cast(VariantType& var) {
    if (T* val = std::get_if<T>(&var)) return *val;
    throw std::runtime_error("Type mismatch when extracting variant data");
}

template<>
inline XPackBuff xpack_cast<XPackBuff>(VariantType& var) {
    if (XPackBuff* val = std::get_if<XPackBuff>(&var)) return std::move(*val);
    throw std::runtime_error("Type mismatch when extracting XPackBuff from variant");
}

template<>
inline std::string xpack_cast<std::string>(VariantType& var) {
    if (std::string* val = std::get_if<std::string>(&var)) return std::move(*val);
    throw std::runtime_error("Type mismatch when extracting std::string from variant");
}

template<typename T>
std::optional<T> xpack_cast_optional(const std::vector<VariantType>& vec, size_t index) {
    if (index >= vec.size()) {
        return std::nullopt;
    }

    try {
        VariantType& var = const_cast<VariantType&>(vec[index]);
        return xpack_cast<T>(var);
    }
    catch (...) {
        return std::nullopt;
    }
}

template<typename... Args>
XPackBuff xpack_pack(bool target_big_endian, const Args&... args);
std::vector<VariantType> xpack_unpack(const char* packed_data, int packed_size);

// =====================================================================================
//                                 内部实现细节
// =====================================================================================

inline bool is_big_endian() {
    const uint32_t test = 0x01020304;
    return *(reinterpret_cast<const uint8_t*>(&test)) == 0x01;
}

template<typename T>
T endian_swap(T value) {
    static_assert(std::is_arithmetic<T>::value, "Only arithmetic types support endian swap");
    char* data = reinterpret_cast<char*>(&value);
    std::reverse(data, data + sizeof(T));
    return value;
}

template<typename T>
TypeEnum get_type_tag() {
    if (std::is_same<T, char>::value) return TypeEnum::Char;
    else if (std::is_same<T, signed char>::value) return TypeEnum::SignedChar;
    else if (std::is_same<T, unsigned char>::value) return TypeEnum::UnsignedChar;
    else if (std::is_same<T, short>::value) return TypeEnum::Short;
    else if (std::is_same<T, unsigned short>::value) return TypeEnum::UnsignedShort;
    else if (std::is_same<T, int>::value) return TypeEnum::Int;
    else if (std::is_same<T, unsigned int>::value) return TypeEnum::UnsignedInt;
    else if (std::is_same<T, long>::value) return TypeEnum::Long;
    else if (std::is_same<T, unsigned long>::value) return TypeEnum::UnsignedLong;
    else if (std::is_same<T, long long>::value) return TypeEnum::LongLong;
    else if (std::is_same<T, unsigned long long>::value) return TypeEnum::UnsignedLongLong;
    else if (std::is_same<T, float>::value) return TypeEnum::Float;
    else if (std::is_same<T, double>::value) return TypeEnum::Double;
    else if (std::is_same<T, long double>::value) return TypeEnum::LongDouble;
    else if (std::is_same<T, bool>::value) return TypeEnum::Bool;
    else if (std::is_same<T, XPackBuff>::value) return TypeEnum::XPackBuff;
    else if (std::is_same<T, std::string>::value) return TypeEnum::String;
    else throw std::invalid_argument("Unsupported type (get_type_tag)");
}

inline int calculate_element_size(const char* str) {
    if (str) {
        int len = static_cast<int>(std::strlen(str)) + 1;
        return 1 + sizeof(int) + len;  // type_tag + length + data
    }
    else {
        return 1 + sizeof(int);  // type_tag + length(0)
    }
}

inline int calculate_element_size(char* str) {
    return calculate_element_size(const_cast<const char*>(str));
}

inline int calculate_element_size(const XPackBuff& arg) {
    return 1 + sizeof(int) + arg.len;
}

inline int calculate_element_size(const std::string& arg) {
    return 1 + sizeof(int) + static_cast<int>(arg.size());  // type_tag + length + data (不包含结尾空字符)
}

template<typename T>
int calculate_element_size(const T& /*arg*/) {
    return 1 + sizeof(T);
}

template<typename... Args>
int calculate_total_size(const Args&... args) {
    return (calculate_element_size(args) + ...);
}

template<typename T>
void pack_basic(char* buffer, int& offset, bool system_big, bool target_big, const T& value) {
    static_assert(std::is_arithmetic<T>::value, "Only arithmetic types can be packed as basic types");
    TypeEnum tag = get_type_tag<T>();
    buffer[offset++] = static_cast<uint8_t>(tag);
    T data = value;
    if (system_big != target_big) data = endian_swap<T>(data);
    std::memcpy(buffer + offset, &data, sizeof(T));
    offset += sizeof(T);
}

inline void pack_buffer(char* buffer, int& offset, bool system_big, bool target_big, const XPackBuff& buff) {
    buffer[offset++] = static_cast<uint8_t>(TypeEnum::XPackBuff);
    int len = buff.len;
    if (system_big != target_big) len = endian_swap<int>(len);
    std::memcpy(buffer + offset, &len, sizeof(int));
    offset += sizeof(int);
    if (buff.len > 0 && buff.data) {
        std::memcpy(buffer + offset, buff.data.get(), buff.len);
        offset += buff.len;
    }
}

inline void pack_string(char* buffer, int& offset, bool system_big, bool target_big, const char* str) {
    // 转换为 XPackBuff 处理
    if (str) {
        XPackBuff buff(str);  // 使用 XPackBuff 的字符串构造函数
        pack_buffer(buffer, offset, system_big, target_big, buff);
    }
    else {
        // 空字符串
        XPackBuff buff;
        pack_buffer(buffer, offset, system_big, target_big, buff);
    }
}

inline void pack_std_string(char* buffer, int& offset, bool system_big, bool target_big, const std::string& str) {
    buffer[offset++] = static_cast<uint8_t>(TypeEnum::String);
    int len = static_cast<int>(str.size());
    if (system_big != target_big) len = endian_swap<int>(len);
    std::memcpy(buffer + offset, &len, sizeof(int));
    offset += sizeof(int);
    if (len > 0) {
        std::memcpy(buffer + offset, str.data(), len);
        offset += len;
    }
}

inline void pack_data(char* buffer, int& offset, bool system_big, bool target_big, const char* value) {
    pack_string(buffer, offset, system_big, target_big, value);
}

inline void pack_data(char* buffer, int& offset, bool system_big, bool target_big, char* value) {
    pack_string(buffer, offset, system_big, target_big, value);
}

inline void pack_data(char* buffer, int& offset, bool system_big, bool target_big, const XPackBuff& value) {
    pack_buffer(buffer, offset, system_big, target_big, value);
}

inline void pack_data(char* buffer, int& offset, bool system_big, bool target_big, const std::string& value) {
    pack_std_string(buffer, offset, system_big, target_big, value);
}

template<typename T>
typename std::enable_if<!std::is_same<T, XPackBuff>::value
    && !std::is_same<T, std::string>::value
    && !std::is_pointer<T>::value>::type
    pack_data(char* buffer, int& offset, bool system_big, bool target_big, const T& value) {
    pack_basic<T>(buffer, offset, system_big, target_big, value);
}

template<typename T, typename... Args>
void pack_data(char* buffer, int& offset, bool system_big, bool target_big, const T& value, const Args&... args) {
    pack_data(buffer, offset, system_big, target_big, value);
    if (sizeof...(args) > 0) pack_data(buffer, offset, system_big, target_big, args...);
}

template<typename... Args>
XPackBuff xpack_pack(bool target_big_endian, const Args&... args) {
    bool system_big = is_big_endian();
    int data_size = calculate_total_size(args...);
    int total_size = 1 + 4 + data_size;
    auto buffer = std::make_unique<char[]>(total_size);
    int offset = 0;
    buffer[offset++] = target_big_endian ? 1 : 0;
    uint32_t total_data_len = static_cast<uint32_t>(data_size);
    if (system_big != target_big_endian) total_data_len = endian_swap<uint32_t>(total_data_len);
    std::memcpy(buffer.get() + offset, &total_data_len, 4);
    offset += 4;
    pack_data(buffer.get(), offset, system_big, target_big_endian, args...);
    return XPackBuff(std::move(buffer), total_size);
}

template<typename T>
T unpack_basic(const char* buffer, int& offset, bool data_big, int& remaining) {
    static_assert(std::is_arithmetic<T>::value, "Only arithmetic types can be unpacked as basic types");
    if (sizeof(T) > remaining) throw std::runtime_error("Insufficient data for basic type");
    T value;
    std::memcpy(&value, buffer + offset, sizeof(T));
    offset += sizeof(T);
    remaining -= sizeof(T);
    if (data_big != is_big_endian()) value = endian_swap<T>(value);
    return value;
}

inline XPackBuff unpack_buffer(const char* buffer, int& offset, bool data_big, int& remaining) {
    if (sizeof(int) > remaining) throw std::runtime_error("Insufficient data for buffer length");
    int len = 0;
    std::memcpy(&len, buffer + offset, sizeof(int));
    offset += sizeof(int);
    remaining -= sizeof(int);
    if (data_big != is_big_endian()) len = endian_swap<int>(len);
    if (len > remaining) throw std::runtime_error("Invalid buffer length");
    auto data = std::make_unique<char[]>(len);
    if (len > 0) std::memcpy(data.get(), buffer + offset, len);
    offset += len;
    remaining -= len;
    return XPackBuff(std::move(data), len);
}

inline std::string unpack_std_string(const char* buffer, int& offset, bool data_big, int& remaining) {
    if (sizeof(int) > remaining) throw std::runtime_error("Insufficient data for string length");
    int len = 0;
    std::memcpy(&len, buffer + offset, sizeof(int));
    offset += sizeof(int);
    remaining -= sizeof(int);
    if (data_big != is_big_endian()) len = endian_swap<int>(len);
    if (len > remaining) throw std::runtime_error("Invalid string length");
    std::string result;
    if (len > 0) {
        result.assign(buffer + offset, len);
        offset += len;
        remaining -= len;
    }
    return result;
}

inline VariantType unpack_single(const char* buffer, int& offset, bool data_big, int& remaining) {
    if (remaining < 1) throw std::runtime_error("Insufficient data for type tag");
    TypeEnum tag = static_cast<TypeEnum>(static_cast<uint8_t>(buffer[offset++]));
    remaining -= 1;
    switch (tag) {
    case TypeEnum::Char: return unpack_basic<char>(buffer, offset, data_big, remaining);
    case TypeEnum::SignedChar: return unpack_basic<signed char>(buffer, offset, data_big, remaining);
    case TypeEnum::UnsignedChar: return unpack_basic<unsigned char>(buffer, offset, data_big, remaining);
    case TypeEnum::Short: return unpack_basic<short>(buffer, offset, data_big, remaining);
    case TypeEnum::UnsignedShort: return unpack_basic<unsigned short>(buffer, offset, data_big, remaining);
    case TypeEnum::Int: return unpack_basic<int>(buffer, offset, data_big, remaining);
    case TypeEnum::UnsignedInt: return unpack_basic<unsigned int>(buffer, offset, data_big, remaining);
    case TypeEnum::Long: return unpack_basic<long>(buffer, offset, data_big, remaining);
    case TypeEnum::UnsignedLong: return unpack_basic<unsigned long>(buffer, offset, data_big, remaining);
    case TypeEnum::LongLong: return unpack_basic<long long>(buffer, offset, data_big, remaining);
    case TypeEnum::UnsignedLongLong: return unpack_basic<unsigned long long>(buffer, offset, data_big, remaining);
    case TypeEnum::Float: return unpack_basic<float>(buffer, offset, data_big, remaining);
    case TypeEnum::Double: return unpack_basic<double>(buffer, offset, data_big, remaining);
    case TypeEnum::LongDouble: return unpack_basic<long double>(buffer, offset, data_big, remaining);
    case TypeEnum::Bool: return unpack_basic<bool>(buffer, offset, data_big, remaining);
    case TypeEnum::XPackBuff: return unpack_buffer(buffer, offset, data_big, remaining);
    case TypeEnum::String: return unpack_std_string(buffer, offset, data_big, remaining);
    default: throw std::runtime_error("Unknown TypeEnum");
    }
}

inline std::vector<VariantType> xpack_unpack(const char* packed_data, int packed_size) {
    if (!packed_data) throw std::invalid_argument("Packed data is null");
    if (packed_size < 1 + 4) throw std::runtime_error("Packed data too small");
    bool data_big_endian = (packed_data[0] == 1);
    int offset = 1;
    uint32_t total_data_len = 0;
    std::memcpy(&total_data_len, packed_data + offset, 4);
    offset += 4;
    if (data_big_endian != is_big_endian()) total_data_len = endian_swap<uint32_t>(total_data_len);
    int remaining = static_cast<int>(total_data_len);
    if (offset + remaining > packed_size) throw std::runtime_error("Packed data is incomplete");
    std::vector<VariantType> result;
    while (remaining > 0) {
        result.push_back(unpack_single(packed_data, offset, data_big_endian, remaining));
    }
    return result;
}

#endif // __XPACK_H__