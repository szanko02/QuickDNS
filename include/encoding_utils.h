#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace QuickDns::Encoding {

    inline std::wstring utf8_to_wide(std::string_view utf8_str) {
        if (utf8_str.empty()) {
            return L"";
        }
        int len = MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8_str.data(),
            static_cast<int>(utf8_str.size()),
            nullptr,
            0
        );
        if (len <= 0) {
            return L"";
        }
        std::wstring result(len, L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8_str.data(),
            static_cast<int>(utf8_str.size()),
            result.data(),
            len
        );
        return result;
    }

    inline std::string wide_to_utf8(std::wstring_view wide_str) {
        if (wide_str.empty()) {
            return "";
        }
        int len = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide_str.data(),
            static_cast<int>(wide_str.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (len <= 0) {
            return "";
        }
        std::string result(len, '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            wide_str.data(),
            static_cast<int>(wide_str.size()),
            result.data(),
            len,
            nullptr,
            nullptr
        );
        return result;
    }

} // namespace QuickDns::Encoding
