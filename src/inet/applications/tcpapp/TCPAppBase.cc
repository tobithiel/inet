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

#include "inet/applications/tcpapp/TCPAppBase.h"

#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/transportlayer/contract/tcp/TCPSocket.h"

namespace inet {

simsignal_t TcpAppBase::connectSignal = registerSignal("connect");
simsignal_t TcpAppBase::rcvdPkSignal = registerSignal("rcvdPk");
simsignal_t TcpAppBase::sentPkSignal = registerSignal("sentPk");

void TcpAppBase::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        numSessions = numBroken = packetsSent = packetsRcvd = bytesSent = bytesRcvd = 0;

        WATCH(numSessions);
        WATCH(numBroken);
        WATCH(packetsSent);
        WATCH(packetsRcvd);
        WATCH(bytesSent);
        WATCH(bytesRcvd);
    }
    else if (stage == INITSTAGE_APPLICATION_LAYER) {
        // parameters
        const char *localAddress = par("localAddress");
        int localPort = par("localPort");
        socket.bind(*localAddress ? L3AddressResolver().resolve(localAddress) : L3Address(), localPort);

        socket.setCallbackObject(this);
        socket.setOutputGate(gate("socketOut"));
    }
}

void TcpAppBase::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage())
        handleTimer(msg);
    else
        socket.processMessage(msg);
}

void TcpAppBase::connect()
{
    // we need a new connId if this is not the first connection
    socket.renewSocket();

    // connect
    const char *connectAddress = par("connectAddress");
    int connectPort = par("connectPort");

    L3Address destination;
    L3AddressResolver().tryResolve(connectAddress, destination);
    if (destination.isUnspecified()) {
        EV_ERROR << "Connecting to " << connectAddress << " port=" << connectPort << ": cannot resolve destination address\n";
    }
    else {
        EV_INFO << "Connecting to " << connectAddress << "(" << destination << ") port=" << connectPort << endl;

        socket.connect(destination, connectPort);

        numSessions++;
        emit(connectSignal, 1L);
    }
}

void TcpAppBase::close()
{
    EV_INFO << "issuing CLOSE command\n";
    socket.close();
    emit(connectSignal, -1L);
}

void TcpAppBase::sendPacket(cPacket *msg)
{
    int numBytes = msg->getByteLength();
    emit(sentPkSignal, msg);
    socket.send(msg);

    packetsSent++;
    bytesSent += numBytes;
}

void TcpAppBase::refreshDisplay() const
{
    getDisplayString().setTagArg("t", 0, TcpSocket::stateName(socket.getState()));
}

void TcpAppBase::socketEstablished(int, void *)
{
    // *redefine* to perform or schedule first sending
    EV_INFO << "connected\n";
}

void TcpAppBase::socketDataArrived(int, void *, Packet *msg, bool)
{
    // *redefine* to perform or schedule next sending
    packetsRcvd++;
    bytesRcvd += msg->getByteLength();
    emit(rcvdPkSignal, msg);
    delete msg;
}

void TcpAppBase::socketPeerClosed(int, void *)
{
    // close the connection (if not already closed)
    if (socket.getState() == TcpSocket::PEER_CLOSED) {
        EV_INFO << "remote TCP closed, closing here as well\n";
        close();
    }
}

void TcpAppBase::socketClosed(int, void *)
{
    // *redefine* to start another session etc.
    EV_INFO << "connection closed\n";
}

void TcpAppBase::socketFailure(int, void *, int code)
{
    // subclasses may override this function, and add code try to reconnect after a delay.
    EV_WARN << "connection broken\n";
    numBroken++;
}

void TcpAppBase::finish()
{
    std::string modulePath = getFullPath();

    EV_INFO << modulePath << ": opened " << numSessions << " sessions\n";
    EV_INFO << modulePath << ": sent " << bytesSent << " bytes in " << packetsSent << " packets\n";
    EV_INFO << modulePath << ": received " << bytesRcvd << " bytes in " << packetsRcvd << " packets\n";
}

} // namespace inet

