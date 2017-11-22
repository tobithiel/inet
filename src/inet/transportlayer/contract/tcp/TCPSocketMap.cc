//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/common/INETDefs.h"

#include "inet/applications/common/SocketTag_m.h"
#include "inet/transportlayer/contract/tcp/TcpSocketMap.h"

namespace inet {

TcpSocket *TcpSocketMap::findSocketFor(cMessage *msg)
{
    int connId = msg->getMandatoryTag<SocketInd>()->getSocketId();
    auto i = socketMap.find(connId);
    ASSERT(i == socketMap.end() || i->first == i->second->getConnectionId());
    return (i == socketMap.end()) ? nullptr : i->second;
}

void TcpSocketMap::addSocket(TcpSocket *socket)
{
    ASSERT(socketMap.find(socket->getConnectionId()) == socketMap.end());
    socketMap[socket->getConnectionId()] = socket;
}

TcpSocket *TcpSocketMap::removeSocket(TcpSocket *socket)
{
    auto i = socketMap.find(socket->getConnectionId());
    if (i != socketMap.end())
        socketMap.erase(i);
    return socket;
}

void TcpSocketMap::deleteSockets()
{
    for (auto & elem : socketMap)
        delete elem.second;
    socketMap.clear();
}

} // namespace inet

