/*
 * =====================================================================================
 *                           Redis RESP2/RESP3 协议打包解包库
 * =====================================================================================
 *
 * 支持 Redis 5 (RESP2) 和 Redis 6+ (RESP3) 协议
 *
 * 新特性：
 *   1. RESP3 布尔值: #t\r\n  #f\r\n
 *   2. RESP3 双精度浮点数: ,3.14\r\n
 *   3. RESP3 大数字: (12345678901234567890\r\n
 *   4. RESP3 映射: %2\r\n+key1\r\n$5\r\nvalue1\r\n+key2\r\n$5\r\nvalue2\r\n
 *   5. RESP3 集合: ~3\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$3\r\nbaz\r\n
 *   6. RESP3 属性: |1\r\n+key-popularity\r\n,0.5\r\n$5\r\nvalue\r\n
 *   7. RESP3 推送数据: >4\r\n+pubsub\r\n+message\r\n$7\r\nchannel\r\n$5\r\nhello\r\n
 *
 * =====================================================================================
 */
#ifndef __REDIS_RESP3_H__
#define __REDIS_RESP3_H__

#include <sstream>
#include <iomanip>
#include <cctype>
#include <string_view>
#include <variant>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include <map>
#include <unordered_set>
#include <optional>

 // =====================================================================================
 //                                 Redis 协议版本
 // =====================================================================================

enum class RedisProtocol {
    RESP2,  // Redis 5 及之前
    RESP3   // Redis 6+
};

// =====================================================================================
//                                 Redis 协议类型定义
// =====================================================================================

enum class RedisType {
    // RESP2 基本类型
    SimpleString,   // 简单字符串 (+)
    Error,          // 错误 (-)
    Integer,        // 整数 (:)
    BulkString,     // 批量字符串 ($)
    Array,          // 数组 (*)
    Null,           // 空值

    // RESP3
    Boolean,        // 布尔值 (#)
    Double,         // 双精度浮点数 (,)
    BigNumber,      // 大数字 (()
    Map,            // 映射 (%)
    Set,            // 集合 (~)
    Attribute,      // 属性 (|)
    Push            // 推送数据 (>)
};

// 前向声明
class RedisObject;

// 映射和集合的类型别名
using RedisMap = std::vector<std::pair<RedisObject, RedisObject>>;
using RedisSet = std::vector<RedisObject>;
using RedisAttributes = std::vector<std::pair<RedisObject, RedisObject>>;

class RedisObject {
private:
    RedisType type_;

    // 使用联合体存储不同类型的数据
    union ValueUnion {
        int64_t int_value;
        bool bool_value;
        double double_value;
        std::string* str_value;
        std::string* big_num_value;
        std::vector<RedisObject>* arr_value;
        RedisMap* map_value;
        RedisSet* set_value;
        RedisAttributes* attr_value;

        ValueUnion() : int_value(0) {}
    } value_;

public:
    // 构造函数
    RedisObject() : type_(RedisType::Null) {
        value_.int_value = 0;
    }

    RedisObject(RedisType type) : type_(type) {
        init_value(type);
    }

    // 析构函数
    ~RedisObject() {
        clear();
    }

    // 拷贝构造函数
    RedisObject(const RedisObject& other) : type_(other.type_) {
        copy_from(other);
    }

    // 移动构造函数
    RedisObject(RedisObject&& other) noexcept : type_(other.type_), value_(other.value_) {
        other.type_ = RedisType::Null;
        other.value_.int_value = 0;
    }

    // 赋值运算符
    RedisObject& operator=(const RedisObject& other) {
        if (this != &other) {
            clear();
            type_ = other.type_;
            copy_from(other);
        }
        return *this;
    }

    RedisObject& operator=(RedisObject&& other) noexcept {
        if (this != &other) {
            clear();
            type_ = other.type_;
            value_ = other.value_;
            other.type_ = RedisType::Null;
            other.value_.int_value = 0;
        }
        return *this;
    }

    // 类型检查
    RedisType type() const { return type_; }

    // 静态工厂方法
    static RedisObject Simple(const std::string& data) {
        RedisObject obj(RedisType::SimpleString);
        *obj.value_.str_value = data;
        return obj;
    }

    static RedisObject Error(const std::string& data) {
        RedisObject obj(RedisType::Error);
        *obj.value_.str_value = data;
        return obj;
    }

    static RedisObject Integer(int64_t val) {
        RedisObject obj(RedisType::Integer);
        obj.value_.int_value = val;
        return obj;
    }

    static RedisObject Bulk(const std::string& data) {
        RedisObject obj(RedisType::BulkString);
        *obj.value_.str_value = data;
        return obj;
    }

