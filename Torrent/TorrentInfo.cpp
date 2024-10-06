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

#include "TorrentInfo.h"

#include <fmt/format.h>

#include <filesystem>
#include <sstream>

#include "Codec/BencodeCodec.h"
#include "Codec/IStructuredDataCodec.h"
#include "Common/Exception.h"
#include "Common/Util.h"

namespace fs = std::filesystem;

namespace {

    std::string CalculateInfoHash(ojson const &torrent) {
        if (!torrent.contains("info")) {
            throw Exception("Torrent file is missing info dictionary");
        }

        std::ostringstream infoStream;
        BencodeCodec().Encode(infoStream, torrent["info"]);
        return Util::CalculateSha1(infoStream.str());
    }

}  // namespace

TorrentInfo::TorrentInfo() = default;

TorrentInfo::TorrentInfo(ojson const &torrent) : m_torrent(torrent), m_infoHash(CalculateInfoHash(m_torrent)) {
    //
}

void TorrentInfo::Encode(std::ostream &stream, IStructuredDataCodec const &codec) const { codec.Encode(stream, m_torrent); }

std::string const &TorrentInfo::GetInfoHash() const { return m_infoHash; }

/**
 * Get individual size of file from files array
 *
 * @param file the current file to retrieve the size of
 * @return block counts of file
 */
[[maybe_unused]] std::uint64_t TorrentInfo::GetIndividualSize(ojson const &file) const {
    std::uint64_t size = 0;
    size += file["length"].as<std::uint64_t>();
    return size;
}

/**
 * Get individual size of file at specific index
 *
 * @param index the index of the array to retrieve the file length from
 * @return block counts of file
 */
[[maybe_unused]] std::uint64_t TorrentInfo::GetIndividualSize(std::uint64_t const &index) const {
    ojson const &info = m_torrent["info"];

    if (!info.contains("files")) {
        return info["length"].as<std::uint64_t>();
    }

    return info["files"][index]["length"].as<std::uint64_t>();
}

/**
 * Sum the length of all files from the Torrent file.
 *
 * @return length of the files in bytes
 */
std::uint64_t TorrentInfo::GetTotalSize() const {
    std::uint64_t result = 0;

    ojson const &info = m_torrent["info"];

    if (!info.contains("files")) {
        result += info["length"].as<std::uint64_t>();
    } else {
        for (ojson const &file : info["files"].array_range()) {
            result += file["length"].as<std::uint64_t>();
        }
    }

    return result;
}

/**
 * Get the value for "piece length" from the Torrent file.
 *
 * @return number of bytes in each piece
 */
std::uint32_t TorrentInfo::GetPieceSize() const {
    ojson const &info = m_torrent["info"];

    return info["piece length"].as<std::uint32_t>();
}

/**
 * Get the total number of pieces from the Torrent file.
 * @return total number of pieces
 */
std::uint64_t TorrentInfo::GetNumberOfPieces() const {
    std::uint64_t totalSize = GetTotalSize();
    std::uint64_t pieceSize = GetPieceSize();

    return static_cast<uint64_t>(std::ceil((totalSize + pieceSize - 1) / pieceSize));
}

std::string TorrentInfo::GetName() const {
    ojson const &info = m_torrent["info"];

    return info["name"].as<std::string>();
}

fs::path TorrentInfo::GetFilePath(std::size_t fileIndex) const {
    fs::path result;

    ojson const &info = m_torrent["info"];

    if (!info.contains("files")) {
        if (fileIndex != 0) {
            throw Exception(fmt::format("Torrent file #{} does not exist", fileIndex));
        }

        result /= GetName();
    } else {
        ojson const &files = info["files"];

        if (fileIndex >= files.size()) {
            throw Exception(fmt::format("Torrent file #{} does not exist", fileIndex));
        }

        for (ojson const &pathPart : files[fileIndex]["path"].array_range()) {
            result /= pathPart.as<std::string>();
        }
    }

    return result;
}

void TorrentInfo::SetTrackers(std::vector<std::vector<std::string>> const &trackers) {
    ojson announceList = ojson::array();

    for (auto const &tier : trackers) {
        announceList.emplace_back(tier);
    }

    m_torrent["announce-list"] = announceList;

    if (announceList.empty()) {
        m_torrent.erase("announce");
    } else {
        m_torrent["announce"] = announceList[0][0];
    }

    Util::SortJsonObjectKeys(m_torrent);
}

std::vector<std::vector<std::string>> TorrentInfo::GetTrackers() {
    std::vector<std::vector<std::string>> tierList;

    std::string announceKey = "announce";
    if (m_torrent.contains("announce-list")) {
        announceKey = "announce-list";
    }

    ojson announceList = m_torrent[announceKey];

    if (announceList.empty()) {
        tierList[0][0] = m_torrent["announce"].as_string();
    } else {
        for (ojson const &tier_obj : announceList.array_range()) {
            std::vector<std::string> tier;
            for (ojson const &tracker : tier_obj.array_range()) {
                tier.push_back(tracker.as_string());
            }
            tierList.push_back(tier);
        }
    }

    return tierList;
}

TorrentInfo TorrentInfo::Decode(std::istream &stream, IStructuredDataCodec const &codec) {
    ojson torrent;
    codec.Decode(stream, torrent);
    return TorrentInfo(torrent);
}
