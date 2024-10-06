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

#include "rTorrentStateStore.h"

#include <fmt/format.h>

#include <filesystem>
#include <fstream>
#include <jsoncons/json.hpp>
#include <locale>
#include <mutex>
#include <string>
#include <string_view>

#include "Codec/BencodeCodec.h"
#include "Common/Exception.h"
#include "Common/IFileStreamProvider.h"
#include "Common/IForwardIterator.h"
#include "Common/Logger.h"
#include "Common/Util.h"
#include "Torrent/Box.h"
#include "Torrent/BoxHelper.h"

namespace fs = std::filesystem;

using namespace std::string_view_literals;

namespace {
    namespace Detail {

        namespace ResumeField {

            // May be the sum of StateField::ChunksDone + StateField::ChunksWanted
            std::string const Bitfield = "bitfield";
            std::string const Files = "files";
            std::string const Trackers = "trackers";

            namespace FileField {

                std::string const Priority = "priority";
                std::string const Completed = "completed";
                std::string const ModTime = "mtime";

            }  // namespace FileField

            namespace PeerField {

                std::string const Failed = "failed";
                std::string const Address = "inet";
                std::string const Last = "last";

            }  // namespace PeerField

            namespace TrackerField {

                std::string const Enabled = "enabled";

            }  // namespace TrackerField

            std::string const UncertainPiecesTimestamp = "uncertain_pieces.timestamp";

        }  // namespace ResumeField

        namespace StateField {

            std::string const ChokeHeuristicsDownLeech = "choke_heuristics.down.leech";
            std::string const ChokeHeuristicsDownSeed = "choke_heuristics.down.seed";
            std::string const ChokeHeuristicsUpLeech = "choke_heuristics.up.leech";
            std::string const ChokeHeuristicsUpSeed = "choke_heuristics.up.seed";
            std::string const ChunksDone = "chunks_done";
            std::string const ChunksWanted = "chunks_wanted";
            std::string const Complete = "complete";
            std::string const ConnectionLeech = "connection_leech";
            std::string const ConnectionSeed = "connection_seed";

            namespace CustomField {

                std::string const AddTime = "addtime";
                std::string const OriginalFilename = "x-filename";

            }  // namespace CustomField

            std::string const Custom1 = "custom1";
            std::string const Custom2 = "custom2";
            std::string const Custom3 = "custom3";
            std::string const Custom4 = "custom4";
            std::string const Custom5 = "custom5";
            std::string const Directory = "directory";
            std::string const Hashing = "hashing";
            std::string const IgnoreCommands = "ignore_commands";
            std::string const Key = "key";
            std::string const LoadedFile = "loaded_file";
            std::string const Priority = "priority";
            std::string const State = "state";
            std::string const StateChanged = "state_changed";
            std::string const StateCounter = "state_counter";
            std::string const ThrottleName = "throttle_name";
            std::string const TiedToFile = "tied_to_file";
            std::string const TimestampFinished = "timestamp.finished";
            std::string const TimestampStarted = "timestamp.started";
            std::string const TotalUploaded = "total_uploaded";
            std::string const TotalDownloaded = "total_downloaded";
            std::string const Views = "views";

        }  // namespace StateField

        std::string const ConfigFilename = ".rtorrent.rc";
        std::string const StateFileExtension = ".rtorrent";
        std::string const LibTorrentStateFileExtension = ".libtorrent_resume";

        std::uint32_t const BlockSize = 16 * 1024;

        enum Priority { DoNotDownloadPriority = 0, MinPriority = -1, MaxPriority = 1 };

    }  // namespace Detail

}  // namespace

namespace {

    class rTorrentTorrentStateIterator : public ITorrentStateIterator {
       public:
        rTorrentTorrentStateIterator(fs::path const &dataDir, IFileStreamProvider const &fileStreamProvider);

       public:
        // ITorrentStateIterator
        bool GetNext(Box &nextBox) override;

       private:
        bool GetNext(fs::path &stateFilePath, fs::path &torrentFilePath, fs::path &libTorrentStateFilePath);