    static RedisObject Null() {
        return RedisObject(RedisType::Null);
    }

    static RedisObject Array(const std::vector<RedisObject>& arr) {
        RedisObject obj(RedisType::Array);
        *obj.value_.arr_value = arr;
        return obj;
    }

    // RESP3 新增类型
    static RedisObject Boolean(bool val) {
        RedisObject obj(RedisType::Boolean);
        obj.value_.bool_value = val;
        return obj;
    }

    static RedisObject Double(double val) {
        RedisObject obj(RedisType::Double);
        obj.value_.double_value = val;
        return obj;
    }

    static RedisObject BigNumber(const std::string& val) {
        RedisObject obj(RedisType::BigNumber);
        *obj.value_.big_num_value = val;
        return obj;
    }

    static RedisObject Map(const RedisMap& map) {
        RedisObject obj(RedisType::Map);
        *obj.value_.map_value = map;
        return obj;
    }

    static RedisObject Set(const RedisSet& set) {
        RedisObject obj(RedisType::Set);
        *obj.value_.set_value = set;
        return obj;
    }

    static RedisObject Attribute(const RedisAttributes& attr) {
        RedisObject obj(RedisType::Attribute);
        *obj.value_.attr_value = attr;
        return obj;
    }

    static RedisObject Push(const std::vector<RedisObject>& data) {
        RedisObject obj(RedisType::Push);
        *obj.value_.arr_value = data;
        return obj;
    }

    // 获取值的方法
    std::string get_string() const {
        switch (type_) {
        case RedisType::SimpleString:
        case RedisType::Error:
        case RedisType::BulkString:
            return *value_.str_value;
        case RedisType::Integer:
            return std::to_string(value_.int_value);
        case RedisType::Double:
            return std::to_string(value_.double_value);
        case RedisType::BigNumber:
            return *value_.big_num_value;
        case RedisType::Boolean:
            return value_.bool_value ? "true" : "false";
        default:
            return "";
        }
    }

    int64_t get_integer() const {
        if (type_ == RedisType::Integer) {
            return value_.int_value;
        }
        else if (type_ == RedisType::Boolean) {
            return value_.bool_value ? 1 : 0;
        }
        return 0;
    }

    bool get_boolean() const {
        if (type_ == RedisType::Boolean) {
            return value_.bool_value;
        }
        else if (type_ == RedisType::Integer) {
            return value_.int_value != 0;
        }
        return false;
    }

    double get_double() const {
        if (type_ == RedisType::Double) {
            return value_.double_value;
        }
        else if (type_ == RedisType::Integer) {
            return static_cast<double>(value_.int_value);
        }
        return 0.0;
    }

    const std::vector<RedisObject>& get_array() const {
        static const std::vector<RedisObject> empty;
        if (type_ == RedisType::Array || type_ == RedisType::Push) {
            return *value_.arr_value;
        }
        return empty;
    }

    const RedisMap& get_map() const {
        static const RedisMap empty;
        if (type_ == RedisType::Map || type_ == RedisType::Attribute) {
            return *value_.map_value;
        }
        return empty;
    }

    const RedisSet& get_set() const {
        static const RedisSet empty;
        if (type_ == RedisType::Set) {
            return *value_.set_value;
        }
        return empty;
    }

    // 判断类型
    bool is_null() const { return type_ == RedisType::Null; }
    bool is_array() const { return type_ == RedisType::Array; }
    bool is_map() const { return type_ == RedisType::Map; }
    bool is_set() const { return type_ == RedisType::Set; }
    bool is_boolean() const { return type_ == RedisType::Boolean; }
    bool is_double() const { return type_ == RedisType::Double; }
    bool is_push() const { return type_ == RedisType::Push; }
    bool is_attribute() const { return type_ == RedisType::Attribute; }

private:
    void init_value(RedisType type) {
        switch (type) {
        case RedisType::SimpleString:
        case RedisType::Error:
        case RedisType::BulkString:
            value_.str_value = new std::string();
            break;
        case RedisType::BigNumber:
            value_.big_num_value = new std::string();
            break;
        case RedisType::Array:
        case RedisType::Push:
            value_.arr_value = new std::vector<RedisObject>();
            break;
        case RedisType::Map:
        case RedisType::Attribute:
            value_.map_value = new RedisMap();
            break;
        case RedisType::Set:
            value_.set_value = new RedisSet();
            break;
        case RedisType::Integer:
            value_.int_value = 0;
            break;
        case RedisType::Boolean:
            value_.bool_value = false;
            break;
        case RedisType::Double:
            value_.double_value = 0.0;
            break;
        case RedisType::Null:
            value_.int_value = 0;
            break;
        }
    }

