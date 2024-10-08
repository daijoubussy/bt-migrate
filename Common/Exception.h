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

#pragma once

#include <exception>
#include <string>

class Exception : public std::exception {
   public:
    explicit Exception(std::string const &message);
    ~Exception() override;

   public:
    // std::exception
    char const *what() const noexcept override;

   private:
    std::string const m_message;
};

class NotImplementedException : public Exception {
   public:
    explicit NotImplementedException(std::string const &place);
    ~NotImplementedException() override;
};
