#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct ChatMessage {
    std::wstring role;
    std::wstring content;
};

struct ConversationSummary {
    std::wstring id;
    std::wstring title;
    unsigned long long modified = 0;
};

std::wstring AppDataDirectory();
bool EnsureAppDataDirectory(std::wstring& error);
bool SaveApiKey(const std::wstring& key, std::wstring& error);
bool LoadApiKey(std::wstring& key, std::wstring& error);
std::vector<ConversationSummary> ListConversations();
bool LoadConversation(const std::wstring& id, std::vector<ChatMessage>& messages, std::wstring& error);
bool SaveConversation(const std::wstring& id, const std::vector<ChatMessage>& messages, std::wstring& error);
bool DeleteConversation(const std::wstring& id, std::wstring& error);
std::wstring NewConversationId();
std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
