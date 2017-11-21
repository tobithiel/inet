//
// Copyright (C) 2005 Andras Varga
// Based on the video streaming app of the similar name by Johnny Lai.
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

#include "inet/applications/udpapp/UDPVideoStreamSvr.h"

#include "inet/common/packet/chunk/ByteCountChunk.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/transportlayer/common/L4PortTag_m.h"
#include "inet/transportlayer/contract/udp/UDPControlInfo_m.h"

namespace inet {

Define_Module(UdpVideoStreamSvr);

simsignal_t UdpVideoStreamSvr::reqStreamBytesSignal = registerSignal("reqStreamBytes");
simsignal_t UdpVideoStreamSvr::sentPkSignal = registerSignal("sentPk");

inline std::ostream& operator<<(std::ostream& out, const UdpVideoStreamSvr::VideoStreamData& d)
{
    out << "client=" << d.clientAddr << ":" << d.clientPort
        << "  size=" << d.videoSize << "  pksent=" << d.numPkSent << "  bytesleft=" << d.bytesLeft;
    return out;
}

UdpVideoStreamSvr::~UdpVideoStreamSvr()
{
    for (auto & elem : streams)
        cancelAndDelete(elem.second.timer);
}

void UdpVideoStreamSvr::initialize(int stage)
{
    ApplicationBase::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        sendInterval = &par("sendInterval");
        packetLen = &par("packetLen");
        videoSize = &par("videoSize");
        localPort = par("localPort");

        // statistics
        numStreams = 0;
        numPkSent = 0;

        WATCH_MAP(streams);
    }
}

void UdpVideoStreamSvr::finish()
{
}

void UdpVideoStreamSvr::handleMessageWhenUp(cMessage *msg)
{
    if (msg->isSelfMessage()) {
        // timer for a particular video stream expired, send packet
        sendStreamData(msg);
    }
    else if (msg->getKind() == UDP_I_DATA) {
        // start streaming
        processStreamRequest(check_and_cast<Packet *>(msg));
    }
    else if (msg->getKind() == UDP_I_ERROR) {
        EV_WARN << "Ignoring UDP error report\n";
        delete msg;
    }
    else {
        throw cRuntimeError("Unrecognized message (%s)%s", msg->getClassName(), msg->getName());
    }
}

void UdpVideoStreamSvr::processStreamRequest(Packet *msg)
{
    // register video stream...
    cMessage *timer = new cMessage("VideoStreamTmr");
    VideoStreamData *d = &streams[timer->getId()];
    d->timer = timer;
    d->clientAddr = msg->getMandatoryTag<L3AddressInd>()->getSrcAddress();
    d->clientPort = msg->getMandatoryTag<L4PortInd>()->getSrcPort();
    d->videoSize = (*videoSize);
    d->bytesLeft = d->videoSize;
    d->numPkSent = 0;
    ASSERT(d->videoSize > 0);
    delete msg;

    numStreams++;
    emit(reqStreamBytesSignal, d->videoSize);

    // ... then transmit first packet right away
    sendStreamData(timer);
}

void UdpVideoStreamSvr::sendStreamData(cMessage *timer)
{
    auto it = streams.find(timer->getId());
    if (it == streams.end())
        throw cRuntimeError("Model error: Stream not found for timer");

    VideoStreamData *d = &(it->second);

    // generate and send a packet
    Packet *pkt = new Packet("VideoStrmPk");
    long pktLen = packetLen->longValue();

    if (pktLen > d->bytesLeft)
        pktLen = d->bytesLeft;
    const auto& payload = makeShared<ByteCountChunk>(B(pktLen));
    payload->markImmutable();
    pkt->append(payload);

    emit(sentPkSignal, pkt);
    socket.sendTo(pkt, d->clientAddr, d->clientPort);

    d->bytesLeft -= pktLen;
    d->numPkSent++;
    numPkSent++;

    // reschedule timer if there's bytes left to send
    if (d->bytesLeft > 0) {
        simtime_t interval = (*sendInterval);
        scheduleAt(simTime() + interval, timer);
    }
    else {
        streams.erase(it);
        delete timer;
    }
}

void UdpVideoStreamSvr::clearStreams()
{
    for (auto & elem : streams)
        cancelAndDelete(elem.second.timer);
    streams.clear();
}

bool UdpVideoStreamSvr::handleNodeStart(IDoneCallback *doneCallback)
{
    socket.setOutputGate(gate("socketOut"));
    socket.bind(localPort);

    return true;
}

bool UdpVideoStreamSvr::handleNodeShutdown(IDoneCallback *doneCallback)
{
    clearStreams();
    //TODO if(socket.isOpened()) socket.close();
    return true;
}

void UdpVideoStreamSvr::handleNodeCrash()
{
    clearStreams();
}

} // namespace inet

