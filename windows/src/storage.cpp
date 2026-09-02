#include "storage.h"
#include <shlobj.h>
#include <wincrypt.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>

namespace {
constexpr uint32_t kMagic = 0x31434B44; // DKC1
constexpr uint32_t kMaxField = 64u * 1024u * 1024u;

struct FileHeader {
    uint32_t magic;
    uint32_t titleBytes;
    uint32_t messageCount;
};

std::wstring ConversationDirectory() {
    const std::wstring app = AppDataDirectory();
    return app.empty() ? std::wstring() : app + L"\\conversations";
}
std::wstring ConversationPath(const std::wstring& id) {
    const std::wstring directory = ConversationDirectory();
    return directory.empty() ? std::wstring() : directory + L"\\" + id + L".chat";
}

std::wstring ErrorText(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = message ? message : L"Unknown error";
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

bool IsValidId(const std::wstring& id) {
    if (id.empty() || id.size() > 80) return false;
    return std::all_of(id.begin(), id.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || c == L'-';
    });
}

bool WriteU32(std::ofstream& out, uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return !!out;
}

bool ReadU32(std::ifstream& in, uint32_t& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return !!in;
}

std::wstring MakeTitle(const std::vector<ChatMessage>& messages) {
    for (const auto& message : messages) {
        if (message.role != L"user") continue;
        std::wstring title = message.content;
        std::replace(title.begin(), title.end(), L'\r', L' ');
        std::replace(title.begin(), title.end(), L'\n', L' ');
        while (!title.empty() && title.front() == L' ') title.erase(title.begin());
        if (title.size() > 54) title = title.substr(0, 51) + L"...";
        return title.empty() ? L"New conversation" : title;
    }
    return L"New conversation";
}
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), &result[0], size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), &result[0], size);
    return result;
}

std::wstring AppDataDirectory() {
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, path))) return {};
    return std::wstring(path) + L"\\DKChatebras";
}

bool EnsureAppDataDirectory(std::wstring& error) {
    const std::wstring app = AppDataDirectory();
    if (app.empty()) {
        error = L"Windows could not locate the Local AppData directory.";
        return false;
    }
    if (!CreateDirectoryW(app.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"Could not create app data directory: " + ErrorText(GetLastError());
        return false;
    }
    const std::wstring conversations = ConversationDirectory();
    if (!CreateDirectoryW(conversations.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"Could not create conversation directory: " + ErrorText(GetLastError());
        return false;
    }
    return true;
}

bool SaveApiKey(const std::wstring& key, std::wstring& error) {
    std::wstring ignored;
    if (!EnsureAppDataDirectory(ignored)) { error = ignored; return false; }
    std::string utf8 = WideToUtf8(key);
    if (!key.empty() && utf8.empty()) { error = L"The API key contains invalid text."; return false; }
    DATA_BLOB input{static_cast<DWORD>(utf8.size()), reinterpret_cast<BYTE*>(&utf8[0])};
    const char entropyBytes[] = "DKChatebras:CerebrasApiKey:v1";
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(entropyBytes) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"DKChatebras API key", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        SecureZeroMemory(&utf8[0], utf8.size());
        error = L"Windows could not protect the API key: " + ErrorText(GetLastError());
        return false;
    }
    SecureZeroMemory(&utf8[0], utf8.size());
    const std::wstring path = AppDataDirectory() + L"\\credentials.dat";
    const std::wstring temporaryPath = path + L".tmp";
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    file.close();
    const bool wrote = !!file;
    LocalFree(output.pbData);
    if (!wrote) {
        DeleteFileW(temporaryPath.c_str());
        error = L"Could not write protected credentials.";
        return false;
    }
    if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        error = L"Could not replace protected credentials: " + ErrorText(GetLastError());
        return false;
    }
    return true;
}

bool LoadApiKey(std::wstring& key, std::wstring& error) {
    const std::wstring app = AppDataDirectory();
    if (app.empty()) { error = L"Windows could not locate the Local AppData directory."; return false; }
    const std::wstring path = app + L"\\credentials.dat";
    std::ifstream file(path, std::ios::binary);
    if (!file) { key.clear(); return true; }
    std::vector<BYTE> encrypted((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (encrypted.empty() || encrypted.size() > (std::numeric_limits<DWORD>::max)()) {
        error = L"The credentials file is invalid."; return false;
    }
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), encrypted.data()};
    const char entropyBytes[] = "DKChatebras:CerebrasApiKey:v1";
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(entropyBytes) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(entropyBytes))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = L"Windows could not decrypt the saved API key: " + ErrorText(GetLastError()); return false;
    }
    key = Utf8ToWide(std::string(reinterpret_cast<char*>(output.pbData), output.cbData));
    const bool valid = output.cbData == 0 || !key.empty();
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    if (!valid) { error = L"The decrypted API key contains invalid text."; return false; }
    return true;
}

