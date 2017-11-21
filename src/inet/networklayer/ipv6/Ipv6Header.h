//
// Copyright (C) 2005 Andras Varga
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

#ifndef __INET_IPV6DATAGRAM_H
#define __INET_IPV6DATAGRAM_H

#include <list>
#include "inet/common/INETDefs.h"
#include "inet/common/ProtocolGroup.h"
#include "inet/networklayer/ipv6/IPv6Header_m.h"

namespace inet {

/**
 * Represents an IPv6 datagram. More info in the IPv6Datagram.msg file
 * (and the documentation generated from it).
 */
class INET_API Ipv6Header : public Ipv6Header_Base
{
  protected:
    typedef std::vector<Ipv6ExtensionHeader *> ExtensionHeaders;
    ExtensionHeaders extensionHeaders;

  private:
    void copy(const Ipv6Header& other);
    void clean();
    int getExtensionHeaderOrder(Ipv6ExtensionHeader *eh);

  public:
    Ipv6Header() : Ipv6Header_Base() {}
    Ipv6Header(const Ipv6Header& other) : Ipv6Header_Base(other) { copy(other); }
    Ipv6Header& operator=(const Ipv6Header& other);
    ~Ipv6Header();

    virtual Ipv6Header *dup() const override { return new Ipv6Header(*this); }

    /**
     * Returns bits 0-5 of the Traffic Class field, a value in the 0..63 range
     */
    virtual int getDiffServCodePoint() const override { return getTrafficClass() & 0x3f; }

    /**
     * Sets bits 0-5 of the Traffic Class field; expects a value in the 0..63 range
     */
    virtual void setDiffServCodePoint(int dscp) override { setTrafficClass((getTrafficClass() & 0xc0) | (dscp & 0x3f)); }

    /**
     * Returns bits 6-7 of the Traffic Class field, a value in the range 0..3
     */
    virtual int getExplicitCongestionNotification() const override { return (getTrafficClass() >> 6) & 0x03; }

    /**
     * Sets bits 6-7 of the Traffic Class field; expects a value in the 0..3 range
     */
    virtual void setExplicitCongestionNotification(int ecn) override { setTrafficClass((getTrafficClass() & 0x3f) | ((ecn & 0x3) << 6)); }

    /** Generated but unused method, should not be called. */
    virtual void setExtensionHeaderArraySize(unsigned int size) override;

    /** Generated but unused method, should not be called. */
    virtual void setExtensionHeader(unsigned int k, Ipv6ExtensionHeader *extensionHeader_var) override;

    /**
     * Returns the number of extension headers in this datagram
     */
    virtual unsigned int getExtensionHeaderArraySize() const override;

    /**
     * Returns the kth extension header in this datagram
     */
    virtual Ipv6ExtensionHeader *getMutableExtensionHeader(unsigned int k) override;
    virtual const Ipv6ExtensionHeader *getExtensionHeader(unsigned int k) const override;

    /**
     * Returns the extension header of the specified type,
     * or nullptr. If index is 0, then the first, if 1 then the
     * second extension is returned. (The datagram might
     * contain two Destination Options extension.)
     */
    virtual Ipv6ExtensionHeader *findMutableExtensionHeaderByType(IpProtocolId extensionType, int index = 0);
    virtual const Ipv6ExtensionHeader *findExtensionHeaderByType(IpProtocolId extensionType, int index = 0) const;

    /**
     * Adds an extension header to the datagram.
     * The atPos parameter should not be used, the extension
     * headers are stored in the order specified in RFC 2460 4.1.
     */
    virtual void addExtensionHeader(Ipv6ExtensionHeader *eh, int atPos = -1);

    /**
     * Calculates the length of the IPv6 header plus the extension
     * headers.
     */
    virtual int calculateHeaderByteLength() const;

    /**
     * Calculates the length of the unfragmentable part of IPv6 header
     * plus the extension headers.
     */
    virtual int calculateUnfragmentableHeaderByteLength() const;

    /**
     * Calculates the length of the payload and extension headers
     * after the Fragment Header.
     */
    virtual int calculateFragmentLength() const;

    /**
     * Removes and returns the first extension header of this datagram
     */
    virtual Ipv6ExtensionHeader *removeFirstExtensionHeader();

    /**
     * Removes and returns the first extension header with the given type.
     */
    virtual Ipv6ExtensionHeader *removeExtensionHeader(IpProtocolId extensionType);

    virtual L3Address getSourceAddress() const override { return L3Address(getSrcAddress()); }
    virtual void setSourceAddress(const L3Address& address) override { setSrcAddress(address.toIPv6()); }
    virtual L3Address getDestinationAddress() const override { return L3Address(getDestAddress()); }
    virtual void setDestinationAddress(const L3Address& address) override { setDestAddress(address.toIPv6()); }
    virtual ConstProtocol *getProtocol() const override { return ProtocolGroup::ipprotocol.findProtocol(getProtocolId()); }
    virtual void setProtocol(ConstProtocol *protocol) override { setProtocolId((IpProtocolId)ProtocolGroup::ipprotocol.getProtocolNumber(protocol)); }
};

std::ostream& operator<<(std::ostream& out, const Ipv6ExtensionHeader&);

} // namespace inet

#endif // ifndef __INET_IPV6DATAGRAM_H

