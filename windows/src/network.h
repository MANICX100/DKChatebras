#pragma once
#include "storage.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct StreamResult {
    bool ok = false;
    bool stopped = false;
    std::wstring error;
};

using StreamChunkCallback = std::function<void(const std::wstring&)>;

void CancelStreamRequest(std::atomic<void*>& activeRequest);

StreamResult StreamChatCompletion(
    const std::wstring& apiKey,
    const std::wstring& model,
    const std::vector<ChatMessage>& messages,
    std::atomic_bool& stopRequested,
    std::atomic<void*>& activeRequest,
    const StreamChunkCallback& onChunk);