       private:
        fs::path const m_dataDir;
        IFileStreamProvider const &m_fileStreamProvider;
        fs::directory_iterator m_directoryIt;
        fs::directory_iterator const m_directoryEnd;
        std::mutex m_directoryItMutex;
        BencodeCodec const m_bencoder;
    };

    rTorrentTorrentStateIterator::rTorrentTorrentStateIterator(fs::path const &dataDir, IFileStreamProvider const &fileStreamProvider)
        : m_dataDir(dataDir), m_fileStreamProvider(fileStreamProvider), m_directoryIt(m_dataDir), m_directoryEnd(), m_directoryItMutex(), m_bencoder() {
        //
    }

    bool rTorrentTorrentStateIterator::GetNext(Box &nextBox) {
        namespace RField = Detail::ResumeField;
        namespace SField = Detail::StateField;

        fs::path stateFilePath;
        fs::path torrentFilePath;
        fs::path libTorrentStateFilePath;
        if (!GetNext(stateFilePath, torrentFilePath, libTorrentStateFilePath)) {
            return false;
        }

        Box box;

        {
            IReadStreamPtr const stream = m_fileStreamProvider.GetReadStream(torrentFilePath);
            box.Torrent = TorrentInfo::Decode(*stream, m_bencoder);

            std::string const infoHash = torrentFilePath.stem().string();
            if (!Util::IsEqualNoCase(box.Torrent.GetInfoHash(), infoHash, std::locale::classic())) {
                throw Exception(fmt::format("Info hashes don't match: {} vs. {}", box.Torrent.GetInfoHash(), infoHash));
            }
        }

        std::string loggerBase = "[" + TorrentClient::ToString(TorrentClient::rTorrent) + "][" + box.Torrent.GetName() + "] ";

        Logger(Logger::Debug) << loggerBase << "Export started";
        Logger(Logger::Debug) << loggerBase << "Infohash: " << box.Torrent.GetInfoHash();

        ojson state;
        {
            IReadStreamPtr const stream = m_fileStreamProvider.GetReadStream(stateFilePath);
            m_bencoder.Decode(*stream, state);
        }

        ojson resume;
        {
            IReadStreamPtr const stream = m_fileStreamProvider.GetReadStream(libTorrentStateFilePath);
            m_bencoder.Decode(*stream, resume);
        }

        Logger(Logger::Debug) << loggerBase << "Starting state/resume migration";

        box.AddedAt = state[SField::TimestampStarted].as<std::time_t>();
        box.CompletedAt = state[SField::TimestampFinished].as<std::time_t>();
        box.IsPaused = state[SField::Priority].as<int>() == 0;
        box.UploadedSize = state[SField::TotalUploaded].as<std::uint64_t>();

        std::string directory = state[SField::Directory].as<std::string>();

        if (box.Torrent.GetName() != directory) {
            box.SavePath = Util::GetPath(state[SField::Directory].as<std::string>());
        }

        box.BlockSize = box.Torrent.GetPieceSize();

        Logger(Logger::Debug) << loggerBase << "Reserving file count";

        box.Files.reserve(resume[RField::Files].size());

        Logger(Logger::Debug) << loggerBase << "Setting individual file properties";

        for (ojson const &file : resume[RField::Files].array_range()) {
            namespace ff = RField::FileField;

            int const filePriority = file[ff::Priority].as<int>();

            Box::FileInfo boxFile;
            boxFile.DoNotDownload = filePriority == Detail::DoNotDownloadPriority;
            boxFile.Priority = boxFile.DoNotDownload ? Box::NormalPriority : BoxHelper::Priority::FromStore(filePriority - 1, Detail::MinPriority, Detail::MaxPriority);
            box.Files.push_back(std::move(boxFile));
        }

        Logger(Logger::Debug) << loggerBase << "Reserving array space for valid blocks";

        std::uint64_t const totalSize = box.Torrent.GetTotalSize();
        std::uint64_t const totalBlockCount = (totalSize + box.BlockSize - 1) / box.BlockSize;
        box.ValidBlocks.reserve(totalBlockCount + 8);

        Logger(Logger::Debug) << loggerBase << "Checking if torrent is completed";

        for (unsigned char const c : resume[RField::Bitfield].as<std::string>()) {
            for (int i = 7; i >= 0; --i) {
                bool const isPieceValid = (c & (1 << i)) != 0;
                box.ValidBlocks.push_back(isPieceValid);
            }
        }

        box.ValidBlocks.resize(totalBlockCount);

        Logger(Logger::Debug) << loggerBase << "Setting trackers";

        for (auto const &tracker : resume[RField::Trackers].object_range()) {
            namespace tf = RField::TrackerField;

            std::string const url{tracker.key()};
            if (url == "dht://") {
                continue;
            }

            ojson const &params = tracker.value();
            if (params[tf::Enabled].as<int>() == 1) {
                box.Trackers.push_back({url});
            }
        }

        Logger(Logger::Debug) << loggerBase << "Incrementing box pointer";

        nextBox = std::move(box);
        return true;
    }

