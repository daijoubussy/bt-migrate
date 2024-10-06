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

#include "TransmissionStateStore.h"

#include <Common/Logger.h>
#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <jsoncons/json.hpp>
#include <pugixml.hpp>
#include <tuple>

#include "Common/Exception.h"
#include "Common/IFileStreamProvider.h"
#include "Common/IForwardIterator.h"
#include "Common/Util.h"
#include "Torrent/Box.h"
#include "Torrent/BoxHelper.h"

namespace fs = std::filesystem;

namespace {
    namespace Detail {

        namespace ResumeField {

            std::string const AddedDate = "added-date";
            std::string const Corrupt = "corrupt";
            std::string const Destination = "destination";
            std::string const Dnd = "dnd";
            std::string const DoneDate = "done-date";
            std::string const Downloaded = "downloaded";
            std::string const Name = "name";
            std::string const Paused = "paused";
            std::string const Priority = "priority";
            std::string const Progress = "progress";
            std::string const RatioLimit = "ratio-limit";
            std::string const SpeedLimitDown = "speed-limit-down";
            std::string const SpeedLimitUp = "speed-limit-up";
            std::string const Uploaded = "uploaded";

            namespace ProgressField {

                std::string const Blocks = "blocks";
                std::string const Have = "have";
                std::string const TimeChecked = "time-checked";

            }  // namespace ProgressField

            namespace RatioLimitField {

                std::string const RatioMode = "ratio-mode";
                std::string const RatioLimit = "ratio-limit";

            }  // namespace RatioLimitField

            namespace SpeedLimitField {

                std::string const SpeedBps = "speed-Bps";
                std::string const UseGlobalSpeedLimit = "use-global-speed-limit";
                std::string const UseSpeedLimit = "use-speed-limit";

            }  // namespace SpeedLimitField

        }  // namespace ResumeField

        enum Priority { MinPriority = -1, MaxPriority = 1 };

        std::string const CommonDataDirName = "transmission";
        std::string const DaemonDataDirName = "transmission-daemon";
        std::string const MacDataDirName = "Transmission";

        std::uint32_t const BlockSize = 16 * 1024;

        fs::path GetResumeDir(fs::path const &dataDir, TransmissionStateType stateType) { return dataDir / (stateType == TransmissionStateType::Mac ? "Resume" : "resume"); }

        fs::path GetResumeFilePath(fs::path const &dataDir, std::string const &basename, TransmissionStateType stateType) {
            return GetResumeDir(dataDir, stateType) / (basename + ".resume");
        }

        fs::path GetTorrentsDir(fs::path const &dataDir, TransmissionStateType stateType) { return dataDir / (stateType == TransmissionStateType::Mac ? "Torrents" : "torrents"); }

        fs::path GetTorrentFilePath(fs::path const &dataDir, std::string const &basename, TransmissionStateType stateType) {
            return GetTorrentsDir(dataDir, stateType) / (basename + ".torrent");
        }

        fs::path GetMacTransfersFilePath(fs::path const &dataDir) { return dataDir / "Transfers.plist"; }

    }  // namespace Detail

    ojson ToStoreDoNotDownload(std::vector<Box::FileInfo> const &files) {
        ojson result = ojson::array();
        for (Box::FileInfo const &file : files) {
            result.push_back(file.DoNotDownload ? 1 : 0);
        }
        return result;
    }

    ojson ToStorePriority(std::vector<Box::FileInfo> const &files) {
        ojson result = ojson::array();
        for (Box::FileInfo const &file : files) {
            result.push_back(BoxHelper::Priority::ToStore(file.Priority, Detail::MinPriority, Detail::MaxPriority));
        }
        return result;
    }

