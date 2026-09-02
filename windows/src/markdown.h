#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct MarkdownMessage {
    std::wstring author;
    std::wstring content;
};

void SetRichEditPlainText(HWND richEdit, const std::wstring& text);
void AppendRichEditPlainText(HWND richEdit, const std::wstring& text);
void RenderMarkdown(HWND richEdit, const std::wstring& markdown, bool darkMode);
void RenderMarkdownTranscript(HWND richEdit, const std::vector<MarkdownMessage>& messages, bool darkMode);
