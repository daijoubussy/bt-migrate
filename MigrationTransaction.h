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

#include <filesystem>
#include <mutex>
#include <set>
#include <string>

#include "Common/IFileStreamProvider.h"

class MigrationTransaction : public IFileStreamProvider {
   public:
    MigrationTransaction(bool writeThrough, bool dryRun);
    ~MigrationTransaction() noexcept(false) override;

    void Commit();

   public:
    // IFileStreamProvider
    IReadStreamPtr GetReadStream(std::filesystem::path const &path) const override;
    IWriteStreamPtr GetWriteStream(std::filesystem::path const &path) override;

   private:
    std::filesystem::path GetTemporaryPath(std::filesystem::path const &path) const;
    std::filesystem::path GetBackupPath(std::filesystem::path const &path) const;

   private:
    bool const m_writeThrough;
    bool const m_dryRun;
    std::string const m_transactionId;
    std::set<std::filesystem::path> m_safePaths;
    std::mutex m_safePathsMutex;
};