    void clear() {
        switch (type_) {
        case RedisType::SimpleString:
        case RedisType::Error:
        case RedisType::BulkString:
            delete value_.str_value;
            break;
        case RedisType::BigNumber:
            delete value_.big_num_value;
            break;
        case RedisType::Array:
        case RedisType::Push:
            delete value_.arr_value;
            break;
        case RedisType::Map:
        case RedisType::Attribute:
            delete value_.map_value;
            break;
        case RedisType::Set:
            delete value_.set_value;
            break;
        default:
            break;
        }
    }

    void copy_from(const RedisObject& other) {
        switch (type_) {
        case RedisType::SimpleString:
        case RedisType::Error:
        case RedisType::BulkString:
            value_.str_value = new std::string(*other.value_.str_value);
            break;
        case RedisType::BigNumber:
            value_.big_num_value = new std::string(*other.value_.big_num_value);
            break;
        case RedisType::Array:
        case RedisType::Push:
            value_.arr_value = new std::vector<RedisObject>(*other.value_.arr_value);
            break;
        case RedisType::Map:
        case RedisType::Attribute:
            value_.map_value = new RedisMap(*other.value_.map_value);
            break;
        case RedisType::Set:
            value_.set_value = new RedisSet(*other.value_.set_value);
            break;
        case RedisType::Integer:
            value_.int_value = other.value_.int_value;
            break;
        case RedisType::Boolean:
            value_.bool_value = other.value_.bool_value;
            break;
        case RedisType::Double:
            value_.double_value = other.value_.double_value;
            break;
        case RedisType::Null:
            value_.int_value = 0;
            break;
        }
    }
};

// =====================================================================================
//                                 Redis RESP2/RESP3 打包器
// =====================================================================================

namespace redis {

    class RESPEncoder {
    private:
        RedisProtocol protocol_;

    public:
        RESPEncoder(RedisProtocol protocol = RedisProtocol::RESP2)
            : protocol_(protocol) {
        }

        void set_protocol(RedisProtocol protocol) {
            protocol_ = protocol;
        }

        std::string encode(const RedisObject& obj) const {
            std::ostringstream oss;

            switch (obj.type()) {
            case RedisType::SimpleString:
                encode_simple_string(oss, obj);
                break;
            case RedisType::Error:
                encode_error(oss, obj);
                break;
            case RedisType::Integer:
                encode_integer(oss, obj);
                break;
            case RedisType::BulkString:
                encode_bulk_string(oss, obj);
                break;
            case RedisType::Array:
                encode_array(oss, obj);
                break;
            case RedisType::Null:
                encode_null(oss, obj);
                break;
            case RedisType::Boolean:
                if (protocol_ == RedisProtocol::RESP3) {
                    encode_boolean(oss, obj);
                }
                else {
                    // RESP2 不支持布尔值，降级为整数
                    encode_integer(oss, RedisObject::Integer(obj.get_integer()));
                }
                break;
            case RedisType::Double:
                if (protocol_ == RedisProtocol::RESP3) {
                    encode_double(oss, obj);
                }
                else {
                    // RESP2 不支持双精度浮点数，降级为字符串
                    encode_bulk_string(oss, RedisObject::Bulk(std::to_string(obj.get_double())));
                }
                break;
            case RedisType::BigNumber:
                if (protocol_ == RedisProtocol::RESP3) {
                    encode_bignumber(oss, obj);
                }
                else {
                    // RESP2 不支持大数字，降级为字符串
                    encode_bulk_string(oss, RedisObject::Bulk(obj.get_string()));
                }
                break;
            case RedisType::Map:
                if (protocol_ == RedisProtocol::RESP3) {
                    encode_map(oss, obj);
                }
                else {
                    // RESP2 不支持映射，降级为数组
                    encode_array_from_map(oss, obj);
                }
                break;
            case RedisType::Set:
                if (protocol_ == RedisProtocol::RESP3) {
                    encode_set(oss, obj);
                }
                else {
                    // RESP2 不支持集合，降级为数组
                    encode_array(oss, RedisObject::Array(obj.get_set()));
                }
                break;
            case RedisType::Attribute:
                if (protocol_ == RedisProtocol::RESP3) {
                    encode_attribute(oss, obj);
                }
                else {
                    // RESP2 不支持属性，忽略属性
                    throw std::runtime_error("Attributes not supported in RESP2");
                }
                break;
            case RedisType::Push:
                if (protocol_ == RedisProtocol::RESP3) {
                    encode_push(oss, obj);
                }
                else {
                    // RESP2 不支持推送
                    throw std::runtime_error("Push data not supported in RESP2");
                }
                break;
            }

            return oss.str();
        }

