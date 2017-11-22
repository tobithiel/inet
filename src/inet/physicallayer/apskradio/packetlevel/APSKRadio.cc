//
// Copyright (C) 2013 OpenSim Ltd.
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

#include "inet/common/packet/chunk/BitCountChunk.h"
#include "inet/common/packet/Packet.h"
#include "inet/physicallayer/apskradio/bitlevel/ApskEncoder.h"
#include "inet/physicallayer/apskradio/bitlevel/ApskLayeredTransmitter.h"
#include "inet/physicallayer/apskradio/packetlevel/APSKPhyHeader_m.h"
#include "inet/physicallayer/apskradio/packetlevel/ApskRadio.h"
#include "inet/physicallayer/base/packetlevel/FlatTransmitterBase.h"

namespace inet {

namespace physicallayer {

Define_Module(ApskRadio);

ApskRadio::ApskRadio() :
    FlatRadioBase()
{
}

b ApskRadio::computePaddingLength(b length, const ConvolutionalCode *forwardErrorCorrection, const ApskModulationBase *modulation) const
{
    int modulationCodeWordSize = modulation->getCodeWordSize();
    int encodedCodeWordSize = forwardErrorCorrection == nullptr ? modulationCodeWordSize : modulationCodeWordSize * forwardErrorCorrection->getCodeRatePuncturingK();
    return b((encodedCodeWordSize - b(length).get() % encodedCodeWordSize) % encodedCodeWordSize);
}

const ApskModulationBase *ApskRadio::getModulation() const
{
    const ApskModulationBase *modulation = nullptr;
    // TODO: const ConvolutionalCode *forwardErrorCorrection = nullptr;
    auto phyHeader = makeShared<ApskPhyHeader>();
    b headerLength = phyHeader->getChunkLength();

    // KLUDGE:
    if (auto flatTransmitter = dynamic_cast<const FlatTransmitterBase *>(transmitter)) {
        headerLength = flatTransmitter->getHeaderLength();
        modulation = check_and_cast<const ApskModulationBase *>(flatTransmitter->getModulation());
    }
    // KLUDGE:
    else if (auto layeredTransmitter = dynamic_cast<const ApskLayeredTransmitter *>(transmitter)) {
        auto encoder = layeredTransmitter->getEncoder();
        if (encoder != nullptr) {
            // const ApskEncoder *apskEncoder = check_and_cast<const ApskEncoder *>(encoder);
            // TODO: forwardErrorCorrection = apskEncoder->getCode()->getConvolutionalCode();
        }
        modulation = check_and_cast<const ApskModulationBase *>(layeredTransmitter->getModulator()->getModulation());
    }
    //FIXME when uses OFDM, ofdm modulator can not cast to apsk modulator, see /examples/wireless/layered80211/ -f omnetpp.ini -c LayeredCompliant80211Ping
    ASSERT(modulation != nullptr);
    return modulation;
}

void ApskRadio::encapsulate(Packet *packet) const
{
    auto phyHeader = makeShared<ApskPhyHeader>();
    phyHeader->setCrc(0);
    phyHeader->setCrcMode(CRC_DISABLED);
    phyHeader->setLengthField(packet->getByteLength());
    b headerLength = phyHeader->getChunkLength();
    if (auto flatTransmitter = dynamic_cast<const FlatTransmitterBase *>(transmitter)) {
        headerLength = flatTransmitter->getHeaderLength();
        if (headerLength > phyHeader->getChunkLength())
            packet->insertHeader(makeShared<BitCountChunk>(headerLength - phyHeader->getChunkLength()));
    }
    packet->insertHeader(phyHeader);
    auto paddingLength = computePaddingLength(headerLength + B(phyHeader->getLengthField()), nullptr, getModulation());
    if (paddingLength != b(0))
        packet->insertTrailer(makeShared<BitCountChunk>(paddingLength));
}

void ApskRadio::decapsulate(Packet *packet) const
{
    const auto& phyHeader = packet->popHeader<ApskPhyHeader>();
    b headerLength = phyHeader->getChunkLength();
    if (auto flatTransmitter = dynamic_cast<const FlatTransmitterBase *>(transmitter)) {
        headerLength = flatTransmitter->getHeaderLength();
        if (headerLength > phyHeader->getChunkLength())
            packet->popHeader(headerLength - phyHeader->getChunkLength());
    }
    auto paddingLength = computePaddingLength(headerLength + B(phyHeader->getLengthField()), nullptr, getModulation());
    if (paddingLength != b(0))
        packet->popTrailer(paddingLength);
}

} // namespace physicallayer

} // namespace inet

