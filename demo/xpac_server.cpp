// xpac_server.cpp - PAC文件管理服务器
#include "xhttpd.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>
#include <unordered_map>
#include "xtimer.h"

#ifndef PAC_SERVER_CONFIG_H
#define PAC_SERVER_CONFIG_H

constexpr const char* SECRET_PASSWORD = "abababab";
constexpr int LISTEN_PORT = 8888;
constexpr const char* LISTEN_HOST = "0.0.0.0";
constexpr const char* LOG_FILE = "logs/pac_server.log";
#endif // PAC_SERVER_CONFIG_H

#ifdef _WIN32
#define sscanf sscanf_s
#endif

class PACFileManager {
private:
    std::string password_;

public:
    PACFileManager(const std::string& password) : password_(password) {}

    bool authenticate(const std::string& input_password) {
        return input_password == password_;
    }

    std::string read_pac_file(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::string read_html_template(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            xlog_err("Failed to open HTML template: %s", filename.c_str());
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    bool add_domain_to_pac(const std::string& domain) {
        const std::string filename = "proxy.pac";
        std::ifstream in_file(filename);

        if (!in_file.is_open()) {
            xlog_err("Failed to open PAC file: %s", filename.c_str());
            return false;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in_file, line)) {
            lines.push_back(line);
        }
        in_file.close();

        while (lines.size() < 50) {
            lines.push_back("");
        }

        size_t pos = 49;
        lines.insert(lines.begin() + pos, "\tshExpMatch(host, '" + domain + "') ||");
        lines.insert(lines.begin() + pos + 1, "\tshExpMatch(host, '*." + domain + "') ||");

        std::ofstream out_file(filename);
        if (!out_file.is_open()) {
            xlog_err("Failed to write PAC file: %s", filename.c_str());
            return false;
        }

        for (const auto& l : lines) {
            out_file << l << "\n";
        }
        out_file.close();

        xlog_info("Added domain to PAC file: %s", domain.c_str());
        return true;
    }

    static bool is_valid_domain(const std::string& domain) {
        if (domain.empty() || domain.length() > 253) {
            return false;
        }

        for (char c : domain) {
            if (!std::isalnum(c) && c != '-' && c != '.') {
                return false;
            }
        }

        return domain.find('.') != std::string::npos;
    }
};

static PACFileManager g_pac_manager(SECRET_PASSWORD);


// 替换模板中的占位符
std::string replace_template_variables(const std::string& template_str,
    const std::vector<std::pair<std::string, std::string>>& vars) {
    std::string result = template_str;
    for (const auto& [key, value] : vars) {
        std::string placeholder = "{" + key + "}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.length(), value);
            pos += value.length();
        }
    }
    return result;
}

void send_error(xChannel* channel, int status_code, const char* message) {
    std::string html_template = g_pac_manager.read_html_template("templates/error.html");
    
    if (html_template.empty()) {
        // 如果无法加载模板文件，回退到原来的错误处理方式
        xhttpd_send_error(channel, status_code, message);
        return;
    }
    
    char time_buf[24];
    time_get_dt(time_get_ms(), time_buf);
    
    html_template = replace_template_variables(html_template, {
        {"ERROR_MESSAGE", message ? message : "Unknown error"},
        {"ERROR_CODE", std::to_string(status_code)},
        {"ERROR_TIME", time_buf}
    });
    
    HttpResponse* resp = xhttpd_get_response(channel);
    if (resp) {
        xhttpd_set_header(resp, "Content-Type", "text/html; charset=utf-8");
        xhttpd_set_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
        xhttpd_set_header(resp, "Pragma", "no-cache");
        xhttpd_set_header(resp, "Expires", "0");
        xhttpd_set_body(resp, html_template.c_str(), html_template.length());
        xhttpd_send_response(channel, resp);
    } else {
        xhttpd_send_error(channel, status_code, message);
    }
}

