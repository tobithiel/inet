//
// Copyright (C) 2013 Opensim Ltd.
// Author: Tamas Borbely
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

#include <algorithm>
#include <functional>

#include "inet/networklayer/contract/IL3AddressType.h"
#include "inet/networklayer/common/HopLimitTag_m.h"
#include "inet/networklayer/common/InterfaceMatcher.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/common/stlutils.h"
#include "inet/common/lifecycle/NodeOperations.h"
#include "inet/common/lifecycle/NodeStatus.h"
#include "inet/common/Simsignals.h"
#include "inet/transportlayer/udp/UDP.h"
#include "inet/common/ModuleAccess.h"
#include "inet/linklayer/common/InterfaceTag_m.h"

#include "inet/routing/rip/RIPPacket_m.h"
#include "inet/routing/rip/RipRouting.h"
#include "inet/transportlayer/common/L4PortTag_m.h"

namespace inet {

Define_Module(RipRouting);

std::ostream& operator<<(std::ostream& os, const RipRoute& e)
{
    os << e.info();
    return os;
}

RipRoute::RipRoute(IRoute *route, RouteType type, int metric, uint16 routeTag)
    : type(type), route(route), metric(metric), changed(false), lastUpdateTime(0)
{
    dest = route->getDestinationAsGeneric();
    prefixLength = route->getPrefixLength();
    nextHop = route->getNextHopAsGeneric();
    ie = route->getInterface();
    tag = routeTag;
}

std::string RipRoute::info() const
{
    std::stringstream out;

    out << "dest:";
    if (dest.isUnspecified())
        out << "*  ";
    else
        out << dest << "  ";
    out << "prefix:" << prefixLength << "  ";
    out << "gw:";
    if (nextHop.isUnspecified())
        out << "*  ";
    else
        out << nextHop << "  ";
    out << "metric:" << metric << " ";
    out << "if:";
    if (!ie)
        out << "*  ";
    else
        out << ie->getInterfaceName() << "  ";
    out << "tag:" << tag << " ";
    out << "upd:" << lastUpdateTime << "s  ";
    switch (type) {
        case RIP_ROUTE_INTERFACE:
            out << "INTERFACE";
            break;

        case RIP_ROUTE_STATIC:
            out << "STATIC";
            break;

        case RIP_ROUTE_DEFAULT:
            out << "DEFAULT";
            break;

        case RIP_ROUTE_RTE:
            out << "RTE";
            break;

        case RIP_ROUTE_REDISTRIBUTE:
            out << "REDISTRIBUTE";
            break;
    }

    return out.str();
}

RipInterfaceEntry::RipInterfaceEntry(const InterfaceEntry *ie)
    : ie(ie), metric(1), mode(NO_RIP)
{
    ASSERT(!ie->isLoopback());
    ASSERT(ie->isMulticast());
}

/**
 * Fills in the parameters of the interface from the matching <interface>
 * element of the configuration.
 */
void RipInterfaceEntry::configure(cXMLElement *config)
{
    const char *metricAttr = config->getAttribute("metric");

    if (metricAttr) {
        int metric = atoi(metricAttr);
        if (metric < 1 || metric >= RIP_INFINITE_METRIC)
            throw cRuntimeError("RIP: invalid metric in <interface> element at %s: %s", config->getSourceLocation(), metricAttr);
        this->metric = metric;
    }

    const char *ripModeAttr = config->getAttribute("mode");
    RipMode mode = !ripModeAttr ? SPLIT_HORIZON_POISONED_REVERSE :
        strcmp(ripModeAttr, "NoRIP") == 0 ? NO_RIP :
        strcmp(ripModeAttr, "NoSplitHorizon") == 0 ? NO_SPLIT_HORIZON :
        strcmp(ripModeAttr, "SplitHorizon") == 0 ? SPLIT_HORIZON :
        strcmp(ripModeAttr, "SplitHorizonPoisonedReverse") == 0 ? SPLIT_HORIZON_POISONED_REVERSE :
                static_cast<RipMode>(-1);
    if (mode == static_cast<RipMode>(-1))
        throw cRuntimeError("RIP: invalid split-horizon-mode attribute in <interface> element at %s: %s",
                config->getSourceLocation(), ripModeAttr);
    this->mode = mode;
}

std::ostream& operator<<(std::ostream& os, const RipInterfaceEntry& e)
{
    os << "if:" << e.ie->getInterfaceName() << "  ";
    os << "metric:" << e.metric << "  ";
    os << "mode: ";
    switch (e.mode) {
        case NO_RIP:
            os << "NoRIP";
            break;

        case NO_SPLIT_HORIZON:
            os << "NoSplitHorizon";
            break;

        case SPLIT_HORIZON:
            os << "SplitHorizon";
            break;

        case SPLIT_HORIZON_POISONED_REVERSE:
            os << "SplitHorizonPoisenedReverse";
            break;

        default:
            os << "<unknown>";
            break;
    }
    return os;
}

RipRouting::RipRouting()
{
}

RipRouting::~RipRouting()
{
    for (auto & elem : ripRoutes)
        delete elem;
    ripRoutes.clear();
    cancelAndDelete(updateTimer);
    cancelAndDelete(triggeredUpdateTimer);
    cancelAndDelete(startupTimer);
    cancelAndDelete(shutdownTimer);
}

simsignal_t RipRouting::sentRequestSignal = registerSignal("sentRequest");
simsignal_t RipRouting::sentUpdateSignal = registerSignal("sentUpdate");
simsignal_t RipRouting::rcvdResponseSignal = registerSignal("rcvdResponse");
simsignal_t RipRouting::badResponseSignal = registerSignal("badResponse");
simsignal_t RipRouting::numRoutesSignal = registerSignal("numRoutes");

void RipRouting::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        host = getContainingNode(this);
        ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);
        rt = getModuleFromPar<IRoutingTable>(par("routingTableModule"), this);
        socket.setOutputGate(gate("socketOut"));

        const char *m = par("mode");
        if (!m)
            throw cRuntimeError("Missing 'mode' parameter.");
        else if (!strcmp(m, "RIPv2"))
            mode = RIPv2;
        else if (!strcmp(m, "RIPng"))
            mode = RIPng;
        else
            throw cRuntimeError("Unrecognized 'mode' parameter: %s", m);

        ripUdpPort = par("udpPort");
        updateInterval = par("updateInterval").doubleValue();
        routeExpiryTime = par("routeExpiryTime").doubleValue();
        routePurgeTime = par("routePurgeTime").doubleValue();
        shutdownTime = par("shutdownTime").doubleValue();

        updateTimer = new cMessage("RIP-timer");
        triggeredUpdateTimer = new cMessage("RIP-trigger");
        startupTimer = new cMessage("RIP-startup");
        shutdownTimer = new cMessage("RIP-shutdown");

        WATCH_VECTOR(ripInterfaces);
        WATCH_PTRVECTOR(ripRoutes);
    }
    else if (stage == INITSTAGE_ROUTING_PROTOCOLS) {    // interfaces and static routes are already initialized
        NodeStatus *nodeStatus = dynamic_cast<NodeStatus *>(findContainingNode(this)->getSubmodule("status"));
        isOperational = !nodeStatus || nodeStatus->getState() == NodeStatus::UP;
        if (isOperational)
            scheduleAt(simTime() + par("startupTime").doubleValue(), startupTimer);
    }
}

