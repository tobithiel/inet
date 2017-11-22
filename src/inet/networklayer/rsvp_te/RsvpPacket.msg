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

import inet.common.INETDefs;
import inet.common.packet.chunk.Chunk;
import inet.networklayer.contract.NetworkHeaderBase;
import inet.networklayer.rsvp_te.IntServ;

cplusplus {{
#define PATH_MESSAGE   1
#define RESV_MESSAGE   2
#define PTEAR_MESSAGE  3
#define RTEAR_MESSAGE  4
#define PERROR_MESSAGE 5
#define RERROR_MESSAGE 6
#define HELLO_MESSAGE   7
}}

namespace inet;

//
// RSVP message common part
//
class RsvpMessage extends FieldsChunk
{
    int rsvpKind;
}

//
// FIXME missing documentation
//
class RsvpPacket extends RsvpMessage
{
    @customize(true);
    SessionObj session;
    bool checksumValid = true;
}

cplusplus {{

/**
 * RSVP message common part.
 *
 * This class adds convenience get() and set() methods to the generated
 * base class, but no extra data.
 */
class INET_API RsvpPacket : public RsvpPacket_Base
{
  public:
    RsvpPacket() : RsvpPacket_Base() {}
    RsvpPacket(const RsvpPacket& other) : RsvpPacket_Base(other) {}
    RsvpPacket& operator=(const RsvpPacket& other) { RsvpPacket_Base::operator=(other); return *this; }
    virtual RsvpPacket *dup() const override { return new RsvpPacket(*this); }

    inline Ipv4Address getDestAddress() { return getSession().DestAddress; }
    inline int getTunnelId() { return getSession().Tunnel_Id; }
    inline int getExTunnelId() { return getSession().Extended_Tunnel_Id; }
    inline int getSetupPri() { return getSession().setupPri; }
    inline int getHoldingPri() { return getSession().holdingPri; }
    inline bool isInSession(SessionObj *s) const
    {
        return getSession().DestAddress == s->DestAddress &&
               getSession().Tunnel_Id == s->Tunnel_Id &&
               getSession().Extended_Tunnel_Id == s->Extended_Tunnel_Id;
    }
};

}}

