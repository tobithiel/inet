//
// (C) 2005 Vojtech Janota
// (C) 2003 Xuan Thang Nguyen
//
// This library is free software, you can redistribute it
// and/or modify
// it under  the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation;
// either version 2 of the License, or any later version.
// The library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//

#ifndef __INET_MPLS_H
#define __INET_MPLS_H

#include <vector>

#include "inet/common/INETDefs.h"

#include "inet/common/IInterfaceRegistrationListener.h"
#include "inet/common/IProtocolRegistrationListener.h"
#include "inet/common/packet/Packet.h"
#include "inet/networklayer/contract/IInterfaceTable.h"
#include "inet/networklayer/ipv4/Ipv4Header.h"
#include "inet/networklayer/mpls/ConstType.h"
#include "inet/networklayer/mpls/IClassifier.h"
#include "inet/networklayer/mpls/LIBTable.h"
#include "inet/networklayer/mpls/MPLSPacket.h"

namespace inet {

/**
 * Implements the MPLS protocol; see the NED file for more info.
 */
class INET_API Mpls : public cSimpleModule, public IProtocolRegistrationListener, public IInterfaceRegistrationListener
{
  protected:
    simtime_t delay1;

    //no longer used, see comment in intialize
    //std::vector<bool> labelIf;

    LibTable *lt;
    IInterfaceTable *ift;
    IClassifier *pct;

  protected:
    virtual void initialize(int stage) override;
    virtual int numInitStages() const override { return NUM_INIT_STAGES; }
    virtual void handleMessage(cMessage *msg) override;

  protected:
    virtual void processPacketFromL3(Packet *msg);
    virtual void processPacketFromL2(Packet *msg);
    virtual void processMPLSPacketFromL2(Packet *mplsPacket);

    virtual bool tryLabelAndForwardIPv4Datagram(Packet *ipdatagram);
    virtual void labelAndForwardIPv4Datagram(Packet *ipdatagram);

    virtual void sendToL2(cMessage *msg);
    virtual void sendToL3(cMessage *msg);

    virtual void doStackOps(MplsHeader *mplsPacket, const LabelOpVector& outLabel);

    //IInterfaceRegistrationListener:
    virtual void handleRegisterInterface(const InterfaceEntry &interface, cGate *gate) override;

    //IProtocolRegistrationListener:
    virtual void handleRegisterProtocol(const Protocol& protocol, cGate *protocolGate) override;
};

} // namespace inet

#endif // ifndef __INET_MPLS_H