/**
 * Creates a RipInterfaceEntry for each interface found in the interface table.
 */
void RipRouting::configureInterfaces(cXMLElement *config)
{
    cXMLElementList interfaceElements = config->getChildrenByTagName("interface");
    InterfaceMatcher matcher(interfaceElements);

    for (int k = 0; k < ift->getNumInterfaces(); ++k) {
        InterfaceEntry *ie = ift->getInterface(k);
        if (ie->isMulticast() && !ie->isLoopback()) {
            int i = matcher.findMatchingSelector(ie);
            addInterface(ie, i >= 0 ? interfaceElements[i] : nullptr);
        }
    }
}

/**
 * Import interface/static/default routes from the routing table.
 */
void RipRouting::configureInitialRoutes()
{
    for (int i = 0; i < rt->getNumRoutes(); ++i) {
        IRoute *route = rt->getRoute(i);
        if (isLoopbackInterfaceRoute(route)) {
            /*ignore*/
            ;
        }
        else if (isLocalInterfaceRoute(route)) {
            InterfaceEntry *ie = check_and_cast<InterfaceEntry *>(route->getSource());
            importRoute(route, RipRoute::RIP_ROUTE_INTERFACE, getInterfaceMetric(ie));
        }
        else if (isDefaultRoute(route))
            importRoute(route, RipRoute::RIP_ROUTE_DEFAULT);
        else {
            const L3Address& destAddr = route->getDestinationAsGeneric();
            if (!destAddr.isMulticast() && !destAddr.isLinkLocal())
                importRoute(route, RipRoute::RIP_ROUTE_STATIC);
        }
    }
}

/**
 * Adds a new route the RIP routing table for an existing IRoute.
 * This route will be advertised with the specified metric and routeTag fields.
 */
RipRoute *RipRouting::importRoute(IRoute *route, RipRoute::RouteType type, int metric, uint16 routeTag)
{
    ASSERT(metric < RIP_INFINITE_METRIC);

    RipRoute *ripRoute = new RipRoute(route, type, metric, routeTag);
    if (type == RipRoute::RIP_ROUTE_INTERFACE) {
        InterfaceEntry *ie = check_and_cast<InterfaceEntry *>(route->getSource());
        ripRoute->setInterface(ie);
    }

    ripRoutes.push_back(ripRoute);
    emit(numRoutesSignal, (unsigned long)ripRoutes.size());
    return ripRoute;
}

/**
 * Sends a RIP request to routers on the specified link.
 */
void RipRouting::sendRIPRequest(const RipInterfaceEntry& ripInterface)
{
    Packet *pk = new Packet("RIP request");
    const  auto& packet = makeShared<RipPacket>();
    packet->setCommand(RIP_REQUEST);
    packet->setEntryArraySize(1);
    RipEntry& entry = packet->getMutableEntry(0);
    entry.addressFamilyId = RIP_AF_NONE;
    entry.metric = RIP_INFINITE_METRIC;
    packet->setChunkLength(B(RIP_HEADER_SIZE + RIP_RTE_SIZE * packet->getEntryArraySize()));
    packet->markImmutable();
    pk->append(packet);
    emit(sentRequestSignal, pk);
    sendPacket(pk, addressType->getLinkLocalRIPRoutersMulticastAddress(), ripUdpPort, ripInterface.ie);
}

/**
 * Listen on interface/route changes and update private data structures.
 */