    ojson ToStoreProgress(std::vector<bool> const &validBlocks, std::uint32_t blockSize, std::uint64_t totalSize, std::size_t fileCount) {
        namespace RPField = Detail::ResumeField::ProgressField;

        std::size_t const validBlockCount = std::count(validBlocks.begin(), validBlocks.end(), true);

        ojson result = ojson::object();
        if (validBlockCount == validBlocks.size()) {
            result[RPField::Blocks] = "all";
            result[RPField::Have] = "all";
        } else if (validBlockCount == 0) {
            result[RPField::Blocks] = "none";
        } else {
            std::uint32_t const trBlocksPerBlock = blockSize / Detail::BlockSize;

            std::string trBlocks;
            trBlocks.reserve((validBlocks.size() * trBlocksPerBlock + 7) / 8);

            std::uint8_t blockPack = 0;
            std::int8_t blockPackShift = 7;
            for (bool const isValidBlock : validBlocks) {
                for (std::uint32_t i = 0; i < trBlocksPerBlock; ++i) {
                    blockPack |= (isValidBlock ? 1 : 0) << blockPackShift;
                    if (--blockPackShift < 0) {
                        trBlocks += static_cast<char>(blockPack);
                        blockPack = 0;
                        blockPackShift = 7;
                    }
                }
            }

            if (blockPackShift < 7) {
                trBlocks += static_cast<char>(blockPack);
            }

            trBlocks.resize(((totalSize + Detail::BlockSize - 1) / Detail::BlockSize + 7) / 8);

            result[RPField::Blocks] = trBlocks;
        }

        std::int64_t const timeChecked = std::time(nullptr);
        result[RPField::TimeChecked] = ojson::array();
        for (std::size_t i = 0; i < fileCount; ++i) {
            result[RPField::TimeChecked].push_back(timeChecked);
        }

        return result;
    }

    ojson ToStoreRatioLimit(Box::LimitInfo const &boxLimit) {
        namespace RRLField = Detail::ResumeField::RatioLimitField;

        ojson result = ojson::object();
        result[RRLField::RatioMode] = boxLimit.Mode == Box::LimitMode::Inherit ? 0 : (boxLimit.Mode == Box::LimitMode::Enabled ? 1 : 2);
        result[RRLField::RatioLimit] = fmt::format("{:.06f}", boxLimit.Value);
        return result;
    }

    ojson ToStoreSpeedLimit(Box::LimitInfo const &boxLimit) {
        namespace RSLField = Detail::ResumeField::SpeedLimitField;

        ojson result = ojson::object();
        result[RSLField::SpeedBps] = static_cast<int>(boxLimit.Value);
        result[RSLField::UseGlobalSpeedLimit] = boxLimit.Mode != Box::LimitMode::Disabled ? 1 : 0;
        result[RSLField::UseSpeedLimit] = boxLimit.Mode == Box::LimitMode::Enabled ? 1 : 0;
        return result;
    }

    std::tuple<pugi::xml_document, pugi::xml_node> CreateMacPropertyList() {
        pugi::xml_document doc;

        auto xmlDecl = doc.append_child(pugi::node_declaration);
        xmlDecl.append_attribute("version") = "1.0";
        xmlDecl.append_attribute("encoding") = "UTF-8";

        auto docType = doc.append_child(pugi::node_doctype);
        docType.set_value(R"(plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd")");

        auto plist = doc.append_child("plist");
        plist.append_attribute("version") = "1.0";

        return {std::move(doc), std::move(plist)};
    }

    void ToMacStoreTransfer(Box const &box, fs::path const &torrentFilePath, pugi::xml_node &transfer) {
        transfer.append_child("key").text() = "Active";
        transfer.append_child(box.IsPaused ? "false" : "true");

        transfer.append_child("key").text() = "GroupValue";
        transfer.append_child("integer").text() = "-1";

        transfer.append_child("key").text() = "InternalTorrentPath";
        transfer.append_child("string").text() = torrentFilePath.string().c_str();

        transfer.append_child("key").text() = "RemoveWhenFinishedSeeding";
        transfer.append_child("false");

        transfer.append_child("key").text() = "TorrentHash";
        transfer.append_child("string").text() = box.Torrent.GetInfoHash().c_str();

        transfer.append_child("key").text() = "WaitToStart";
        transfer.append_child("false");
    }

}  // namespace

namespace {

    class TransmissionTorrentStateIterator : public ITorrentStateIterator {
       public:
        TransmissionTorrentStateIterator(fs::path const &resumeDir, fs::path const &torrentDir, IFileStreamProvider const &fileStreamProvider);

       public:
        // ITorrentStateIterator
        bool GetNext(Box &nextBox) override;

