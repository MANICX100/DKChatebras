#include "markdown.h"
#include <cwctype>
#include <richedit.h>
#include <vector>

namespace {
enum class Style {
    Bold, Italic, BoldItalic, Strike, Code, CodeBlock,
    Heading1, Heading2, Heading3, Heading4, Heading5, Heading6,
    Link, Quote
};
struct Span { LONG start; LONG length; Style style; };

void AddSpan(std::vector<Span>& spans, size_t start, size_t end, Style style) {
    if (end > start)
        spans.push_back({static_cast<LONG>(start), static_cast<LONG>(end - start), style});
}

void Apply(HWND edit, LONG start, LONG length, DWORD effects, DWORD mask,
           COLORREF color = 0, COLORREF background = 0,
           const wchar_t* face = nullptr, LONG height = 0) {
    CHARRANGE range{start, start + length};
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = mask;
    format.dwEffects = effects;
    if (mask & CFM_COLOR) format.crTextColor = color;
    if (mask & CFM_BACKCOLOR) format.crBackColor = background;
    if ((mask & CFM_FACE) && face) wcsncpy_s(format.szFaceName, face, _TRUNCATE);
    if (mask & CFM_SIZE) format.yHeight = height;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
}

size_t FindClosing(const std::wstring& input, const std::wstring& marker, size_t from) {
    size_t position = from;
    while ((position = input.find(marker, position)) != std::wstring::npos) {
        size_t slashes = 0;
        for (size_t i = position; i > 0 && input[i - 1] == L'\\'; --i) ++slashes;
        if ((slashes & 1U) == 0) return position;
        position += marker.size();
    }
    return std::wstring::npos;
}

void ParseInline(const std::wstring& input, std::wstring& output, std::vector<Span>& spans) {
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == L'\\' && i + 1 < input.size() && iswpunct(input[i + 1])) {
            output.push_back(input[i + 1]);
            i += 2;
            continue;
        }

        if (input[i] == L'`') {
            size_t ticks = 1;
            while (i + ticks < input.size() && input[i + ticks] == L'`') ++ticks;
            const std::wstring marker(ticks, L'`');
            const size_t end = FindClosing(input, marker, i + ticks);
            if (end != std::wstring::npos) {
                const size_t start = output.size();
                output.append(input, i + ticks, end - i - ticks);
                AddSpan(spans, start, output.size(), Style::Code);
                i = end + ticks;
                continue;
            }
        }

        if (i + 1 < input.size() && input[i] == L'!' && input[i + 1] == L'[') {
            const size_t close = input.find(L']', i + 2);
            if (close != std::wstring::npos && close + 1 < input.size() && input[close + 1] == L'(') {
                const size_t end = input.find(L')', close + 2);
                if (end != std::wstring::npos) {
                    output.append(input, i + 2, close - i - 2);
                    i = end + 1;
                    continue;
                }
            }
        }

        if (input[i] == L'[') {
            const size_t close = input.find(L']', i + 1);
            if (close != std::wstring::npos && close + 1 < input.size() && input[close + 1] == L'(') {
                const size_t end = input.find(L')', close + 2);
                if (end != std::wstring::npos) {
                    ParseInline(input.substr(i + 1, close - i - 1), output, spans);
                    const std::wstring url = input.substr(close + 2, end - close - 2);
                    if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) {
                        output += L" (";
                        const size_t linkStart = output.size();
                        output += url;
                        AddSpan(spans, linkStart, output.size(), Style::Link);
                        output += L')';
                    }
                    i = end + 1;
                    continue;
                }
            }
        }

        struct Marker { const wchar_t* text; Style style; };
        static constexpr Marker markers[] = {
            {L"***", Style::BoldItalic}, {L"___", Style::BoldItalic},
            {L"**", Style::Bold}, {L"__", Style::Bold},
            {L"~~", Style::Strike}, {L"*", Style::Italic}, {L"_", Style::Italic}
        };
        bool matched = false;
        for (const auto& marker : markers) {
            const std::wstring token(marker.text);
            if (input.compare(i, token.size(), token) != 0) continue;
            const size_t end = FindClosing(input, token, i + token.size());
            if (end == std::wstring::npos || end == i + token.size()) continue;
            const size_t start = output.size();
            ParseInline(input.substr(i + token.size(), end - i - token.size()), output, spans);
            AddSpan(spans, start, output.size(), marker.style);
            i = end + token.size();
            matched = true;
            break;
        }
        if (matched) continue;
        output.push_back(input[i++]);
    }
}