void RipRouting::receiveSignal(cComponent *source, simsignal_t signalID, cObject *obj, cObject *details)
{
    Enter_Method_Silent("RipRouting::receiveChangeNotification(%s)", cComponent::getSignalName(signalID));

    const InterfaceEntry *ie;
    const InterfaceEntryChangeDetails *change;

    if (signalID == NF_INTERFACE_CREATED) {
        // configure interface for RIP
        ie = check_and_cast<const InterfaceEntry *>(obj);
        if (ie->isMulticast() && !ie->isLoopback()) {
            cXMLElementList config = par("ripConfig").xmlValue()->getChildrenByTagName("interface");
            int i = InterfaceMatcher(config).findMatchingSelector(ie);
            if (i >= 0)
                addInterface(ie, config[i]);
        }
    }
    else if (signalID == NF_INTERFACE_DELETED) {
        // delete interfaces and routes referencing the deleted interface
        ie = check_and_cast<const InterfaceEntry *>(obj);
        deleteInterface(ie);
    }
    else if (signalID == NF_INTERFACE_STATE_CHANGED) {
        change = check_and_cast<const InterfaceEntryChangeDetails *>(obj);
        if (change->getFieldId() == InterfaceEntry::F_CARRIER || change->getFieldId() == InterfaceEntry::F_STATE) {
            ie = change->getInterfaceEntry();
            if (!ie->isUp()) {
                invalidateRoutes(ie);
            }
            else {
                RipInterfaceEntry *ripInterfacePtr = findInterfaceById(ie->getInterfaceId());
                if (ripInterfacePtr && ripInterfacePtr->mode != NO_RIP)
                    sendRIPRequest(*ripInterfacePtr);
            }
        }
    }
    else if (signalID == NF_ROUTE_DELETED) {
        // remove references to the deleted route and invalidate the RIP route
        const IRoute *route = check_and_cast<const IRoute *>(obj);
        if (route->getSource() != this) {
            for (auto & elem : ripRoutes)
                if ((elem)->getRoute() == route) {
                    (elem)->setRoute(nullptr);
                    invalidateRoute(elem);
                }
        }
    }
    else if (signalID == NF_ROUTE_ADDED) {
        // add or update the RIP route
        IRoute *route = const_cast<IRoute *>(check_and_cast<const IRoute *>(obj));
        if (route->getSource() != this) {
            if (isLoopbackInterfaceRoute(route)) {
                /*ignore*/
                ;
            }
            else if (isLocalInterfaceRoute(route)) {
                InterfaceEntry *ie = check_and_cast<InterfaceEntry *>(route->getSource());
                RipRoute *ripRoute = findRoute(ie, RipRoute::RIP_ROUTE_INTERFACE);
                if (ripRoute) {    // readded
                    RipInterfaceEntry *ripIe = findInterfaceById(ie->getInterfaceId());
                    ripRoute->setRoute(route);
                    ripRoute->setMetric(ripIe ? ripIe->metric : 1);
                    ripRoute->setChanged(true);
                    triggerUpdate();
                }
                else
                    importRoute(route, RipRoute::RIP_ROUTE_INTERFACE, getInterfaceMetric(ie));
            }
            else {
                // TODO import external routes from other routing daemons
            }
        }
    }
    else if (signalID == NF_ROUTE_CHANGED) {
        const IRoute *route = check_and_cast<const IRoute *>(obj);
        if (route->getSource() != this) {
            RipRoute *ripRoute = findRoute(route);
            if (ripRoute) {
                // TODO check and update tag
                bool changed = route->getDestinationAsGeneric() != ripRoute->getDestination() ||
                    route->getPrefixLength() != ripRoute->getPrefixLength() ||
                    route->getNextHopAsGeneric() != ripRoute->getNextHop() ||
                    route->getInterface() != ripRoute->getInterface();
                ripRoute->setDestination(route->getDestinationAsGeneric());
                ripRoute->setPrefixLength(route->getPrefixLength());
                ripRoute->setNextHop(route->getNextHopAsGeneric());
                ripRoute->setInterface(route->getInterface());
                if (changed) {
                    ripRoute->setChanged(changed);
                    triggerUpdate();
                }
            }
        }
    }
    else
        throw cRuntimeError("Unexpected signal: %s", getSignalName(signalID));
}

bool RipRouting::handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback)
{
    Enter_Method_Silent();

    if (dynamic_cast<NodeStartOperation *>(operation)) {
        if ((NodeStartOperation::Stage)stage == NodeStartOperation::STAGE_ROUTING_PROTOCOLS) {
            isOperational = true;
            cancelEvent(startupTimer);
            scheduleAt(simTime() + par("startupTime").doubleValue(), startupTimer);
            return true;
        }
    }
    else if (dynamic_cast<NodeShutdownOperation *>(operation)) {
        if ((NodeShutdownOperation::Stage)stage == NodeShutdownOperation::STAGE_ROUTING_PROTOCOLS) {
            // invalidate routes
            for (auto & elem : ripRoutes)
                invalidateRoute(elem);
            // send updates to neighbors
            for (auto & elem : ripInterfaces)
                sendRoutes(addressType->getLinkLocalRIPRoutersMulticastAddress(), ripUdpPort, elem, false);

            stopRIPRouting();

            // wait a few seconds before calling doneCallback, so that UDP can send the messages
            shutdownTimer->setContextPointer(doneCallback);
            scheduleAt(simTime() + shutdownTime, shutdownTimer);

            return false;
        }
    }
    else if (dynamic_cast<NodeCrashOperation *>(operation)) {
        if ((NodeCrashOperation::Stage)stage == NodeCrashOperation::STAGE_CRASH) {
            stopRIPRouting();
            isOperational = false;
            return true;
        }
    }

    return true;
}

