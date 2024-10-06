// bt-migrate, torrent state migration tool
// Copyright (C) 2014 Mike Gelfand <mikedld@mikedld.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include "Util.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <digestpp.hpp>
#include <filesystem>
#include <locale>

#include "Exception.h"
#include "Logger.h"

namespace fs = std::filesystem;

using namespace std::string_view_literals;

namespace Util {

    namespace {

        std::string FixPathSeparators(std::string_view nativePath) {
            if (nativePath.size() >= 3 && std::isalpha(nativePath[0]) && nativePath[1] == ':' && (nativePath[2] == '/' || nativePath[2] == '\\')) {
                // Looks like Windows path
                auto result = std::string(nativePath);
                std::replace(result.begin(), result.end(), '\\', '/');
                return result;
            }

            return std::string(nativePath);
        }

    }  // namespace

    long long StringToInt(std::string const &text) {
        errno = 0;
        long long const result = std::strtoll(text.c_str(), nullptr, 10);
        if (result == 0 && errno != 0) {
            throw Exception(fmt::format("Unable to convert \"{}\" to integer", text));
        }

        return result;
    }

    fs::path GetPath(std::string_view nativePath) {
        std::string const fixedPath = FixPathSeparators(nativePath);

        try {
            return fs::path{std::u8string_view{reinterpret_cast<char8_t const *>(fixedPath.data()), fixedPath.size()}};
        } catch (std::exception const &) {
            Logger(Logger::Warning) << "Path " << std::quoted(fixedPath) << " is invalid";
            return fs::path{fixedPath};
        }
    }

    std::string CalculateSha1(std::string const &data) { return digestpp::sha1().absorb(data).hexdigest(); }

    std::string BinaryToHex(std::string const &data) {
        static char const *const HexAlphabet = "0123456789abcdef";

        std::string result;

        for (char const c : data) {
            result += HexAlphabet[(c >> 4) & 0x0f];
            result += HexAlphabet[c & 0x0f];
        }

        return result;
    }

    void SortJsonObjectKeys(ojson &object) {
        std::sort(object.object_range().begin(), object.object_range().end(), [](auto const &lhs, auto const &rhs) { return lhs.key().compare(rhs.key()) < 0; });
    }

    std::string GetEnvironmentVariable(std::string const &name, std::string const &defaultValue) {
        auto const *value = std::getenv(name.c_str());
        return value != nullptr ? value : defaultValue;
    }

    bool IsEqualNoCase(std::string_view lhs, std::string_view rhs, std::locale const &locale) {
        return lhs.size() == rhs.size() &&
               std::equal(lhs.begin(), lhs.end(), rhs.begin(), [&locale](char lhsChar, char rhsChar) { return std::tolower(lhsChar, locale) == std::tolower(rhsChar, locale); });
    }

    std::string_view Trim(std::string_view text) {
        static constexpr auto Blanks = " \t\r\n"sv;

        if (auto const pos = text.find_first_not_of(Blanks); pos != std::string_view::npos) {
            text.remove_prefix(pos);
        }

        if (auto const pos = text.find_last_not_of(Blanks); pos != std::string_view::npos) {
            text.remove_suffix(text.size() - pos - 1);
        }

        return text;
    }

}  // namespace Util