bool IsHorizontalRule(const std::wstring& line) {
    wchar_t marker = 0;
    int count = 0;
    for (wchar_t character : line) {
        if (iswspace(character)) continue;
        if (character != L'-' && character != L'*' && character != L'_') return false;
        if (marker == 0) marker = character;
        if (character != marker) return false;
        ++count;
    }
    return count >= 3;
}

bool StripListPrefix(std::wstring& line) {
    size_t indent = 0;
    while (indent < line.size() && iswspace(line[indent])) ++indent;
    if (indent + 1 < line.size() &&
        (line[indent] == L'-' || line[indent] == L'*' || line[indent] == L'+') &&
        iswspace(line[indent + 1])) {
        line = std::wstring(indent / 2, L' ') + L"• " + line.substr(indent + 2);
        return true;
    }
    size_t digit = indent;
    while (digit < line.size() && iswdigit(line[digit])) ++digit;
    if (digit > indent && digit + 1 < line.size() &&
        (line[digit] == L'.' || line[digit] == L')') && iswspace(line[digit + 1])) {
        line = std::wstring(indent / 2, L' ') + line.substr(indent, digit - indent + 1) + L" " + line.substr(digit + 2);
        return true;
    }
    return false;
}

std::wstring Trim(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return L"";
    const size_t last = value.find_last_not_of(L" \t");
    return value.substr(first, last - first + 1);
}