void RipRouting::startRIPRouting()
{
    addressType = rt->getRouterIdAsGeneric().getAddressType();

    // configure interfaces
    configureInterfaces(par("ripConfig").xmlValue());

    // import interface routes
    configureInitialRoutes();

    // subscribe to notifications
    host->subscribe(NF_INTERFACE_CREATED, this);
    host->subscribe(NF_INTERFACE_DELETED, this);
    host->subscribe(NF_INTERFACE_STATE_CHANGED, this);
    host->subscribe(NF_ROUTE_DELETED, this);
    host->subscribe(NF_ROUTE_ADDED, this);
    host->subscribe(NF_ROUTE_CHANGED, this);

    // configure socket
    socket.setMulticastLoop(false);
    socket.bind(ripUdpPort);

    for (auto & elem : ripInterfaces)
        if (elem.mode != NO_RIP)
            socket.joinMulticastGroup(addressType->getLinkLocalRIPRoutersMulticastAddress(), elem.ie->getInterfaceId());


    for (auto & elem : ripInterfaces)
        if (elem.mode != NO_RIP)
            sendRIPRequest(elem);


    // set update timer
    scheduleAt(simTime() + updateInterval, updateTimer);
}

void RipRouting::stopRIPRouting()
{
    if (startupTimer->isScheduled())
        cancelEvent(startupTimer);
    else {
        socket.close();

        // unsubscribe to notifications
        host->unsubscribe(NF_INTERFACE_CREATED, this);
        host->unsubscribe(NF_INTERFACE_DELETED, this);
        host->unsubscribe(NF_INTERFACE_STATE_CHANGED, this);
        host->unsubscribe(NF_ROUTE_DELETED, this);
        host->unsubscribe(NF_ROUTE_ADDED, this);
        host->unsubscribe(NF_ROUTE_CHANGED, this);
    }

    // cancel timers
    cancelEvent(updateTimer);
    cancelEvent(triggeredUpdateTimer);

    // clear data
    for (auto& elem : ripRoutes)
        delete elem;
    ripRoutes.clear();
    ripInterfaces.clear();
}

void RipRouting::handleMessage(cMessage *msg)
{
    if (!isOperational) {
        if (msg->isSelfMessage())
            throw cRuntimeError("Model error: self msg '%s' received when isOperational is false", msg->getName());
        EV_ERROR << "Application is turned off, dropping '" << msg->getName() << "' message\n";
        delete msg;
        return;
    }

    if (msg->isSelfMessage()) {
        if (msg == updateTimer) {
            processUpdate(false);
            scheduleAt(simTime() + updateInterval, msg);
        }
        else if (msg == triggeredUpdateTimer) {
            processUpdate(true);
        }
        else if (msg == startupTimer) {
            startRIPRouting();
        }
        else if (msg == shutdownTimer) {
            isOperational = false;
            IDoneCallback *doneCallback = (IDoneCallback *)msg->getContextPointer();
            msg->setContextPointer(nullptr);
            doneCallback->invoke();
        }
    }
    else if (msg->getKind() == UDP_I_DATA) {
        Packet *pk = check_and_cast<Packet *>(msg);
        unsigned char command = pk->peekHeader<RipPacket>()->getCommand();
        if (command == RIP_REQUEST)
            processRequest(pk);
        else if (command == RIP_RESPONSE)
            processResponse(pk);
        else
            throw cRuntimeError("RIP: unknown command (%d)", (int)command);
    }
    else if (msg->getKind() == UDP_I_ERROR) {
        EV_DETAIL << "Ignoring UDP error report\n";
        delete msg;
    }
}

/**
 * This method called when a triggered or regular update timer expired.
 * It either sends the changed/all routes to neighbors.
 */
void RipRouting::processUpdate(bool triggered)
{
    if (triggered)
        EV_INFO << "sending triggered updates on all interfaces.\n";
    else
        EV_INFO << "sending regular updates on all interfaces\n";

    for (auto & elem : ripInterfaces)
        if (elem.mode != NO_RIP)
            sendRoutes(addressType->getLinkLocalRIPRoutersMulticastAddress(), ripUdpPort, elem, triggered);


    // clear changed flags
    for (auto & elem : ripRoutes)
        (elem)->setChanged(false);
}

/**
 * Processes a request received from a RIP router or a monitoring process.
 * The request processing follows the guidelines described in RFC 2453 3.9.1.
 *
 * There are two cases:
 * - the request enumerates the requested prefixes
 *     There is an RipEntry for each requested route in the packet.
 *     The RIP module simply looks up the prefix in its table, and
 *     if it sets the metric field of the entry to the metric of the
 *     found route, or to infinity (16) if not found. Once all entries
 *     are have been filled in, change the command from Request to Response,
 *     and sent the packet back to the requestor. If there are no
 *     entries in the request, then no response is sent; the request is
 *     silently discarded.
 * - the whole routing table is requested
 *     In this case the RipPacket contains only one entry, with addressFamilyId 0,
 *     and metric 16 (infinity). In this case the whole routing table is sent,
 *     using the normal output process (sendRoutes() method).
 */
