#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <climits>
#include <string>
#include <string_view>

namespace QuickDns::Encoding {

    inline std::wstring utf8_to_wide(std::string_view utf8_str) {
        if (utf8_str.empty() || utf8_str.size() > static_cast<size_t>(INT_MAX)) {
            return L"";
        }
        const int src_len = static_cast<int>(utf8_str.size());
        int len = MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8_str.data(),
            src_len,
            nullptr,
            0
        );
        if (len <= 0) {
            return L"";
        }
        std::wstring result(static_cast<size_t>(len), L'\0');
        int written = MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8_str.data(),
            src_len,
            result.data(),
            len
        );
        if (written <= 0) {
            return L"";
        }
        return result;
    }

    inline std::string wide_to_utf8(std::wstring_view wide_str) {
        if (wide_str.empty() || wide_str.size() > static_cast<size_t>(INT_MAX)) {
            return "";
        }
        const int src_len = static_cast<int>(wide_str.size());
        int len = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide_str.data(),
            src_len,
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (len <= 0) {
            return "";
        }
        std::string result(static_cast<size_t>(len), '\0');
        int written = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide_str.data(),
            src_len,
            result.data(),
            len,
            nullptr,
            nullptr
        );
        if (written <= 0) {
            return "";
        }
        return result;
    }

} // namespace QuickDns::Encoding
