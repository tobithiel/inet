//
// Copyright (C) 2013 OpenSim Ltd
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
// author: Zoltan Bojthe
//

#ifndef __INET_IDEALMAC_H
#define __INET_IDEALMAC_H

#include "inet/common/INETDefs.h"
#include "inet/physicallayer/contract/packetlevel/IRadio.h"
#include "inet/linklayer/common/MacAddress.h"
#include "inet/linklayer/base/MacProtocolBase.h"

namespace inet {

class IdealMacHeader;
class InterfaceEntry;
class IPassiveQueue;

/**
 * Implements a simplified ideal MAC.
 *
 * See the NED file for details.
 */
class INET_API IdealMac : public MacProtocolBase
{
  protected:
    // parameters
    int headerLength = 0;    // IdealMacFrame header length in bytes
    double bitrate = 0;    // [bits per sec]
    bool promiscuous = false;    // promiscuous mode
    MacAddress address;    // MAC address
    bool fullDuplex = false;
    bool useAck = true;

    physicallayer::IRadio *radio = nullptr;
    physicallayer::IRadio::TransmissionState transmissionState = physicallayer::IRadio::TRANSMISSION_STATE_UNDEFINED;
    IPassiveQueue *queueModule = nullptr;

    int outStandingRequests = 0;
    Packet *lastSentPk = nullptr;
    simtime_t ackTimeout;
    cMessage *ackTimeoutMsg = nullptr;

  protected:
    /** implements MacBase functions */
    //@{
    virtual void flushQueue();
    virtual void clearQueue();
    virtual InterfaceEntry *createInterfaceEntry() override;
    //@}

    virtual void startTransmitting(Packet *msg);
    virtual bool dropFrameNotForUs(Packet *frame);
    virtual void encapsulate(Packet *msg);
    virtual void decapsulate(Packet *frame);
    virtual void initializeMACAddress();
    virtual void acked(Packet *packet);    // called by other IdealMac module, when receiving a packet with my moduleID

    // get MSG from queue
    virtual void getNextMsgFromHL();

    //cListener:
    virtual void receiveSignal(cComponent *src, simsignal_t id, long value, cObject *details) override;

    /** implements MacProtocolBase functions */
    //@{
    virtual void handleUpperPacket(Packet *packet) override;
    virtual void handleLowerPacket(Packet *packet) override;
    virtual void handleSelfMessage(cMessage *message) override;
    //@}

  public:
    IdealMac();
    virtual ~IdealMac();

  protected:
    virtual int numInitStages() const override { return NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
};

} // namespace inet

#endif // ifndef __INET_IDEALMAC_H