void RipRouting::processRequest(Packet *packet)
{
    const auto& ripPacket = dynamicPtrCast<RipPacket>(packet->peekHeader<RipPacket>()->dupShared());

    int numEntries = ripPacket->getEntryArraySize();
    if (numEntries == 0) {
        EV_INFO << "received empty request, ignoring.\n";
        delete packet;
        return;
    }

    L3Address srcAddr = packet->getMandatoryTag<L3AddressInd>()->getSrcAddress();
    int srcPort = packet->getMandatoryTag<L4PortInd>()->getSrcPort();
    int interfaceId = packet->getMandatoryTag<InterfaceInd>()->getInterfaceId();

    EV_INFO << "received request from " << srcAddr << "\n";

    for (int i = 0; i < numEntries; ++i) {
        RipEntry& entry = ripPacket->getMutableEntry(i);
        switch (entry.addressFamilyId) {
            case RIP_AF_NONE:
                if (numEntries == 1 && entry.metric == RIP_INFINITE_METRIC) {
                    RipInterfaceEntry *ripInterface = findInterfaceById(interfaceId);
                    if (ripInterface)
                        sendRoutes(srcAddr, srcPort, *ripInterface, false);
                    delete packet;
                    return;
                }
                else {
                    throw cRuntimeError("RIP: invalid request.");
                }
                break;

            case RIP_AF_INET: {
                RipRoute *ripRoute = findRoute(entry.address, entry.prefixLength);
                entry.metric = ripRoute ? ripRoute->getMetric() : RIP_INFINITE_METRIC;
                // entry.nextHop, entry.routeTag?
                break;
            }

            default:
                throw cRuntimeError("RIP: request has invalid addressFamilyId: %d.", (int)entry.addressFamilyId);
        }
    }

    ripPacket->setCommand(RIP_RESPONSE);
    Packet *outPacket = new Packet("RIP response");
    ripPacket->markImmutable();
    outPacket->append(ripPacket);
    socket.sendTo(outPacket, srcAddr, srcPort);
}

/**
 * Send all or changed part of the routing table to address/port on the specified interface.
 * This method is called by regular updates (every 30s), triggered updates (when some route changed),
 * and when RIP requests are processed.
 */
void RipRouting::sendRoutes(const L3Address& address, int port, const RipInterfaceEntry& ripInterface, bool changedOnly)
{
    EV_DEBUG << "Sending " << (changedOnly ? "changed" : "all") << " routes on " << ripInterface.ie->getFullName() << std::endl;

    int maxEntries = mode == RIPv2 ? 25 : (ripInterface.ie->getMTU() - 40    /*IPv6_HEADER_BYTES*/ - 8    /*UDP_HEADER_BYTES*/ - RIP_HEADER_SIZE) / RIP_RTE_SIZE;

    Packet *pk = new Packet("RIP response");
    auto packet = makeShared<RipPacket>();
    packet->setCommand(RIP_RESPONSE);
    packet->setEntryArraySize(maxEntries);
    int k = 0;    // index into RIP entries

    for (auto & elem : ripRoutes) {
        RipRoute *ripRoute = checkRouteIsExpired(elem);
        if (!ripRoute)
            continue;

        if (changedOnly && !ripRoute->isChanged())
            continue;

        // Split Horizon check:
        //   Omit routes learned from one neighbor in updates sent to that neighbor.
        //   In the case of a broadcast network, all routes learned from any neighbor on
        //   that network are omitted from updates sent on that network.
        // Split Horizon with Poisoned Reverse:
        //   Do include such routes in updates, but sets their metrics to infinity.
        int metric = ripRoute->getMetric();
        if (ripRoute->getInterface() == ripInterface.ie) {
            if (ripInterface.mode == SPLIT_HORIZON)
                continue;
            else if (ripInterface.mode == SPLIT_HORIZON_POISONED_REVERSE)
                metric = RIP_INFINITE_METRIC;
        }

        EV_DEBUG << "Add entry for " << ripRoute->getDestination() << "/" << ripRoute->getPrefixLength() << ": "
                 << " metric=" << metric << std::endl;

        // fill next entry
        RipEntry& entry = packet->getMutableEntry(k++);
        entry.addressFamilyId = RIP_AF_INET;
        entry.address = ripRoute->getDestination();
        entry.prefixLength = ripRoute->getPrefixLength();
        entry.nextHop = addressType->getUnspecifiedAddress();    //route->getNextHop() if local ?
        entry.routeTag = ripRoute->getRouteTag();
        entry.metric = metric;

        // if packet is full, then send it and allocate a new one
        if (k >= maxEntries) {
            packet->setChunkLength(B(RIP_HEADER_SIZE + RIP_RTE_SIZE * packet->getEntryArraySize()));
            packet->markImmutable();
            pk->append(packet);

            emit(sentUpdateSignal, pk);
            sendPacket(pk, address, port, ripInterface.ie);
            pk = new Packet("RIP response");
            packet = makeShared<RipPacket>();
            packet->setCommand(RIP_RESPONSE);
            packet->setEntryArraySize(maxEntries);
            k = 0;
        }
    }

    // send last packet if it has entries
    if (k > 0) {
        packet->setEntryArraySize(k);
        packet->setChunkLength(B(RIP_HEADER_SIZE + RIP_RTE_SIZE * packet->getEntryArraySize()));
        packet->markImmutable();
        pk->append(packet);

        emit(sentUpdateSignal, pk);
        sendPacket(pk, address, port, ripInterface.ie);
    }
    else
        delete pk;
}