    private:
        void encode_simple_string(std::ostringstream& oss, const RedisObject& obj) const {
            oss << "+" << obj.get_string() << "\r\n";
        }

        void encode_error(std::ostringstream& oss, const RedisObject& obj) const {
            oss << "-" << obj.get_string() << "\r\n";
        }

        void encode_integer(std::ostringstream& oss, const RedisObject& obj) const {
            oss << ":" << obj.get_integer() << "\r\n";
        }

        void encode_bulk_string(std::ostringstream& oss, const RedisObject& obj) const {
            const std::string& str = obj.get_string();
            oss << "$" << str.length() << "\r\n" << str << "\r\n";
        }

        void encode_array(std::ostringstream& oss, const RedisObject& obj) const {
            const auto& arr = obj.get_array();
            oss << "*" << arr.size() << "\r\n";
            for (const auto& elem : arr) {
                oss << encode(elem);
            }
        }

        void encode_null(std::ostringstream& oss, const RedisObject& obj) const {
            if (protocol_ == RedisProtocol::RESP3) {
                oss << "_\r\n";  // RESP3 空值
            }
            else {
                oss << "$-1\r\n";  // RESP2 空值
            }
        }

        void encode_boolean(std::ostringstream& oss, const RedisObject& obj) const {
            oss << "#" << (obj.get_boolean() ? "t" : "f") << "\r\n";
        }

        void encode_double(std::ostringstream& oss, const RedisObject& obj) const {
            oss << "," << std::fixed << std::setprecision(17) << obj.get_double() << "\r\n";
        }

        void encode_bignumber(std::ostringstream& oss, const RedisObject& obj) const {
            oss << "(" << obj.get_string() << "\r\n";
        }

        void encode_map(std::ostringstream& oss, const RedisObject& obj) const {
            const auto& map = obj.get_map();
            oss << "%" << map.size() << "\r\n";
            for (const auto& [key, value] : map) {
                oss << encode(key) << encode(value);
            }
        }

        void encode_array_from_map(std::ostringstream& oss, const RedisObject& obj) const {
            const auto& map = obj.get_map();
            std::vector<RedisObject> arr;
            arr.reserve(map.size() * 2);
            for (const auto& [key, value] : map) {
                arr.push_back(key);
                arr.push_back(value);
            }
            oss << encode(RedisObject::Array(arr));
        }

        void encode_set(std::ostringstream& oss, const RedisObject& obj) const {
            const auto& set = obj.get_set();
            oss << "~" << set.size() << "\r\n";
            for (const auto& elem : set) {
                oss << encode(elem);
            }
        }

        void encode_attribute(std::ostringstream& oss, const RedisObject& obj) const {
            const auto& attrs = obj.get_map();
            oss << "|" << attrs.size() << "\r\n";
            for (const auto& [key, value] : attrs) {
                oss << encode(key) << encode(value);
            }
        }

        void encode_push(std::ostringstream& oss, const RedisObject& obj) const {
            const auto& data = obj.get_array();
            oss << ">" << data.size() << "\r\n";
            for (const auto& elem : data) {
                oss << encode(elem);
            }
        }
    };

    // =====================================================================================
    //                                 Redis RESP2/RESP3 解析器
    // =====================================================================================

    class RESPDecoder {
    private:
        const char* data_;
        int size_;
        int pos_;
        RedisProtocol protocol_;

    public:
        RESPDecoder(const char* data, int size, RedisProtocol protocol = RedisProtocol::RESP2)
            : data_(data), size_(size), pos_(0), protocol_(protocol) {
        }

        void set_protocol(RedisProtocol protocol) {
            protocol_ = protocol;
        }

        std::vector<RedisObject> decode() {
            std::vector<RedisObject> result;

            while (pos_ < size_) {
                skip_whitespace();
                if (pos_ >= size_) break;

                try {
                    result.push_back(decode_object());
                }
                catch (const std::exception& e) {
                    throw std::runtime_error("RESP decode error at position " +
                        std::to_string(pos_) + ": " + e.what());
                }
            }

            return result;
        }