       private:
        bool GetNext(fs::path &resumeFilePath, fs::path &torrentFilePath);

       private:
        fs::path const m_resumeDir;
        fs::path const m_torrentDir;
        IFileStreamProvider const &m_fileStreamProvider;
        fs::directory_iterator m_directoryIt;
        fs::directory_iterator const m_directoryEnd;
        std::mutex m_directoryItMutex;
        BencodeCodec const m_bencoder;
    };

    TransmissionTorrentStateIterator::TransmissionTorrentStateIterator(fs::path const &resumeDir, fs::path const &torrentDir, IFileStreamProvider const &fileStreamProvider)
        : m_resumeDir(resumeDir),
          m_torrentDir(torrentDir),
          m_fileStreamProvider(fileStreamProvider),
          m_directoryIt(m_resumeDir),
          m_directoryEnd(),
          m_directoryItMutex(),
          m_bencoder() {
        //
    }

    bool TransmissionTorrentStateIterator::GetNext(Box &nextBox) {
        namespace RField = Detail::ResumeField;

        fs::path resumeFilePath;
        fs::path torrentFilePath;
        if (!GetNext(resumeFilePath, torrentFilePath)) {
            return false;
        }

        Logger(Logger::Debug) << "[Transmission] Loading " << std::quoted(resumeFilePath.string());

        Box box;

        {
            IReadStreamPtr const stream = m_fileStreamProvider.GetReadStream(torrentFilePath);
            box.Torrent = TorrentInfo::Decode(*stream, m_bencoder);
        }

        ojson resume;
        {
            IReadStreamPtr const stream = m_fileStreamProvider.GetReadStream(resumeFilePath);
            m_bencoder.Decode(*stream, resume);
        }

        // TODO: don't do this, implement incomplete torrent migration.
        {
            namespace RPField = Detail::ResumeField::ProgressField;

            if (resume[RField::Progress][RPField::Blocks].as<std::string>() != "all") {
                Logger(Logger::Debug) << "[Transmission] Exporting in-progress torrents is not supported at this time. Continuing... (" << std::quoted(box.Torrent.GetName())
                                      << ")";
                box.Ignore = true;
                return true;
            }
        }

        Logger(Logger::Debug) << "[Transmission] Transferring resume information (" << std::quoted(box.Torrent.GetName()) << ")";

        box.AddedAt = resume[RField::AddedDate].as<std::time_t>();
        box.CompletedAt = resume[RField::DoneDate].as<std::time_t>();
        box.IsPaused = resume[RField::Paused].as<int>() == 1;
        box.UploadedSize = resume[RField::Uploaded].as<std::uint64_t>();
        box.DownloadedSize = resume[RField::Downloaded].as<std::uint64_t>();
        box.SavePath = Util::GetPath(resume[RField::Destination].as<std::string>()) / resume[RField::Name].as<std::string>();

        Logger(Logger::Debug) << "[Transmission] Migrating files (" << std::quoted(box.Torrent.GetName()) << ")";

        std::uint64_t fileCount = resume[RField::Progress][RField::ProgressField::TimeChecked].size();
        box.Files.reserve(fileCount);

        for (std::uint32_t i = 0; i < fileCount; ++i) {
            Box::FileInfo boxFile;
            boxFile.DoNotDownload = false;
            boxFile.Priority = 0;
            box.Files.push_back(std::move(boxFile));
        }

        Logger(Logger::Debug) << "[Transmission] Migrating block information (" << std::quoted(box.Torrent.GetName()) << ")";

        box.CorruptedSize = resume[RField::Corrupt].as<std::uint64_t>();
        box.BlockSize = box.Torrent.GetPieceSize();
        std::uint64_t const totalSize = box.Torrent.GetTotalSize();
        std::uint64_t const totalBlockCount = (totalSize + box.BlockSize - 1) / box.BlockSize;
        box.ValidBlocks.reserve(totalBlockCount + 8);

        for (std::uint64_t i = 0; i < totalBlockCount; ++i) {
            bool const isPieceValid = true;
            box.ValidBlocks.push_back(isPieceValid);
        }

        box.ValidBlocks.resize(totalBlockCount);

        Logger(Logger::Debug) << "[Transmission] Setting trackers";

        box.Trackers = box.Torrent.GetTrackers();

        nextBox = std::move(box);
        return true;
    }

