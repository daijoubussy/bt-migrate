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

#include "Common/IForwardIterator.h"
#include "ITorrentStateStore.h"

class DebugTorrentStateIterator : public ITorrentStateIterator {
   public:
    DebugTorrentStateIterator(ITorrentStateIteratorPtr decoratee);
    ~DebugTorrentStateIterator() override;

   public:
    // ITorrentStateIterator
    bool GetNext(Box &nextBox) override;

   private:
    ITorrentStateIteratorPtr const m_decoratee;
};