        RedisObject decode_object() {
            if (pos_ >= size_) {
                throw std::runtime_error("Unexpected end of data");
            }

            char type_char = data_[pos_++];

            switch (type_char) {
            case '+': return decode_simple_string();
            case '-': return decode_error();
            case ':': return decode_integer();
            case '$': return decode_bulk_string();
            case '*': return decode_array();
            case '_': return decode_null();           // RESP3 null
            case '#': return decode_boolean();        // RESP3 boolean
            case ',': return decode_double();         // RESP3 double
            case '(': return decode_bignumber();      // RESP3 bignumber
            case '%': return decode_map();            // RESP3 map
            case '~': return decode_set();            // RESP3 set
            case '|': return decode_attribute();      // RESP3 attribute
            case '>': return decode_push();           // RESP3 push
            default:
                throw std::runtime_error("Unknown RESP type: " + std::string(1, type_char));
            }
        }

        // =====================================================================================
        //                                 数据包完整性检查接口
        // =====================================================================================

        /**
         * @brief 检查数据包是否完整
         * @return 返回值的含义：
         *         >0: 完整的数据包长度（字节数）
         *         0: 数据包不完整，需要更多数据
         *         -1: 数据包格式错误
         */
        int check_complete() {
            int saved_pos = pos_;  // 保存当前位置
            int result = check_complete_internal();
            pos_ = saved_pos;      // 恢复位置
            return result;
        }

        /**
         * @brief 静态方法：检查数据包是否完整
         * @param data 数据指针
         * @param size 数据大小
         * @param protocol 协议版本
         * @return 返回值的含义：
         *         >0: 完整的数据包长度（字节数）
         *         0: 数据包不完整，需要更多数据
         *         -1: 数据包格式错误
         */
        static int check_complete(const char* data, int size, RedisProtocol protocol = RedisProtocol::RESP2) {
            RESPDecoder decoder(data, size, protocol);
            return decoder.check_complete();
        }

        /**
         * @brief 检查缓冲区是否包含至少一个完整的RESP对象
         * @param data 数据指针
         * @param size 数据大小
         * @param protocol 协议版本
         * @return 如果包含完整对象返回true，否则返回false
         */
        static bool has_complete_packet(const char* data, int size, RedisProtocol protocol = RedisProtocol::RESP2) {
            int result = check_complete(data, size, protocol);
            return result > 0;
        }

        /**
         * @brief 获取下一个完整数据包的长度
         * @param data 数据指针
         * @param size 数据大小
         * @param protocol 协议版本
         * @return 完整数据包的长度，如果不完整或格式错误返回0
         */
        static int next_packet_length(const char* data, int size, RedisProtocol protocol = RedisProtocol::RESP2) {
            int result = check_complete(data, size, protocol);
            return result > 0 ? result : 0;
        }

    private:
        int check_complete_internal() {
            if (pos_ >= size_) {
                return 0;  // 没有数据
            }

            char type_char = data_[pos_];

            switch (type_char) {
            case '+':  // 简单字符串
            case '-':  // 错误
            case ':':  // 整数
            case '_':  // RESP3 null
            case '#':  // RESP3 boolean
            case ',':  // RESP3 double
            case '(':  // RESP3 bignumber
                return check_simple_type();

            case '$':  // 批量字符串
                return check_bulk_string();

            case '*':  // 数组
                return check_array();

            case '%':  // RESP3 映射
                return check_map();

            case '~':  // RESP3 集合
                return check_set();

            case '|':  // RESP3 属性
                return check_attribute();

            case '>':  // RESP3 推送
                return check_push();

            default:
                return -1;  // 未知类型，格式错误
            }
        }

        // 检查简单类型（以\r\n结尾）
        int check_simple_type() {
            // 跳过类型字符
            int current_pos = pos_ + 1;

            while (current_pos < size_) {
                if (current_pos + 1 < size_ &&
                    data_[current_pos] == '\r' &&
                    data_[current_pos + 1] == '\n') {
                    // 找到了完整的行，返回总长度
                    return current_pos + 2 - pos_;
                }
                current_pos++;
            }

            return 0;  // 数据不完整
        }

        // 检查批量字符串
        int check_bulk_string() {
            // 跳过 $
            int current_pos = pos_ + 1;

            // 查找长度行的结尾
            while (current_pos < size_) {
                if (current_pos + 1 < size_ &&
                    data_[current_pos] == '\r' &&
                    data_[current_pos + 1] == '\n') {
                    break;
                }
                current_pos++;
            }

            if (current_pos + 1 >= size_) {
                return 0;  // 长度行不完整
            }

            // 解析长度
            int length_start = pos_ + 1;
            int length_end = current_pos;
            std::string length_str(data_ + length_start, length_end - length_start);

            // 跳过 \r\n
            current_pos += 2;

            if (length_str == "-1") {
                // NULL 批量字符串，格式为 "$-1\r\n"
                return current_pos - pos_;
            }

            try {
                int64_t length = std::stoll(length_str);

                if (length < 0) {
                    return -1;  // 格式错误：长度不能为负数（除了-1）
                }

                // 检查是否有足够的数据
                if (current_pos + length + 2 > size_) {
                    return 0;  // 数据不完整
                }

                // 检查结尾的 \r\n
                if (data_[current_pos + length] != '\r' ||
                    data_[current_pos + length + 1] != '\n') {
                    return -1;  // 格式错误：缺少结尾的 \r\n
                }

                return current_pos + length + 2 - pos_;  // 完整的数据包长度
            }
            catch (const std::exception&) {
                return -1;  // 长度解析失败，格式错误
            }
        }