    bool TransmissionTorrentStateIterator::GetNext(fs::path &resumeFilePath, fs::path &torrentFilePath) {
        std::lock_guard<std::mutex> lock(m_directoryItMutex);

        for (; m_directoryIt != m_directoryEnd; ++m_directoryIt) {
            // Filenames are the same for .resume and .torrent

            resumeFilePath = m_directoryIt->path();
            if (resumeFilePath.extension().string() != ".resume") {
                continue;
            }

            if (!fs::is_regular_file(*m_directoryIt)) {
                Logger(Logger::Warning) << "File " << resumeFilePath << " is not a regular file, skipping";
                continue;
            }

            torrentFilePath = m_torrentDir / (resumeFilePath.stem().string() + ".torrent");
            if (torrentFilePath.extension().string() != ".torrent") {
                continue;
            }

            if (!fs::is_regular_file(torrentFilePath)) {
                Logger(Logger::Warning) << "File " << torrentFilePath << " is not a regular file, skipping";
                continue;
            }

            ++m_directoryIt;
            return true;
        }

        return false;
    }

}  // namespace

TransmissionStateStore::TransmissionStateStore(TransmissionStateType stateType) : m_stateType(stateType), m_bencoder(), m_tranfersPlistMutex() {
    //
}

TransmissionStateStore::~TransmissionStateStore() = default;

TorrentClient::Enum TransmissionStateStore::GetTorrentClient() const { return TorrentClient::Transmission; }

fs::path TransmissionStateStore::GuessDataDir([[maybe_unused]] Intention::Enum intention) const {
#if !defined(_WIN32)

    fs::path const homeDir = Util::GetEnvironmentVariable("HOME", {});
    if (homeDir.empty()) {
        return {};
    }

#if defined(__APPLE__)

    fs::path const appSupportDir = homeDir / "Library" / "Application Support";

    fs::path const macDataDir = appSupportDir / Detail::MacDataDirName;
    if (!homeDir.empty() && IsValidDataDir(macDataDir, intention)) {
        return macDataDir;
    }

#endif

    fs::path const xdgConfigHome = Util::GetEnvironmentVariable("XDG_CONFIG_HOME", {});
    fs::path const xdgConfigDir = !xdgConfigHome.empty() ? xdgConfigHome : homeDir / ".config";

    fs::path const commonDataDir = xdgConfigDir / Detail::CommonDataDirName;
    if (IsValidDataDir(commonDataDir, intention)) {
        return commonDataDir;
    }

    fs::path const daemonDataDir = xdgConfigDir / Detail::DaemonDataDirName;
    if (IsValidDataDir(daemonDataDir, intention)) {
        return daemonDataDir;
    }

#endif

    return {};
}

bool TransmissionStateStore::IsValidDataDir(fs::path const &dataDir, Intention::Enum intention) const {
    if (intention == Intention::Import) {
        return fs::is_directory(dataDir);
    }

    return fs::is_directory(Detail::GetResumeDir(dataDir, m_stateType)) && fs::is_directory(Detail::GetTorrentsDir(dataDir, m_stateType));
}

ITorrentStateIteratorPtr TransmissionStateStore::Export(fs::path const &dataDir, IFileStreamProvider const &fileStreamProvider) const {
    fs::path const resumeDir = Detail::GetResumeDir(dataDir, m_stateType);
    fs::path const torrentDir = Detail::GetTorrentsDir(dataDir, m_stateType);

    Logger(Logger::Debug) << "[Transmission] Loading state & torrents directories";

    return std::make_unique<TransmissionTorrentStateIterator>(resumeDir, torrentDir, fileStreamProvider);
}

