//
// Copyright (C) 2013 OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/applications/common/SocketTag_m.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/linklayer/tun/TunControlInfo_m.h"
#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/networklayer/contract/IInterfaceTable.h"
#include "inet/transportlayer/contract/udp/UdpControlInfo.h"

#include "inet/applications/tunapp/TunnelApp.h"

namespace inet {

Define_Module(TunnelApp);

TunnelApp::TunnelApp()
{
}

TunnelApp::~TunnelApp()
{
}

void TunnelApp::initialize(int stage)
{
    ApplicationBase::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        interface = par("interface");
        const char *protocolName = par("protocol");
        protocol = Protocol::getProtocol(protocolName);
        destinationAddress = par("destinationAddress");
        if (protocol == &Protocol::udp) {
            destinationPort = par("destinationPort");
            localPort = par("localPort");
        }
    }
    else if (stage == INITSTAGE_APPLICATION_LAYER) {
        if (protocol == &Protocol::ipv4) {
            l3Socket.setOutputGate(gate("socketOut"));
            l3Socket.setControlInfoProtocolId(Protocol::ipv4.getId());
            l3Socket.bind(IP_PROT_IP);
        }
        if (protocol == &Protocol::udp) {
            serverSocket.setOutputGate(gate("socketOut"));
            if (localPort != -1)
                serverSocket.bind(localPort);
            clientSocket.setOutputGate(gate("socketOut"));
            if (destinationPort != -1)
                clientSocket.connect(L3AddressResolver().resolve(destinationAddress), destinationPort);
        }
        IInterfaceTable *interfaceTable = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);
        InterfaceEntry *interfaceEntry = interfaceTable->getInterfaceByName(interface);
        if (interfaceEntry == nullptr)
            throw cRuntimeError("TUN interface not found: %s", interface);
        tunSocket.setOutputGate(gate("socketOut"));
        tunSocket.open(interfaceEntry->getInterfaceId());
    }
}

void TunnelApp::handleMessageWhenUp(cMessage *message)
{
    if (message->arrivedOn("socketIn")) {
        ASSERT(message->getControlInfo() == nullptr);

        auto sockInd = message->getTag<SocketInd>();
        int sockId = (sockInd != nullptr) ? sockInd->getSocketId() : -1;
        if (sockInd != nullptr && sockId == tunSocket.getSocketId()) {
            // InterfaceInd says packet is from tunnel interface and socket id is present and equals to tunSocket
            if (protocol == &Protocol::ipv4) {
                message->clearTags();
                message->ensureTag<L3AddressReq>()->setDestAddress(L3AddressResolver().resolve(destinationAddress));
                message->ensureTag<PacketProtocolTag>()->setProtocol(&Protocol::ipv4);
                l3Socket.send(check_and_cast<Packet *>(message));
            }
            else if (protocol == &Protocol::udp) {
                message->clearTags();
                clientSocket.send(check_and_cast<Packet *>(message));
            }
            else
                throw cRuntimeError("Unknown protocol: %s", protocol->getName());;
        }
        else {
            auto packetProtocol = message->getMandatoryTag<PacketProtocolTag>()->getProtocol();
            if (packetProtocol == protocol) {
                delete message->removeControlInfo();
                message->clearTags();
                tunSocket.send(check_and_cast<Packet *>(message));
            }
            else
                throw cRuntimeError("Unknown protocol: %s", packetProtocol->getName());;
        }
    }
    else
        throw cRuntimeError("Message arrived on unknown gate %s", message->getArrivalGate()->getFullName());
}

} // namespace inet