        // 检查数组
        int check_array() {
            return check_aggregate_type('*');
        }

        // 检查映射
        int check_map() {
            if (protocol_ != RedisProtocol::RESP3) {
                return -1;  // RESP2 不支持映射
            }
            return check_aggregate_type('%');
        }

        // 检查集合
        int check_set() {
            if (protocol_ != RedisProtocol::RESP3) {
                return -1;  // RESP2 不支持集合
            }
            return check_aggregate_type('~');
        }

        // 检查属性
        int check_attribute() {
            if (protocol_ != RedisProtocol::RESP3) {
                return -1;  // RESP2 不支持属性
            }
            return check_aggregate_type('|');
        }

        // 检查推送
        int check_push() {
            if (protocol_ != RedisProtocol::RESP3) {
                return -1;  // RESP2 不支持推送
            }
            return check_aggregate_type('>');
        }

        // 检查聚合类型（数组、映射、集合、属性、推送）
        int check_aggregate_type(char expected_type) {
            // 检查类型字符
            if (data_[pos_] != expected_type) {
                return -1;
            }

            // 跳过类型字符
            int current_pos = pos_ + 1;

            // 查找长度行的结尾
            while (current_pos < size_) {
                if (current_pos + 1 < size_ &&
                    data_[current_pos] == '\r' &&
                    data_[current_pos + 1] == '\n') {
                    break;
                }
                current_pos++;
            }

            if (current_pos + 1 >= size_) {
                return 0;  // 长度行不完整
            }

            // 解析长度
            int length_start = pos_ + 1;
            int length_end = current_pos;
            std::string length_str(data_ + length_start, length_end - length_start);

            // 跳过 \r\n
            current_pos += 2;

            try {
                int64_t count = std::stoll(length_str);

                if (count == -1) {
                    // 空数组/映射/集合等
                    return current_pos - pos_;
                }

                if (count < -1) {
                    return -1;  // 格式错误：长度不能小于-1
                }

                // 对于映射和属性，元素数量需要是键值对的数量
                int element_multiplier = 1;
                if (expected_type == '%' || expected_type == '|') {
                    element_multiplier = 2;  // 每个键值对包含两个元素
                }

                // 检查每个元素
                int saved_pos = pos_;
                pos_ = current_pos;  // 临时设置当前位置

                for (int64_t i = 0; i < count * element_multiplier; ++i) {
                    int element_result = check_complete_internal();
                    if (element_result <= 0) {
                        pos_ = saved_pos;  // 恢复位置
                        return element_result;  // 返回子元素的结果
                    }
                    // 移动到下一个元素
                    pos_ += element_result;
                }

                int total_length = pos_ - saved_pos;
                pos_ = saved_pos;  // 恢复位置
                return total_length;
            }
            catch (const std::exception&) {
                return -1;  // 长度解析失败，格式错误
            }
        }

        std::string read_line() {
            int start = pos_;

            while (pos_ < size_) {
                if (data_[pos_] == '\r') {
                    if (pos_ + 1 < size_ && data_[pos_ + 1] == '\n') {
                        std::string line(data_ + start, pos_ - start);
                        pos_ += 2;
                        return line;
                    }
                }
                ++pos_;
            }

            throw std::runtime_error("Incomplete line, missing \\r\\n");
        }

        void skip_whitespace() {
            while (pos_ < size_ && std::isspace(static_cast<unsigned char>(data_[pos_]))) {
                ++pos_;
            }
        }

        RedisObject decode_simple_string() {
            return RedisObject::Simple(read_line());
        }

        RedisObject decode_error() {
            return RedisObject::Error(read_line());
        }

        RedisObject decode_integer() {
            std::string line = read_line();
            try {
                int64_t val = std::stoll(line);
                return RedisObject::Integer(val);
            }
            catch (const std::exception&) {
                throw std::runtime_error("Invalid integer: " + line);
            }
        }