void TransmissionStateStore::Import(fs::path const &dataDir, Box const &box, IFileStreamProvider &fileStreamProvider) const {
    namespace RField = Detail::ResumeField;

    if (box.BlockSize % Detail::BlockSize != 0) {
        // See trac #4005.
        throw ImportCancelledException(
            fmt::format("Transmission does not support torrents with piece length "
                        "not multiple of two: {}",
                        box.BlockSize));
    }

    for (Box::FileInfo const &file : box.Files) {
        if (!file.Path.is_relative()) {
            throw ImportCancelledException(
                fmt::format("Transmission does not support moving files outside of "
                            "download directory: {}",
                            file.Path));
        }
    }

    ojson resume = ojson::object();

    // resume["activity-date"] = 0;
    resume[RField::AddedDate] = static_cast<std::int64_t>(box.AddedAt);
    // resume["bandwidth-priority"] = 0;
    resume[RField::Corrupt] = box.CorruptedSize;
    resume[RField::Destination] = box.SavePath.parent_path().string();
    resume[RField::Dnd] = ToStoreDoNotDownload(box.Files);
    resume[RField::DoneDate] = static_cast<std::int64_t>(box.CompletedAt);
    resume[RField::Downloaded] = box.DownloadedSize;
    // resume["downloading-time-seconds"] = 0;
    // resume["idle-limit"] = ojson::object();
    // resume["max-peers"] = 5;
    resume[RField::Name] = box.SavePath.filename().string();
    resume[RField::Paused] = box.IsPaused ? 1 : 0;
    // resume["peers2"] = "";
    resume[RField::Priority] = ToStorePriority(box.Files);
    resume[RField::Progress] = ToStoreProgress(box.ValidBlocks, box.BlockSize, box.Torrent.GetTotalSize(), box.Files.size());
    resume[RField::RatioLimit] = ToStoreRatioLimit(box.RatioLimit);
    // resume["seeding-time-seconds"] = 0;
    resume[RField::SpeedLimitDown] = ToStoreSpeedLimit(box.DownloadSpeedLimit);
    resume[RField::SpeedLimitUp] = ToStoreSpeedLimit(box.UploadSpeedLimit);
    resume[RField::Uploaded] = box.UploadedSize;

    Util::SortJsonObjectKeys(resume);

    TorrentInfo torrent = box.Torrent;
    torrent.SetTrackers(box.Trackers);

    std::string const baseName = Util::GetEnvironmentVariable("BT_MIGRATE_TRANSMISSION_2_9X", {}).empty()
                                     ? torrent.GetInfoHash()
                                     : resume[RField::Name].as_string() + '.' + torrent.GetInfoHash().substr(0, 16);

    fs::path const torrentFilePath = Detail::GetTorrentFilePath(dataDir, baseName, m_stateType);
    fs::create_directories(torrentFilePath.parent_path());

    fs::path const resumeFilePath = Detail::GetResumeFilePath(dataDir, baseName, m_stateType);
    fs::create_directories(resumeFilePath.parent_path());

    if (m_stateType == TransmissionStateType::Mac) {
        fs::path const transfersPlistPath = Detail::GetMacTransfersFilePath(dataDir);

        pugi::xml_document plistDoc;
        pugi::xml_node plistNode;
        pugi::xml_node arrayNode;

        // Avoid concurrent access to Transfers.plist, could lead to file corruption
        std::lock_guard<std::mutex> lock(m_tranfersPlistMutex);

        try {
            IReadStreamPtr const readStream = fileStreamProvider.GetReadStream(transfersPlistPath);
            plistDoc.load(*readStream, pugi::parse_default | pugi::parse_declaration | pugi::parse_doctype);
            plistNode = plistDoc.child("plist");
            arrayNode = plistNode.child("array");
        } catch (Exception const &) {
        }

        if (arrayNode.empty()) {
            std::tie(plistDoc, plistNode) = CreateMacPropertyList();
            arrayNode = plistNode.append_child("array");
        }

        pugi::xml_node dictNode = arrayNode.append_child("dict");
        ToMacStoreTransfer(box, torrentFilePath, dictNode);

        IWriteStreamPtr const writeStream = fileStreamProvider.GetWriteStream(transfersPlistPath);
        plistDoc.save(*writeStream);
    }

    {
        IWriteStreamPtr const stream = fileStreamProvider.GetWriteStream(torrentFilePath);
        torrent.Encode(*stream, m_bencoder);
    }

    {
        IWriteStreamPtr const stream = fileStreamProvider.GetWriteStream(resumeFilePath);
        m_bencoder.Encode(*stream, resume);
    }
}