bool IsTableSeparator(const std::wstring& line) {
    std::wstring value = Trim(line);
    if (!value.empty() && value.front() == L'|') value.erase(value.begin());
    if (!value.empty() && value.back() == L'|') value.pop_back();
    size_t start = 0;
    int columns = 0;
    while (start <= value.size()) {
        const size_t end = value.find(L'|', start);
        std::wstring cell = Trim(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!cell.empty() && cell.front() == L':') cell.erase(cell.begin());
        if (!cell.empty() && cell.back() == L':') cell.pop_back();
        cell = Trim(cell);
        if (cell.size() < 3 || cell.find_first_not_of(L'-') != std::wstring::npos) return false;
        ++columns;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return columns > 0;
}

void ParseTableRow(const std::wstring& line, std::wstring& output, std::vector<Span>& spans, bool header) {
    std::wstring value = Trim(line);
    if (!value.empty() && value.front() == L'|') value.erase(value.begin());
    if (!value.empty() && value.back() == L'|') value.pop_back();
    const size_t rowStart = output.size();
    size_t start = 0;
    bool first = true;
    while (start <= value.size()) {
        const size_t end = value.find(L'|', start);
        if (!first) output += L'\t';
        ParseInline(Trim(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start)), output, spans);
        first = false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (header) AddSpan(spans, rowStart, output.size(), Style::Bold);
}

void ReplaceAll(std::wstring& text, const std::wstring& from, const std::wstring& to) {
    size_t position = 0;
    while ((position = text.find(from, position)) != std::wstring::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

bool EqualsIgnoreCase(const std::wstring& text, size_t position, const wchar_t* token) {
    for (size_t i = 0; token[i] != L'\0'; ++i) {
        if (position + i >= text.size() || towlower(text[position + i]) != token[i]) return false;
    }
    return true;
}

// Maps bare LaTeX macros to display text; wrapper macros map to "" and keep their argument.
const wchar_t* SymbolFor(const std::wstring& name) {
    static constexpr struct { const wchar_t* name; const wchar_t* symbol; } table[] = {
        {L"times", L"\u00d7"}, {L"div", L"\u00f7"}, {L"cdot", L"\u00b7"}, {L"pm", L"\u00b1"}, {L"mp", L"\u2213"},
        {L"approx", L"\u2248"}, {L"le", L"\u2264"}, {L"leq", L"\u2264"}, {L"ge", L"\u2265"}, {L"geq", L"\u2265"},
        {L"ne", L"\u2260"}, {L"neq", L"\u2260"}, {L"sim", L"~"}, {L"propto", L"\u221d"}, {L"infty", L"\u221e"},
        {L"to", L"\u2192"}, {L"rightarrow", L"\u2192"}, {L"leftarrow", L"\u2190"}, {L"Rightarrow", L"\u21d2"},
        {L"Leftarrow", L"\u21d0"}, {L"leftrightarrow", L"\u2194"}, {L"sum", L"\u03a3"}, {L"prod", L"\u03a0"},
        {L"int", L"\u222b"}, {L"sqrt", L"\u221a"}, {L"ldots", L"\u2026"}, {L"dots", L"\u2026"}, {L"cdots", L"\u22ef"},
        {L"quad", L" "}, {L"qquad", L"  "}, {L"left", L""}, {L"right", L""}, {L"text", L""}, {L"mathrm", L""},
        {L"mathbf", L""}, {L"mathit", L""}, {L"operatorname", L""}, {L"displaystyle", L""}, {L"boxed", L""},
        {L"alpha", L"\u03b1"}, {L"beta", L"\u03b2"}, {L"gamma", L"\u03b3"}, {L"delta", L"\u03b4"},
        {L"epsilon", L"\u03b5"}, {L"zeta", L"\u03b6"}, {L"eta", L"\u03b7"}, {L"theta", L"\u03b8"},
        {L"lambda", L"\u03bb"}, {L"mu", L"\u03bc"}, {L"nu", L"\u03bd"}, {L"xi", L"\u03be"}, {L"pi", L"\u03c0"},
        {L"rho", L"\u03c1"}, {L"sigma", L"\u03c3"}, {L"tau", L"\u03c4"}, {L"phi", L"\u03c6"}, {L"chi", L"\u03c7"},
        {L"psi", L"\u03c8"}, {L"omega", L"\u03c9"}, {L"Gamma", L"\u0393"}, {L"Delta", L"\u0394"},
        {L"Theta", L"\u0398"}, {L"Lambda", L"\u039b"}, {L"Pi", L"\u03a0"}, {L"Sigma", L"\u03a3"},
        {L"Phi", L"\u03a6"}, {L"Psi", L"\u03a8"}, {L"Omega", L"\u03a9"},
    };
    for (const auto& entry : table)
        if (name == entry.name) return entry.symbol;
    return nullptr;
}

size_t MatchBrace(const std::wstring& text, size_t open) {
    int depth = 0;
    for (size_t j = open; j < text.size(); ++j) {
        if (text[j] == L'\\') { ++j; continue; }
        if (text[j] == L'{') ++depth;
        else if (text[j] == L'}' && --depth == 0) return j;
    }
    return std::wstring::npos;
}

// Generically unwraps LaTeX macros, scripts, and HTML breaks in prose text.
std::wstring StripMath(const std::wstring& segment) {
    std::wstring text = segment;
    for (int pass = 0; pass < 8; ++pass) {
        std::wstring out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            const wchar_t c = text[i];
            if (c == L'<' && EqualsIgnoreCase(text, i, L"<br")) {
                const size_t close = text.find(L'>', i + 3);
                if (close != std::wstring::npos && close - i <= 6) { out += L"\r\n"; i = close + 1; continue; }
            }
            if (c == L'\\' && i + 1 < text.size()) {
                const wchar_t next = text[i + 1];
                if (iswalpha(next)) {
                    size_t wordEnd = i + 1;
                    while (wordEnd < text.size() && iswalpha(text[wordEnd])) ++wordEnd;
                    const std::wstring name = text.substr(i + 1, wordEnd - i - 1);
                    if (wordEnd < text.size() && text[wordEnd] == L'{') {
                        const size_t close = MatchBrace(text, wordEnd);
                        if (close != std::wstring::npos) {
                            const std::wstring argument = text.substr(wordEnd + 1, close - wordEnd - 1);
                            if ((name == L"frac" || name == L"dfrac" || name == L"tfrac") &&
                                close + 1 < text.size() && text[close + 1] == L'{') {
                                const size_t close2 = MatchBrace(text, close + 1);
                                if (close2 != std::wstring::npos) {
                                    out += argument;
                                    out += L'\u2044';
                                    out += text.substr(close + 2, close2 - close - 2);
                                    i = close2 + 1;
                                    continue;
                                }
                            }
                            if (const wchar_t* symbol = SymbolFor(name)) out += symbol;
                            out += argument;
                            i = close + 1;
                            continue;
                        }
                    }
                    if (const wchar_t* symbol = SymbolFor(name)) { out += symbol; i = wordEnd; continue; }
                    out.append(text, i, wordEnd - i);
                    i = wordEnd;
                    continue;
                }
                if (next == L'[' || next == L']' || next == L'(' || next == L')') { i += 2; continue; }
                if (next == L';' || next == L',' || next == L':' || next == L'!') { out += L' '; i += 2; continue; }
            }
            if ((c == L'_' || c == L'^') && i + 1 < text.size() && text[i + 1] == L'{') {
                const size_t close = MatchBrace(text, i + 1);
                if (close != std::wstring::npos) {
                    const std::wstring argument = text.substr(i + 2, close - i - 2);
                    if (argument.size() <= 3) out += argument;
                    else { out += L" ("; out += argument; out += L')'; }
                    i = close + 1;
                    continue;
                }
            }
            out += c;
            ++i;
        }
        ReplaceAll(out, L"$$", L"");
        if (out == text) break;
        text = out;
    }
    return text;
}

// Applies StripMath outside fenced code blocks and inline code spans.
std::wstring NormalizeMarkdown(const std::wstring& markdown) {
    std::wstring result;
    result.reserve(markdown.size());
    bool fenced = false;
    size_t position = 0;
    while (position <= markdown.size()) {
        size_t end = markdown.find(L'\n', position);
        const bool last = end == std::wstring::npos;
        if (last) end = markdown.size();
        std::wstring line = markdown.substr(position, end - position);
        const bool hadReturn = !line.empty() && line.back() == L'\r';
        if (hadReturn) line.pop_back();

        const std::wstring trimmed = Trim(line);
        if (trimmed.rfind(L"```", 0) == 0 || trimmed.rfind(L"~~~", 0) == 0) {
            fenced = !fenced;
            result += line;
        } else if (fenced) {
            result += line;
        } else {
            size_t i = 0;
            bool code = false;
            for (;;) {
                const size_t tick = line.find(L'`', i);
                const std::wstring part = line.substr(i, tick == std::wstring::npos ? std::wstring::npos : tick - i);
                result += code ? part : StripMath(part);
                if (tick == std::wstring::npos) break;
                result += L'`';
                code = !code;
                i = tick + 1;
            }
        }
        if (hadReturn) result += L'\r';
        if (last) break;
        result += L'\n';
        position = end + 1;
    }
    return result;
}

Style HeadingStyle(size_t level) {
    switch (level) {
    case 1: return Style::Heading1;
    case 2: return Style::Heading2;
    case 3: return Style::Heading3;
    case 4: return Style::Heading4;
    case 5: return Style::Heading5;
    default: return Style::Heading6;
    }
}
}

void SetRichEditPlainText(HWND richEdit, const std::wstring& text) {
    SETTEXTEX set{ST_DEFAULT, 1200};
    SendMessageW(richEdit, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&set), reinterpret_cast<LPARAM>(text.c_str()));
}