        RedisObject decode_bulk_string() {
            std::string length_str = read_line();

            if (length_str == "-1") {
                return RedisObject::Null();
            }

            try {
                int64_t length = std::stoll(length_str);

                if (length < 0) {
                    throw std::runtime_error("Invalid bulk string length: " + length_str);
                }

                if (pos_ + length + 2 > size_) {
                    throw std::runtime_error("Insufficient data for bulk string");
                }

                std::string str(data_ + pos_, static_cast<size_t>(length));
                pos_ += static_cast<int>(length);

                if (pos_ + 2 > size_ || data_[pos_] != '\r' || data_[pos_ + 1] != '\n') {
                    throw std::runtime_error("Bulk string not terminated with \\r\\n");
                }
                pos_ += 2;

                return RedisObject::Bulk(str);

            }
            catch (const std::exception& e) {
                throw std::runtime_error("Failed to parse bulk string length: " +
                    std::string(e.what()));
            }
        }

        RedisObject decode_array() {
            std::string length_str = read_line();

            if (length_str == "-1") {
                return RedisObject::Array({});
            }

            try {
                int64_t count = std::stoll(length_str);

                if (count < 0) {
                    throw std::runtime_error("Invalid array length: " + length_str);
                }

                std::vector<RedisObject> arr;
                arr.reserve(static_cast<size_t>(count));

                for (int64_t i = 0; i < count; ++i) {
                    arr.push_back(decode_object());
                }

                return RedisObject::Array(arr);

            }
            catch (const std::exception& e) {
                throw std::runtime_error("Failed to parse array length: " +
                    std::string(e.what()));
            }
        }

        RedisObject decode_null() {
            // RESP3 null: _\r\n
            read_line();  // 读取并验证格式
            return RedisObject::Null();
        }

        RedisObject decode_boolean() {
            std::string line = read_line();
            if (line == "t") {
                return RedisObject::Boolean(true);
            }
            else if (line == "f") {
                return RedisObject::Boolean(false);
            }
            else {
                throw std::runtime_error("Invalid boolean: " + line);
            }
        }

        RedisObject decode_double() {
            std::string line = read_line();
            try {
                double val = std::stod(line);
                return RedisObject::Double(val);
            }
            catch (const std::exception&) {
                throw std::runtime_error("Invalid double: " + line);
            }
        }

        RedisObject decode_bignumber() {
            std::string line = read_line();
            // 验证大数字格式（只包含数字，可能以负号开头）
            if (!line.empty()) {
                size_t start = 0;
                if (line[0] == '-') start = 1;
                for (size_t i = start; i < line.length(); ++i) {
                    if (!std::isdigit(line[i])) {
                        throw std::runtime_error("Invalid bignumber format: " + line);
                    }
                }
            }
            return RedisObject::BigNumber(line);
        }

        RedisObject decode_map() {
            std::string length_str = read_line();

            try {
                int64_t count = std::stoll(length_str);

                if (count < 0) {
                    throw std::runtime_error("Invalid map length: " + length_str);
                }

                RedisMap map;
                map.reserve(static_cast<size_t>(count));

                for (int64_t i = 0; i < count; ++i) {
                    auto key = decode_object();
                    auto value = decode_object();
                    map.emplace_back(std::move(key), std::move(value));
                }

                return RedisObject::Map(map);

            }
            catch (const std::exception& e) {
                throw std::runtime_error("Failed to parse map length: " +
                    std::string(e.what()));
            }
        }

        RedisObject decode_set() {
            std::string length_str = read_line();

            try {
                int64_t count = std::stoll(length_str);

                if (count < 0) {
                    throw std::runtime_error("Invalid set length: " + length_str);
                }

                RedisSet set;
                set.reserve(static_cast<size_t>(count));

                for (int64_t i = 0; i < count; ++i) {
                    set.push_back(decode_object());
                }

                return RedisObject::Set(set);

            }
            catch (const std::exception& e) {
                throw std::runtime_error("Failed to parse set length: " +
                    std::string(e.what()));
            }
        }

        RedisObject decode_attribute() {
            std::string length_str = read_line();

            try {
                int64_t count = std::stoll(length_str);

                if (count < 0) {
                    throw std::runtime_error("Invalid attribute length: " + length_str);
                }

                RedisMap attrs;
                attrs.reserve(static_cast<size_t>(count));

                for (int64_t i = 0; i < count; ++i) {
                    auto key = decode_object();
                    auto value = decode_object();
                    attrs.emplace_back(std::move(key), std::move(value));
                }

                return RedisObject::Attribute(attrs);

            }
            catch (const std::exception& e) {
                throw std::runtime_error("Failed to parse attribute length: " +
                    std::string(e.what()));
            }
        }