/**
 * Processes the RIP response and updates the routing table.
 *
 * First it validates the packet to avoid corrupting the routing
 * table with a wrong packet. Valid responses must come from a neighboring
 * RIP router.
 *
 * Next each RipEntry is processed one by one. Check that destination address
 * and metric are valid. Then compute the new metric by adding the metric
 * of the interface to the metric found in the entry.
 *
 *   If there is no route to the destination, and the new metric is not infinity,
 *   then add a new route to the routing table.
 *
 *   If there is an existing route to the destination,
 *
 * 1. validate packet
 * 2. for each entry:
 *      metric = MIN(p.metric + cost of if it arrived at, infinity)
 *      if there is no route for the dest address:
 *        add new route to the routing table unless the metric is infinity
 *      else:
 *        if received from the route.gateway
 *          reinitialize timeout
 *        if (received from route.gateway AND route.metric != metric) OR metric < route.metric
 *          updateRoute(route)
 */
void RipRouting::processResponse(Packet *packet)
{
    emit(rcvdResponseSignal, packet);

    bool isValid = isValidResponse(packet);
    if (!isValid) {
        EV_INFO << "dropping invalid response.\n";
        emit(badResponseSignal, packet);
        delete packet;
        return;
    }

    L3Address srcAddr = packet->getMandatoryTag<L3AddressInd>()->getSrcAddress();
    int interfaceId = packet->getMandatoryTag<InterfaceInd>()->getInterfaceId();
    packet->clearTags();

    RipInterfaceEntry *incomingIe = findInterfaceById(interfaceId);
    if (!incomingIe) {
        EV_INFO << "dropping unexpected RIP response.\n";
        emit(badResponseSignal, packet);
        delete packet;
        return;
    }

    const auto& ripPacket = packet->peekHeader<RipPacket>();

    EV_INFO << "response received from " << srcAddr << "\n";
    int numEntries = ripPacket->getEntryArraySize();
    for (int i = 0; i < numEntries; ++i) {
        const RipEntry& entry = ripPacket->getEntry(i);
        int metric = std::min((int)entry.metric + incomingIe->metric, RIP_INFINITE_METRIC);
        L3Address nextHop = entry.nextHop.isUnspecified() ? srcAddr : entry.nextHop;

        RipRoute *ripRoute = findRoute(entry.address, entry.prefixLength);
        if (ripRoute) {
            RipRoute::RouteType routeType = ripRoute->getType();
            int routeMetric = ripRoute->getMetric();
            if ((routeType == RipRoute::RIP_ROUTE_STATIC || routeType == RipRoute::RIP_ROUTE_DEFAULT) && routeMetric != RIP_INFINITE_METRIC)
                continue;
            if (ripRoute->getFrom() == srcAddr)
                ripRoute->setLastUpdateTime(simTime());
            if ((ripRoute->getFrom() == srcAddr && ripRoute->getMetric() != metric) || metric < ripRoute->getMetric())
                updateRoute(ripRoute, incomingIe->ie, nextHop, metric, entry.routeTag, srcAddr);
            // TODO RIPng: if the metric is the same as the old one, and the old route is aboute to expire (i.e. at least halfway to the expiration point)
            //             then update the old route with the new RTE
        }
        else {
            if (metric != RIP_INFINITE_METRIC)
                addRoute(entry.address, entry.prefixLength, incomingIe->ie, nextHop, metric, entry.routeTag, srcAddr);
        }
    }

    delete packet;
}

bool RipRouting::isValidResponse(Packet *packet)
{
    // check that received from ripUdpPort
    if (packet->getMandatoryTag<L4PortInd>()->getSrcPort() != ripUdpPort) {
        EV_WARN << "source port is not " << ripUdpPort << "\n";
        return false;
    }

    L3Address srcAddr = packet->getMandatoryTag<L3AddressInd>()->getSrcAddress();

    // check that it is not our response (received own multicast message)
    if (rt->isLocalAddress(srcAddr)) {
        EV_WARN << "received own response\n";
        return false;
    }

    if (mode == RIPng) {
        if (!srcAddr.isLinkLocal()) {
            EV_WARN << "source address is not link-local: " << srcAddr << "\n";
            return false;
        }
        if (packet->getMandatoryTag<HopLimitInd>()->getHopLimit() != 255) {
            EV_WARN << "ttl is not 255";
            return false;
        }
    }
    else {
        // check that source is on a directly connected network
        if (!ift->isNeighborAddress(srcAddr)) {
            EV_WARN << "source is not directly connected " << srcAddr << "\n";
            return false;
        }
    }

    const auto& ripPacket = packet->peekHeader<RipPacket>();
    // validate entries
    int numEntries = ripPacket->getEntryArraySize();
    for (int i = 0; i < numEntries; ++i) {
        const RipEntry& entry = ripPacket->getEntry(i);

        // check that metric is in range [1,16]
        if (entry.metric < 1 || entry.metric > RIP_INFINITE_METRIC) {
            EV_WARN << "received metric is not in the [1," << RIP_INFINITE_METRIC << "] range.\n";
            return false;
        }

        // check that destination address is a unicast address
        // TODO exclude 0.x.x.x, 127.x.x.x too
        if (!entry.address.isUnicast()) {
            EV_WARN << "destination address of an entry is not unicast: " << entry.address << "\n";
            return false;
        }

        if (mode == RIPng) {
            if (entry.address.isLinkLocal()) {
                EV_WARN << "destination address of an entry is link-local: " << entry.address << "\n";
                return false;
            }
            if (entry.prefixLength < 0 || entry.prefixLength > addressType->getMaxPrefixLength()) {
                EV_WARN << "prefixLength is outside of the [0," << addressType->getMaxPrefixLength() << "] interval\n";
                return false;
            }
        }
    }

    return true;
}