void AppendRichEditPlainText(HWND richEdit, const std::wstring& text) {
    CHARRANGE end{-1, -1};
    SendMessageW(richEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    SendMessageW(richEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(richEdit, EM_SCROLLCARET, 0, 0);
}

void RenderMarkdown(HWND richEdit, const std::wstring& source, bool darkMode) {
    const std::wstring markdown = NormalizeMarkdown(source);
    std::wstring output;
    std::vector<Span> spans;
    bool codeBlock = false;
    bool tableMode = false;
    size_t position = 0;

    while (position <= markdown.size()) {
        size_t end = markdown.find(L'\n', position);
        const bool last = end == std::wstring::npos;
        if (last) end = markdown.size();
        std::wstring line = markdown.substr(position, end - position);
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        std::wstring nextLine;
        if (!last) {
            const size_t nextStart = end + 1;
            size_t nextEnd = markdown.find(L'\n', nextStart);
            if (nextEnd == std::wstring::npos) nextEnd = markdown.size();
            nextLine = markdown.substr(nextStart, nextEnd - nextStart);
            if (!nextLine.empty() && nextLine.back() == L'\r') nextLine.pop_back();
        }
        const bool beginsTable = line.find(L'|') != std::wstring::npos && IsTableSeparator(nextLine);
        const bool tableRow = line.find(L'|') != std::wstring::npos && (tableMode || beginsTable);
        const std::wstring trimmed = Trim(line);
        if (trimmed.rfind(L"```", 0) == 0 || trimmed.rfind(L"~~~", 0) == 0) {
            codeBlock = !codeBlock;
        } else if (codeBlock) {
            const size_t start = output.size();
            output += line;
            AddSpan(spans, start, output.size(), Style::CodeBlock);
            if (!last) output += L'\r';
        } else if (tableMode && IsTableSeparator(line)) {
            // The GFM separator row controls table structure but is not displayed.
        } else if (tableRow) {
            ParseTableRow(line, output, spans, beginsTable);
            if (!last) output += L'\r';
            tableMode = true;
        } else if (IsHorizontalRule(line)) {
            tableMode = false;
            output += L"────────────────────────────────────────";
            if (!last) output += L'\r';
        } else {
            tableMode = false;
            size_t heading = 0;
            while (heading < line.size() && heading < 6 && line[heading] == L'#') ++heading;
            if (heading == 0 || heading >= line.size() || !iswspace(line[heading])) heading = 0;

            bool quote = false;
            size_t quotePrefix = 0;
            while (quotePrefix + 1 < line.size() && line[quotePrefix] == L'>' && iswspace(line[quotePrefix + 1])) {
                quote = true;
                quotePrefix += 2;
            }
            if (quote) line = L"│ " + line.substr(quotePrefix);
            StripListPrefix(line);

            const size_t start = output.size();
            ParseInline(heading ? line.substr(heading + 1) : line, output, spans);
            if (heading) AddSpan(spans, start, output.size(), HeadingStyle(heading));
            if (quote) AddSpan(spans, start, output.size(), Style::Quote);
            if (!last) output += L'\r';
        }
        if (last) break;
        position = end + 1;
    }

    SetRichEditPlainText(richEdit, output);
    CHARRANGE all{0, -1};
    SendMessageW(richEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&all));
    CHARFORMAT2W base{};
    base.cbSize = sizeof(base);
    base.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BACKCOLOR |
                  CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_LINK;
    base.dwEffects = 0;
    base.yHeight = 210;
    base.crTextColor = darkMode ? RGB(229, 231, 235) : RGB(31, 41, 55);
    base.crBackColor = darkMode ? RGB(24, 24, 27) : RGB(250, 250, 252);
    wcscpy_s(base.szFaceName, L"Segoe UI");
    SendMessageW(richEdit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&base));

    const COLORREF codeText = darkMode ? RGB(232, 226, 255) : RGB(43, 35, 75);
    const COLORREF codeBackground = darkMode ? RGB(38, 38, 45) : RGB(235, 231, 242);
    for (const auto& span : spans) {
        switch (span.style) {
        case Style::Bold: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD); break;
        case Style::Italic: Apply(richEdit, span.start, span.length, CFE_ITALIC, CFM_ITALIC); break;
        case Style::BoldItalic: Apply(richEdit, span.start, span.length, CFE_BOLD | CFE_ITALIC, CFM_BOLD | CFM_ITALIC); break;
        case Style::Strike: Apply(richEdit, span.start, span.length, CFE_STRIKEOUT, CFM_STRIKEOUT); break;
        case Style::Code:
        case Style::CodeBlock: Apply(richEdit, span.start, span.length, 0, CFM_FACE | CFM_COLOR | CFM_BACKCOLOR, codeText, codeBackground, L"Consolas"); break;
        case Style::Heading1: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 390); break;
        case Style::Heading2: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 330); break;
        case Style::Heading3: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 285); break;
        case Style::Heading4: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 250); break;
        case Style::Heading5: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 225); break;
        case Style::Heading6: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE | CFM_COLOR, darkMode ? RGB(175, 178, 190) : RGB(78, 84, 98), 0, nullptr, 210); break;
        case Style::Link: Apply(richEdit, span.start, span.length, CFE_LINK, CFM_LINK | CFM_COLOR, darkMode ? RGB(130, 160, 255) : RGB(37, 99, 235)); break;
        case Style::Quote: Apply(richEdit, span.start, span.length, CFE_ITALIC, CFM_ITALIC | CFM_COLOR, darkMode ? RGB(180, 184, 198) : RGB(86, 91, 104)); break;
        }
    }
    CHARRANGE finish{-1, -1};
    SendMessageW(richEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&finish));
    SendMessageW(richEdit, EM_SCROLLCARET, 0, 0);
}

void RenderMarkdownTranscript(HWND richEdit, const std::vector<MarkdownMessage>& messages, bool darkMode) {
    std::wstring markdown;
    for (const auto& message : messages) {
        markdown += L"## ";
        markdown += message.author;
        markdown += L"\r\n\r\n";
        markdown += message.content;
        markdown += L"\r\n\r\n";
    }
    RenderMarkdown(richEdit, markdown, darkMode);
}