        RedisObject decode_push() {
            std::string length_str = read_line();

            try {
                int64_t count = std::stoll(length_str);

                if (count < 0) {
                    throw std::runtime_error("Invalid push data length: " + length_str);
                }

                std::vector<RedisObject> data;
                data.reserve(static_cast<size_t>(count));

                for (int64_t i = 0; i < count; ++i) {
                    data.push_back(decode_object());
                }

                return RedisObject::Push(data);

            }
            catch (const std::exception& e) {
                throw std::runtime_error("Failed to parse push data length: " +
                    std::string(e.what()));
            }
        }
    };

    // =====================================================================================
    //                                 简化的公共接口
    // =====================================================================================

    /**
     * @brief 打包 Redis 对象
     * @param obj Redis 对象
     * @param protocol 协议版本（RESP2 或 RESP3）
     * @return RESP 协议字符串
     */
    inline std::string redis_pack(const RedisObject& obj,
        RedisProtocol protocol = RedisProtocol::RESP2) {
        RESPEncoder encoder(protocol);
        return encoder.encode(obj);
    }

    /**
     * @brief 解包 RESP 协议字符串
     * @param data RESP 字符串
     * @param size 字符串长度
     * @param protocol 协议版本（RESP2 或 RESP3）
     * @return Redis 对象列表
     */
    inline std::vector<RedisObject> redis_unpack(const char* data, int size,
        RedisProtocol protocol = RedisProtocol::RESP2) {
        RESPDecoder decoder(data, size, protocol);
        return decoder.decode();
    }

    /**
     * @brief 解包 RESP 协议字符串
     * @param resp_str RESP 字符串
     * @param protocol 协议版本（RESP2 或 RESP3）
     * @return Redis 对象列表
     */
    inline std::vector<RedisObject> redis_unpack(const std::string& resp_str,
        RedisProtocol protocol = RedisProtocol::RESP2) {
        return redis_unpack(resp_str.c_str(), static_cast<int>(resp_str.length()), protocol);
    }

    /**
     * @brief 检查数据包是否完整
     * @param data 数据指针
     * @param size 数据大小
     * @param protocol 协议版本
     * @return 返回值的含义：
     *         >0: 完整的数据包长度（字节数）
     *         0: 数据包不完整，需要更多数据
     *         -1: 数据包格式错误
     */
    inline int redis_check_complete(const char* data, int size,
        RedisProtocol protocol = RedisProtocol::RESP2) {
        return RESPDecoder::check_complete(data, size, protocol);
    }

    /**
     * @brief 检查缓冲区是否包含至少一个完整的RESP对象
     * @param data 数据指针
     * @param size 数据大小
     * @param protocol 协议版本
     * @return 如果包含完整对象返回true，否则返回false
     */
    inline bool redis_has_complete_packet(const char* data, int size,
        RedisProtocol protocol = RedisProtocol::RESP2) {
        return RESPDecoder::has_complete_packet(data, size, protocol);
    }

    /**
     * @brief 获取下一个完整数据包的长度
     * @param data 数据指针
     * @param size 数据大小
     * @param protocol 协议版本
     * @return 完整数据包的长度，如果不完整或格式错误返回0
     */
    inline int redis_next_packet_length(const char* data, int size,
        RedisProtocol protocol = RedisProtocol::RESP2) {
        return RESPDecoder::next_packet_length(data, size, protocol);
    }

    // =====================================================================================
    //                                 高级包装函数（兼容 RESP2/RESP3）
    // =====================================================================================

    /**
     * @brief 创建 Redis 命令（指定协议版本）
     * @tparam Args 参数类型
     * @param protocol 协议版本
     * @param args 命令参数
     * @return RESP 协议字符串
     */
    template<typename... Args>
    std::string redis_command(RedisProtocol protocol, Args&&... args) {
        std::vector<RedisObject> arr;

        // 将每个参数转为 RedisObject（使用 RESP2 格式）
        auto to_redis_string = [](auto&& arg) -> std::string {
            std::ostringstream oss;
            oss << arg;
            return oss.str();
            };

        (arr.emplace_back(RedisObject::Bulk(to_redis_string(std::forward<Args>(args)))), ...);

        // 打包成数组
        auto array_obj = RedisObject::Array(arr);
        return redis_pack(array_obj, protocol);
    }

    /**
     * @brief 创建 Redis 命令（使用默认 RESP2 协议）
     * @tparam Args 参数类型
     * @param args 命令参数
     * @return RESP 协议字符串
     */
    template<typename... Args>
    std::string redis_command(Args&&... args) {
        return redis_command(RedisProtocol::RESP2, std::forward<Args>(args)...);
    }

} // namespace redis

#endif // __REDIS_RESP3_H__