/**
 * RFC 2453 3.9.2:
 *
 * Adding a route to the routing table consists of:
 *
 * - Setting the destination address to the destination address in the RTE
 * - Setting the metric to the newly calculated metric
 * - Set the next hop address to be the address of the router from which
 *   the datagram came
 * - Initialize the timeout for the route.  If the garbage-collection
 *   timer is running for this route, stop it
 * - Set the route change flag
 * - Signal the output process to trigger an update
 */
void RipRouting::addRoute(const L3Address& dest, int prefixLength, const InterfaceEntry *ie, const L3Address& nextHop,
        int metric, uint16 routeTag, const L3Address& from)
{
    EV_DEBUG << "Add route to " << dest << "/" << prefixLength << ": "
             << "nextHop=" << nextHop << " metric=" << metric << std::endl;

    IRoute *route = addRoute(dest, prefixLength, ie, nextHop, metric);

    RipRoute *ripRoute = new RipRoute(route, RipRoute::RIP_ROUTE_RTE, metric, routeTag);
    ripRoute->setFrom(from);
    ripRoute->setLastUpdateTime(simTime());
    ripRoute->setChanged(true);
    ripRoutes.push_back(ripRoute);
    emit(numRoutesSignal, (unsigned long)ripRoutes.size());
    triggerUpdate();
}

/**
 * Updates an existing route with the information learned from a RIP packet.
 * If the metric is infinite (16), then the route is invalidated.
 * It triggers an update, so neighbor routers are notified about the change.
 *
 * RFC 2453 3.9.2:
 *
 * Do the following actions:
 *
 *  - Adopt the route from the datagram (i.e., put the new metric in and
 *    adjust the next hop address, if necessary).
 *  - Set the route change flag and signal the output process to trigger
 *    an update
 *  - If the new metric is infinity, start the deletion process
 *    (described above); otherwise, re-initialize the timeout
 */
void RipRouting::updateRoute(RipRoute *ripRoute, const InterfaceEntry *ie, const L3Address& nextHop, int metric, uint16 routeTag, const L3Address& from)
{
    //ASSERT(ripRoute && ripRoute->getType() == RipRoute::RIP_ROUTE_RTE);
    //ASSERT(!ripRoute->getRoute() || ripRoute->getRoute()->getSource() == this);

    EV_DEBUG << "Updating route to " << ripRoute->getDestination() << "/" << ripRoute->getPrefixLength() << ": "
             << "nextHop=" << nextHop << " metric=" << metric << std::endl;

    int oldMetric = ripRoute->getMetric();
    ripRoute->setInterface(const_cast<InterfaceEntry *>(ie));
    ripRoute->setMetric(metric);
    ripRoute->setFrom(from);
    ripRoute->setRouteTag(routeTag);

    if (oldMetric == RIP_INFINITE_METRIC && metric < RIP_INFINITE_METRIC) {
        ASSERT(!ripRoute->getRoute());
        ripRoute->setType(RipRoute::RIP_ROUTE_RTE);
        ripRoute->setNextHop(nextHop);

        IRoute *route = addRoute(ripRoute->getDestination(), ripRoute->getPrefixLength(), ie, nextHop, metric);
        ripRoute->setRoute(route);
    }
    if (oldMetric != RIP_INFINITE_METRIC) {
        IRoute *route = ripRoute->getRoute();
        ASSERT(route);

        ripRoute->setRoute(nullptr);
        deleteRoute(route);

        ripRoute->setNextHop(nextHop);
        if (metric < RIP_INFINITE_METRIC) {
            route = addRoute(ripRoute->getDestination(), ripRoute->getPrefixLength(), ie, nextHop, metric);
            ripRoute->setRoute(route);
        }
    }

    ripRoute->setChanged(true);
    triggerUpdate();

    if (metric == RIP_INFINITE_METRIC && oldMetric != RIP_INFINITE_METRIC)
        invalidateRoute(ripRoute);
    else
        ripRoute->setLastUpdateTime(simTime());
}

/**
 * Sets the update timer to trigger an update in the [1s,5s] interval.
 * If the update is already scheduled, it does nothing.
 */
void RipRouting::triggerUpdate()
{
    if (!triggeredUpdateTimer->isScheduled()) {
        double delay = par("triggeredUpdateDelay");
        simtime_t updateTime = simTime() + delay;
        // Triggered updates may be suppressed if a regular
        // update is due by the time the triggered update would be sent.
        if (!updateTimer->isScheduled() || updateTimer->getArrivalTime() > updateTime)
            scheduleAt(updateTime, triggeredUpdateTimer);
    }
}

/**
 * Should be called regularly to handle expiry and purge of routes.
 * If the route is valid, then returns it, otherwise returns nullptr.
 */
RipRoute *RipRouting::checkRouteIsExpired(RipRoute *route)
{
    if (route->getType() == RipRoute::RIP_ROUTE_RTE) {
        simtime_t now = simTime();
        if (now >= route->getLastUpdateTime() + routeExpiryTime + routePurgeTime) {
            purgeRoute(route);
            return nullptr;
        }
        if (now >= route->getLastUpdateTime() + routeExpiryTime) {
            invalidateRoute(route);
            return nullptr;
        }
    }
    return route;
}

/*
 * Invalidates the route, i.e. marks it invalid, but keeps it in the routing table for 120s,
 * so the neighbors are notified about the broken route in the next update.
 *
 * Called when the timeout expires, or a metric is set to 16 because an update received from the current router.
 * It will
 * - set purgeTime to expiryTime + 120s
 * - set metric of the route to 16 (infinity)
 * - set routeChangeFlag
 * - signal the output process to trigger a response
 */