// HTTP处理函数
xCoroTaskT<bool> handle_root(HttpRequest* req, HttpResponse* resp) {
    std::string html = g_pac_manager.read_html_template("templates/login.html");
    char time_buf[24];
    time_get_dt(time_get_ms(), time_buf);

    html = replace_template_variables(html, {
        {"TIME", time_buf},
        {"CONNECTIONS", std::to_string(xhttpd_get_active_connections())}
        });

    xhttpd_set_header(resp, "Content-Type", "text/html; charset=utf-8");

    xhttpd_set_body(resp, html.c_str(), html.length());
    xhttpd_send_response(req->channel, resp);

    co_return true;
}

xCoroTaskT<bool> handle_proxy_pac(HttpRequest* req, HttpResponse* resp) {
    std::string content = g_pac_manager.read_pac_file("proxy.pac");
    if (content.empty()) {
        xhttpd_send_error(req->channel, 404, "PAC file not found");
        co_return false;
    }
    co_await coroutine_sleep(50);// for async test

    // 明确禁用压缩和缓存，便于调试
    xhttpd_set_header(resp, "Content-Type", "application/x-ns-proxy-autoconfig; charset=utf-8");
    xhttpd_set_header(resp, "Content-Disposition", "filename=proxy.pac");
    xhttpd_set_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
    xhttpd_set_header(resp, "Pragma", "no-cache");
    xhttpd_set_header(resp, "Expires", "0");

    // 显式拒绝压缩
    xhttpd_set_header(resp, "Accept-Encoding", "identity");
    xhttpd_set_header(resp, "Transfer-Encoding", "identity");
    
    xhttpd_set_body(resp, content.c_str(), content.length());
    xhttpd_send_response(req->channel, resp);
    co_return true;
}

xCoroTaskT<bool> handle_proxy1081_pac(HttpRequest* req, HttpResponse* resp) {
    std::string content = g_pac_manager.read_pac_file("proxy1081.pac");
    if (content.empty()) {
        xhttpd_send_error(req->channel, 404, "PAC-1080 file not found");
        co_return false;
    }

    xhttpd_set_header(resp, "Content-Type", "application/x-ns-proxy-autoconfig; charset=utf-8");
    xhttpd_set_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
    xhttpd_set_header(resp, "Pragma", "no-cache");
    xhttpd_set_header(resp, "Expires", "0");

    xhttpd_set_body(resp, content.c_str(), content.length());
    xhttpd_send_response(req->channel, resp);

    co_return true;
}
xCoroTaskT<bool> handle_proxy_all_pac(HttpRequest* req, HttpResponse* resp) {
    std::string content = g_pac_manager.read_pac_file("proxy.all.pac");
    if (content.empty()) {
        xhttpd_send_error(req->channel, 404, "PAC-ALL file not found");
        co_return false;
    }

    xhttpd_set_header(resp, "Content-Type", "application/x-ns-proxy-autoconfig; charset=utf-8");
    xhttpd_set_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
    xhttpd_set_header(resp, "Pragma", "no-cache");
    xhttpd_set_header(resp, "Expires", "0");

    xhttpd_set_body(resp, content.c_str(), content.length());
    xhttpd_send_response(req->channel, resp);

    co_return true;
}
xCoroTaskT<bool> handle_add_domain(HttpRequest* req, HttpResponse* resp) {
    // 检查是否为POST请求
    if (req->method != HTTP_POST) {
        xhttpd_send_error(req->channel, 405, "Method Not Allowed");
        co_return false;
    }

    size_t password_len = 0;
    const char* password = xhttpd_get_query_param(req, "password", &password_len);
    if (password_len==0) {
        xhttpd_send_error(req->channel, 400, "Missing required parameters");
        co_return false;
    }

    if (!g_pac_manager.authenticate(std::string(password, password_len))) {
        xhttpd_send_error(req->channel, 401, "Invalid Password");
        co_return false;
    }

    // 验证域名
    size_t domain_len = 0;
    const char* domain = xhttpd_get_query_param(req, "domain", &domain_len);
    if (!PACFileManager::is_valid_domain(std::string(domain, domain_len))) {
        xhttpd_send_error(req->channel, 400, "Invalid domain format");
        co_return false;
    }

    // 查找是否已经存在
    std::string content = g_pac_manager.read_pac_file("proxy.pac");
    if (content.empty()) {
        xhttpd_send_error(req->channel, 404, "PAC file not found");
        co_return false;
    }
    if (xhttpd_memsearch(content.c_str(), content.length(), domain, domain_len) != nullptr) {
        xhttpd_send_error(req->channel, 400, "Domain already exists");
        co_return false;
    }
    // 添加域名到PAC文件
    if (!g_pac_manager.add_domain_to_pac(std::string(domain, domain_len))) {
        xhttpd_send_error(req->channel, 500, "Failed to update PAC file");
        co_return false;
    }

    // 返回成功页面   
    std::string html = g_pac_manager.read_html_template("templates/success.html");
    xhttpd_set_header(resp, "Content-Type", "text/html; charset=utf-8");
    xhttpd_set_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
    xhttpd_set_header(resp, "Pragma", "no-cache");
    xhttpd_set_header(resp, "Expires", "0");
   
    xhttpd_set_body(resp, html.c_str(), html.length());
    xhttpd_send_response(req->channel, resp);
    co_return true;
}