    bool rTorrentTorrentStateIterator::GetNext(fs::path &stateFilePath, fs::path &torrentFilePath, fs::path &libTorrentStateFilePath) {
        std::lock_guard<std::mutex> lock(m_directoryItMutex);

        for (; m_directoryIt != m_directoryEnd; ++m_directoryIt) {
            stateFilePath = m_directoryIt->path();
            if (stateFilePath.extension().string() != Detail::StateFileExtension) {
                continue;
            }

            if (!fs::is_regular_file(*m_directoryIt)) {
                Logger(Logger::Warning) << "File " << stateFilePath << " is not a regular file, skipping";
                continue;
            }

            torrentFilePath = stateFilePath;
            torrentFilePath.replace_extension(fs::path());
            if (!fs::is_regular_file(torrentFilePath)) {
                Logger(Logger::Warning) << "File " << torrentFilePath << " is not a regular file, skipping";
                continue;
            }

            libTorrentStateFilePath = stateFilePath;
            libTorrentStateFilePath.replace_extension(Detail::LibTorrentStateFileExtension);
            if (!fs::is_regular_file(libTorrentStateFilePath)) {
                Logger(Logger::Warning) << "File " << libTorrentStateFilePath << " is not a regular file, skipping";
                continue;
            }

            ++m_directoryIt;
            return true;
        }

        return false;
    }

}  // namespace

rTorrentStateStore::rTorrentStateStore() = default;
rTorrentStateStore::~rTorrentStateStore() = default;

TorrentClient::Enum rTorrentStateStore::GetTorrentClient() const { return TorrentClient::rTorrent; }

fs::path rTorrentStateStore::GuessDataDir([[maybe_unused]] Intention::Enum intention) const {
#ifndef _WIN32

    fs::path const homeDir = Util::GetEnvironmentVariable("HOME", {});

    if (homeDir.empty() || !fs::is_regular_file(homeDir / Detail::ConfigFilename)) {
        return {};
    }

    auto dataDirPath = fs::path();

    {
        auto stream = std::ifstream();
        stream.exceptions(std::ifstream::badbit | std::ifstream::failbit);
        stream.open(homeDir / Detail::ConfigFilename);

        auto line = std::string();
        while (std::getline(stream, line)) {
            auto const equalsPos = line.find('=');
            if (equalsPos == std::string::npos) {
                continue;
            }

            if (auto const key = Util::Trim(std::string_view(line).substr(0, equalsPos)); key != "session"sv) {
                continue;
            }

            dataDirPath = Util::GetPath(Util::Trim(std::string_view(line).substr(equalsPos + 1)));
            break;
        }
    }

    if (!dataDirPath.empty() && IsValidDataDir(dataDirPath, intention)) {
        return dataDirPath;
    }

#endif

    return {};
}

bool rTorrentStateStore::IsValidDataDir(fs::path const &dataDir, Intention::Enum intention) const {
    if (intention == Intention::Import) {
        return fs::is_directory(dataDir);
    }

    for (fs::directory_iterator it(dataDir), end; it != end; ++it) {
        fs::path path = it->path();
        if (path.extension() != Detail::StateFileExtension || !fs::is_regular_file(it->status())) {
            continue;
        }

        if (!fs::is_regular_file(path.replace_extension(Detail::LibTorrentStateFileExtension))) {
            continue;
        }

        if (!fs::is_regular_file(path.replace_extension(fs::path()))) {
            continue;
        }

        return true;
    }

    return false;
}