std::vector<ConversationSummary> ListConversations() {
    std::vector<ConversationSummary> result;
    WIN32_FIND_DATAW data{};
    const std::wstring directory = ConversationDirectory();
    if (directory.empty()) return result;
    const std::wstring pattern = directory + L"\\*.chat";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return result;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring filename = data.cFileName;
        if (filename.size() <= 5) continue;
        std::wstring id = filename.substr(0, filename.size() - 5);
        std::ifstream in(ConversationPath(id), std::ios::binary);
        FileHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || header.magic != kMagic || header.titleBytes > 4096) continue;
        std::string title(header.titleBytes, '\0');
        in.read(&title[0], header.titleBytes);
        if (!in) continue;
        ULARGE_INTEGER modified{};
        modified.LowPart = data.ftLastWriteTime.dwLowDateTime;
        modified.HighPart = data.ftLastWriteTime.dwHighDateTime;
        std::wstring wideTitle = Utf8ToWide(title);
        if (!title.empty() && wideTitle.empty()) continue;
        result.push_back({id, std::move(wideTitle), modified.QuadPart});
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.modified > b.modified; });
    return result;
}

bool LoadConversation(const std::wstring& id, std::vector<ChatMessage>& messages, std::wstring& error) {
    messages.clear();
    if (!IsValidId(id)) { error = L"Invalid conversation identifier."; return false; }
    const std::wstring path = ConversationPath(id);
    if (path.empty()) { error = L"Windows could not locate the Local AppData directory."; return false; }
    std::ifstream in(path, std::ios::binary);
    FileHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || header.magic != kMagic || header.titleBytes > 4096 || header.messageCount > 10000) {
        error = L"The conversation file is invalid."; return false;
    }
    in.seekg(header.titleBytes, std::ios::cur);
    for (uint32_t i = 0; i < header.messageCount; ++i) {
        uint32_t roleSize = 0, contentSize = 0;
        if (!ReadU32(in, roleSize) || !ReadU32(in, contentSize) || roleSize > 32 || contentSize > kMaxField) {
            error = L"The conversation file is damaged."; messages.clear(); return false;
        }
        std::string role(roleSize, '\0'), content(contentSize, '\0');
        in.read(&role[0], roleSize);
        in.read(&content[0], contentSize);
        if (!in) { error = L"The conversation file is incomplete."; messages.clear(); return false; }
        std::wstring wideRole = Utf8ToWide(role), wideContent = Utf8ToWide(content);
        if ((!role.empty() && wideRole.empty()) || (!content.empty() && wideContent.empty())) {
            error = L"The conversation file contains invalid text."; messages.clear(); return false;
        }
        messages.push_back({std::move(wideRole), std::move(wideContent)});
    }
    return true;
}

bool SaveConversation(const std::wstring& id, const std::vector<ChatMessage>& messages, std::wstring& error) {
    if (!IsValidId(id)) { error = L"Invalid conversation identifier."; return false; }
    if (messages.size() > 10000) { error = L"The conversation has too many messages to save."; return false; }
    std::wstring ignored;
    if (!EnsureAppDataDirectory(ignored)) { error = ignored; return false; }
    const std::string title = WideToUtf8(MakeTitle(messages));
    const std::wstring finalPath = ConversationPath(id);
    const std::wstring temporaryPath = finalPath + L".tmp";
    std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
    FileHeader header{kMagic, static_cast<uint32_t>(title.size()), static_cast<uint32_t>(messages.size())};
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(title.data(), title.size());
    for (const auto& message : messages) {
        const std::string role = WideToUtf8(message.role);
        const std::string content = WideToUtf8(message.content);
        if ((message.role.size() && role.empty()) || (message.content.size() && content.empty()) ||
            role.size() > 32 || content.size() > kMaxField) {
            out.close(); DeleteFileW(temporaryPath.c_str());
            error = L"A message contains invalid text or is too large to save."; return false;
        }
        WriteU32(out, static_cast<uint32_t>(role.size()));
        WriteU32(out, static_cast<uint32_t>(content.size()));
        out.write(role.data(), role.size());
        out.write(content.data(), content.size());
    }
    out.close();
    if (!out) { DeleteFileW(temporaryPath.c_str()); error = L"Could not save the conversation."; return false; }
    if (!MoveFileExW(temporaryPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str()); error = L"Could not replace the conversation file: " + ErrorText(GetLastError()); return false;
    }
    return true;
}

bool DeleteConversation(const std::wstring& id, std::wstring& error) {
    if (!IsValidId(id)) { error = L"Invalid conversation identifier."; return false; }
    const std::wstring path = ConversationPath(id);
    if (path.empty()) { error = L"Windows could not locate the Local AppData directory."; return false; }
    if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = L"Could not delete the conversation: " + ErrorText(GetLastError()); return false;
    }
    return true;
}

std::wstring NewConversationId() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    std::wstringstream stream;
    stream << std::hex << value.QuadPart << L"-" << GetCurrentProcessId() << L"-" << GetTickCount();
    return stream.str();
}