void RipRouting::invalidateRoute(RipRoute *ripRoute)
{
    IRoute *route = ripRoute->getRoute();
    if (route) {
        ripRoute->setRoute(nullptr);
        deleteRoute(route);
    }
    ripRoute->setMetric(RIP_INFINITE_METRIC);
    ripRoute->setChanged(true);
    triggerUpdate();
}

/**
 * Removes the route from the routing table.
 */
void RipRouting::purgeRoute(RipRoute *ripRoute)
{
    ASSERT(ripRoute->getType() == RipRoute::RIP_ROUTE_RTE);

    IRoute *route = ripRoute->getRoute();
    if (route) {
        ripRoute->setRoute(nullptr);
        deleteRoute(route);
    }

    remove(ripRoutes, ripRoute);
    delete ripRoute;

    emit(numRoutesSignal, (unsigned long)ripRoutes.size());
}

/**
 * Sends the packet to the specified destination.
 * If the destAddr is a multicast, then the destInterface must be specified.
 */
void RipRouting::sendPacket(Packet *packet, const L3Address& destAddr, int destPort, const InterfaceEntry *destInterface)
{
    if (destAddr.isMulticast()) {
        packet->ensureTag<InterfaceReq>()->setInterfaceId(destInterface->getInterfaceId());
        if (mode == RIPng) {
            socket.setTimeToLive(255);
            packet->ensureTag<L3AddressReq>()->setSrcAddress(addressType->getLinkLocalAddress(destInterface));
        }
    }
    socket.sendTo(packet, destAddr, destPort);
}

/*----------------------------------------
 *      private methods
 *----------------------------------------*/

RipInterfaceEntry *RipRouting::findInterfaceById(int interfaceId)
{
    for (auto & elem : ripInterfaces)
        if (elem.ie->getInterfaceId() == interfaceId)
            return &(elem);

    return nullptr;
}

RipRoute *RipRouting::findRoute(const L3Address& destination, int prefixLength)
{
    for (auto & elem : ripRoutes)
        if ((elem)->getDestination() == destination && (elem)->getPrefixLength() == prefixLength)
            return elem;

    return nullptr;
}

RipRoute *RipRouting::findRoute(const L3Address& destination, int prefixLength, RipRoute::RouteType type)
{
    for (auto & elem : ripRoutes)
        if ((elem)->getType() == type && (elem)->getDestination() == destination && (elem)->getPrefixLength() == prefixLength)
            return elem;

    return nullptr;
}

RipRoute *RipRouting::findRoute(const IRoute *route)
{
    for (auto & elem : ripRoutes)
        if ((elem)->getRoute() == route)
            return elem;

    return nullptr;
}

RipRoute *RipRouting::findRoute(const InterfaceEntry *ie, RipRoute::RouteType type)
{
    for (auto & elem : ripRoutes)
        if ((elem)->getType() == type && (elem)->getInterface() == ie)
            return elem;

    return nullptr;
}

void RipRouting::addInterface(const InterfaceEntry *ie, cXMLElement *config)
{
    RipInterfaceEntry ripInterface(ie);
    if (config)
        ripInterface.configure(config);
    ripInterfaces.push_back(ripInterface);
}

void RipRouting::deleteInterface(const InterfaceEntry *ie)
{
    // delete interfaces and routes referencing ie
    for (auto it = ripInterfaces.begin(); it != ripInterfaces.end(); ) {
        if (it->ie == ie)
            it = ripInterfaces.erase(it);
        else
            it++;
    }
    bool emitNumRoutesSignal = false;
    for (auto it = ripRoutes.begin(); it != ripRoutes.end(); ) {
        if ((*it)->getInterface() == ie) {
            delete *it;
            it = ripRoutes.erase(it);
            emitNumRoutesSignal = true;
        }
        else
            it++;
    }
    if (emitNumRoutesSignal)
        emit(numRoutesSignal, (unsigned long)ripRoutes.size());
}

int RipRouting::getInterfaceMetric(InterfaceEntry *ie)
{
    RipInterfaceEntry *ripIe = findInterfaceById(ie->getInterfaceId());
    return ripIe ? ripIe->metric : 1;
}

void RipRouting::invalidateRoutes(const InterfaceEntry *ie)
{
    for (auto & elem : ripRoutes)
        if ((elem)->getInterface() == ie)
            invalidateRoute(elem);

}

IRoute *RipRouting::addRoute(const L3Address& dest, int prefixLength, const InterfaceEntry *ie, const L3Address& nextHop, int metric)
{
    IRoute *route = rt->createRoute();
    route->setSourceType(IRoute::RIP);
    route->setSource(this);
    route->setDestination(dest);
    route->setPrefixLength(prefixLength);
    route->setInterface(const_cast<InterfaceEntry *>(ie));
    route->setNextHop(nextHop);
    route->setMetric(metric);
    rt->addRoute(route);
    return route;
}

void RipRouting::deleteRoute(IRoute *route)
{
    rt->deleteRoute(route);
}

bool RipRouting::isLoopbackInterfaceRoute(const IRoute *route)
{
    InterfaceEntry *ie = dynamic_cast<InterfaceEntry *>(route->getSource());
    return ie && ie->isLoopback();
}

bool RipRouting::isLocalInterfaceRoute(const IRoute *route)
{
    InterfaceEntry *ie = dynamic_cast<InterfaceEntry *>(route->getSource());
    return ie && !ie->isLoopback();
}

} // namespace inet

