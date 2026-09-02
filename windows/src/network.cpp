#include "network.h"
#include <winhttp.h>
#include <cctype>
#include <memory>
#include <sstream>

namespace {
struct InternetCloser { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using InternetHandle = std::unique_ptr<void, InternetCloser>;

class PublishedRequest {
public:
    PublishedRequest(InternetHandle& request, std::atomic<void*>& activeRequest)
        : request_(request), activeRequest_(activeRequest) {
        activeRequest_.store(request_.get(), std::memory_order_release);
    }

    ~PublishedRequest() {
        void* expected = request_.get();
        if (!activeRequest_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            void* closedRequest = request_.release();
            (void)closedRequest;
        }
    }

private:
    InternetHandle& request_;
    std::atomic<void*>& activeRequest_;
};

std::wstring WinError(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring text = message ? message : L"Unknown network error";
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text;
}

std::string JsonEscape(const std::wstring& value) {
    const std::string utf8 = WideToUtf8(value);
    std::string out;
    out.reserve(utf8.size() + 16);
    for (unsigned char c : utf8) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            } else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void AppendCodepoint(std::wstring& out, unsigned value) {
    if (value <= 0xFFFF) out.push_back(static_cast<wchar_t>(value));
    else {
        value -= 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (value >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
    }
}

bool ParseJsonString(const std::string& json, size_t quote, std::wstring& result, size_t* end = nullptr) {
    if (quote >= json.size() || json[quote] != '"') return false;
    std::string utf8;
    std::wstring decoded;
    for (size_t i = quote + 1; i < json.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(json[i]);
        if (c == '"') {
            decoded += Utf8ToWide(utf8);
            result = decoded;
            if (end) *end = i + 1;
            return true;
        }
        if (c != '\\') { utf8.push_back(static_cast<char>(c)); continue; }
        decoded += Utf8ToWide(utf8); utf8.clear();
        if (++i >= json.size()) return false;
        switch (json[i]) {
        case '"': decoded.push_back(L'"'); break;
        case '\\': decoded.push_back(L'\\'); break;
        case '/': decoded.push_back(L'/'); break;
        case 'b': decoded.push_back(L'\b'); break;
        case 'f': decoded.push_back(L'\f'); break;
        case 'n': decoded.push_back(L'\n'); break;
        case 'r': decoded.push_back(L'\r'); break;
        case 't': decoded.push_back(L'\t'); break;
        case 'u': {
            if (i + 4 >= json.size()) return false;
            unsigned value = 0;
            for (int j = 0; j < 4; ++j) { int h = Hex(json[++i]); if (h < 0) return false; value = value * 16 + static_cast<unsigned>(h); }
            if (value >= 0xD800 && value <= 0xDBFF && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u') {
                unsigned low = 0; size_t p = i + 2;
                for (int j = 0; j < 4; ++j) { int h = Hex(json[++p]); if (h < 0) return false; low = low * 16 + static_cast<unsigned>(h); }
                if (low >= 0xDC00 && low <= 0xDFFF) { value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00); i = p; }
            }
            AppendCodepoint(decoded, value);
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool ExtractContent(const std::string& json, std::wstring& content) {
    size_t delta = json.find("\"delta\"");
    if (delta == std::string::npos) return false;
    size_t key = json.find("\"content\"", delta);
    if (key == std::string::npos) return false;
    size_t colon = json.find(':', key + 9);
    if (colon == std::string::npos) return false;
    size_t quote = json.find_first_not_of(" \t\r\n", colon + 1);
    return quote != std::string::npos && ParseJsonString(json, quote, content);
}

std::wstring ExtractError(const std::string& body) {
    size_t key = body.find("\"message\"");
    if (key != std::string::npos) {
        size_t colon = body.find(':', key + 9);
        size_t quote = colon == std::string::npos ? std::string::npos : body.find_first_not_of(" \t\r\n", colon + 1);
        std::wstring message;
        if (quote != std::string::npos && ParseJsonString(body, quote, message)) return message;
    }
    std::wstring plain = Utf8ToWide(body.substr(0, 1000));
    return plain.empty() ? L"The service returned an empty error response." : plain;
}

std::string BuildRequest(const std::wstring& model, const std::vector<ChatMessage>& messages) {
    std::string body = "{\"model\":\"" + JsonEscape(model) + "\",\"stream\":true,\"messages\":[";
    bool first = true;
    for (const auto& message : messages) {
        if (!first) body += ',';
        first = false;
        body += "{\"role\":\"" + JsonEscape(message.role) + "\",\"content\":\"" + JsonEscape(message.content) + "\"}";
    }
    body += "]}";
    return body;
}
}

void CancelStreamRequest(std::atomic<void*>& activeRequest) {
    void* request = activeRequest.exchange(nullptr, std::memory_order_acq_rel);
    if (request) WinHttpCloseHandle(request);
}

StreamResult StreamChatCompletion(const std::wstring& apiKey, const std::wstring& model,
    const std::vector<ChatMessage>& messages, std::atomic_bool& stopRequested,
    std::atomic<void*>& activeRequest, const StreamChunkCallback& onChunk) {
    StreamResult result;
    if (stopRequested.load()) { result.stopped = true; return result; }
    InternetHandle session(WinHttpOpen(L"DKChatebras/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { result.error = L"WinHTTP initialization failed: " + WinError(GetLastError()); return result; }
    WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    InternetHandle connection(WinHttpConnect(session.get(), L"api.cerebras.ai", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) { result.error = L"Could not connect to Cerebras: " + WinError(GetLastError()); return result; }
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"POST", L"/v1/chat/completions", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) { result.error = L"Could not create the request: " + WinError(GetLastError()); return result; }
    PublishedRequest publishedRequest(request, activeRequest);
    if (stopRequested.load()) {
        CancelStreamRequest(activeRequest);
        result.stopped = true;
        return result;
    }

    const std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream\r\nAuthorization: Bearer " + apiKey + L"\r\n";
    const std::string body = BuildRequest(model, messages);
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        if (stopRequested.load()) { result.stopped = true; return result; }
        result.error = L"The Cerebras request failed: " + WinError(GetLastError()); return result;
    }

    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        if (stopRequested.load()) { result.stopped = true; return result; }
        result.error = L"Could not read the Cerebras response status: " + WinError(GetLastError()); return result;
    }

    std::string pending, responseBody;
    bool done = false;
    while (!done && !stopRequested.load()) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            if (stopRequested.load()) { result.stopped = true; return result; }
            result.error = L"Reading the response failed: " + WinError(GetLastError()); return result;
        }
        if (available == 0) break;
        std::string block(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), &block[0], available, &read)) {
            if (stopRequested.load()) { result.stopped = true; return result; }
            result.error = L"Reading the response failed: " + WinError(GetLastError()); return result;
        }
        block.resize(read);
        if (status < 200 || status >= 300) { responseBody += block; continue; }
        pending += block;
        size_t lineEnd = 0;
        while ((lineEnd = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, lineEnd);
            pending.erase(0, lineEnd + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 5, "data:") != 0) continue;
            std::string data = line.substr(5);
            while (!data.empty() && std::isspace(static_cast<unsigned char>(data.front()))) data.erase(data.begin());
            if (data == "[DONE]") { done = true; break; }
            std::wstring content;
            if (ExtractContent(data, content) && !content.empty()) onChunk(content);
        }
    }
    if (stopRequested.load()) { result.stopped = true; return result; }
    if (status < 200 || status >= 300) {
        std::wstringstream text;
        text << L"Cerebras returned HTTP " << status << L": " << ExtractError(responseBody);
        result.error = text.str(); return result;
    }
    result.ok = true;
    return result;
}