ITorrentStateIteratorPtr rTorrentStateStore::Export(fs::path const &dataDir, IFileStreamProvider const &fileStreamProvider) const {
    return std::make_unique<rTorrentTorrentStateIterator>(dataDir, fileStreamProvider);
}

void rTorrentStateStore::Import(fs::path const &dataDir, Box const &box, IFileStreamProvider &fileStreamProvider) const {
    namespace StateField = Detail::StateField;
    namespace ResumeField = Detail::ResumeField;

    std::string loggerBase = "[" + TorrentClient::ToString(TorrentClient::rTorrent) + "][" + box.Torrent.GetName() + "] ";

    std::size_t const chunksDone = std::count(box.ValidBlocks.begin(), box.ValidBlocks.end(), true);
    std::size_t const chunksWanted = std::count(box.ValidBlocks.begin(), box.ValidBlocks.end(), false);
    int const isComplete = chunksWanted > 0 ? 1 : 0;

    Logger(Logger::Debug) << loggerBase << "Creating state object and assigning easy 1:1 values";

    ojson state = ojson::object();

    state[StateField::TimestampStarted] = static_cast<std::int64_t>(box.AddedAt);
    state[StateField::TimestampFinished] = static_cast<std::int64_t>(box.CompletedAt);
    state[StateField::TotalDownloaded] = box.DownloadedSize;
    state[StateField::TotalUploaded] = box.UploadedSize;
    state[StateField::Directory] = box.SavePath.parent_path().string();
    state[StateField::State] = box.IsPaused ? 0 : 1;
    state[StateField::ChunksDone] = chunksDone;
    state[StateField::ChunksWanted] = chunksWanted;
    state[StateField::Complete] = isComplete;
    // custom{}.x-filename is urlencoded filename of torrent file?

    Util::SortJsonObjectKeys(state);

    Logger(Logger::Debug) << loggerBase << "Creating resume object";

    ojson resume = ojson::object();

    Logger(Logger::Debug) << loggerBase << "Processing bitfield";

    if (isComplete) {
        resume[ResumeField::Bitfield] = chunksDone;
    } else {
        // TODO: how to build Bitfield.
        resume[ResumeField::Bitfield] = 0;
    }

    Logger(Logger::Debug) << loggerBase << "Processing files";

    resume[ResumeField::Files] = ojson::array();
    std::size_t fileCount = box.Files.size();
    for (std::size_t i = 0; i < fileCount; i++) {
        // calculate blocks per file from torrent
        std::uint64_t fileBlocks = std::ceil(box.Torrent.GetIndividualSize(i) / box.Torrent.GetPieceSize());
        std::uint64_t mtime = std::time(nullptr);

        ojson file = ojson::object();
        file[ResumeField::FileField::Completed] = fileBlocks;
        file[ResumeField::FileField::Priority] = 1;
        file[ResumeField::FileField::ModTime] = mtime;

        resume[ResumeField::Files].push_back(file);
    }

    Util::SortJsonObjectKeys(resume);

    Logger(Logger::Debug) << loggerBase << "Processing trackers";

    TorrentInfo torrent = box.Torrent;
    torrent.SetTrackers(box.Trackers);

    std::string const baseName = torrent.GetInfoHash() + ".torrent";

    // Filenames are just the SHA1 of the infohash(?), with the proper extensions.
    fs::path const torrentFilePath = dataDir / baseName;
    fs::path const resumeFilePath = dataDir / (baseName + ".libtorrent_resume");
    fs::path const stateFilePath = dataDir / (baseName + ".rtorrent");

    Logger(Logger::Debug) << loggerBase << "Writing state/resume files to session directory";

    {
        IWriteStreamPtr const stream = fileStreamProvider.GetWriteStream(torrentFilePath);
        torrent.Encode(*stream, m_bencoder);
    }

    {
        IWriteStreamPtr const stream = fileStreamProvider.GetWriteStream(resumeFilePath);
        m_bencoder.Encode(*stream, resume);
    }

    {
        IWriteStreamPtr const stream = fileStreamProvider.GetWriteStream(stateFilePath);
        m_bencoder.Encode(*stream, state);
    }
}