xCoroTaskT<bool> handle_pac_status(HttpRequest* req, HttpResponse* resp) {
    char json[512];
    snprintf(json, sizeof(json),
        "{\"status\": \"OK\", "
        "\"active_connections\": %zu, "
        "\"total_requests\": %llu, "
        "\"server_time\": %llu, "
        "\"pac_files\": [\"proxy.pac\", \"proxy1081.pac\", \"proxy.all.pac\"]}",
        xhttpd_get_active_connections(),
        (unsigned long long)xhttpd_get_total_requests(),
        (unsigned long long)std::time(nullptr));

    xhttpd_send_json(req->channel, 200, json);
    co_return true;
}

xCoroTaskT<bool> handle_favicon(HttpRequest* req, HttpResponse* resp) {
    // 简单的 1x1 像素透明 GIF
    static const unsigned char transparent_gif[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x21, 0xf9, 0x04, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b
    };

    xhttpd_set_header(resp, "Content-Type", "image/gif");
    xhttpd_set_header(resp, "Cache-Control", "public, max-age=86400");

    xhttpd_set_body(resp, (const char*)transparent_gif, sizeof(transparent_gif));
    xhttpd_send_response(req->channel, resp);

    co_return true;
}

// 主函数
int main() {
    // 初始化事件循环
    aeEventLoop* el = aeCreateEventLoop(1024);
    if (!el) {
        xlog_err("Failed to create event loop");
        return -1;
    }

    // 初始化日志系统
    xlog_init(XLOG_DEBUG, true, true, LOG_FILE);
    xlog_set_show_thread_name(true);

    char buffer[MAX_PATH];
    DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
    if (length > 0)
        xlog_warn("current dir: %s", buffer);

    // 初始化协程系统
    coroutine_init();

    // 初始化HTTP服务器
    if (!xhttpd_init()) {
        std::cerr << "Failed to initialize HTTP server" << std::endl;
        return 1;
    }

    // 配置服务器
    HttpServerConfig config = {
        .port = LISTEN_PORT,
        .host = LISTEN_HOST,
        .max_connections = 1000,
        .request_timeout_ms = 30000,
        .max_body_size = 1 * 1024 * 1024, // 1MB
        .enable_cors = true,
        .cors_origin = "*"
    };

    // 注册路由
    xhttpd_register_route(HTTP_GET, "/", handle_root, NULL);
    xhttpd_register_route(HTTP_GET, "/favicon.ico", handle_favicon, NULL);
    xhttpd_register_route(HTTP_GET, "/proxy.pac", handle_proxy_pac, NULL);
    xhttpd_register_route(HTTP_GET, "/proxy1081.pac", handle_proxy1081_pac, NULL);
    xhttpd_register_route(HTTP_GET, "/proxy.all.pac", handle_proxy_all_pac, NULL);
    xhttpd_register_route(HTTP_POST, "/add-domain", handle_add_domain, NULL);
    xhttpd_register_route(HTTP_GET, "/api/status", handle_pac_status, NULL);

    // 启动服务器
    if (!xhttpd_start(&config)) {
        std::cerr << "Failed to start HTTP server" << std::endl;
        xhttpd_uninit();
        return 1;
    }

    std::cout << "=========================================" << std::endl;
    std::cout << "PAC Management Server Started!" << std::endl;
    std::cout << "URL: http://" << config.host << ":" << config.port << std::endl;
    std::cout << "Password: " << SECRET_PASSWORD << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Available endpoints:" << std::endl;
    std::cout << "  GET  /              - PAC Manager Web Interface" << std::endl;
    std::cout << "  GET  /proxy.pac     - Download proxy.pac file" << std::endl;
    std::cout << "  GET  /proxy1081.pac - Download proxy1081.pac file" << std::endl;
    std::cout << "  GET  /proxy.all.pac - Download proxy.all.pac file" << std::endl;
    std::cout << "  POST /add-domain    - Add domain to PAC (password required)" << std::endl;
    std::cout << "  GET  /api/status    - Server status" << std::endl;
    std::cout << "=========================================" << std::endl;

    std::ifstream test_file("proxy.pac");
    if (!test_file.is_open()) {
        std::cout << "\nCreating sample PAC files..." << std::endl;

        // 创建示例proxy.pac
        std::ofstream pac_file("proxy.pac");

        char time_buf[24] = {0};
        time_get_dt(time_get_ms(), time_buf);
        if (pac_file.is_open()) {
            pac_file << "function FindProxyForURL(url, host) {\n";
            pac_file << "    // PAC file managed by PAC Manager Server\n";
            pac_file << "    // Generated at: " << time_buf;
            pac_file << "\n";
            pac_file << "    // Local addresses bypass proxy\n";
            pac_file << "    if (isPlainHostName(host) ||\n";
            pac_file << "        shExpMatch(host, \"localhost\") ||\n";
            pac_file << "        shExpMatch(host, \"127.*\") ||\n";
            pac_file << "        shExpMatch(host, \"10.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.16.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.17.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.18.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.19.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.20.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.21.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.22.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.23.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.24.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.25.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.26.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.27.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.28.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.29.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.30.*\") ||\n";
            pac_file << "        shExpMatch(host, \"172.31.*\") ||\n";
            pac_file << "        shExpMatch(host, \"192.168.*\")) {\n";
            pac_file << "        return \"DIRECT\";\n";
            pac_file << "    }\n\n";
            pac_file << "    // Domains that use proxy (add more using web interface)\n";
            pac_file << "    if (false // Placeholder for added domains\n";
            for (int i = 0; i < 30; i++) {
                pac_file << "        // Line " << (i + 21) << "\n";
            }
            pac_file << "        ) {\n";
            pac_file << "        return \"PROXY 127.0.0.1:8080\";\n";
            pac_file << "    }\n\n";
            pac_file << "    // Default: direct connection\n";
            pac_file << "    return \"DIRECT\";\n";
            pac_file << "}\n";
            pac_file.close();
            std::cout << "Created proxy.pac" << std::endl;
        }

        // 创建其他示例文件
        std::ofstream pac1081("proxy1081.pac");
        if (pac1081.is_open()) {
            pac1081 << "function FindProxyForURL(url, host) {\n";
            pac1081 << "    // PAC file for port 1081\n";
            pac1081 << "    return \"PROXY 127.0.0.1:1081\";\n";
            pac1081 << "}\n";
            pac1081.close();
            std::cout << "Created proxy1081.pac" << std::endl;
        }

        std::ofstream pac_all("proxy.all.pac");
        if (pac_all.is_open()) {
            pac_all << "function FindProxyForURL(url, host) {\n";
            pac_all << "    // PAC file - all traffic through proxy\n";
            pac_all << "    return \"PROXY 127.0.0.1:8080\";\n";
            pac_all << "}\n";
            pac_all.close();
            std::cout << "Created proxy.all.pac" << std::endl;
        }

        std::cout << "\nSample PAC files created. You can now add domains via web interface.\n";
    }

    // 主事件循环
    while (true) {
        aeProcessEvents(el, AE_ALL_EVENTS);
        aeWait(-1, 0, 10000);  // 10ms等待
    }

    // 清理
    xhttpd_stop();
    xhttpd_uninit();
    coroutine_uninit();
    xlog_uninit();
    aeDeleteEventLoop(el);

    return 0;
}
